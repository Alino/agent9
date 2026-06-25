#!/bin/bash
# run-tests.sh — build+run each cc9/test/suite/*.cpp on cirno, compare its
# output to the `// EXPECT:` line. The cc9 regression suite.
set -uo pipefail
CC9="$(cd "$(dirname "$0")/.." && pwd)"
pass=0; fail=0; failed=""
for src in "$CC9"/test/suite/*.cpp; do
  name="$(basename "$src" .cpp)"
  exp="$(grep -m1 '// EXPECT:' "$src" | sed 's#.*// EXPECT: ##')"
  got="$("$CC9"/host/cc9 run "$src" 2>/dev/null | tr -d '\r' | head -1)"
  if [ "$got" = "$exp" ]; then printf 'PASS  %s\n' "$name"; pass=$((pass+1))
  else printf 'FAIL  %s\n      want: %s\n      got:  %s\n' "$name" "$exp" "$got"; fail=$((fail+1)); failed="$failed $name"; fi
done
echo "----"; echo "$pass passed, $fail failed${failed:+ ($failed )}"
[ "$fail" -eq 0 ]
