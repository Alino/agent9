#!/usr/bin/env python3
"""Build CPython 3.11 for 9front with cc9 (host clang -> Plan 9 a.out).

Second build profile alongside the kencc/APE one (port/plan9/): same static
module table, but SysV LP64, real pthreads, poll(2), openlibm. Cross-compiles
on the host (fast), unlike the kencc build which ran on the VM.

  python3 port/cc9/build.py            # -> port/cc9/_out/python.aout
  python3 port/cc9/build.py -v        # show every compile command
"""
import os, re, subprocess, sys, shutil
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
PY9 = os.path.dirname(os.path.dirname(HERE))            # python9/
AGENT9 = os.path.dirname(PY9)
SRC = os.path.join(PY9, "cpython", "src")
CC9 = os.path.join(AGENT9, "cc9")
OUT = os.path.join(HERE, "_out")
OBJ = os.path.join(OUT, "obj")

LLVM = os.environ.get("CC9_LLVM", "/opt/homebrew/opt/llvm/bin")
LLD = os.environ.get("CC9_LLD") or os.path.join(
    subprocess.check_output(["brew", "--prefix", "lld"], text=True).strip(), "bin", "ld.lld")

CFLAGS = [
    "--target=x86_64-unknown-none", "-nostdlib", "-O2",
    "-femulated-tls", "-fno-pic", "-mno-red-zone", "-funwind-tables",
    "-D__plan9__", "-DNDEBUG",
    "-isystem", os.path.join(CC9, "runtime", "include"),
    "-I", HERE,                       # pyconfig.h (must beat any src-root one)
    "-I", SRC,
    "-I", os.path.join(SRC, "Include"),
    "-I", os.path.join(SRC, "Include", "internal"),
    "-I", os.path.join(AGENT9, "ssl9", "vendor", "openssl-3.0.17", "include"),
    "-Wno-macro-redefined",
]
SSL9 = os.path.join(AGENT9, "ssl9", "_out")

CORE = ["-DPy_BUILD_CORE"]
BUILTIN = ["-DPy_BUILD_CORE_BUILTIN"]

def srcs_in(d, exclude=()):
    return sorted("%s/%s" % (d, f) for f in os.listdir(os.path.join(SRC, d))
                  if f.endswith(".c") and f not in exclude)

# Core object set mirrors the host Makefile's, minus dynload (static build).
PARSER = srcs_in("Parser") + srcs_in("Parser/tokenizer") if os.path.isdir(
    os.path.join(SRC, "Parser/tokenizer")) else srcs_in("Parser")
OBJECTS = srcs_in("Objects")
PYTHON = [s for s in srcs_in("Python", exclude=("dynload_shlib.c", "dynload_hpux.c",
          "dynload_stub.c", "dynload_win.c", "dynload_aix.c", "sysmodule_stub.c",
          "frozenmain.c", "dup2.c", "emscripten_signal.c", "emscripten_trampoline.c",
          "asm_trampoline.S"))] + ["Python/deepfreeze/deepfreeze.c"]

# Static modules: exactly the port's config.c table (kencc-parity module set).
MODULES = [
    "Modules/atexitmodule.c", "Modules/posixmodule.c", "Modules/signalmodule.c",
    "Modules/_tracemalloc.c", "Modules/_codecsmodule.c",
    "Modules/_collectionsmodule.c", "Modules/errnomodule.c",
    "Modules/_io/_iomodule.c", "Modules/_io/bufferedio.c", "Modules/_io/bytesio.c",
    "Modules/_io/fileio.c", "Modules/_io/iobase.c", "Modules/_io/stringio.c",
    "Modules/_io/textio.c",
    "Modules/itertoolsmodule.c", "Modules/_sre/sre.c", "Modules/_threadmodule.c",
    "Modules/timemodule.c", "Modules/_weakref.c", "Modules/_abc.c",
    "Modules/_functoolsmodule.c", "Modules/_operator.c", "Modules/_stat.c",
    "Modules/symtablemodule.c", "Modules/xxsubtype.c",
    "Modules/mathmodule.c", "Modules/cmathmodule.c", "Modules/_randommodule.c",
    "Modules/md5module.c", "Modules/sha1module.c", "Modules/sha256module.c",
    "Modules/sha512module.c", "Modules/_bisectmodule.c", "Modules/_heapqmodule.c",
    "Modules/_blake2/blake2module.c", "Modules/_blake2/blake2b_impl.c",
    "Modules/_blake2/blake2s_impl.c", "Modules/_blake2/impl/blake2b-ref.c",
    "Modules/_blake2/impl/blake2s-ref.c", "Modules/_sha3/sha3module.c",
    "Modules/zlibmodule.c",
    "Modules/_sqlite/blob.c", "Modules/_sqlite/connection.c",
    "Modules/_sqlite/cursor.c", "Modules/_sqlite/microprotocols.c",
    "Modules/_sqlite/module.c", "Modules/_sqlite/prepare_protocol.c",
    "Modules/_sqlite/row.c", "Modules/_sqlite/statement.c", "Modules/_sqlite/util.c",
    "Modules/_json.c", "Modules/_csv.c", "Modules/_struct.c",
    "Modules/arraymodule.c", "Modules/_datetimemodule.c",
    "Modules/_statisticsmodule.c", "Modules/_contextvarsmodule.c",
    "Modules/_opcode.c", "Modules/_pickle.c", "Modules/binascii.c",
    "Modules/_queuemodule.c", "Modules/unicodedata.c", "Modules/selectmodule.c",
    "Modules/_posixsubprocess.c", "Modules/socketmodule.c",
    "Modules/_ssl.c", "Modules/_hashopenssl.c", "Modules/pyexpat.c",
    "Modules/expat/xmlparse.c", "Modules/expat/xmlrole.c", "Modules/expat/xmltok.c",
    "Modules/getbuildinfo.c", "Modules/getpath.c", "Modules/main.c",
    "Modules/gcmodule.c",
]

PORT = [  # port-local TUs (absolute paths)
    os.path.join(HERE, "config.c"),
    os.path.join(HERE, "cc9_compat.c"),
    os.path.join(PY9, "port", "plan9", "faulthandler_stub.c"),
]

PROGRAM = ["Programs/python.c"]

ZLIB = os.path.join(HERE, "deps", "zlib-1.3.1")
SQLITE = os.path.join(HERE, "deps", "sqlite-amalgamation-3460100")
PORT += [os.path.join(SQLITE, "sqlite3.c")]
PORT += [os.path.join(ZLIB, z) for z in
         ("adler32.c","compress.c","crc32.c","deflate.c",
          "infback.c","inffast.c","inflate.c","inftrees.c",
          "trees.c","uncompr.c","zutil.c")]  # gz*.c dropped (file I/O, unused by Py zlib)


EXPAT_FLAGS = ["-I", os.path.join(SRC, "Modules", "expat"),
               "-DHAVE_EXPAT_CONFIG_H", "-DXML_POOR_ENTROPY", "-DUSE_PYEXPAT_CAPI"]

def flags_for(rel):
    if rel.startswith("Modules/expat/"):
        return BUILTIN + EXPAT_FLAGS
    if rel == "Modules/pyexpat.c":
        return BUILTIN + ["-I", os.path.join(SRC, "Modules", "expat")]
    if rel.startswith("Modules/_io/"):
        return BUILTIN + ["-I", os.path.join(SRC, "Modules", "_io")]
    if rel in ("Modules/_ssl.c", "Modules/_hashopenssl.c"):
        return BUILTIN
    if rel == "Modules/zlibmodule.c":
        return BUILTIN + ["-I", os.path.join(HERE, "deps", "zlib-1.3.1")]
    if rel.startswith("Modules/_sqlite/"):
        return BUILTIN + ["-I", os.path.join(HERE, "deps", "sqlite-amalgamation-3460100"), "-DMODULE_NAME=\"sqlite3\""]
    if rel.startswith("Modules/_blake2/"):
        return BUILTIN + ["-I", os.path.join(SRC, "Modules", "_blake2", "impl")]
    if rel.startswith("Modules/_sha3/"):
        return BUILTIN + ["-I", os.path.join(SRC, "Modules", "_sha3")]
    if rel.startswith("Modules/") and rel not in (
            "Modules/getbuildinfo.c", "Modules/getpath.c", "Modules/main.c",
            "Modules/gcmodule.c", "Modules/posixmodule.c",
            "Modules/signalmodule.c", "Modules/_threadmodule.c",
            "Modules/atexitmodule.c", "Modules/_tracemalloc.c"):
        return BUILTIN
    return CORE

def obj_path(path):
    rel = os.path.relpath(path, SRC) if path.startswith(SRC + os.sep) or not os.path.isabs(path) else os.path.basename(path)
    return os.path.join(OBJ, re.sub(r"[/]", "__", rel) + ".o")

verbose = "-v" in sys.argv

def compile_one(rel):
    src = rel if os.path.isabs(rel) else os.path.join(SRC, rel)
    obj = obj_path(rel if not os.path.isabs(rel) else rel)
    if os.path.exists(obj) and os.path.getmtime(obj) > max(
            os.path.getmtime(src), os.path.getmtime(os.path.join(HERE, "pyconfig.h"))):
        return None
    if os.path.isabs(rel) and rel.endswith("sqlite3.c"):
        extra = ["-DSQLITE_THREADSAFE=1", "-DSQLITE_OMIT_LOAD_EXTENSION",
                 "-DSQLITE_ENABLE_FTS4", "-DSQLITE_ENABLE_FTS5", "-DSQLITE_ENABLE_JSON1"]
    else:
        extra = []
    cmd = [os.path.join(LLVM, "clang"), *CFLAGS,
           *(flags_for(rel) if not os.path.isabs(rel) else CORE), *extra,
           "-c", src, "-o", obj]
    if verbose:
        print(" ".join(cmd))
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        return (rel, p.stderr)
    if p.stderr.strip() and verbose:
        print(p.stderr, file=sys.stderr)
    return None

def main():
    os.makedirs(OBJ, exist_ok=True)
    tus = PARSER + OBJECTS + PYTHON + MODULES + PROGRAM + PORT
    print(f"{len(tus)} TUs")
    fails = []
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
        for r in ex.map(compile_one, tus):
            if r:
                fails.append(r)
    if fails:
        for rel, err in fails[:12]:
            print(f"\n=== FAIL {rel} ===\n{err[:3000]}")
        print(f"\n{len(fails)}/{len(tus)} TUs failed")
        sys.exit(1)
    print("compile OK; linking")
    elf = os.path.join(OUT, "python.elf")
    aout = os.path.join(OUT, "python.aout")
    objs = [obj_path(t) for t in tus]
    PYO3DEMO = os.path.join(AGENT9, "pyo39", "demo", "target",
                            "x86_64-unknown-plan9", "release", "libcc9demo.a")
    JITER = os.path.join(AGENT9, "pyo39", "jiter-0.8.2", "target",
                         "x86_64-unknown-plan9", "release", "libjiter_python.a")
    PYDANTIC = os.path.join(AGENT9, "pyo39", "pydantic-core-2.33.1", "target",
                            "x86_64-unknown-plan9", "release", "lib_pydantic_core.a")
    cmd = [LLD, "-o", elf, *objs,
           "--start-group",
           *( [PYDANTIC, JITER, os.path.join(AGENT9, "pyo39", "demo", "target", "n9unwind.o")]
              if os.path.exists(PYDANTIC) else [] ),
           os.path.join(SSL9, "libssl.a"),
           os.path.join(SSL9, "libcrypto.a"),
           os.path.join(CC9, "lib", "libcc9cxx.a"),
           os.path.join(CC9, "lib", "libcc9m.a"), "--end-group",
           "-T", os.path.join(CC9, "test", "plan9.ld"), "-static", "-nostdlib",
           "-z", "muldefs", "--allow-multiple-definition"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        print(p.stderr[:8000])
        sys.exit(1)
    subprocess.check_call(["python3", os.path.join(CC9, "host", "elf2aout.py"), elf, aout])
    print("->", aout, os.path.getsize(aout), "bytes")

if __name__ == "__main__":
    main()
