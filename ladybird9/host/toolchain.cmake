# ladybird9 CMake toolchain: cross-compile Ladybird to 9front amd64 with cc9.
# Adapted from cc9/native/toolchain.cmake (proven cross-building LLVM itself).
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=.../ladybird9/host/toolchain.cmake ...
#
# "Linux" sets UNIX=1 so Ladybird's CMake takes its POSIX paths without leaking
# a Linux ABI: the compile wrappers pin --target=x86_64-unknown-none, so
# __linux__ stays undefined and AK_OS_PLAN9 (via -D__plan9__ in the wrappers)
# is the only OS the source sees. NOT vcpkg's toolchain -> root CMakeLists
# skips vcpkg entirely; every dep resolves from the _out/deps sysroot.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(_lb9_host "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(LB9_ROOT "${_lb9_host}/.." ABSOLUTE)
get_filename_component(AGENT9_ROOT "${LB9_ROOT}/.." ABSOLUTE)
set(CC9_ROOT "${AGENT9_ROOT}/cc9")

if(NOT DEFINED ENV{CC9_LLVM})
  set(ENV{CC9_LLVM} "/opt/homebrew/opt/llvm/bin")
endif()
set(_llvm "$ENV{CC9_LLVM}")

# The cc(1)-shaped wrappers (servo9-proven): strip caller --target, pin the
# plan9 flags, libc++ include order, ld.lld link with cc9 libs.
set(CMAKE_C_COMPILER   "${AGENT9_ROOT}/servo9/host/cc9-cc")
set(CMAKE_CXX_COMPILER "${AGENT9_ROOT}/servo9/host/cc9-c++")
set(CMAKE_ASM_COMPILER "${AGENT9_ROOT}/servo9/host/cc9-cc")
set(CMAKE_AR      "${_llvm}/llvm-ar"     CACHE FILEPATH "cc9 archiver")
set(CMAKE_RANLIB  "${_llvm}/llvm-ranlib" CACHE FILEPATH "cc9 ranlib")
set(CMAKE_NM      "${_llvm}/llvm-nm"     CACHE FILEPATH "cc9 nm")
# The thin-LTO path uses the COMPILER-specific ar/ranlib, which CMake normally
# derives by interrogating the compiler binary. cc9-cc/cc9-c++ are wrapper
# scripts, so that probe yields *-NOTFOUND and CMake's LTO capability test dies
# with "CMAKE_CXX_COMPILER_AR-NOTFOUND ... code=127" on a FRESH build tree
# (an existing cache hides it). Point them at the same LLVM tools.
foreach(_lang C CXX)
  set(CMAKE_${_lang}_COMPILER_AR     "${_llvm}/llvm-ar"     CACHE FILEPATH "cc9 ${_lang} LTO archiver")
  set(CMAKE_${_lang}_COMPILER_RANLIB "${_llvm}/llvm-ranlib" CACHE FILEPATH "cc9 ${_lang} LTO ranlib")
endforeach()

# Thin-LTO is OFF for this target, deliberately. Plan 9 has no native TLS, so
# cc9 compiles everything -femulated-tls; that is a CODEGEN option, and LTO
# defers codegen to the linker plugin, which does not inherit it. The result is
# objects carrying STT_TLS symbols with no PT_TLS segment and a hard ld.lld
# error. This was previously "off" only by accident — CMake's LTO probe failed
# on CMAKE_CXX_COMPILER_AR-NOTFOUND — so fixing the probe above silently turned
# LTO on and broke the link. Use Ladybird's own knob (Meta/CMake/cmake_options
# .cmake), which is what drives CMAKE_INTERPROCEDURAL_OPTIMIZATION in
# compile_options.cmake; setting the derived variable here does NOT stick.
# The toolchain file is read before project(), so this pre-seeds the option().
# ponytail: re-enable only with -emulated-tls threaded into the LTO backend.
set(ENABLE_LTO_FOR_RELEASE OFF CACHE BOOL "cc9: LTO drops -femulated-tls" FORCE)

# Compiler checks must never run a 9front binary on the host.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Deps sysroot: static .a + headers + pkgconfig + cmake package configs,
# provisioned by host/deps/ recipes (ICU, Skia, curl, codecs, ...).
set(LB9_SYSROOT "${LB9_ROOT}/_out/deps")
set(CMAKE_FIND_ROOT_PATH "${LB9_SYSROOT}" "${CC9_ROOT}")
list(APPEND CMAKE_PREFIX_PATH "${LB9_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# Ladybird finds skia + woff2 via pkg-config: point it at the sysroot only.
set(ENV{PKG_CONFIG_LIBDIR} "${LB9_SYSROOT}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

# Feature toggles + platform decisions injected into project() without
# patching the root CMakeLists.
set(CMAKE_PROJECT_INCLUDE "${_lb9_host}/plan9-inject.cmake")

# Executables: link via lld against the cc9 runtime; elf2aout runs as a
# post-build pass (build-ladybird.sh), keeping CMake's link step host-clean.
set(CMAKE_CXX_LINK_EXECUTABLE
  "${CC9_ROOT}/native/cc9-link <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES> -o <TARGET>")
set(CMAKE_C_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}")
