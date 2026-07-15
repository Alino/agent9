#!/usr/bin/env python3
# build-gl9.py — compile every harvested Mesa TU (+ the gl9 shim) with cc9's clang
# for x86_64-plan9, then archive into libgl9mesa.a. Mirrors cc9/host/cc9 over a
# whole file set instead of one source.
#
#   python3 build-gl9.py enumerate   # compile all, DON'T stop on error, print the
#                                     # aggregate gap list (the plan's Phase-0 spike)
#   python3 build-gl9.py build       # compile all + archive; fail on first error
#   python3 build-gl9.py link MAIN   # build the archive, then link MAIN.c(pp) +
#                                     # cc9 runtime -> a Plan 9 a.out (via elf2aout)
#
# Config (env): CC9_LLVM (clang bin dir), CC9_LIBCXX (freestanding libc++ headers),
# CC9 (cc9 checkout). The freestanding libc++ tree must be complete — see
# port/plan9/NOTES.md if <new>/<vector> are missing (ephemeral /tmp gotcha).
import concurrent.futures as cf, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
GL9 = os.path.dirname(HERE)
REPO = os.path.dirname(GL9)
CC9 = os.environ.get("CC9", os.path.join(REPO, "cc9"))
LLVM = os.environ.get("CC9_LLVM", "/opt/homebrew/opt/llvm/bin")
LIBCXX = os.environ.get("CC9_LIBCXX", "/tmp/libcxx-thr/include/c++/v1")
INC = os.path.join(CC9, "runtime", "include")
SHIM_INC = os.path.join(GL9, "port", "plan9", "shim", "include")
PRE = os.path.join(GL9, "port", "plan9", "shim", "gl9_pre.h")
SHIM_SRCS = [os.path.join(GL9, "port", "plan9", "shim", "gl9_os_extra.c")]
OUT = os.path.join(GL9, "_out")
OBJ = os.path.join(OUT, "obj")

# TUs meson configures but softpipe+OSMesa doesn't need (tools, tests, dynamic
# loaders, SPIR-V, other-GPU backends). Skipped so their unportable deps
# (getopt, dlopen, drm, expat) don't matter. If the link later needs a symbol
# from one of these, revisit — but softpipe is static and GLES2/GL3.3 has no
# ARB_gl_spirv. Matched as path substrings.
EXCLUDE = [
    "/src/gtest/", "/src/loader/", "/gallium/auxiliary/pipe-loader/",
    "/src/virtio/", "/src/util/mesa_cache_db.c", "/src/util/xmlconfig.c",
    "/glsl/glcpp/glcpp.c", "/glsl/main.cpp", "/glsl/standalone.cpp",
    "/glsl/test_optpass.cpp", "/glsl/shader_cache.cpp",
    "/compiler/spirv/spirv2nir.c",   # standalone tool (has main); keep the lib
]

# cc9 target flags, mirroring cc9/host/cc9. -include gl9_pre.h fixes the compile-time
# gaps (endian, static_assert, pthread_barrier, alloca, errno, M_*, libc protos).
# No -nostdlib for -c. SHIM_INC provides <strings.h> etc.
TARGET = ["--target=x86_64-unknown-none", "-femulated-tls", "-funwind-tables",
          "-fno-pic", "-O2", "-w", "-include", PRE, "-isystem", SHIM_INC]
# libc++ headers MUST precede cc9's C -isystem INC: <cstdlib> pulls <stdlib.h>
# and must find libc++'s wrapper first, not cc9's C stdlib.h.
CXX_EXTRA = ["-nostdinc++", "-isystem", LIBCXX,
             "-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE", "-D_LIBCPP_HAS_CLOCK_GETTIME"]


def load():
    return json.load(open(os.path.join(HERE, "harvest.json")))["entries"]


def cmd_for(e):
    cc = os.path.join(LLVM, "clang++" if e["lang"] == "c++" else "clang")
    std = e["std"] or ("-std=c++17" if e["lang"] == "c++" else "-std=c11")
    obj = os.path.join(OBJ, uniq_obj(e))
    base = [cc] + TARGET
    if e["lang"] == "c++":
        base += CXX_EXTRA          # libc++ -isystem BEFORE cc9's C -isystem INC
    base += ["-isystem", INC]
    return base + [std] + e["defines"] + e["includes"] + e["arch"] + e["forced"] + \
        ["-c", e["src"], "-o", obj], obj


def excluded(e):
    # Skip hand-written asm: the software renderer needs no SIMD/asm-dispatch. blake3
    # falls back to portable C (BLAKE3_NO_* in gl9_pre.h); glapi uses C dispatch.
    if e["src"].endswith(".S"):
        return True
    # The glsl "standalone" tool library recompiles shared sources + scaffolding
    # stubs of _mesa_* that duplicate the real libmesa — never linked by OSMesa.
    if "standalone" in e.get("obj", ""):
        return True
    return any(x in e["src"] for x in EXCLUDE)


def uniq_obj(e):
    # meson's output path is unique across the tree; flatten to a filename.
    return e["obj"].replace("/", "__") + ".o"


def uptodate(obj, src):
    # incremental: reuse an .o newer than its source AND the force-included shim.
    if not os.path.exists(obj):
        return False
    ot = os.path.getmtime(obj)
    return ot > os.path.getmtime(src) and ot > os.path.getmtime(PRE)


def compile_one(e):
    c, obj = cmd_for(e)
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    if uptodate(obj, e["src"]):
        return e, obj, 0, ""
    r = subprocess.run(c, capture_output=True, text=True)
    return e, obj, r.returncode, r.stderr


def compile_shim():
    os.makedirs(OBJ, exist_ok=True)
    objs = []
    for s in SHIM_SRCS:
        obj = os.path.join(OBJ, "shim__" + os.path.basename(s) + ".o")
        # -fno-builtin so clang doesn't intercept our sprintf/stpcpy/str* defs.
        c = [os.path.join(LLVM, "clang")] + TARGET + ["-fno-builtin", "-isystem",
             INC, "-std=c11", "-c", s, "-o", obj]
        r = subprocess.run(c, capture_output=True, text=True)
        if r.returncode:
            print("SHIM FAIL", s); print(r.stderr); sys.exit(1)
        objs.append(obj)
    return objs


def compile_all(stop_on_error):
    entries = [e for e in load() if not excluded(e)]
    os.makedirs(OBJ, exist_ok=True)
    fails, objs = [], []
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        for e, obj, rc, err in ex.map(compile_one, entries):
            if rc == 0:
                objs.append(obj)
            else:
                fails.append((e["src"], err))
                if stop_on_error:
                    print("FAIL", e["src"]); print(err); sys.exit(1)
    return entries, objs, fails


def enumerate_gaps():
    entries, objs, fails = compile_all(stop_on_error=False)
    print(f"\ncompiled {len(objs)}/{len(entries)} TUs; {len(fails)} failed")
    # aggregate distinct error signatures (missing headers/identifiers) across fails
    import re, collections
    sig = collections.Counter()
    for src, err in fails:
        for l in err.splitlines():
            m = re.search(r"fatal error: '([^']+)' file not found", l)
            if m: sig["missing header: " + m.group(1)] += 1
            m = re.search(r"error: use of undeclared identifier '([^']+)'", l)
            if m: sig["undeclared: " + m.group(1)] += 1
            m = re.search(r"error: unknown type name '([^']+)'", l)
            if m: sig["unknown type: " + m.group(1)] += 1
            m = re.search(r"error: call to undeclared function '([^']+)'", l)
            if m: sig["undeclared func: " + m.group(1)] += 1
    print("\n== aggregate gap signatures (count) ==")
    for s, n in sig.most_common(60):
        print(f"  {n:4d}  {s}")
    print("\n== sample failing files (first 15) ==")
    for src, _ in fails[:15]:
        print("  ", src.split("vendor/mesa/")[-1])


def archive(objs):
    ar = os.path.join(LLVM, "llvm-ar")
    lib = os.path.join(OUT, "libgl9mesa.a")
    if os.path.exists(lib):
        os.remove(lib)
    # write objs to a response file (834 paths blow the arg limit)
    rsp = os.path.join(OUT, "ar.rsp")
    open(rsp, "w").write("\n".join(objs))
    subprocess.run([ar, "rcs", lib, "@" + rsp], check=True)
    print("archived", lib, os.path.getsize(lib), "bytes")
    return lib


def link(main_src, extra_srcs=()):
    # GL9_LLVM=1 links the llvm-enabled Mesa (softpipe AND llvmpipe) + LLVM
    # itself, so the driver can be chosen at runtime via GALLIUM_DRIVER. It costs
    # ~57 MB of statically-linked LLVM per binary (9front has no dynamic linking),
    # so it is opt-in. See llvm9/ for how those archives are built.
    if os.environ.get("GL9_LLVM"):
        lib = os.path.join(OUT, "libgl9mesa-llvm.a")
        libllvm = [os.path.join(REPO, "llvm9", "_out", "libllvm9.a")]
        if not os.path.exists(libllvm[0]):
            print("no libllvm9.a — run llvm9/host/build-llvm9.py first"); sys.exit(1)
    else:
        lib = os.path.join(OUT, "libgl9mesa.a")
        libllvm = []
    if not os.path.exists(lib):
        print("no " + os.path.basename(lib) + " — build it first"); sys.exit(1)
    lld = os.environ.get("CC9_LLD", "/opt/homebrew/bin/ld.lld")
    cc9lib = os.path.join(CC9, "lib", "libcc9cxx.a")
    cc9m = os.path.join(CC9, "lib", "libcc9m.a")
    lds = os.path.join(CC9, "test", "plan9.ld")
    mesa_inc = os.path.join(GL9, "vendor", "mesa", "include")
    base = os.path.splitext(os.path.basename(main_src))[0]
    elf = os.path.join(OUT, base + ".elf")
    aout = os.path.join(OUT, base + ".aout")
    cc = os.path.join(LLVM, "clang")
    # compile the app + any extra sources (e.g. gl9egl.c) against Mesa's public
    # headers and every source dir (so each finds the others' local headers).
    # source dirs FIRST so a source-local header (e.g. port/plan9/egl/EGL/
    # eglplatform.h) shadows Mesa's — Mesa's eglplatform.h #errors on 9front.
    srcdirs = {os.path.dirname(os.path.abspath(s)) for s in (main_src, *extra_srcs)}
    incs = []
    for d in srcdirs:
        incs += ["-I", d]
    incs += ["-I", mesa_inc]
    objs = []
    for src in (main_src, *extra_srcs):
        o = os.path.join(OUT, os.path.splitext(os.path.basename(src))[0] + ".app.o")
        c = [cc] + TARGET + ["-isystem", INC] + incs + ["-std=c11", "-c", src, "-o", o]
        subprocess.run(c, check=True)
        objs.append(o)
    # link: app objects + gl9 archive + cc9 runtime, in one group (mutually dependent).
    l = [lld, "-o", elf, *objs, "--start-group", lib, *libllvm, cc9lib, cc9m,
         "--end-group", "-T", lds, "-static", "-nostdlib"]
    r = subprocess.run(l, capture_output=True, text=True)
    if r.returncode:
        # surface undefined symbols — the real integration signal
        print("LINK FAILED:"); print(r.stderr[-4000:]); sys.exit(1)
    subprocess.run(["python3", os.path.join(CC9, "host", "elf2aout.py"), elf, aout], check=True)
    print("linked ->", aout, os.path.getsize(aout), "bytes")


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "enumerate"
    if mode == "enumerate":
        enumerate_gaps()
    elif mode == "build":
        shim = compile_shim()
        entries, objs, fails = compile_all(stop_on_error=True)
        archive(objs + shim)
    elif mode == "link":
        if len(sys.argv) < 3:
            print("usage: build-gl9.py link MAIN.c [EXTRA.c ...]"); sys.exit(2)
        link(sys.argv[2], sys.argv[3:])
    else:
        print(__doc__ or "usage: build-gl9.py enumerate|build|link"); sys.exit(2)


if __name__ == "__main__":
    main()
