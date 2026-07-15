# plan9-inject.cmake — pulled into Ladybird's top-level project() via
# CMAKE_PROJECT_INCLUDE (toolchain.cmake). Feature decisions for the plan9
# port that would otherwise need patches to build files. Everything here is
# a documented parity deferral (see ladybird9/parity/) or a platform fact.

# The platform predicate patches 0004+ branch on (mirrors upstream's ANDROID
# rows in check_for_dependencies.cmake).
set(PLAN9 ON CACHE BOOL "" FORCE)

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
