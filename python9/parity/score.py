#!/usr/bin/env python3
"""Diff a port manifest against the reference manifest and compute the parity
score.

The bar is NOT 100% of the suite -- it is: of every testcase that PASSES on the
reference build and is NOT on the justified skip-list, what fraction passes on
the port?

    parity = port_passes_applicable / applicable_reference_passes

A "regression" is a testcase that passes on the reference, is not skipped, and
does not pass on the port (it failed, errored, or never ran). Those are the
work queue. Everything moved to skiplist.txt must carry a one-line reason.

Stdlib-only, conservative 3.11 syntax.

Usage:
    score.py --reference manifests/host-reference.json \
             --port      manifests/9front-port.json \
             --skiplist  skiplist.txt
"""

import argparse
import json
import sys


def load(path):
    with open(path) as fh:
        return json.load(fh)


def load_skiplist(path):
    """Return list of (pattern, reason). A pattern matches a test_id by exact
    match or as a prefix ending at a '::' boundary (so a module name skips all
    its cases)."""
    rules = []
    if not path:
        return rules
    try:
        fh = open(path)
    except FileNotFoundError:
        sys.stderr.write("WARN: skiplist %s not found; treating as empty\n" % path)
        return rules
    with fh:
        for raw in fh:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            reason = ""
            if "#" in raw:
                reason = raw.split("#", 1)[1].strip()
            rules.append((line, reason))
    return rules


def is_skipped(test_id, rules):
    for pattern, _reason in rules:
        if test_id == pattern:
            return True
        # prefix match at a path boundary: "test.test_os" skips
        # "test.test_os::Cls::meth" and "test.test_os.Sub::..."
        if test_id.startswith(pattern) and test_id[len(pattern):][:1] in ("", ":", "."):
            return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reference", required=True)
    ap.add_argument("--port", required=True)
    ap.add_argument("--skiplist", default=None)
    ap.add_argument("--json", default=None, help="write the full diff report here")
    ap.add_argument("--show", type=int, default=25,
                    help="how many regressions to list (default 25)")
    args = ap.parse_args()

    ref = load(args.reference)
    port = load(args.port)
    rules = load_skiplist(args.skiplist)

    ref_tests = ref.get("tests", {})
    port_tests = port.get("tests", {})

    applicable = []          # ref passes, not skipped
    skipped_but_ref_pass = []
    regressions = []         # applicable but port != pass
    fixed_or_ok = []         # applicable and port == pass

    for tid, ref_status in ref_tests.items():
        if ref_status != "pass":
            continue
        if is_skipped(tid, rules):
            skipped_but_ref_pass.append(tid)
            continue
        applicable.append(tid)
        port_status = port_tests.get(tid, "missing")
        if port_status == "pass":
            fixed_or_ok.append(tid)
        else:
            regressions.append((tid, port_status))

    n_app = len(applicable)
    n_pass = len(fixed_or_ok)
    parity = (n_pass / n_app) if n_app else 0.0

    # Module-level health from the port side (catches whole-module crashes).
    port_modules = port.get("modules", {})
    bad_modules = sorted(m for m, s in port_modules.items()
                         if s in ("failed", "error", "missing"))

    print("=" * 64)
    print("PARITY REPORT")
    print("=" * 64)
    print("reference : %s  (%s on %s)" % (
        args.reference, ref.get("meta", {}).get("version", "?"),
        ref.get("meta", {}).get("platform", "?")))
    print("port      : %s  (%s on %s)" % (
        args.port, port.get("meta", {}).get("version", "?"),
        port.get("meta", {}).get("platform", "?")))
    print("-" * 64)
    print("reference passing testcases : %d" % sum(
        1 for s in ref_tests.values() if s == "pass"))
    print("justified skips (ref-pass)  : %d" % len(skipped_but_ref_pass))
    print("applicable testcases        : %d" % n_app)
    print("port passing (applicable)   : %d" % n_pass)
    print("regressions                 : %d" % len(regressions))
    print("-" * 64)
    print("PARITY SCORE                : %.2f%%  (%d / %d)" % (
        parity * 100.0, n_pass, n_app))
    print("=" * 64)

    if bad_modules:
        print("\nport modules failing/missing (%d):" % len(bad_modules))
        for m in bad_modules[:args.show]:
            print("  %-28s %s" % (m, port_modules[m]))
        if len(bad_modules) > args.show:
            print("  ... (%d more)" % (len(bad_modules) - args.show))

    if regressions:
        print("\ntop regressions (port status):")
        by_status = {}
        for tid, st in regressions:
            by_status.setdefault(st, []).append(tid)
        for st in sorted(by_status):
            ids = by_status[st]
            print("  [%s] %d" % (st, len(ids)))
            for tid in ids[:args.show]:
                print("      %s" % tid)
            if len(ids) > args.show:
                print("      ... (%d more)" % (len(ids) - args.show))

    if args.json:
        report = {
            "parity": parity,
            "applicable": n_app,
            "port_passing": n_pass,
            "regressions": [{"test": t, "port_status": s} for t, s in regressions],
            "justified_skips": sorted(skipped_but_ref_pass),
            "port_bad_modules": {m: port_modules[m] for m in bad_modules},
        }
        with open(args.json, "w") as fh:
            json.dump(report, fh, indent=1, sort_keys=True)
        print("\nwrote diff report -> %s" % args.json)

    return 0


if __name__ == "__main__":
    sys.exit(main())
