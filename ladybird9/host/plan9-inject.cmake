# plan9-inject.cmake — pulled into Ladybird's top-level project() via
# CMAKE_PROJECT_INCLUDE (toolchain.cmake). Feature decisions for the plan9
# port that would otherwise need patches to build files. Everything here is
# a documented parity deferral (see ladybird9/parity/) or a platform fact.

# The platform predicate patches 0004+ branch on (mirrors upstream's ANDROID
# rows in check_for_dependencies.cmake).
set(PLAN9 ON CACHE BOOL "" FORCE)

# No dynamic linker on Plan 9: everything static (Ladybird defaults to shared
# lagom libs; helper processes are standalone static a.outs).
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# M1-M4 bring-up: engine + services only; the chrome is UI/Plan9 (M5).
set(ENABLE_GUI_TARGETS OFF CACHE BOOL "" FORCE)

# No QT/AppKit chrome, no installer targets.
set(ENABLE_QT OFF CACHE BOOL "" FORCE)

# Parity deferrals (upstream's own toggles where they exist):
#   video/audio (ffmpeg) and WebGL (angle/vulkan) land after M5.
set(ENABLE_VIDEO OFF CACHE BOOL "" FORCE)
set(ENABLE_AUDIO OFF CACHE BOOL "" FORCE)

# Sandbox: Services/RendererSandboxUnimplemented.cpp fallback (no seccomp).
# (Selected automatically for non-Linux/mac platforms; stated here for record.)

# ICU data is shipped as an archive file, not a static .a in every process:
# /lib/icu/icudt78l.dat on the box (LibUnicode patch sets u_setDataDirectory).

# The 10-crate Rust workspace cross-compiles with the rust-src-full stage1
# rustc (x86_64-unknown-plan9 is a BUILT-IN target there; prebuilt plan9 std
# in its sysroot — no build-std). build-ladybird.sh exports
# RUSTUP_TOOLCHAIN=plan9 (rustup link of the stage1) for cargo; the triple is
# a documented rust_crate.cmake cache seam:
set(RUST_TARGET_TRIPLE "x86_64-unknown-plan9" CACHE INTERNAL "Rust target triple")

# Host-run build tools a cross build cannot execute from the target build:
# asmintgen (host cargo build, from the M0 spike) and the pre-generated
# asm offsets (ABI-proven by the 123-static_assert gate; regenerate + re-gate
# on every pin bump via test/m0/asmint-spike.sh).
get_filename_component(_lb9_inject_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(LADYBIRD_ASMINTGEN
    "${_lb9_inject_root}/vendor/ladybird/Libraries/LibJS/AsmIntGen/target/release/asmintgen"
    CACHE FILEPATH "host asmintgen")
set(LADYBIRD_ASM_OFFSETS_FILE
    "${_lb9_inject_root}/vendor/ladybird/Build/release/Libraries/LibJS/Bytecode/AsmInterpreter/asm_offsets.conf"
    CACHE FILEPATH "pre-generated, gate-verified asm offsets")
