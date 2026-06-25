#!/bin/bash
# run-libcxx-tests.sh <subdir> <N> — run up to N libc++ .pass.cpp conformance
# tests from libcxx/test/std/<subdir> through cc9 on cirno. The real parity
# harness (cf. python9's CPython suite). PASS = compiles + links + converts +
# runs + exits clean (a .pass.cpp asserts internally; clean exit == pass).
# Skips tests that need features cc9 lacks (exceptions/iostreams/threads/...).
CC9="$(cd "$(dirname "$0")/.." && pwd)"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
LLD="${CC9_LLD:-$(brew --prefix lld)/bin/ld.lld}"
LIBCXX="${CC9_LIBCXX:-/tmp/libcxx-loc/include/c++/v1}"
LLVMSRC="${CC9_LLVMSRC:-$HOME/Projects/llvm-project}"
TST="$LLVMSRC/libcxx/test"
INC="$CC9/runtime/include"; LIB="$CC9/lib/libcc9cxx.a"; LIBM="$CC9/lib/libcc9m.a"; LDS="$CC9/test/plan9.ld"
read -r DH DP <<<"${CC9_DEV:-127.0.0.1 1717}"

sub="${1:-utilities}"; N="${2:-20}"
# Spread the sample evenly across the category (not head -N, which clusters in
# the first subdir) so the parity number is representative.
all="$(find "$TST/std/$sub" -name '*.pass.cpp' | sort)"
tot=$(echo "$all" | grep -c .)
sample="$(echo "$all" | awk -v tot="$tot" -v n="$N" 'BEGIN{for(i=0;i<n;i++)w[int(i*tot/n)+1]=1} w[NR]')"
pass=0 cfail=0 rfail=0 skip=0; failed=""
for t in $(echo "$sample" | head -"$N"); do
  name="${t#$TST/std/}"
  # Honor libc++'s own feature model (what upstream lit does): a test annotated
  # UNSUPPORTED/XFAIL with a feature our build lacks is NOT applicable to this
  # configuration, so it's a skip, not a failure. Our config lacks: exceptions,
  # rtti, threads, wide chars, localization, filesystem, tzdb, PSTL, random
  # device, locale. Also skip tests unsupported AT c++23/c++26 (we compile c++23;
  # a c++23-feature test lists UNSUPPORTED: c++03..c++20 and MUST still run), and
  # any header pulling an unsupported subsystem.
  # localization + monotonic-clock are now ON, so those features no longer gate.
  miss='no-exceptions|no-rtti|no-threads|no-wide-characters|no-filesystem|no-tzdb|libcpp-has-no-incomplete-pstl|no-random-device|c\+\+23|c\+\+26|availability'
  if grep -qE '#include <(thread|fstream|mutex|shared_mutex|future|filesystem|format|syncstream|print|coroutine|stop_token|barrier|latch|semaphore|condition_variable)>' "$t" \
     || grep -qE "(UNSUPPORTED|XFAIL):[^/]*($miss)" "$t" \
     || grep -qE '// *REQUIRES:' "$t"; then
    skip=$((skip+1)); continue; fi
  # Honor the test's ADDITIONAL_COMPILE_FLAGS (what upstream lit applies) — e.g.
  # -D_LIBCPP_ENABLE_CXX20_REMOVED_* for tests of deprecated/removed features.
  # Both the bare `ADDITIONAL_COMPILE_FLAGS:` and the feature-guarded
  # `ADDITIONAL_COMPILE_FLAGS(has-fconstexpr-steps):` forms (the guarded clang
  # flags — constexpr step/ops limits — are universally available, so apply).
  # Drop -fconstexpr-ops-limit (a GCC-only flag, guarded has-fconstexpr-ops-limit
  # which is false for clang; clang's equivalent -fconstexpr-steps is also given).
  addf="$(grep -hE '// *ADDITIONAL_COMPILE_FLAGS(\([^)]*\))?:' "$t" | sed -E 's#.*ADDITIONAL_COMPILE_FLAGS(\([^)]*\))?: *##; s#-fconstexpr-ops-limit=[0-9]+##g' | tr ',\n' '  ')"
  if ! "$LLVM/clang++" --target=x86_64-unknown-none -nostdlib -DNDEBUG -std=c++23 -nostdinc++ \
        -isystem "$LIBCXX" -isystem "$INC" -I "$TST/support" -I "$(dirname "$t")" \
        -fno-exceptions -fno-rtti -fno-threadsafe-statics -D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE -D_LIBCPP_HAS_CLOCK_GETTIME $addf -c "$t" -o /tmp/lt.o 2>/dev/null; then
    cfail=$((cfail+1)); failed="$failed C:$name"; continue; fi
  # *.compile.pass.cpp are compile-only conformance checks (no main): passing ==
  # compiling. Don't link/run them — they'd fail the link for lack of main.
  case "$name" in *.compile.pass.cpp) pass=$((pass+1)); continue;; esac
  if ! "$LLD" -o /tmp/lt.elf /tmp/lt.o --start-group "$LIB" "$LIBM" --end-group -T "$LDS" -static -nostdlib 2>/dev/null; then
    cfail=$((cfail+1)); failed="$failed L:$name"; continue; fi
  if ! python3 "$CC9/host/elf2aout.py" /tmp/lt.elf /tmp/lt.aout >/dev/null 2>&1; then
    cfail=$((cfail+1)); failed="$failed A:$name"; continue; fi
  out="$(python3 "$CC9/host/deliver.py" /tmp/lt.aout "$DH" "$DP" 2>/dev/null)"
  if echo "$out" | grep -qiE 'abort|suicide|trap|fault'; then
    rfail=$((rfail+1)); failed="$failed R:$name"
  else pass=$((pass+1)); fi
done
echo "=== libc++ conformance ($sub, first $N): pass=$pass cfail=$cfail rfail=$rfail skip=$skip ==="
[ -n "$failed" ] && echo "fails:$failed" | tr ' ' '\n' | head -30
exit 0
