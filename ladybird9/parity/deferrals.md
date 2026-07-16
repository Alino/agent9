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
| Default fonts (RESOLVED 2026-07-16) | Bundle DejaVu Sans/Serif/Mono as the sans/serif/mono defaults instead of the hand-drawn SerenitySans house font (which read as "comic sans" with sparse Unicode coverage). FontPlugin PLAN9 arm prepends "DejaVu Sans/Serif/Sans Mono" to the generic fallback lists; TTFs bundled via ResourceFiles.cmake PLAN9 arm + port/assets/fonts. | patch 0007 (FontPlugin) + 0004 (ResourceFiles) + port assets | done — normal fonts, broad glyph coverage (Latin/Cyrillic/Greek/symbols), validated |
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
| Persistent profile (PARTIAL 2026-07-16) | The SQLite "locking protocol" open failure is FIXED: LibDatabase (Database.cpp) opens with the built-in "unix-none" VFS on PLAN9 (no fcntl locks; safe for a single-writer browser) + a rollback journal (WAL needs shm-locking unix-none can't do). A FRESH persistent profile opens/renders/persists. BUT reopening it on the next launch wedges (helpers stuck in Semacqui — a teardown-leftover appears to hold the store), so the launcher keeps `--temporary-profile` for now. `--disable-sql-database` IS dropped (SQL works in-session). | Database.cpp + RequestServer/main.cpp PLAN9 arms (patches 0003/0007) | reopen-hang + browser-exit teardown hardened |
| HTTP disk cache | Non-fatal warn "Unable to create disk cache": the cache-index DB open now reaches SQLite (VFS fixed) but still fails ("unable to open database file" / earlier a `statvfs` ENOSYS from a cc9 runtime change). Non-essential; browser runs. | Services/RequestServer/main.cpp honors --http-disk-cache-mode again (force-off removed) | cache-dir open + statvfs shim |
| Compositor painting path | CPU painting forced on (Skia WrapPixels raster); GPU/Vulkan compositing path not taken | Services/Compositor/main.cpp AK_OS_PLAN9 arm defaults force_cpu_painting=true (no GPU context on 9front); `--force-cpu-painting` still parsed | gl9/llvmpipe-backed GL context if ever (same blocker as WebGL) |
| GC address-space reservation | `Core::System::reserve_address_space` returns a plain allocation, not a PROT_NONE guard region: over-commit isn't lazily backed and out-of-reservation access won't fault-trap | LibGC/BlockAllocator.cpp AK_OS_PLAN9 arm (cc9 mmap is malloc-backed; no PROT_NONE) — behavior is correct, only the guard semantics are absent | cc9 gains real anonymous PROT_NONE reservations (segattach-based) |

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
- Code-review rebuild (2026-07-16): after applying 23 review findings, all 6
  binaries recompiled + relinked against the fixed cc9 runtime (new shm9.o +
  fs.o close-hook). The shm runtime changes (stale-entry close hook, lock-safe
  offset table, fork-without-exec pool ownership, multi-pool chaining) validated
  on the VM via `segshm_gate` PASS 10/10 (true cross-process sharing across
  fork+exec, refcounted map/unmap, detach→re-import, 256-buffer pool probe with
  no NSEG hit). Text-dump parity is preserved by construction — none of the 23
  fixes touch DOM parsing, layout, style, or text rendering (they are shm
  runtime, UI input handling, RequestServer cache warn, an include reorder, and
  a PLAN9 font-bundle gate), so the recorded byte-identical `--headless=text`
  results stand unchanged.

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

## M5.3 on-screen gl9win2 window — VALIDATED (2026-07-16, commit 4786322)

The pac9-installed browser runs interactively in a real rio window and responds
to input — the full presenter chain live end to end (Ladybird → GL9B frames →
gl9win2 → libdraw window; input records gl9win2 → fd0 → Plan9WebView →
WebContent → repaint). Proven on the dev VM with in-guest captures:
- `screenshots/m5-window-initial.png`: the browser window showing the scroll-test
  page (LINE 001–029, scrollbar at top).
- `screenshots/m5-window-scrolled.png`: after injected wheel/PageDown input, the
  page shows LINE 035–070 with the scrollbar thumb moved. The browser also
  resized to the window interior at startup (the resize record drove it).

Two things had masked it earlier (both fixed): the launcher needs
`--temporary-profile` (a prior run's profile lock silently wedged startup), and
QMP screendumps capture the window BEHIND the listener terminals — the dosbox9
shot.rc pattern (re-enter the window namespace by winid, raise via wctl, read
/dev/screen — or /dev/window for the overdraw-immune backing image) is the
correct capture method. host/vm/{lb9win.rc,lb9shot.rc} ship that dev loop.

Frame-capture proof (M5.2) still holds as the pixel-parity check: the GL9B stream
off fd 1 decodes pixel-identical to the M4/M6 renders.
