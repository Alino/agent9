#!/bin/bash
# Generate cc1.template: the clang -cc1 argument list (with @SRC@/@OBJ@/@SRCBASE@
# placeholders) that the on-box cc.rc driver fills in. Derived from the host clang
# driver (-###) since the cc9 clang has no on-box driver (it can't spawn -cc1).
# Paths are remapped to the on-box staging tree (/tmp/{sysinc,res}). Run on the host.
set -euo pipefail
CC9="$(cd "$(dirname "$0")/.." && pwd)"
LLVM="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
LIBCXX="${CC9_LIBCXX:-/tmp/libcxx-thr/include/c++/v1}"
RES="$("$LLVM/clang" -print-resource-dir)"
echo 'int main(){}' > /tmp/_tmpl.cpp
python3 - "$LIBCXX" "$RES" "$CC9" <<'PY'
import subprocess,shlex,sys
LLVM=__import__('os').environ.get('CC9_LLVM','/opt/homebrew/opt/llvm/bin')
LIBCXX,RES,CC9=sys.argv[1],sys.argv[2],sys.argv[3]
cmd=[f'{LLVM}/clang','-###','--target=x86_64-unknown-none','-std=c++23','-nostdinc++','-isystem',LIBCXX,'-isystem',CC9+'/runtime/include','-fexceptions','-frtti','-funwind-tables','-fno-pic','-femulated-tls','-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE','-D_LIBCPP_HAS_CLOCK_GETTIME','-c','/tmp/_tmpl.cpp','-o','/tmp/_tmpl.o']
r=subprocess.run(cmd,capture_output=True,text=True)
toks=shlex.split([l for l in r.stderr.splitlines() if '-cc1' in l][0])[1:]
SUB=[(LIBCXX,'/tmp/sysinc/cxxv1'),(CC9+'/runtime/include','/tmp/sysinc/cc9'),(RES+'/include','/tmp/res'),(RES,'/tmp/res_rd')]
def fix(t):
  for a,b in SUB:
    if t==a or t.startswith(a+'/'): return b+t[len(a):]
  return t
toks=[fix(t) for t in toks]
toks=['@SRC@' if t=='/tmp/_tmpl.cpp' else '@OBJ@' if t=='/tmp/_tmpl.o' else '@SRCBASE@' if t=='_tmpl.cpp' else t for t in toks]
toks+=['-idirafter','/tmp/res','-idirafter','/tmp/sysinc/cc9']
open(CC9+'/native/cc1.template','w').write(' '.join(toks)+'\n')
print('wrote native/cc1.template ('+str(len(toks))+' tokens)')
PY
