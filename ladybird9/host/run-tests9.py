#!/usr/bin/env python3
"""run-tests9.py — drive the LibWeb test suite on the 9front box, in chunks.

  run-tests9.py [--host H] [--port P] [--wpt] [--prefix DIR] [--tests DIR]

Why chunked instead of one `test-web` run over the whole tree: WebContent has a
retain leak (per-test Documents stay reachable), so a single long run degrades —
measured 93.5% chunked vs 33% continuous on the same binaries. Worse, a run that
grows unbounded can drive the box out of memory, at which point listen1 can no
longer fork and the machine needs a power cycle (see the wedged-listener notes).
Chunking by test directory bounds each process's lifetime, so the numbers reflect
the engine rather than the leak.

Each chunk is one `test-web` invocation listing --batch exact test paths, one per
-f. Between chunks every helper is killed by name: killing only test-web orphans
the five helpers it spawned, and those accumulate until the box dies.

Output: one line per chunk plus a totals block, so a bad chunk is attributable
instead of vanishing into an aggregate.

ponytail: fixed-size batches, not per-directory. Directory globs would be the
obvious chunking, but test-web's `*` matches `/`, so `-f Text/input/*` selects
every one of the 5441 tests below it and the chunks overlap. Exact paths are
disjoint, which is what makes the totals trustworthy.
"""
import argparse
import re
import socket
import sys
import time

HELPERS = "test-web WebContent Compositor WebWorker RequestServer ImageDecoder"


def once(host, port, cmd, wait):
    s = socket.create_connection((host, port), timeout=20)
    s.sendall(cmd.encode() + b"\n")
    s.shutdown(socket.SHUT_WR)
    s.settimeout(wait)
    out = b""
    try:
        while True:
            b = s.recv(8192)
            if not b:
                break
            out += b
    except socket.timeout:
        pass
    s.close()
    return out.decode("latin-1")


def run(host, port, cmd, wait=600, tries=5):
    """Retry while the box is fork-starved: it still completes the TCP handshake
    (the kernel does that for an announced port), but can't spawn a shell, so the
    reply is empty rather than refused."""
    for i in range(tries):
        try:
            r = once(host, port, cmd, wait)
            if r.strip():
                return r
        except Exception:
            pass
        if i < tries - 1:
            time.sleep(15)
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.88.159")
    ap.add_argument("--port", type=int, default=17010)
    ap.add_argument("--prefix", default="/usr/glenda/ladybird9")
    ap.add_argument("--tests", default="/usr/glenda/lbtest/Tests/LibWeb")
    ap.add_argument("--wpt", action="store_true", help="include the wpt-import corpus")
    ap.add_argument("--timeout", type=int, default=30, help="per-test timeout (s)")
    ap.add_argument("--batch", type=int, default=60, help="tests per chunk")
    ap.add_argument("--tests-from", help="reuse a saved --dry-run listing "
                                        "instead of re-enumerating on the box")
    ap.add_argument("--save-list", help="write the enumeration here for reuse")
    ap.add_argument("--start", type=int, default=1, help="resume from chunk N")

    args = ap.parse_args()

    if args.tests_from:
        listing = open(args.tests_from).read()
    else:
        print("== enumerating ==", flush=True)
        listing = run(args.host, args.port,
                      f"cd {args.prefix}; bin/test-web --test-path {args.tests} --dry-run >[2=1]",
                      wait=400)
        if args.save_list:
            open(args.save_list, "w").write(listing)
    tests = [m.group(1) for line in listing.split("\n")
             if (m := re.match(r"\s*\d+/\d+:\s*(\S+)", line))]
    if not tests:
        print("no tests enumerated — is the box up?", file=sys.stderr)
        return 1
    if not args.wpt:
        tests = [t for t in tests if "wpt-import" not in t]

    # Chunks are fixed-size batches of EXACT test paths, each passed as its own
    # -f. Directory globs cannot be used here: test-web's `*` matches `/` too, so
    # `-f Text/input/*` selects all 5441 tests beneath it, and per-directory
    # chunks would overlap and multiply-count every nested test. A glob with no
    # wildcard matches exactly one test, which makes the partition disjoint and
    # the totals mean what they say.
    tests.sort()
    chunks = [tests[i:i + args.batch] for i in range(0, len(tests), args.batch)]
    print(f"{len(tests)} tests in {len(chunks)} chunks of {args.batch}\n", flush=True)

    tot = {"Pass": 0, "Fail": 0, "Skipped": 0, "Timeout": 0, "Crashed": 0}
    missing = 0
    for i, ch in enumerate(chunks, 1):
        if i < args.start:
            continue
        globs = " ".join(f"-f '{t}'" for t in ch)
        label = f"{ch[0].rsplit('/', 1)[0]} +{len(ch)}"
        cmd = (f"cd {args.prefix}\n"
               f"ICU_DATA={args.prefix}/share/icu\n"
               f"bin/test-web --test-path {args.tests} {globs} -j 1 "
               f"-t {args.timeout} >[2=1]\n"
               f"for(n in {HELPERS})"+" { kill $n | rc }\n")
        # Budget for the worst case — every test burning its full timeout — not
        # the ~2s/test the healthy chunks average. A chunk of WebGL canvas tests
        # really does sit at the timeout for each one, and cutting the socket
        # early does NOT stop the run: test-web keeps going on the box and its
        # five helpers are orphaned, which is how the machine gets to an
        # unforkable state.
        out = run(args.host, args.port, cmd, wait=max(600, len(ch) * (args.timeout + 5)))
        m = re.search(r"Pass: (\d+), Fail: (\d+), Skipped: (\d+), "
                      r"Timeout: (\d+), Crashed: (\d+)", out)
        if not m:
            # The in-band kill never ran, so sweep explicitly before moving on.
            print(f"[{i}/{len(chunks)}] {label:44s} NO RESULT ({len(ch)} tests)", flush=True)
            missing += len(ch)
            run(args.host, args.port,
                f"for(n in {HELPERS})" + " { kill $n | rc }\necho SWEPT\n", wait=120, tries=3)
            continue
        p, f, s, t, c = (int(x) for x in m.groups())
        for k, v in zip(tot, (p, f, s, t, c)):
            tot[k] += v
        # A -f that matches nothing is dropped silently, which would quietly
        # shrink the denominator and inflate the pass rate. Account for it.
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
    print(f"  {'Missing':8s} {missing}   (enumerated but never reported on)")
    if ran:
        print(f"  pass rate (excl. skipped): {100.0 * tot['Pass'] / ran:.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
