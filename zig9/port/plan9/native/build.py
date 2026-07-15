#!/usr/bin/env python3
"""Build the Zig compiler to run natively ON 9front (host -> Plan 9 a.out).

Route: Zig's own CBE bootstrap. An already-working host zig emits the whole
compiler as C (zig2.c) + compiler_rt.c targeting x86_64-plan9 over the PATCHED
vendor tree; cc9's clang/lld pipeline (same as python9/port/cc9) compiles that C
into a native a.out. The resulting `zig9` compiles Zig programs on 9front with
the self-hosted x86_64 backend + Plan9 linker (all 14 cross patches baked in).

  build.py                 # full: emit zig2.c + compiler_rt.c, compile, link
  build.py --emit-only     # just (re)emit the C
  build.py --compile-only  # skip emit, compile existing _out/*.c
  build.py --smoke FILE.zig  # build one small .zig the same way (fast shakeout)
  build.py --libtar        # pack the lib/ tree for shipping to the box
  build.py --package       # assemble the pac9 tarball (amd64/bin + sys/lib + rc/bin)
  build.py -v              # echo every command

Env overrides: ZIG9_EMITTER (host zig, default vendor/zig-host/zig),
CC9_LLVM (clang dir), CC9_LLD (ld.lld path), CC9_OPT (zig2.c opt, default -O2).
"""
import os, sys, subprocess, tarfile, time

HERE = os.path.dirname(os.path.abspath(__file__))          # zig9/port/plan9/native
ZIG9 = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))  # zig9/
AGENT9 = os.path.dirname(ZIG9)
VENDOR = os.path.join(ZIG9, "vendor", "zig")
CC9 = os.path.join(AGENT9, "cc9")
OUT = os.path.join(HERE, "_out")

EMITTER = os.environ.get("ZIG9_EMITTER", os.path.join(ZIG9, "vendor", "zig-host", "zig"))
LLVM = os.environ.get("CC9_LLVM", "/opt/homebrew/opt/llvm/bin")
LLD = os.environ.get("CC9_LLD") or "/opt/homebrew/opt/lld/bin/ld.lld"
CC9_OPT = os.environ.get("CC9_OPT", "-O2")
VERBOSE = "-v" in sys.argv

TARGET = "x86_64-plan9"
ZFLAGS = ["--zig-lib-dir", "lib", "-ofmt=c", "-OReleaseSmall", "-fsingle-threaded",
          "-target", TARGET]

# cc9 compile flags (mirrors python9/port/cc9/build.py), plus zig.h include.
CFLAGS = ["--target=x86_64-unknown-none", "-nostdlib", "-std=c99",
          "-femulated-tls", "-fno-pic", "-mno-red-zone", "-funwind-tables",
          "-fno-stack-protector", "-D__plan9__", "-DNDEBUG",
          "-Wno-macro-redefined", "-Wno-incompatible-library-redeclaration",
          "-isystem", os.path.join(CC9, "runtime", "include"),
          "-I", os.path.join(VENDOR, "lib")]        # zig.h

def run(cmd, cwd=None):
    if VERBOSE:
        print("+", " ".join(cmd))
    subprocess.check_call(cmd, cwd=cwd)

def emit():
    os.makedirs(OUT, exist_ok=True)
    print("emitting zig2.c (this reads the whole compiler; ~1 min)")
    run([EMITTER, "build-exe", *ZFLAGS,
         "--name", "zig2", "-femit-bin=" + os.path.join(OUT, "zig2.c"),
         "--dep", "build_options", "--dep", "aro",
         "-Mroot=src/main.zig",
         "-Mbuild_options=" + os.path.join(HERE, "config.zig"),
         "-Maro=lib/compiler/aro/aro.zig"], cwd=VENDOR)
    print("emitting compiler_rt.c")
    run([EMITTER, "build-obj", *ZFLAGS,
         "--name", "compiler_rt", "-femit-bin=" + os.path.join(OUT, "compiler_rt.c"),
         "-Mroot=lib/compiler_rt.zig"], cwd=VENDOR)

def compile_c(src, obj, opt):
    run([os.path.join(LLVM, "clang"), *CFLAGS, opt, "-c", src, "-o", obj])

def compile_asm(src, obj):
    run([os.path.join(LLVM, "clang"), "--target=x86_64-unknown-none", "-c", src, "-o", obj])

def link(objs, elf, aout):
    cmd = [LLD, "-o", elf, *objs,
           "--start-group",
           os.path.join(CC9, "lib", "libcc9cxx.a"),
           os.path.join(CC9, "lib", "libcc9m.a"), "--end-group",
           "-T", os.path.join(CC9, "test", "plan9.ld"), "-static", "-nostdlib",
           "-z", "muldefs", "--allow-multiple-definition"]
    run(cmd)
    run(["python3", os.path.join(CC9, "host", "elf2aout.py"), elf, aout])

def build_full():
    if "--compile-only" not in sys.argv:
        emit()
        if "--emit-only" in sys.argv:
            return
    t0 = time.time()
    print("compiling compiler_rt.c")
    compile_c(os.path.join(OUT, "compiler_rt.c"), os.path.join(OUT, "compiler_rt.o"), "-O2")
    print("compiling zig9syscall.s + zig9compat.c")
    compile_asm(os.path.join(HERE, "zig9syscall.s"), os.path.join(OUT, "zig9syscall.o"))
    compile_c(os.path.join(HERE, "zig9compat.c"), os.path.join(OUT, "zig9compat.o"), "-O2")
    print("compiling zig2.c (%s; the slow step, 10-40 min)" % CC9_OPT)
    compile_c(os.path.join(OUT, "zig2.c"), os.path.join(OUT, "zig2.o"), CC9_OPT)
    print("linking (%.0fs elapsed)" % (time.time() - t0))
    link([os.path.join(OUT, o) for o in ("zig2.o", "compiler_rt.o", "zig9syscall.o", "zig9compat.o")],
         os.path.join(OUT, "zig9.elf"), os.path.join(OUT, "zig9.aout"))
    print("done in %.0fs -> %s" % (time.time() - t0, os.path.join(OUT, "zig9.aout")))

def build_smoke(zig_src):
    os.makedirs(OUT, exist_ok=True)
    base = os.path.splitext(os.path.basename(zig_src))[0]
    cfile = os.path.join(OUT, base + ".c")
    src_abs = zig_src if os.path.isabs(zig_src) else os.path.join(os.getcwd(), zig_src)
    run([EMITTER, "build-exe", *ZFLAGS, "--name", base, "-femit-bin=" + cfile,
         "-Mroot=" + src_abs], cwd=VENDOR)
    compile_c(cfile, os.path.join(OUT, base + ".o"), "-O2")
    compile_asm(os.path.join(HERE, "zig9syscall.s"), os.path.join(OUT, "zig9syscall.o"))
    compile_c(os.path.join(HERE, "zig9compat.c"), os.path.join(OUT, "zig9compat.o"), "-O2")
    link([os.path.join(OUT, o) for o in (base + ".o", "zig9syscall.o", "zig9compat.o")],
         os.path.join(OUT, base + ".elf"), os.path.join(OUT, base + ".aout"))
    print("->", os.path.join(OUT, base + ".aout"))

# The lib/ subtree the native zig needs at runtime (drops foreign-libc headers).
LIB_DROP = {"libc", "libcxx", "libcxxabi", "libunwind", "tsan", "docs"}

def libtar():
    os.makedirs(OUT, exist_ok=True)
    tarpath = os.path.join(OUT, "zig9-lib.tar.gz")
    libdir = os.path.join(VENDOR, "lib")
    with tarfile.open(tarpath, "w:gz") as t:
        for name in sorted(os.listdir(libdir)):
            if name in LIB_DROP:
                continue
            t.add(os.path.join(libdir, name), arcname="lib/" + name)
    print("->", tarpath, os.path.getsize(tarpath), "bytes")

def package():
    """Assemble the pac9 tarball: amd64/bin/zig9, sys/lib/zig9/lib, rc/bin/zig.
    Requires _out/zig9big.aout (full build) to exist."""
    os.makedirs(OUT, exist_ok=True)
    aout = os.path.join(OUT, "zig9big.aout")
    if not os.path.exists(aout):
        sys.exit("missing " + aout + " — run a full build first")
    wrapper = os.path.join(HERE, "zig.rc")
    libdir = os.path.join(VENDOR, "lib")
    tarpath = os.path.join(OUT, "zig9-amd64.tar.gz")
    # pac9 installs with a plain `tar xf` and does NOT chmod afterward, so the
    # binary + wrapper must carry the execute bits in the tarball itself (the
    # host copies are 0644). Without this `pac9 install zig9` leaves a
    # non-executable /amd64/bin/zig9.
    def executable(ti):
        ti.mode = 0o755
        return ti
    with tarfile.open(tarpath, "w:gz") as t:
        t.add(aout, arcname="amd64/bin/zig9", filter=executable)
        t.add(wrapper, arcname="rc/bin/zig", filter=executable)
        for name in sorted(os.listdir(libdir)):
            if name in LIB_DROP:
                continue
            t.add(os.path.join(libdir, name), arcname="sys/lib/zig9/lib/" + name)
    print("->", tarpath, os.path.getsize(tarpath), "bytes")

def main():
    if "--smoke" in sys.argv:
        build_smoke(sys.argv[sys.argv.index("--smoke") + 1])
    elif "--libtar" in sys.argv:
        libtar()
    elif "--package" in sys.argv:
        package()
    else:
        build_full()

if __name__ == "__main__":
    main()
