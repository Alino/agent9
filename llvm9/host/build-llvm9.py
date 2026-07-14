#!/usr/bin/env python3
# build-llvm9.py — cross-compile LLVM's JIT/codegen libs to 9front a.out objects
# with cc9's clang, from build9/compile_commands.json. Best-effort: compile every
# configured TU, archive the successes into libllvm9.a, and REPORT the failures
# grouped by error signature. We don't need every TU — the final LLJIT link pulls
# only what it references — so triage is driven by link-time undefineds, not by
# fixing every compile error here. (gl9 harvest pattern: remap /work->host, scrub
# glibc/libstdc++ -D + host -f flags, add cc9 target + freestanding libc++/libc.)
#
#   python3 build-llvm9.py [build|report] [-jN] [--filter SUBSTR]
import json, os, shlex, subprocess, sys, collections
from concurrent.futures import ThreadPoolExecutor

HOME = os.path.expanduser("~")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LLVMSRC = os.environ.get("CC9_LLVMSRC", f"{HOME}/Projects/llvm-project")
BUILD = f"{LLVMSRC}/build9"
CC = f"{BUILD}/compile_commands.json"
CLANGXX = os.environ.get("CC9_LLVM", "/opt/homebrew/opt/llvm/bin") + "/clang++"
AR = os.environ.get("CC9_LLVM", "/opt/homebrew/opt/llvm/bin") + "/llvm-ar"
LIBCXX = os.environ.get("CC9_LIBCXX", "/tmp/libcxx-thr/include/c++/v1")
INC = f"{REPO}/cc9/runtime/include"
CONTAINER = "/work/llvm-project"
OUT = f"{REPO}/llvm9/_out"
OBJ = f"{OUT}/obj"
ARCHIVE = f"{OUT}/libllvm9.a"

DROP_D = {"_GNU_SOURCE", "_GLIBCXX_USE_CXX11_ABI", "_FORTIFY_SOURCE"}
CC9_FLAGS = ["--target=x86_64-unknown-none", "-nostdlib", "-DNDEBUG",
             "-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE", "-D_LIBCPP_HAS_CLOCK_GETTIME",
             "-femulated-tls", "-funwind-tables", "-fno-pic",
             "-fno-exceptions", "-fno-rtti", "-fvisibility-inlines-hidden",
             "-nostdinc++", "-isystem", LIBCXX, "-isystem", INC]

def remap(p): return p.replace(CONTAINER, LLVMSRC)

def objname(src):
    return src.replace(LLVMSRC + "/", "").replace("/", "__") + ".o"

def cc9_cmd(entry, obj):
    toks = shlex.split(entry.get("command") or " ".join(entry["arguments"]))[1:]
    keep, std = [], "-std=c++17"
    i = 0
    while i < len(toks):
        t = toks[i]
        if t.startswith("-I"): keep.append("-I" + remap(t[2:]))
        elif t == "-isystem": keep += ["-isystem", remap(toks[i+1])]; i += 1
        elif t.startswith("-D"):
            if t[2:].split("=",1)[0] not in DROP_D: keep.append(t)
        elif t.startswith("-std="): std = t
        i += 1
    src = remap(entry["file"])
    return [CLANGXX, *CC9_FLAGS, std, *keep, "-O1", "-c", src, "-o", obj], src

def load(filt=None):
    e = [x for x in json.load(open(CC)) if x["file"].endswith((".cpp", ".cc"))]
    # exclude tool/unittest TUs that slip in; keep lib/*
    e = [x for x in e if "/lib/" in x["file"]]
    if filt: e = [x for x in e if filt in x["file"]]
    return e

def compile_one(entry):
    obj = os.path.join(OBJ, objname(remap(entry["file"])))
    src = remap(entry["file"])
    if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src):
        return (True, src, obj, "")            # incremental hit
    cmd, src = cc9_cmd(entry, obj)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode == 0:
        return (True, src, obj, "")
    err = next((l for l in r.stderr.splitlines() if ": error:" in l),
               (r.stderr.splitlines()[-1] if r.stderr else "?"))
    return (False, src, None, err)

def sig(err):
    # collapse an error line to a coarse signature for grouping
    e = err.split(": error:", 1)[-1].strip()
    for cut in ["'", '"']:
        if cut in e: return e.split(cut)[0][:60] + "…"
    return e[:60]

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "build"
    jobs = 8
    filt = None
    for i, a in enumerate(sys.argv):
        if a.startswith("-j"): jobs = int(a[2:])
        if a == "--filter": filt = sys.argv[i+1]
    os.makedirs(OBJ, exist_ok=True)
    entries = load(filt)
    print(f"{len(entries)} TUs (-j{jobs}){' filter='+filt if filt else ''}")
    ok, fails = [], []
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for n, res in enumerate(ex.map(compile_one, entries), 1):
            good, src, obj, err = res
            if good: ok.append(obj)
            else: fails.append((src, err))
            if n % 100 == 0:
                print(f"  {n}/{len(entries)}  ok={len(ok)} fail={len(fails)}")
    print(f"\n=== {len(ok)}/{len(entries)} compiled; {len(fails)} failed ===")
    groups = collections.Counter(sig(e) for _, e in fails)
    for s, c in groups.most_common(25):
        print(f"  {c:4d}  {s}")
    # archive successes (pull-on-demand at link, extras harmless)
    if mode == "build" and ok:
        if os.path.exists(ARCHIVE): os.remove(ARCHIVE)
        # llvm-ar has an arg limit; batch
        subprocess.run([AR, "rcs", ARCHIVE] + ok[:1], check=True)
        for i in range(1, len(ok), 500):
            subprocess.run([AR, "rs", ARCHIVE] + ok[i:i+500], check=True)
        print(f"\narchived {len(ok)} objs -> {ARCHIVE} ({os.path.getsize(ARCHIVE)//1024//1024} MB)")
    # dump full fail list for triage
    with open(f"{OUT}/fails.txt", "w") as f:
        for src, err in fails:
            f.write(f"{src}\n    {err}\n")
    print(f"fail detail -> {OUT}/fails.txt")

if __name__ == "__main__":
    main()
