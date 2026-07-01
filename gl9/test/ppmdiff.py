#!/usr/bin/env python3
# ppmdiff.py GOLD TARGET [--tol T] [--pass P] — compare two ASCII P3 PPMs with a
# per-channel tolerance. Prints pass% (fraction of channels within |Δ|<=tol) and
# max delta; exits 0 iff pass% >= P. Tolerance absorbs openlibm-vs-host-libm
# rounding — the goldens are the SAME Mesa softpipe, so differences are tiny.
import sys


def read_p3(path):
    t = open(path).read().split()
    if t[0] != "P3":
        raise ValueError(f"{path}: not P3")
    w, h = int(t[1]), int(t[2])
    vals = list(map(int, t[4:4 + w * h * 3]))
    if len(vals) != w * h * 3:
        raise ValueError(f"{path}: short pixel data ({len(vals)} != {w*h*3})")
    return w, h, vals


def main():
    a = sys.argv
    gold, tgt = a[1], a[2]
    tol = int(a[a.index("--tol") + 1]) if "--tol" in a else 2
    passpct = float(a[a.index("--pass") + 1]) if "--pass" in a else 99.5
    gw, gh, gv = read_p3(gold)
    tw, th, tv = read_p3(tgt)
    if (gw, gh) != (tw, th):
        print(f"SIZE MISMATCH gold={gw}x{gh} target={tw}x{th}")
        sys.exit(2)
    within = maxd = diffpx = 0
    for x, y in zip(gv, tv):
        d = abs(x - y)
        if d > maxd:
            maxd = d
        if d <= tol:
            within += 1
        else:
            diffpx += 1
    n = len(gv)
    pct = 100.0 * within / n
    ok = pct >= passpct
    print(f"{'PASS' if ok else 'FAIL'} {pct:.2f}% within|Δ|<={tol}  maxΔ={maxd}  "
          f"off={diffpx}/{n}  ({gw}x{gh})")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
