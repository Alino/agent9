#!/bin/bash
# plan9-env.sh — source this to cross-build Rust crates with C/C++ deps for
# x86_64-unknown-plan9 via cc9.
#
#   . servo9/host/plan9-env.sh
#   cargo +nightly build --release --target "$PLAN9_TARGET" \
#       -Zbuild-std=std,panic_abort -Zjson-target-spec
#
# Why each piece:
#  * CC/CXX + TARGET_CC/TARGET_CXX -> the cc9 wrapper, so cc-rs and mozilla's
#    configure both compile through cc9. (cc-rs prefers TARGET_*; configure reads
#    CC/CXX.)
#  * HOST_CC/HOST_CXX -> real clang: build-time tools must run on THIS machine.
#  * The binutils are LLVM's; there is no x86_64-unknown-plan9-objcopy and
#    configure looks for one before falling back.
#  * BINDGEN_EXTRA_CLANG_ARGS -> bindgen drives libclang directly and never sees
#    the cc9 wrapper, so it needs the target and include paths spelled out or it
#    parses jsapi.h against the HOST's headers and silently generates wrong
#    bindings (wrong type sizes, wrong layouts).
set -a

# Do NOT derive this from BASH_SOURCE: this file is meant to be *sourced*, and it
# gets sourced from zsh as often as bash. zsh leaves BASH_SOURCE unset, so the
# dirname trick silently resolves against the CWD and every path below comes out
# wrong-but-plausible. Take it from the environment, or fall back to the repo's
# real location.
AGENT9="${AGENT9:-/Users/claw/Projects/agent9}"
if [ ! -d "$AGENT9/cc9" ]; then
  echo "plan9-env.sh: AGENT9=$AGENT9 is not the agent9 repo; set AGENT9 explicitly" >&2
  return 1 2>/dev/null || exit 1
fi
CC9_HOST="$AGENT9/servo9/host"
LLVMBIN="${CC9_LLVM:-/opt/homebrew/opt/llvm/bin}"
LIBCXX="${CC9_LIBCXX:-/tmp/libcxx-thr/include/c++/v1}"

PLAN9_TARGET="$AGENT9/rust9/targets/x86_64-unknown-plan9.json"

CC="$CC9_HOST/cc9-cc"
CXX="$CC9_HOST/cc9-c++"
TARGET_CC="$CC9_HOST/cc9-cc"
TARGET_CXX="$CC9_HOST/cc9-c++"
HOST_CC=clang
HOST_CXX=clang++

AR="$LLVMBIN/llvm-ar"
NM="$LLVMBIN/llvm-nm"
RANLIB="$LLVMBIN/llvm-ranlib"
STRIP="$LLVMBIN/llvm-strip"
OBJCOPY="$LLVMBIN/llvm-objcopy"
READELF="$LLVMBIN/llvm-readelf"
TARGET_AR="$AR"
TARGET_OBJCOPY="$OBJCOPY"
TARGET_READELF="$READELF"

CC9_SHIM_INC="$AGENT9/servo9/port/mozjs/include"

# gl9 (Mesa + the gl9egl seam) supplies the EGL/GL symbols surfman calls. Plan 9
# has no dynamic linker, so they are linked statically into the binary; rust9-ld
# picks these up when RUST9_GL9 is set.
RUST9_GL9="$AGENT9/gl9/_out"
MOZJS_FROM_SOURCE=1

# getrandom has no plan9 target and hard-errors ("target is not supported").
# Its escape hatch is a custom backend: this cfg makes it call an embedder-provided
# __getrandom_v03_custom, which servo9/port/plan9-getrandom supplies over
# /dev/random. Any final binary must depend on that crate or the link fails.
RUSTFLAGS="${RUSTFLAGS:-} --cfg getrandom_backend=\"custom\""

BINDGEN_EXTRA_CLANG_ARGS="--target=x86_64-unknown-none -D__plan9__ \
-nostdinc++ -isystem $LIBCXX \
-isystem $CC9_SHIM_INC \
-isystem $AGENT9/cc9/runtime/include \
-D_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE -D_LIBCPP_HAS_CLOCK_GETTIME"

set +a
echo "plan9 cross env: CC=$CC  target=$PLAN9_TARGET" >&2
