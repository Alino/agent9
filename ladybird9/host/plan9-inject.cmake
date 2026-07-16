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

# M4+: Services (WebContent/Compositor/RequestServer/ImageDecoder) + the
# headless UI driver. The bespoke UI/Plan9 libdraw chrome is M5.
set(ENABLE_GUI_TARGETS ON CACHE BOOL "" FORCE)

# No QT/AppKit chrome, no installer targets.
set(ENABLE_QT OFF CACHE BOOL "" FORCE)

# Video/audio: ENABLED — ffmpeg 7.1.1 cross-built C-only into the sysroot
# (host/deps/build-ffmpeg.sh, --disable-asm; decoders+demuxers+parsers). LibMedia
# links the real FFmpeg TUs; audio playback falls to the null PlaybackStream
# (no 9front audio-out backend wired — decode is the deliverable). WebGL is
# enabled separately (OpenGLContext.cpp over gl9). See parity/deferrals.md.
set(ENABLE_VIDEO ON CACHE BOOL "" FORCE)
set(ENABLE_AUDIO ON CACHE BOOL "" FORCE)

# Wasm runs on LibWasm's bytecode interpreter: stock 9front enforces W^X
# (no exec pages), so the cranelift AOT path can never run. Upstream's own
# toggle selects CraneliftStubs.cpp. Also avoids target-lexicon's build.rs
# rejecting the x86_64-unknown-plan9 triple.
set(ENABLE_CRANELIFT_JIT OFF CACHE BOOL "" FORCE)

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

# ANGLE-only headers (gl2ext_angle.h) that gl9/Mesa doesn't ship: a compat
# shim mapping onto Mesa's Khronos gl2ext.h (see host/compat/GLES2/).
include_directories(SYSTEM "${_lb9_inject_root}/host/compat")

# gl9's EGL entry points live in a single cc9 object next to libgl9mesa.a's
# source tree (they aren't archived); check_for_dependencies appends it to
# PkgConfig::angle on plan9.
set(LB9_GL9EGL_OBJECT "${_lb9_inject_root}/../gl9/_out/gl9egl.app.o"
    CACHE FILEPATH "gl9 EGL front-end object")

# Khronos eglplatform.h selects EGLNative* types per windowing system and
# recognizes none for --target=x86_64-unknown-none. Its own opt-out makes
# them void*, which is exactly right for gl9's surfaceless EGL (no native
# window/pixmap/display on 9front; the Compositor renders offscreen).
add_compile_definitions(EGL_NO_PLATFORM_SPECIFIC_TYPES)

# Skia pathops (Op/Simplify) reaches upstream libskia.a only via the pdf/xps
# optionals, both off in the sysroot build — LibGfx calls Op() directly.
# Until build-skia.sh harvests :pathops, the objects are compiled from the
# pinned vendor/skia tree into the build dir (see M4 notes) and appended to
# PkgConfig::skia in check_for_dependencies alongside skia's freetype dep.
set(LB9_SKIA_PATHOPS_ARCHIVE
    "${CMAKE_BINARY_DIR}/lib9pathops/libskiapathops.a"
    CACHE FILEPATH "skia :pathops objects (cc9-built from vendor/skia)")

# SDL3 virtual-joystick stubs (InternalGamepad links them; the sysroot
# sdl3-shim predates them — fold into port/sdl3-shim on next regeneration).
set(LB9_SDL3_VJOYSTICK_OBJECT
    "${CMAKE_BINARY_DIR}/lib9pathops/sdl3-virtual-joystick-stubs.o"
    CACHE FILEPATH "SDL3 virtual-joystick stub object (host/compat source)")

# Static lagom (BUILD_SHARED_LIBS OFF, above) links several Rust staticlibs
# into one binary; each carries rustc's identical allocator forwarding shims
# (__rustc::__rust_alloc etc.). First-definition-wins is safe: same compiler,
# same shims. cc9-link passes exactly this flag through to lld.
add_link_options("-Wl,--allow-multiple-definition")
set(LADYBIRD_ASMINTGEN
    "${_lb9_inject_root}/vendor/ladybird/Libraries/LibJS/AsmIntGen/target/release/asmintgen"
    CACHE FILEPATH "host asmintgen")
set(LADYBIRD_ASM_OFFSETS_FILE
    "${_lb9_inject_root}/vendor/ladybird/Build/release/Libraries/LibJS/Bytecode/AsmInterpreter/asm_offsets.conf"
    CACHE FILEPATH "pre-generated, gate-verified asm offsets")
