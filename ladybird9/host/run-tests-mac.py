#!/usr/bin/env python3
"""run-tests-mac.py — run the same LibWeb test set on the macOS build, batched
identically to run-tests9.py, so the two totals are comparable.

macOS has no leak to work around, but it is batched anyway: the point is that
both sides run the SAME partition of the SAME enumerated tests, so a difference
in the totals is a difference in the engine and not in how the run was sliced.
"""
import argparse
import re
import subprocess
import sys

ROOT = "Tests/LibWeb"
BIN = "Build/release/bin/test-web"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default="/Users/claw/Projects/agent9/ladybird9/vendor/ladybird")
    ap.add_argument("--wpt", action="store_true")
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--batch", type=int, default=60)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--tests-from", help="file of test paths (or a test-web --dry-run "
                                         "listing) to run instead of enumerating locally")
    args = ap.parse_args()

    # The 9front box carries an OLDER copy of Tests/LibWeb than this checkout
    # (2825 native tests there vs 3852 here). Enumerating locally would compare
    # two different corpora and call the difference a parity gap, so the box's
    # own list is passed in and used verbatim.
    if args.tests_from:
        src = open(args.tests_from).read()
    else:
        src = subprocess.run([BIN, "--test-path", ROOT, "--dry-run"],
                             cwd=args.repo, capture_output=True, text=True).stdout
    tests = [m.group(1) for line in src.split("\n")
             if (m := re.match(r"\s*\d+/\d+:\s*(\S+)", line))]
    if not tests:   # a plain list of paths, one per line
        tests = [l.strip() for l in src.split("\n") if l.strip() and not l.startswith("#")]
    if not args.wpt:
        tests = [t for t in tests if "wpt-import" not in t]
    tests.sort()
    chunks = [tests[i:i + args.batch] for i in range(0, len(tests), args.batch)]
    print(f"{len(tests)} tests in {len(chunks)} chunks of {args.batch}\n", flush=True)

    tot = {"Pass": 0, "Fail": 0, "Skipped": 0, "Timeout": 0, "Crashed": 0}
    missing = 0
    for i, ch in enumerate(chunks, 1):
        cmd = [BIN, "--test-path", ROOT, "-j", str(args.jobs), "-t", str(args.timeout)]
        for t in ch:
            cmd += ["-f", t]
        r = subprocess.run(cmd, cwd=args.repo, capture_output=True, text=True)
        out = r.stdout + r.stderr
        m = re.search(r"Pass: (\d+), Fail: (\d+), Skipped: (\d+), "
                      r"Timeout: (\d+), Crashed: (\d+)", out)
        label = f"{ch[0].rsplit('/', 1)[0]} +{len(ch)}"
        if not m:
            print(f"[{i}/{len(chunks)}] {label:44s} NO RESULT ({len(ch)})", flush=True)
            missing += len(ch)
            continue
        p, f, s, t, c = (int(x) for x in m.groups())
        for k, v in zip(tot, (p, f, s, t, c)):
            tot[k] += v
        seen = p + f + s + t + c
        if seen != len(ch):
            missing += len(ch) - seen
            flag = f"  UNACCOUNTED {len(ch) - seen}"
        else:
            flag = "" if (f or t or c) else "  ok"
        print(f"[{i}/{len(chunks)}] {label:44s} pass {p:4d} fail {f:3d} "
              f"skip {s:3d} timeout {t:3d} crash {c:3d}{flag}", flush=True)

    ran = sum(tot.values()) - tot["Skipped"]
    print("\n== totals ==")
    for k, v in tot.items():
        print(f"  {k:8s} {v}")
    print(f"  {'Missing':8s} {missing}")
    if ran:
        print(f"  pass rate (excl. skipped): {100.0 * tot['Pass'] / ran:.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
