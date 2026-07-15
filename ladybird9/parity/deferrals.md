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

2. **[ROOT-CAUSED + FIXED] TransportPlan9 wrongly triggered the peer-pid
   handshake.** HelperProcess guards the `init_transport` sync handshake with
   `if constexpr (requires { transport().set_peer_pid(0); })`; the Unix
   TransportSocket omits set_peer_pid so the handshake is skipped, but
   TransportPlan9 kept it (no-op), making the guard fire. The helper-side
   init_transport handler is `#ifdef AK_OS_WINDOWS` (VERIFY_NOT_REACHED
   elsewhere), so the parent's send_sync deadlocked after spawning the first
   helper — no ImageDecoder/Compositor/WebContent ever came up. Fix: remove
   set_peer_pid from TransportPlan9 (patch 0005) so Plan 9 matches Unix (no
   peer-pid needed — shm/fd handles are addressed by global name). The
   gl9win2/swgl9 procs I first suspected were a concurrent servo9 session's,
   not ladybird's.

## M4 render-stage blockers (after the IPC deadlock was fixed, 2026-07-16)

The full multi-process stack now spawns, connects, and reaches "Taking
screenshot after 8 seconds" on cirno. Two render-stage bugs remain before a PNG:

3. **No monospace font → WebContent VERIFY crash.** FontPlugin.cpp:56
   `VERIFY(m_default_fixed_width_font)` fails: the bundled fonts
   (Base/res/fonts: SerenitySans + NotoEmoji) have NO monospace, and
   generic_font_name(UiMonospace) tries {DejaVu Sans Mono, Liberation Mono,
   Noto Sans Mono, ...} — none present. Linux gets these from fontconfig.
   Fix: bundle a monospace TTF whose internal name matches a fallback (ship
   DejaVu Sans Mono / Liberation Mono into Base/res/fonts + /lib/ladybird/fonts).

4. **Compositor shm backing-store IPC parse failure — likely the Phase A shm
   segment cap.** DidAllocateBackingStores (Compositor->UI, carrying
   Vector<Gfx::SharedImage> = shm AnonymousBuffers) fails to parse ("endpoint
   magic number mismatch") and the Compositor disconnects/restarts. The
   TransportPlan9 attachment framing is symmetric (verified), so the leading
   suspect is the documented Phase A limitation: one #g named segment PER
   bitmap against a ~60-64 segment SYSTEM-WIDE kernel cap. A page render
   allocates many backing stores; exhausting the cap yields invalid
   AnonymousBuffers that corrupt the message. Fix: the planned **Phase B shm
   pool allocator** (per-process pool segments carved into many buffers, same
   {name,offset,len} wire format — the offset field is already there for
   exactly this). ~500 lines, specified in docs/plans/2026-07-15-ladybird9-
   browser.md. Confirm by tracing shm create failures during the render.
