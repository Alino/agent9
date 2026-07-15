#!/usr/bin/env python3
# build-iristri.py — link Mesa's iris + the gpu9 shim into a runnable 9front
# a.out that drives iris and logs what it asks the "kernel" for (M8).
#
# Mirrors gl9/host/build-gl9.py's link recipe: cc9 clang -> ld.lld with the
# plan9 linker script -> elf2aout. The app/shim are C; iris is the prebuilt
# archive gl9/_out/libgl9mesa-iris.a.
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
GPU9 = os.path.dirname(HERE)
REPO = os.path.dirname(GPU9)
CC9  = os.environ.get("CC9", os.path.join(REPO, "cc9"))
GL9  = os.path.join(REPO, "gl9")
LLVM = os.environ.get("CC9_LLVM", "/opt/homebrew/opt/llvm/bin")
LLD  = os.environ.get("CC9_LLD", "/opt/homebrew/bin/ld.lld")
INC  = os.path.join(CC9, "runtime", "include")
OUT  = os.path.join(GPU9, "_out")
os.makedirs(OUT, exist_ok=True)

TARGET = ["--target=x86_64-unknown-none", "-femulated-tls", "-funwind-tables",
          "-fno-pic", "-O2", "-w"]
MESA = os.path.join(GL9, "vendor", "mesa")
INCS = ["-isystem", os.path.join(GPU9, "shim", "include"),        # xf86drm.h, sys/ioccom.h
        "-isystem", os.path.join(GL9, "vendor", "mesa", "include"),  # drm-uapi
        # the harness builds iris's driconf tables, so it needs Mesa's util +
        # the two driinfo fragment headers (gallium common + iris specific)
        "-I", os.path.join(MESA, "src"),
        "-I", os.path.join(MESA, "src", "util"),
        "-I", os.path.join(MESA, "src", "gallium", "auxiliary", "pipe-loader"),
        "-I", os.path.join(MESA, "src", "gallium", "drivers", "iris"),
        "-I", os.path.join(MESA, "src", "gallium", "include"),
        "-isystem", INC]

ARCHIVE = os.path.join(GL9, "_out", "libgl9mesa-iris.a")
CC9CXX  = os.path.join(CC9, "lib", "libcc9cxx.a")
CC9M    = os.path.join(CC9, "lib", "libcc9m.a")
LDS     = os.path.join(CC9, "test", "plan9.ld")

SRCS = [os.path.join(GPU9, "test", "iristri.c"),
        os.path.join(GPU9, "shim", "drmshim.c"),
        os.path.join(GPU9, "shim", "stubs.c"),
        os.path.join(GPU9, "shim", "gpu9_ioctl.c")]

if not os.path.exists(ARCHIVE):
    print("no libgl9mesa-iris.a — build iris first"); sys.exit(1)

objs = []
for s in SRCS:
    o = os.path.join(OUT, os.path.basename(s)[:-2] + ".o")
    cmd = [os.path.join(LLVM, "clang")] + TARGET + INCS + ["-std=c11", "-c", s, "-o", o]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print("COMPILE FAIL:", s); print(r.stderr[-3000:]); sys.exit(1)
    objs.append(o)

elf  = os.path.join(OUT, "iristri.elf")
aout = os.path.join(OUT, "iristri.aout")
link = [LLD, "-o", elf, *objs, "--start-group", ARCHIVE, CC9CXX, CC9M,
        "--end-group", "-T", LDS, "-static", "-nostdlib"]
r = subprocess.run(link, capture_output=True, text=True)
if r.returncode:
    print("LINK FAILED:"); print(r.stderr[-4000:]); sys.exit(1)
subprocess.run(["python3", os.path.join(CC9, "host", "elf2aout.py"), elf, aout], check=True)
print("built ->", aout, os.path.getsize(aout), "bytes")
