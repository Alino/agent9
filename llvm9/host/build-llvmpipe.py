#!/usr/bin/env python3
# build-llvmpipe.py — cross-compile the llvm-enabled Mesa (gallium swrast =
# softpipe+llvmpipe + draw-llvm + gallivm) to 9front objects with cc9, from
# gl9/build-gen-llvm/compile_commands.json. Fuses the gl9 treatment (Linux -D
# scrub + gl9_pre.h force-include + shim) with the LLVM-header remap (/work-llvm
# -> our llvm-project), and archives into gl9/_out/libgl9mesa-llvm.a. That archive
# replaces libgl9mesa.a when linking an llvmpipe binary (+ libllvm9.a).
#
# Best-effort like build-llvm9.py: compile all, archive successes, report the
# grouped failures — the link's undefined symbols say which failures matter.
#   python3 build-llvmpipe.py [build] [-jN] [--filter SUBSTR]
import json, os, shlex, subprocess, sys, collections
from concurrent.futures import ThreadPoolExecutor

HOME = os.path.expanduser("~")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GL9 = f"{REPO}/gl9"
LLVMSRC = os.environ.get("CC9_LLVMSRC", f"{HOME}/Projects/llvm-project")
# GL9_BUILDGEN/GL9_ARCHIVE let this same pipeline serve both Mesa configures:
# build-gen-llvm (softpipe+llvmpipe) and build-gen-iris (iris HW + swrast).
BUILDGEN = os.environ.get("GL9_BUILDGEN", "build-gen-llvm")
CC = f"{GL9}/{BUILDGEN}/compile_commands.json"
BIN = os.environ.get("CC9_LLVM", "/opt/homebrew/opt/llvm/bin")
LIBCXX = os.environ.get("CC9_LIBCXX", "/tmp/libcxx-thr/include/c++/v1")
INC = f"{REPO}/cc9/runtime/include"
SHIM_INC = f"{GL9}/port/plan9/shim/include"
PRE = f"{GL9}/port/plan9/shim/gl9_pre.h"
OUT = f"{GL9}/_out"
OBJ = f"{OUT}/obj-" + BUILDGEN.replace("build-gen-","")
ARCHIVE = f"{OUT}/" + os.environ.get("GL9_ARCHIVE", "libgl9mesa-llvm.a")

# container-path -> host-path (longest prefix first)
REMAP = [("/work-llvm", LLVMSRC), ("/work", REPO)]
def remap(p):
    for a, b in REMAP:
        if p.startswith(a): return b + p[len(a):]
    return p
def resolve(p, base):
    # remap container-absolute; resolve relative against the (remapped) build dir
    p = remap(p)
    if not os.path.isabs(p): p = os.path.normpath(os.path.join(base, p))
    return p
def basedir(entry): return remap(entry["directory"])

# gl9's Linux/glibc -D scrub (harvest.py SCRUB) — force Mesa's portable paths.
SCRUB = {
  "HAVE_ENDIAN_H","HAVE_DLFCN_H","HAVE_SYS_SHM_H","HAVE_SYS_INOTIFY_H",
  "HAVE_LINUX_FUTEX_H","HAVE_CET_H","MAJOR_IN_SYSMACROS","HAVE_MEMFD_CREATE",
  "HAS_SCHED_H","HAS_SCHED_GETAFFINITY","HAVE_PTHREAD_SETAFFINITY","ALLOW_KCMP",
  "HAVE_FLOCK","HAVE_POSIX_FALLOCATE","HAVE_MKOSTEMP","HAVE_GETRANDOM",
  "HAVE_RANDOM_R","HAVE_SECURE_GETENV","HAVE_GNU_QSORT_R","HAVE_STRTOD_L",
  "HAVE_PROGRAM_INVOCATION_NAME","HAVE_DLADDR","HAVE_DL_ITERATE_PHDR",
  "HAVE_REALLOCARRAY","HAVE_POSIX_MEMALIGN","HAVE_DIRENT_D_TYPE","HAVE_ISSIGNALING",
  "HAVE_ZLIB","HAVE_COMPRESSION","USE_DRICONF","HAVE_OPENMP","USE_X86_64_ASM",
}
# GCC-only -f flags clang rejects (meson's native compiler is g++). Prefix match.
DROP_F = ("-fdiagnostics-color","-pipe","-fno-common","-flifetime-dse",
          "-fno-semantic-interposition","-fconserve-stack")
# same non-softpipe skips as build-gl9.py (tools/tests/loaders/spirv tool)
EXCLUDE = ["/src/gtest/","/src/loader/","/gallium/auxiliary/pipe-loader/",
  "/src/virtio/","/src/util/mesa_cache_db.c","/src/util/xmlconfig.c",
  "/glsl/glcpp/glcpp.c","/glsl/main.cpp","/glsl/standalone.cpp",
  "/glsl/test_optpass.cpp","/glsl/shader_cache.cpp","/compiler/spirv/spirv2nir.c",
  "/mesa/program/dummy_errors.c",  # stub _mesa_*; real errors.c wins
  "standalone"]  # glsl standalone scaffolding stubs (dup _mesa_*)

# Key the object on meson's unique output path, NOT the source: some sources
# (mapi entry.c) are compiled several times with different -D (glapi/es2api/...),
# and keying on the source would collide them -> lost entrypoints.
def objname(entry): return entry["output"].replace("/","__")+".o"

SHIM_SRC = f"{GL9}/port/plan9/shim/gl9_os_extra.c"
def compile_shim():
    obj = os.path.join(OBJ, "shim__gl9_os_extra.c.o")
    cmd = [BIN+"/clang", "--target=x86_64-unknown-none", "-femulated-tls",
           "-funwind-tables", "-fno-pic", "-O2", "-w", "-include", PRE,
           "-isystem", SHIM_INC, "-fno-builtin", "-isystem", INC, "-std=c11",
           "-c", SHIM_SRC, "-o", obj]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode: print("SHIM FAIL:\n"+r.stderr); sys.exit(1)
    return obj

def parse(entry, base):
    toks = shlex.split(entry.get("command") or " ".join(entry["arguments"]))[1:]
    defs, incs, forced, std, arch, fflags = [], [], [], None, [], []
    i = 0
    while i < len(toks):
        t = toks[i]
        if t.startswith("-D"):
            if t[2:].split("=",1)[0] not in SCRUB:
                for a,b in REMAP: t = t.replace(a, b)   # remap paths embedded in -D values
                defs.append(t)
        elif t == "-I": incs += ["-I"+resolve(toks[i+1], base)]; i += 1
        elif t.startswith("-I"): incs.append("-I"+resolve(t[2:], base))
        elif t == "-isystem": incs += ["-isystem", resolve(toks[i+1], base)]; i += 1
        elif t.startswith("-isystem"): incs += ["-isystem", resolve(t[len("-isystem"):], base)]
        elif t == "-include": forced += ["-include", resolve(toks[i+1], base)]; i += 1
        elif t.startswith("-std="): std = t
        elif t.startswith("-m"): arch.append(t)
        elif t.startswith("-f") and not t.startswith(DROP_F): fflags.append(t)
        i += 1
    return defs, incs, forced, std, arch, fflags

def cc9_cmd(entry, obj):
    base = basedir(entry)
    src = resolve(entry["file"], base)
    is_cpp = src.endswith((".cpp",".cc",".cxx"))
    defs, incs, forced, std, arch, fflags = parse(entry, base)
    if not std: std = "-std=c++17" if is_cpp else "-std=c11"
    cc = BIN+"/clang++" if is_cpp else BIN+"/clang"
    base = [cc, "--target=x86_64-unknown-none", "-femulated-tls", "-funwind-tables",
            "-fno-pic", "-O2", "-w", "-include", PRE, "-isystem", SHIM_INC]
    if is_cpp:
        base += ["-nostdinc++", "-isystem", LIBCXX,
                 "-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE", "-D_LIBCPP_HAS_CLOCK_GETTIME"]
    base += ["-isystem", INC]
    return base + [std] + defs + incs + arch + fflags + forced + ["-c", src, "-o", obj], src

# Some sources are compiled once PER KERNEL BACKEND (iris_batch.c ->
# i915_iris_batch.c.o, xe_iris_batch.c.o, iris_batch.c.o). "xe" is the modern Xe
# kernel driver (Tiger Lake+); Broadwell is i915, so the xe_* variants are dead
# weight that only fail on Xe uapi headers we will never have. Exclude by OUTPUT,
# since the SOURCE is shared with the i915 variant we need.
EXCLUDE_OUT = ("xe_",)

def excluded_out(entry):
    out = (entry.get("output") or "").split("/")[-1]
    return out.startswith(EXCLUDE_OUT)

def excluded(src):
    if src.endswith(".S"): return True
    return any(x in src for x in EXCLUDE)

def load(filt=None):
    e = [x for x in json.load(open(CC)) if x["file"].endswith((".c",".cc",".cpp",".cxx"))]
    e = [x for x in e if not excluded(resolve(x["file"], basedir(x)))]
    e = [x for x in e if not excluded_out(x)]
    if filt: e = [x for x in e if filt in x["file"]]
    return e

def compile_one(entry):
    src = resolve(entry["file"], basedir(entry))
    obj = os.path.join(OBJ, objname(entry))
    if os.path.exists(obj) and os.path.getmtime(obj) >= max(os.path.getmtime(src), os.path.getmtime(PRE)):
        return (True, src, obj, "")
    cmd, src = cc9_cmd(entry, obj)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode == 0: return (True, src, obj, "")
    err = next((l for l in r.stderr.splitlines() if ": error:" in l),
               (r.stderr.splitlines()[-1] if r.stderr else "?"))
    return (False, src, None, err)

def sig(err):
    e = err.split(": error:",1)[-1].strip()
    for c in ["'",'"']:
        if c in e: return e.split(c)[0][:60]+"…"
    return e[:60]

def main():
    jobs = 8; filt = None
    for i,a in enumerate(sys.argv):
        if a.startswith("-j"): jobs = int(a[2:])
        if a == "--filter": filt = sys.argv[i+1]
    os.makedirs(OBJ, exist_ok=True)
    entries = load(filt)
    print(f"{len(entries)} TUs (-j{jobs}){' filter='+filt if filt else ''}")
    ok, fails = [], []
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for n,res in enumerate(ex.map(compile_one, entries),1):
            good, src, obj, err = res
            (ok if good else fails).append(obj if good else (src,err))
            if n % 100 == 0: print(f"  {n}/{len(entries)} ok={len(ok)} fail={len(fails)}")
    print(f"\n=== {len(ok)}/{len(entries)} compiled; {len(fails)} failed ===")
    for s,c in collections.Counter(sig(e) for _,e in fails).most_common(25):
        print(f"  {c:4d}  {s}")
    if ok and "--filter" not in sys.argv:
        ok.append(compile_shim())   # gl9 shim: pthread_barrier, stpcpy, open_memstream, ...
        if os.path.exists(ARCHIVE): os.remove(ARCHIVE)
        subprocess.run([BIN+"/llvm-ar","rcs",ARCHIVE]+ok[:1], check=True)
        for i in range(1,len(ok),500):
            subprocess.run([BIN+"/llvm-ar","rs",ARCHIVE]+ok[i:i+500], check=True)
        print(f"\narchived {len(ok)} -> {ARCHIVE} ({os.path.getsize(ARCHIVE)//1024//1024} MB)")
    with open(f"{OUT}/fails-{BUILDGEN}.txt","w") as f:
        for src,err in fails: f.write(f"{src}\n    {err}\n")
    print(f"fail detail -> {OUT}/fails-{BUILDGEN}.txt")

if __name__ == "__main__":
    main()
