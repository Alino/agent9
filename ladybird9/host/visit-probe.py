#!/usr/bin/env python3
"""visit-probe.py — measure ONE page load's cost on the box.

NOTE: section markers are --NAME--, never ==NAME==. In rc a word beginning with
`=` parses as an assignment, so `echo ==BEFORE==` is a syntax error that kills
the whole script and returns NOTHING -- which reads exactly like a wedged box.

  visit-probe.py [url] [--wait N]

Answers the question that repeated soak wedges could not: what does a single
browser run actually leave behind? Samples /dev/swap (memory) and the process
count at three points — before, after the browser has finished or timed out, and
after an explicit sweep — and reports the deltas plus any surviving process
names.

That distinguishes the two candidate causes, which need opposite fixes:
  - processes survive the sweep        -> cleanup is incomplete
  - processes die but memory does not  -> something is not being released
  - both return to baseline            -> the footprint simply does not fit,
                                          and runs must be spaced out instead
Unlike the soak this WAITS for the browser to exit on its own (headless
--screenshot-path does), so a forced kill mid-teardown cannot muddy the numbers.
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

PREFIX = "/usr/glenda/ladybird9"
KILL = "ladybird gl9win2 " + _rt9.HELPERS


def run(cmd, wait=300, tries=2):
    return _rt9.run("192.168.88.159", 17010, cmd, wait=wait, tries=tries)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url", nargs="?", default="https://www.dsl.sk/")
    ap.add_argument("--wait", type=int, default=40, help="poll iterations (3s each)")
    args = ap.parse_args()

    # rc has no convenient arithmetic; unroll the poll as a literal word list.
    ticks = " ".join(str(i) for i in range(1, args.wait + 1))

    cmd = f"""cd {PREFIX}
ICU_DATA={PREFIX}/share/icu
echo --BEFORE--
cat /dev/swap
ps | wc -l
rm -f /tmp/vp.png /tmp/vp.log
@{{ rfork s
   bin/ladybird --headless --certificate {PREFIX}/share/ca.pem --screenshot-path /tmp/vp.png '{args.url}' >/tmp/vp.log >[2=1]
}} &
done=no
for(i in {ticks}){{
	if(~ $done no){{
		sleep 3
		if(~ `{{ps | grep ladybird | wc -l}} 0) done=yes
	}}
}}
echo --EXITED-- $done
echo --AFTER-RUN--
cat /dev/swap
ps | wc -l
echo --SURVIVORS--
ps | grep ladybird | wc -l
ps | grep WebContent | wc -l
ps | grep Compositor | wc -l
ps | grep ImageDecoder | wc -l
for(n in {KILL}){{ kill $n | rc }}
sleep 4
echo --AFTER-SWEEP--
cat /dev/swap
ps | wc -l
echo --SHOT--
ls -l /tmp/vp.png >[2=1]
"""
    out = run(cmd, wait=args.wait * 3 + 180)
    if not out.strip():
        print("box did not answer — it is wedged", file=sys.stderr)
        return 1
    print(out)

    # Pull the "N/M user" style memory lines out of /dev/swap for a quick delta.
    def mem(section):
        m = re.search(r"--%s--(.*?)(?:--[A-Z]|\Z)" % section, out, re.S)
        if not m:
            return None
        u = re.search(r"(\d+)/(\d+)\s+user", m.group(1))
        return (int(u.group(1)), int(u.group(2))) if u else None

    b, a, s = mem("BEFORE"), mem("AFTER-RUN"), mem("AFTER-SWEEP")
    if b and a and s:
        print("\n== user memory (pages) ==")
        print("  before %d/%d" % b)
        print("  after  %d/%d   (delta %+d)" % (a[0], a[1], a[0] - b[0]))
        print("  swept  %d/%d   (delta %+d from baseline)" % (s[0], s[1], s[0] - b[0]))
        print("\n  A near-zero swept delta means cleanup works and the box simply"
              "\n  cannot hold two of these at once; a large one is a real leak.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
