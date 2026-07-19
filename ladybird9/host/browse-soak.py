#!/usr/bin/env python3
"""browse-soak.py — drive the real browser over a list of sites on the 9front box,
capture every diagnostic, and classify what went wrong.

  browse-soak.py [--rounds N] [--sites FILE] [--out DIR]

Each site is one headless run with its own stderr log. Between sites every helper
is killed by name: killing only the driver orphans the five helpers it spawned,
and those pile up until the box can no longer fork (see the wedged-listener
notes). Screenshot size is recorded as a crude "did it actually render" signal —
a few hundred bytes is a blank page, hundreds of KB is a real one.

Output per round: one line per site, plus an aggregated table of error signatures
so a recurring fault is obvious rather than buried in thousands of log lines.
"""
import argparse
import importlib.util
import os
import re
import sys

_spec = importlib.util.spec_from_file_location(
    "rt9", os.path.join(os.path.dirname(os.path.abspath(__file__)), "run-tests9.py"))
_rt9 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_rt9)

HELPERS = _rt9.HELPERS
PREFIX = "/usr/glenda/ladybird9"

SITES = [
    "https://example.com",
    "https://www.dsl.sk/",
    "https://www.bazos.sk/",
    "https://en.wikipedia.org/wiki/Plan_9_from_Bell_Labs",
    "https://news.ycombinator.com/",
    "https://9front.org/",
    "http://info.cern.ch/hypertext/WWW/TheProject.html",
    "https://www.sme.sk/",
    "https://lite.duckduckgo.com/lite/?q=plan9",
    "https://danluu.com/",
]

# Signatures worth counting separately. Order matters: first match wins.
SIGNATURES = [
    ("shm-overlap-unhealed", r"segments overlap"),
    ("ipc-magic-mismatch",   r"magic number mismatch"),
    ("ipc-parse-failed",     r"Failed to parse IPC message"),
    ("peer-disconnected",    r"Disconnecting misbehaving peer"),
    ("closed-pipe-fault",    r"write on closed pipe"),
    ("suicide",              r"suicide:"),
    ("verify-failed",        r"VERIFICATION FAILED|VERIFY.*failed"),
    ("ssl-failure",          r"SSL verification failed|SSL handshake failed"),
    ("resource-load-fail",   r"ResourceLoader: Failed load"),
    ("timed-out-phase",      r"timed out in phase"),
    ("assertion",            r"ASSERTION FAILED"),
    ("shm-import-failed",    r"shm import FAILED|/srv open FAILED"),
    ("out-of-memory",        r"out of memory|Cannot allocate"),
]


def run(cmd, wait=240, tries=2):
    return _rt9.run("192.168.88.159", 17010, cmd, wait=wait, tries=tries)


def sweep():
    run("for(n in %s){ kill $n | rc }\nsleep 1\necho SWEPT\n" % HELPERS, wait=90, tries=3)


def classify(text):
    hits = []
    for name, pat in SIGNATURES:
        n = len(re.findall(pat, text))
        if n:
            hits.append((name, n))
    return hits


def visit(url, idx, outdir, budget=45, shm_trace=False):
    """One headless page load. Returns (shot_bytes, log_text)."""
    shot = "/tmp/soak%d.png" % idx
    log = "/tmp/soak%d.log" % idx
    # The page load is bounded ON THE BOX, not by our socket timeout. Dropping the
    # connection does NOT stop a running browser: it and its five helpers keep
    # going, and enough of those orphans make the machine unable to fork at all
    # (ICMP up, port open, nothing served). So run it in the background here,
    # give it a fixed budget, then sweep unconditionally — the cleanup happens
    # even if we lose the connection or get killed mid-run.
    cmd = (
        "cd %s\n"
        "ICU_DATA=%s/share/icu\n"
        "%s"
        "rm -f %s %s\n"
        "@{ bin/ladybird --headless --certificate %s/share/ca.pem "
        "--screenshot-path %s '%s' >%s >[2=1] } &\n"
        "sleep %d\n"
        "for(n in %s){ kill $n | rc }\n"
        "sleep 1\n"
        "echo ---SOAK-DONE---\n"
        "ls -l %s >[2=1]\n"
        "echo ---SOAK-LOG---\n"
        "cat %s\n"
    ) % (PREFIX, PREFIX, "CC9_SHM_TRACE=1\n" if shm_trace else "", shot, log, PREFIX, shot, url, log, budget, HELPERS, shot, log)
    out = run(cmd, wait=budget + 120)
    m = re.search(r"---SOAK-DONE---(.*?)---SOAK-LOG---(.*)", out, re.S)
    if not m:
        return -1, out
    size = -1
    sm = re.search(r"glenda\s+(\d+)\s", m.group(1))
    if sm:
        size = int(sm.group(1))
    return size, m.group(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--budget", type=int, default=45,
                    help="seconds allowed per page load, enforced on the box")
    ap.add_argument("--shm-trace", action="store_true",
                    help="enable CC9_SHM_TRACE (segment create/map/attach lines)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--sites", default=None, help="file with one URL per line")
    args = ap.parse_args()

    sites = SITES
    if args.sites:
        sites = [l.strip() for l in open(args.sites) if l.strip() and not l.startswith("#")]

    outdir = args.out or os.environ.get("CLAUDE_JOB_DIR", "/tmp") + "/soak"
    os.makedirs(outdir, exist_ok=True)

    totals = {}
    for rnd in range(1, args.rounds + 1):
        print("\n===== round %d/%d =====" % (rnd, args.rounds), flush=True)
        for i, url in enumerate(sites):
            sweep()
            size, log = visit(url, i, outdir, args.budget, args.shm_trace)
            hits = classify(log)
            for name, n in hits:
                totals[name] = totals.get(name, 0) + n
            with open(os.path.join(outdir, "r%d_%02d.log" % (rnd, i)), "w") as f:
                f.write("URL: %s\nSHOT: %s\n\n%s" % (url, size, log))
            flag = "  " + " ".join("%s=%d" % h for h in hits) if hits else "  clean"
            print("[r%d %2d] %-58s shot=%-8s%s" % (rnd, i, url[:58], size, flag), flush=True)

    print("\n===== error signature totals =====")
    if not totals:
        print("  (none)")
    for name, n in sorted(totals.items(), key=lambda kv: -kv[1]):
        print("  %-24s %d" % (name, n))
    print("\nlogs in %s" % outdir)
    sweep()
    return 0


if __name__ == "__main__":
    sys.exit(main())
