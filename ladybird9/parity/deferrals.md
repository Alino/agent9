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

3. **No monospace font → WebContent VERIFY crash. [FIXED — commit fdbcbd2]**
   FontPlugin.cpp:56 `VERIFY(m_default_fixed_width_font)` failed: bundled fonts
   (SerenitySans + NotoEmoji) have NO monospace and Plan 9 has no fontconfig, so
   generic_font_name(UiMonospace)'s fallbacks {DejaVu Sans Mono, ...} all missed.
   Fixed by bundling DejaVuSansMono.ttf (internal name matches the fallback list)
   into the res font set via patch 0004 + port/assets/fonts/ + fetch.sh copy.
   Also fixed a latent deploy bug: Plan 9 tar reads stdin as /fd/0 not '-', so
   the Lagom resource tarball was shipped but never extracted on-box.

4. **Backing-store shm segment fails to attach on the receiver — cc9 #g
   segattach bug, NOT an IPC/framing bug.** [Root-caused this session from a
   full on-box run; the earlier "framing drift" and "segment cap" theories were
   BOTH wrong — corrected here.] Reproduce with:
   `ladybird --headless --temporary-profile --disable-sql-database
   --screenshot-path shot.png file:///…/test.html` (the `--temporary-profile`
   is essential — see the profile-lock note below).

   What actually happens: the Compositor sends CompositorControlClient::
   DidAllocateBackingStores (Vector<Gfx::SharedImage> = shm AnonymousBuffers).
   The message parses FINE — the magic matches, decode proceeds. It fails inside
   the message body: `AnonymousBufferImpl::create` does
   `mmap(MAP_SHARED, fd)` → cc9 routes that to `cc9_shm_try_map` →
   `n9_segattach(0, name, 0, 0)` which FAILS, so mmap returns MAP_FAILED with
   errno mapped to ENOENT → the decode returns "No such file or directory". The
   "Endpoint magic number mismatch" line is a RED HERRING: try_parse_message
   tries local then peer endpoint; local (the right one) failed with the mmap
   ENOENT, peer failed on magic — the dbgln prints the peer error last.

   Key facts that pin it to segattach (not the segment being gone):
   - `deserialize_attachment` already did `cc9_shm_import` = `open(#g/name/data)`
     and its `VERIFY(fd>=0)` did NOT fire → the segment DIR EXISTS and is
     openable. So it is NOT swept/removed and NOT a lifetime/close-races-import
     problem. `open` succeeds; only the subsequent `segattach` fails.
   - Deterministic: all 3 Compositor auto-restarts fail identically.
   - The single-segment `segshm_gate` (create→mmap→fork+exec child→import→mmap)
     PASSES with the identical code path, so the primitive works — the failure
     is specific to the real multi-process/multi-segment flow.
   - Same root cause crashes WebContent at AK/RefPtr.h:275 (VERIFY(m_ptr)): its
     backing bitmap is null because ITS mmap of the backing store segattach-
     failed too. One bug, two symptoms.

   Leading hypothesis: **VA collision.** shm9.c pins every segment at a fixed VA
   `SHM9_BASE + ((getpid()&0xFFFF)<<30) + bump` and all importers must map at
   that same VA (a #g global segment can't relocate). `getpid()&0xFFFF` gives
   only 64K slabs; with pid recycling across many procs, two live creators can
   share a slab and mint overlapping VAs, so an importer's 2nd segattach lands
   on an occupied VA and fails. Fix direction: a collision-free GLOBAL VA
   allocator (a well-known #g counter segment handing out non-overlapping
   ranges), replacing the pid-keyed slab. Confirm first with an env-gated trace
   in cc9_shm_try_map logging {name, va, seglen, segattach result, errstr}
   (read errstr non-destructively via the n9_errstr swap trick, poll.c:129) —
   relinking JUST the `ladybird` main against the traced libcc9cxx.a is enough
   to capture the import-side failure + the colliding VAs.

   NOTE — headless needs `--temporary-profile`: the per-profile SQLite store is
   opened with a Plan 9 exclusive lock; a prior run's process holding it makes
   every subsequent run die at startup with "Runtime error: locking protocol".
   A temporary/fresh profile sidesteps the contended lock. (Also fixed this
   session: deploy9.sh extracted the Lagom resource tar with `tar xf -`, which
   Plan 9 tar rejects — must be `/fd/0` — so resources silently never landed.)
