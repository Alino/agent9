#!/usr/bin/env python3
"""
run-suite.py — run an already-delivered libtest binary (/tmp/cc9bin) on the box
in module CHUNKS, each to a file with a per-chunk timeout, so one hanging test
can't sink the whole suite. Aggregates pass/fail across chunks and names any
chunk that stalls. Assumes the binary is already at /tmp/cc9bin.

    python3 run-suite.py "mod1 mod2 ..."   # space-separated filter prefixes
"""
import re
import sys

sys.path.insert(0, "cc9/host")
from deliver import send

HOST, PORT = "127.0.0.1", 1717


def run_chunk(filt, timeout):
    send(HOST, PORT, "rm -f /tmp/tc\n", 10)
    # no --test-threads (that path reaches the run loop); terse; to a file.
    send(HOST, PORT, f"/tmp/cc9bin -q {filt} >/tmp/tc >[2=1]; echo E-$status\n", timeout)
    out = send(HOST, PORT, "cat /tmp/tc\n", 30)
    m = re.search(r"(\d+) passed; (\d+) failed", out)
    if m:
        return int(m.group(1)), int(m.group(2)), out
    return None, None, out  # stalled or no summary


def main():
    mods = sys.argv[1].split() if len(sys.argv) > 1 else []
    timeout = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    tot_p = tot_f = 0
    stalled = []
    for mod in mods:
        p, f, out = run_chunk(mod, timeout)
        if p is None:
            # count how far it got (terse prints '.'/'F' per test)
            dots = out.count(".") + out.count("F")
            stalled.append((mod, dots))
            print(f"{mod:24} STALLED (~{dots} tests ran before stall)")
        else:
            tot_p += p
            tot_f += f
            print(f"{mod:24} {p:5} passed  {f:3} failed")
    print(f"\nTOTAL: {tot_p} passed, {tot_f} failed; stalled chunks: {stalled}")


if __name__ == "__main__":
    main()
