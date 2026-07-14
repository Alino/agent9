#!/usr/bin/env python3
# cc9-try.py — the go/no-go experiment: can cc9's clang digest LLVM's heavy C++?
# Takes LLVM's build9/compile_commands.json, and for a sample of lib/Support +
# lib/MC TUs, recompiles them with cc9's target + freestanding libc++/libc (the
# gl9 harvest pattern) — compile-only (-fsyntax/-c to /dev/null). Reports how many
# parse+codegen clean vs. choke, with the first error per failure. If Support+MC
# come through, the full JIT-lib set is a grind not a wall.
#
#   python3 llvm9/host/cc9-try.py [N]
import json, os, shlex, subprocess, sys

HOME = os.path.expanduser("~")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LLVMSRC = os.environ.get("CC9_LLVMSRC", f"{HOME}/Projects/llvm-project")
CC = f"{LLVMSRC}/build9/compile_commands.json"
CLANGXX = "/opt/homebrew/opt/llvm/bin/clang++"
LIBCXX = "/tmp/libcxx-thr/include/c++/v1"
INC = f"{REPO}/cc9/runtime/include"
CONTAINER = "/work/llvm-project"

# -D to drop (glibc / libstdc++ ABI selectors that fight libc++/cc9). Keep the
# __STDC_*_MACROS — LLVM needs them for <cinttypes> limit/format macros.
DROP_D = {"_GNU_SOURCE", "_GLIBCXX_USE_CXX11_ABI", "_FORTIFY_SOURCE"}

def remap(p):
    return p.replace(CONTAINER, LLVMSRC)

def cc9_cmd(entry):
    toks = shlex.split(entry.get("command") or " ".join(entry["arguments"]))[1:]
    keep, std = [], "-std=c++17"
    i = 0
    while i < len(toks):
        t = toks[i]
        if t.startswith("-I"):
            keep += ["-I" + remap(t[2:])]
        elif t == "-isystem":
            keep += ["-isystem", remap(toks[i+1])]; i += 1
        elif t.startswith("-D"):
            if t[2:].split("=",1)[0] not in DROP_D: keep.append(t)
        elif t.startswith("-std="):
            std = t
        # drop everything else: -fPIC, -W*, -pedantic, -g, -O, host noise
        i += 1
    src = remap(entry["file"])
    return [CLANGXX, "--target=x86_64-unknown-none", "-nostdlib", "-DNDEBUG",
            "-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE", "-D_LIBCPP_HAS_CLOCK_GETTIME",
            "-femulated-tls", "-funwind-tables", "-fno-pic",
            "-fno-exceptions", "-fno-rtti", "-fvisibility-inlines-hidden",
            std, "-nostdinc++", "-isystem", LIBCXX, "-isystem", INC,
            *keep, "-O1", "-c", src, "-o", "/dev/null"], src

def pick(entries, n):
    # bias toward the hairy TUs, then even-spread the rest for coverage
    hairy = ["Path.cpp","JSON.cpp","YAMLParser.cpp","APFloat.cpp","APInt.cpp",
             "StringMap.cpp","raw_ostream.cpp","CommandLine.cpp","SmallVector.cpp",
             "MCAssembler.cpp","MCStreamer.cpp","MCExpr.cpp","MCObjectStreamer.cpp",
             "MCContext.cpp","ELFObjectWriter.cpp"]
    by = {os.path.basename(e["file"]): e for e in entries}
    out = [by[h] for h in hairy if h in by]
    rest = [e for e in entries if e not in out]
    step = max(1, len(rest)//max(1, n-len(out)))
    out += rest[::step]
    return out[:n]

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    entries = [e for e in json.load(open(CC))
               if e["file"].endswith(".cpp")
               and ("/lib/Support/" in e["file"] or "/lib/MC/" in e["file"])]
    sample = pick(entries, n)
    ok = fail = 0
    fails = []
    for e in sample:
        cmd, src = cc9_cmd(e)
        r = subprocess.run(cmd, capture_output=True, text=True)
        name = src.replace(LLVMSRC+"/llvm/", "")
        if r.returncode == 0:
            ok += 1; print(f"  ok   {name}")
        else:
            fail += 1
            err = next((l for l in r.stderr.splitlines() if ": error:" in l), r.stderr.splitlines()[-1] if r.stderr else "?")
            fails.append((name, err))
            print(f"  FAIL {name}")
    print(f"\n=== cc9 vs LLVM: {ok}/{ok+fail} TUs compiled clean ===")
    for name, err in fails:
        print(f"\n[{name}]\n  {err[:300]}")

if __name__ == "__main__":
    main()
