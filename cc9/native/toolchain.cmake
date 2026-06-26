# CMake toolchain file: cross-compile to 9front amd64 with cc9.
#   cmake -DCMAKE_TOOLCHAIN_FILE=<repo>/cc9/native/toolchain.cmake ...
# Pairs the cc9 compile/link wrappers with llvm-ar/ranlib and a static-library
# try-compile (so CMake never tries to run a 9front binary on the host). Used to
# cross-build LLVM/clang itself (G1+ of the native-compiler plan).
# "Linux" makes CMake set UNIX=1 so LLVM defines LLVM_ON_UNIX and uses its own
# POSIX Unix/*.inc implementations (open/read/stat/mmap/getcwd/…) — which cc9's
# runtime backs. It does NOT leak a Linux ABI into codegen: the compile wrappers
# pin --target=x86_64-unknown-none (freestanding), so __linux__ stays undefined
# and only the CMake-level OS abstraction switches on. Plan 9 is POSIX-ish here.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(_cc9_native "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(CC9_ROOT "${_cc9_native}/.." ABSOLUTE)

if(NOT DEFINED ENV{CC9_LLVM})
  set(ENV{CC9_LLVM} "/opt/homebrew/opt/llvm/bin")
endif()
set(_llvm "$ENV{CC9_LLVM}")

set(CMAKE_C_COMPILER   "${_cc9_native}/cc9-clang")
set(CMAKE_CXX_COMPILER "${_cc9_native}/cc9-clang++")
set(CMAKE_AR      "${_llvm}/llvm-ar"     CACHE FILEPATH "cc9 archiver")
set(CMAKE_RANLIB  "${_llvm}/llvm-ranlib" CACHE FILEPATH "cc9 ranlib")
set(CMAKE_NM      "${_llvm}/llvm-nm"     CACHE FILEPATH "cc9 nm")

# The compiler check must not link+run a host executable — archive a static lib.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Programs from the host; headers/libs only from the cc9 sysroot.
set(CMAKE_FIND_ROOT_PATH "${CC9_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Link executables via the cc9 lld->elf2aout wrapper instead of the compiler.
set(CMAKE_CXX_LINK_EXECUTABLE
  "${_cc9_native}/cc9-link <OBJECTS> <LINK_LIBRARIES> -o <TARGET>")
set(CMAKE_C_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}")
