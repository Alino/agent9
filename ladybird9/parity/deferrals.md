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

- M1 js (2026-07-15): m1-smoke.js battery (AsmInt, exceptions, BigInt, promises,
  private fields, Unicode final-sigma, Intl de-DE/ICU) — BYTE-IDENTICAL to the
  same-commit host `Build/release/bin/js` on bare-metal cirno.
- M4/M5 LibWeb text dumps (2026-07-16): `--headless=text` DOM text of two pages
  (test.html: h1/pre/styled-p; tallpage.html: 79 paragraphs, 4673 bytes) —
  BYTE-IDENTICAL (same md5) between the pac9-installed browser on the 9front VM
  and the same-commit host macOS build. Method: host
  `Ladybird.app/Contents/MacOS/Ladybird --headless=text <file>` vs on-box
  `ladybird '--headless=text' --temporary-profile --disable-sql-database
  file://...`, byte compare.
- M6 WPT smoke — TBD (needs the WPT runner harness; post-release)

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

## M4 DONE — Ladybird renders a live page on bare-metal 9front (2026-07-16)

`screenshots/m4-headless-test-page.png` is a real 1000x700 render by the full
multi-process stack (UI + WebContent + Compositor + RequestServer +
ImageDecoder) on cirno: SerenitySans h1, DejaVu Sans Mono <pre>, CSS colors and
layout all correct. Reproduce:
  ladybird --headless --temporary-profile --disable-sql-database \
    --screenshot-path shot.png --window-width 1000 --window-height 700 \
    file:///usr/glenda/lb9/lb/test.html
The four render-stage blockers below were all resolved (kept for the record):

## M4 render-stage blockers — ALL RESOLVED

The full multi-process stack now spawns, connects, and reaches "Taking
screenshot after N seconds" on cirno. Four bugs were fixed to get the PNG:

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

   **CONFIRMED root cause (CC9_SHM_TRACE run, 2026-07-16): the Plan 9
   per-process segment limit (NSEG).** The trace shows the receiver attaching
   segments successfully — its own two, then imported .0/.1/.2 — then the SIXTH
   segattach fails:
     `SHM9 attach ... name=shm.1077858.3 va=0x4c988367a000 len=0x495000
      res=-1 err=virtual memory allocation failed`  (Plan 9 Enovmem)
   The VAs are all distinct and NON-overlapping (adjacent segments like .1→.2
   both succeed), so it is NOT a VA collision and NOT the earlier VA-slab
   hypothesis. It is a hard per-process cap on the number of attached segments
   (Plan 9 `Proc.seg[NSEG]`, ~10-12 total incl. text/data/bss/stack). A page
   render allocates one #g segment PER backing-store bitmap (the Compositor made
   6: sizes 0x2ac000 and 0x495000 for a 1000x700 shot), and each of WebContent +
   Compositor + UI must attach them all → the ~6th attach in a process fails →
   null bitmap → RefPtr.h:275 crash + the UI decode ENOENT above.

   **Fix = the Phase B pool allocator** (already specified in
   docs/plans/2026-07-15-ladybird9-browser.md): per-process pool segments
   `lbp.<pid>.<k>` (e.g. 256 MiB, demand-paged), sub-allocate many bitmaps out
   of ONE segattach, page-aligned carvings, header page w/ refcounts. The
   {name,offset,len} wire format ALREADY carries the offset field (Phase A
   always sends 0) — Phase B just makes offset meaningful, so TransportPlan9 and
   Core::System::anon_create need no change; it is contained to shm9.c +
   posix_llvm.c mmap routing. This collapses N bitmaps → few segments/proc,
   under NSEG. (A 1-line kernel NSEG bump would also work but needs a patched
   kernel + reboot on cirno — the userspace pool is the stock-kernel fix.)
   Diagnostic tooling in place: CC9_SHM_TRACE env in shm9.c logs every
   create/import/attach {name, va, len, result, errstr} to stderr (off by
   default); errstr read non-destructively via the n9_errstr swap (poll.c:129).

   NOTE — headless needs `--temporary-profile`: the per-profile SQLite store is
   opened with a Plan 9 exclusive lock; a prior run's process holding it makes
   every subsequent run die at startup with "Runtime error: locking protocol".
   A temporary/fresh profile sidesteps the contended lock. (Also fixed this
   session: deploy9.sh extracted the Lagom resource tar with `tar xf -`, which
   Plan 9 tar rejects — must be `/fd/0` — so resources silently never landed.)

   **RESOLVED (cc9 702e3b9): Phase B pool allocator built** exactly as specified
   (pool segment `#g/shmp.<pid>`, per-buffer offsets, one segattach/pool). Trace
   confirmed the receiver imports a sender's pool 4x but attaches it once — the
   Enovmem is gone and DidAllocateBackingStores decodes.

5. **No sans-serif/serif generic → null default font → StyleComputer crash.
   [FIXED — patch 0007]** After the pool fix, WebContent crashed at RefPtr.h:275
   (`as_nonnull_ptr`). Stack-walked via the cc9 fault-file dump to
   `StyleComputer::StyleComputer` → `FontPlugin::default_font(16)->pixel_metrics()`.
   default_font resolves GenericFont::UiSansSerif; compute_generic_font_name
   returned an unloaded family ("Arial") because SerenitySans is in NO fallback
   list and Plan 9 has no fontconfig to resolve "sans-serif" via
   TypefaceSkia::resolve_generic_family. Fix: append the bundled SerenitySans to
   the sans-serif and serif fallback lists on plan9 (monospace already had DejaVu).

6. **/srv fd-pass: read-only channel re-opened O_RDWR → EACCES.
   [FIXED — patch 0005]** Non-shm fd attachments travel via /srv (cc9_srv_post).
   The receiver's `open("/srv/<name>", O_RDWR)` failed with Plan 9 "permission
   denied" for fds whose posted channel is read-only (e.g. a pipe read end) —
   Plan 9 rejects re-opening a /srv channel with a WIDER mode than it was posted
   with. Fix: open the widest mode that works — O_RDWR (bidirectional sockets,
   the common case), then O_RDONLY, then O_WRONLY. RDWR-first never downgrades a
   real socket. (The srvfd_gate passed because it posts a bidirectional pipe.)

## M5.3 on-screen gl9win2 window — status (2026-07-16)

Interactive RENDERING is proven (M5.2): the Plan9WebView streams the correctly
-rendered page as GL9B frames, captured off fd 1 and decoded pixel-identical to
the M4/M6 renders — validated by running `ladybird` with manual fd redirects
(`{sleep N; cat quitrec} | ladybird ... >frames.bin`).

NOT yet validated: the full gl9win2-spawns-browser integration displayed on a
real rio window. On the dev VM, `mount /srv/rio.glenda.N /n/w 'new -r ...';
bind -b /n/w /dev; gl9win2 ladybird <url>` opens the window (visible via HMP
screendump, focused) and gl9win2 runs with no error, but no browser frame blits
to it within ~3 min (the window keeps rio's default terminal content). The
gl9win2→browser pipe path (fd0 events / fd1 frames set up by gl9win2, vs the
manual redirects used for the frame-capture proof) is the untested seam; the VM
is also ~5x slower than cirno. Next: trace whether the browser streams a frame
to gl9win2's fd 1 under the presenter (add a byte counter in gl9win2's
framereader), and whether gl9win2 blits (kbddebug). Likely a small fd/pipe or
first-frame-timing issue, not a rendering defect.
