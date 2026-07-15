# ladybird9 parity ledger — deferrals vs pin 8cc5d7a5ff

Every entry is a deliberate, visible divergence from the upstream build on
supported platforms, with its restoration path. Anything NOT listed here is
expected to behave identically to the same-commit host build.

| Area | Deferral | Mechanism | Restore when |
|---|---|---|---|
| `<video>`/`<audio>` | No media decode (ffmpeg quartet not built) | patch 0004 plan9 row skips avcodec/avformat/avutil/swresample; LibMedia PLAN9 arm compiles FFmpeg/FFmpegStubPlan9.cpp + Codecs/VorbisStubPlan9.cpp (every decoder factory reports unsupported) instead of the FFmpeg-backed TUs | ffmpeg cross-build lands (post-M5) |
| JPEG XL images | No libjxl decode | patch 0004 plan9 row; LibImageDecoders PLAN9 arm drops JPEGXLLoader.cpp + the jxl link; ImageDecoder.cpp AK_OS_PLAN9 guard skips the sniff-table entry | libjxl + highway cross-build |
| AVIF images | No libavif/dav1d decode | patch 0004 plan9 row (LIBAVIF find gated with the jxl block); LibImageDecoders PLAN9 arm drops AVIFLoader.cpp + the avif link; ImageDecoder.cpp AK_OS_PLAN9 guard skips the sniff-table entry | dav1d cross-build (needs asm off) |
| Font discovery | No fontconfig | patch 0004; FontDatabase::font_directories AK_OS_PLAN9 arm = /lib/ladybird/fonts + $home/lib/ladybird/fonts; TypefaceSkia AK_OS_PLAN9 arm = SkFontMgr_New_Custom_Directory("/lib/ladybird/fonts") | never (platform-correct replacement, like macOS CoreText path) |
| Wasm AOT (cranelift) | Bytecode interpreter only; no cranelift AOT, no compiled-fault recovery | ENABLE_CRANELIFT_JIT OFF (upstream toggle, via plan9-inject.cmake) selects upstream CraneliftStubs.cpp; WASM_COMPILED_FAULT_RECOVERY_SUPPORTED gated NOT PLAN9 | never on stock 9front (W^X, no exec pages); wxallow kernel + a plan9 arm in target-lexicon if ever |
| js REPL line editing | No libedit; raw stdin | patch 0004 (libedit skipped on plan9); js interactive uses fallback | real libedit port if interactive REPL wanted on-box |
| WebGL | No angle/vulkan; ENABLE_WEBGL stays undefined in Compositor/OpenGLContext.cpp (needs AK_OS_LINUX/MACOS/Vulkan), so context creation returns null | HAS_VULKAN stays off (upstream QUIET find fails naturally); compile/link surface satisfied by gl9: host/compat/GLES2/gl2ext_angle.h (robust-ANGLE inlines over core GLES3 + OES/EXT→core aliases), EGL_NO_PLATFORM_SPECIFIC_TYPES (plan9-inject), gl9egl.app.o on PkgConfig::angle | gl9/llvmpipe-backed GL if ever |
| Gamepad virtual joysticks | InternalGamepad's SDL_AttachVirtualJoystick always fails (no virtual devices) | host/compat/sdl3-virtual-joystick-stubs.c appended to SDL3::SDL3 (PLAN9 arm); fold into port/sdl3-shim next sysroot regen | with the sysroot sdl3-shim regeneration |
| Sandbox | RendererSandboxUnimplemented | upstream's own fallback for unknown platforms | 9front has no seccomp equivalent; permanent, matches other tier-2 platforms |
| ICU data | Archive file `/lib/icu/icudt78l.dat` + `ICU_DATA` env (launcher-set), not a linked-in .a | build-icu.sh archive packaging | n/a (upstream vcpkg uses static data; archive is ICU-sanctioned and saves ~30MB/process) |
| HTTP/3 | curl built without quiche/ngtcp2 | build-curl recipe | if ever needed |
| Proxy discovery | no libproxy | direct connections | env-var proxy support via curl if needed |
| Gamepad API | No gamepads on 9front; SDL3 is a shim ≡ real SDL3's dummy joystick backend (init OK, zero devices, no events) against the real 3.2.24 headers | port/sdl3-shim + build-sdl3-shim.sh | real SDL3 port if input devices ever matter |
| PSL IDNA runtime | libpsl is REAL (builtin DAFSA from the pinned 0.21.5 public suffix list) but built --disable-runtime: non-ASCII lookup input isn't IDNA-mapped (Ladybird's LibURL always feeds punycoded ASCII hosts, so no behavior gap) | build-libpsl.sh | link libidn2/libunistring if raw-unicode psl lookups ever appear |

## Parity measurements (filled per milestone)

- M1 js: test262 smoke subset vs same-commit host `Build/release/bin/js` — TBD
- M4 LibWeb: text/layout test subset vs host — TBD
- M6 WPT smoke — TBD

## M4 runtime blockers (headless screenshot) — diagnosed, fix pending

The full multi-process stack RUNS on cirno (RequestServer fetched a live HTTPS
page over TLS; verified 2026-07-15). Two runtime issues gate the headless PNG:

1. **SQLite "locking protocol" on Plan 9.** Both the main cookie/history DB and
   the RequestServer disk cache (`Database::Database::create`) fail with the
   Plan 9 errstr "locking protocol". cc9's SQLite VFS uses fcntl record locks
   (cc9 fcntl returns success for F_SETLK), but the DB open/journal path still
   hits a real kernel lock error. Bypass: `--disable-sql-database` (main DB) +
   the disk-cache failure is non-fatal (warnln). Durable fix: a cc9 SQLite VFS
   lock shim (Plan 9 exclusive-open or a no-op lock VFS), OR build SQLite with
   `SQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_LOCKING_MODE=1` / a unix-none VFS.

2. **Headless paint/present does not complete the PNG.** With
   `--disable-sql-database` the main process starts, spawns helpers, and stays
   alive (no crash), but no screenshot is written after minutes. Compositor now
   defaults `--force-cpu-painting` on plan9 (`SkiaBackendContext::
   initialize_gpu_backend` skipped; `the_main_thread_context()` returns null →
   no GPU). Root cause NOT yet isolated — the on-box picture is contaminated by
   `gl9win2`/`swgl9` processes that are almost certainly a CONCURRENT servo9
   session's (swgl9 is servo9's binary; gl9egl.c only writes frames to fd 1, it
   doesn't spawn a presenter), so they are likely a red herring, not ladybird's.
   Next: reproduce on a QUIET box (no concurrent servo9), trace the WebContent→
   Compositor paint IPC and the screenshot-write path (load_page_for_screenshot_
   and_exit + the CPU WrapPixels readback), and confirm whether the render
   completes or the paint/present IPC deadlocks. Candidate areas: the shm
   AnonymousBuffer bitmap handoff under real load (Phase A single-segment cap),
   the poll write-ring backpressure, or the screenshot timer firing before
   first paint (a known upstream race). Needs interactive debugging, not
   autonomous hammering.
