#!/usr/bin/env python3
"""Run the CPython regression suite with a target interpreter and emit a
normalized JSON parity manifest.

This is the *portable* half of the parity harness. The exact same script runs:
  - on the build host against the reference CPython 3.11.14 build, and
  - inside the 9front VM against the ported interpreter.

Because both sides run THIS script over the SAME vendored Lib/test tree, the
test code is byte-identical and any pass/fail difference is attributable to the
platform, not to version skew.

Stdlib-only and written in conservative 3.11 syntax so it also runs under the
(eventually) ported interpreter. Do not add third-party imports.

Usage:
    run_suite.py --python ./python --out manifests/host-reference.json
    run_suite.py --python /n/c/python --out /tmp/port.json --tests test_os test_io

The manifest schema (v1):
    {
      "meta": { interpreter, version, platform, machine, argv,
                started_utc, duration_s, regrtest_returncode,
                module_summary: { good, fail, error, skip, ... } },
      "tests":   { "<module>::<Class>::<method>": "pass|fail|error|skip" },
      "modules": { "<module>": "good|failed|error|skipped|missing" }
    }
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


def parse_junit(xml_path):
    """Return {test_id: status} from a regrtest --junit-xml file.

    A <testcase> with no failure/error/skipped child is a pass. test_id is
    "<classname>::<name>"; classname already encodes the module for unittest
    based tests (e.g. "test.test_os.PosixTester").
    """
    tests = {}
    if not os.path.exists(xml_path):
        return tests
    try:
        tree = ET.parse(xml_path)
    except ET.ParseError as exc:
        sys.stderr.write("WARN: could not parse junit xml: %s\n" % exc)
        return tests
    root = tree.getroot()
    # Accept either <testsuites><testsuite>... or a bare <testsuite>.
    suites = root.iter("testsuite")
    for suite in suites:
        for case in suite.iter("testcase"):
            cls = case.get("classname") or suite.get("name") or "?"
            name = case.get("name") or "?"
            tid = "%s::%s" % (cls, name)
            status = "pass"
            for child in case:
                tag = child.tag.lower()
                if tag.endswith("failure"):
                    status = "fail"
                elif tag.endswith("error"):
                    status = "error"
                elif tag.endswith("skipped"):
                    status = "skip"
            tests[tid] = status
    return tests


# regrtest prints a final summary block. We scrape per-module outcomes from it
# as a robust backstop -- on a half-ported interpreter a whole test module can
# segfault or fail to import, in which case junit never sees it.
_SUMMARY_SECTIONS = {
    "failed": re.compile(r"^\d+ tests? failed:\s*$"),
    "error": re.compile(r"^\d+ tests? (?:failed to load|with errors):\s*$"),
    "skipped": re.compile(r"^\d+ tests? skipped:\s*$"),
}


def parse_module_summary(stdout, all_tests):
    """Map each module name to good|failed|error|skipped|missing."""
    modules = {}
    current = None
    for raw in stdout.splitlines():
        line = raw.strip()
        matched = None
        for label, rx in _SUMMARY_SECTIONS.items():
            if rx.match(line):
                matched = label
                break
        if matched:
            current = matched
            continue
        if current and line.startswith("test_"):
            for name in line.split():
                modules[name] = current
        elif current and not line:
            current = None
    for name in all_tests:
        modules.setdefault(name, "good")
    return modules


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--python", required=True,
                    help="path to the target interpreter to validate")
    ap.add_argument("--out", required=True, help="manifest JSON output path")
    ap.add_argument("--tests", nargs="*", default=None,
                    help="explicit test module list (default: whole suite)")
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel workers (0 = auto; use 1 in the VM)")
    ap.add_argument("--timeout", type=int, default=1200,
                    help="per-test timeout in seconds passed to regrtest")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[],
                    help="extra args passed verbatim to regrtest")
    args = ap.parse_args()

    py = os.path.abspath(args.python) if os.path.exists(args.python) else args.python
    workdir = tempfile.mkdtemp(prefix="parity-")
    xml_path = os.path.join(workdir, "results.xml")

    # Default resource set (no -u): network/largefile/etc tests are skipped
    # deterministically on BOTH sides, which is exactly what we want.
    cmd = [py, "-m", "test",
           "--junit-xml", xml_path,
           "-j", str(args.jobs),
           "--timeout", str(args.timeout)]
    cmd += args.extra
    if args.tests:
        cmd += args.tests

    sys.stderr.write("RUN: %s\n" % " ".join(cmd))
    started = time.time()
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          universal_newlines=True)
    duration = time.time() - started
    sys.stdout.write(proc.stdout)

    tests = parse_junit(xml_path)
    # Derive the set of modules that were targeted so "missing" is meaningful.
    targeted = set(args.tests) if args.tests else set()
    modules = parse_module_summary(proc.stdout, targeted)

    # Best-effort interpreter identity.
    try:
        ver = subprocess.check_output(
            [py, "-c", "import platform,sys;"
                       "print(platform.python_version());"
                       "print(sys.platform);"
                       "print(platform.machine())"],
            universal_newlines=True).strip().splitlines()
        version, platform_name, machine = (ver + ["", "", ""])[:3]
    except Exception as exc:  # noqa: BLE001 - identity is non-critical
        version, platform_name, machine = "unknown", sys.platform, ""
        sys.stderr.write("WARN: interpreter identity probe failed: %s\n" % exc)

    counts = {}
    for status in tests.values():
        counts[status] = counts.get(status, 0) + 1

    manifest = {
        "schema": "parity-manifest/v1",
        "meta": {
            "interpreter": py,
            "version": version,
            "platform": platform_name,
            "machine": machine,
            "argv": cmd,
            "started_utc": datetime.datetime.utcnow().isoformat() + "Z",
            "duration_s": round(duration, 1),
            "regrtest_returncode": proc.returncode,
            "testcase_counts": counts,
        },
        "tests": tests,
        "modules": modules,
    }

    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as fh:
        json.dump(manifest, fh, indent=1, sort_keys=True)
    sys.stderr.write(
        "\nWROTE %s: %d testcases (%s), %d modules, rc=%d in %.0fs\n"
        % (out, len(tests),
           ", ".join("%s=%d" % kv for kv in sorted(counts.items())),
           len(modules), proc.returncode, duration))
    # Exit 0 even if regrtest found failures -- this tool records, it does not gate.
    return 0


if __name__ == "__main__":
    sys.exit(main())
