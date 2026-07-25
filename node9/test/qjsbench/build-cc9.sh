#!/bin/bash
# build qjsbench for 9front with cc9 (clang -> ELF -> a.out)
set -e
CC9=/Users/claw/Projects/agent9/cc9
CLANG=/opt/homebrew/opt/llvm/bin/clang
LLD=$(brew --prefix lld)/bin/ld.lld
OPT=${OPT:--O2}
for f in quickjs libregexp libunicode dtoa bench; do
  echo "== $f"
  "$CLANG" --target=x86_64-unknown-none -nostdlib -DNDEBUG $OPT \
    -DNO_TM_GMTOFF -Dalloca=__builtin_alloca \
    -isystem "$CC9/runtime/include" -fno-pic -femulated-tls -funwind-tables \
    -c "$f.c" -o "$f.o"
done
"$LLD" -o qjsbench.elf quickjs.o libregexp.o libunicode.o dtoa.o bench.o \
  --start-group "$CC9/lib/libcc9cxx.a" "$CC9/lib/libcc9m.a" --end-group \
  -T "$CC9/test/plan9.ld" -static -nostdlib
python3 "$CC9/host/elf2aout.py" qjsbench.elf qjsbench-cc9
ls -l qjsbench-cc9
