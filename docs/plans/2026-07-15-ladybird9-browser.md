# ladybird9 — Port Ladybird to 9front

## Context

The user asked: can we port Ladybird (https://github.com/LadybirdBrowser/ladybird) to Plan 9?
**Verdict: yes.** Ladybird is arguably the best-fit real browser engine for 9front — pure C++23
(cc9 is proven at LLVM scale), an interpreter-only JS engine (no JIT → stock W^X kernel is fine),
Skia with an automatic CPU-raster fallback (no GPU needed), and deliberately corralled platform
seams (documented in upstream `Documentation/Porting.md`). It joins servo9 as a **parallel**
browser track in a new `ladybird9/` dir; both share the cc9/ssl9/gl9 substrate.

Decisions locked with the user (2026-07-15):
- **Pin: current master** (mid-2026). Upstream has no releases and is maintainers-only since
  June 2026 → permanent pinned fork; pin is a commit hash. This puts the AsmInt assembly
  interpreter and the new Compositor process in scope.
- **Acceptance bar: interactive browsing** in a libdraw window on bare-metal cirno, released via
  pac9. Headless screenshot of a live HTTPS page is the intermediate milestone.
- **servo9 continues in parallel** (its swgl compositor wiring is untouched by this work).

## Why it's feasible (established facts)

Upstream (verified mid-2026):
- ~1.2M LoC C++23, clang ≥ 21, CMake ≥ 3.30, pinned Rust 1.96.1; ~100K LoC mandatory Rust in 10
  crates merged as static archives (`Meta/CMake/rust_crate.cmake`) — rust9's
  `x86_64-unknown-plan9` target covers this.
- Multi-process, **no single-process mode**: UI + per-tab WebContent + Compositor + RequestServer
  + ImageDecoder (+ WebWorker). IPC substrate is the critical path — nothing renders before it.
- `IPC::Transport` is a compile-time seam with 3 shipped backends; the Windows backend
  (`TransportSocketWindows`) already proves in-band handle serialization (no SCM_RIGHTS) works.
- All shared bitmaps funnel through one chokepoint: `Core::System::anon_create()` (3 backends) +
  `Core::AnonymousBuffer` (mmap MAP_SHARED over the fd).
- LibJS: portable C++ interpreter **removed June 2026**; only "AsmInt" — a build-time-generated
  x86_64/aarch64 SysV assembly interpreter (static .S, no runtime JIT → NX-safe).
- All code generators are Python; the only compiled host tool is AsmIntGen (Rust, runs on host).
  vcpkg is bypassable (find_package; NixOS/FreeBSD builds prove it). Android port proves
  cross-compilation.
- Skia is the only rasterizer but `LibGfx/PaintingSurface.cpp` auto-falls back to CPU raster
  (`SkSurfaces::WrapPixels`). RequestServer links libcurl + OpenSSL directly.

Local substrate (file refs in /Users/claw/Projects/agent9):
- cc9: full C++23 runtime; REAL fork/execve/waitpid+SIGCHLD, poll layer (`cc9/runtime/poll.c`),
  complete pthreads, TCP/UDP over /net, `socketpair()` = full-duplex Plan 9 pipe
  (`cc9/runtime/posix_llvm.c:786`). Working CMake cross-toolchain `cc9/native/toolchain.cmake`
  (built to cross LLVM itself) + cc(1)-shaped wrapper `servo9/host/cc9-cc`.
- Deps done: OpenSSL 3.0.17 (ssl9, TLS 1.3 proven live), zlib, sqlite; freetype+harfbuzz proven
  to build/run via servo9.
- Presentation: cc9 a.out can't link kencc libdraw → proven two-process split (gl9win /
  alacritty9 presenter, 13ms frames, input events flow back).

## The three walls (and why they fall)

1. **Cross-process shared memory** — cc9 mmap silently ignores MAP_SHARED (private malloc,
   `posix_llvm.c:133-156`); shm_open/memfd are stubs. Fix: named 9front segments
   (segment(3)/devsegment) behind a new cc9 shm layer + a 4th `anon_create` backend upstream.
2. **fd-passing IPC** — SCM_RIGHTS hard-refused (`net9.c:543-548`). Fix: `TransportPlan9`
   mirroring the Windows in-band-serialization backend; shm handles become names; genuine fd
   transfer (ConnectNewClient sockets) via /srv post/open.
3. **AsmInt** — generated SysV .S; cc9 is SysV but -mno-red-zone + emulated TLS need an audit.
   Fallback: resurrect the deleted C++ bytecode interpreter from git history as a carried patch.

Sizing hazards (not walls): cc9 ponytail caps (PFD_MAX=64 fds, 8KB poll rings, 4 forkers/32
zombies, POLLOUT always-ready) must be raised for a 6-process browser; the dep pile (Skia CPU-only,
ICU, curl+nghttp2, image codecs, woff2/brotli, simdutf/simdjson, fmt, libxml2) is a long but
proven-pattern harvest grind; /tmp RAMFS delivery cap (use `servo9/host/ship.py`).

## Parity discipline (user requirement: 1:1, no divergence)

The port must read like an upstream platform port, not a fork with opinions:

- **Surgical, additive patches only.** New platform backends in new files
  (`TransportPlan9.{h,cpp}`, an `anon_create` plan9 backend, `UI/Plan9/`), plus minimal
  `AK_OS_PLAN9` platform-detection arms — mirroring exactly how the Windows/Android/FreeBSD
  arms landed. Never rewrite shared logic; never "simplify" upstream code.
- **AsmInt is ported, not replaced.** The generated assembly interpreter is upstream's only
  interpreter on master; carrying the deleted C++ interpreter would be structural divergence.
  It remains a documented emergency fallback only, to be burned if the M0 spike fails.
- **No silent feature stubs.** Anything deferred (video/ffmpeg, WebGL/angle) is deferred via
  upstream's own feature gates / platform qualifiers (the same way vcpkg.json already
  platform-gates vulkan, dbus, fontconfig), and recorded in a parity ledger.
- **Parity is measured, not asserted.** Upstream's own suites run on 9front and are diffed
  against the same-commit host Lagom run: LibJS test262, LibWeb text/layout/ref tests, then a
  WPT smoke subset. Deltas tracked in `ladybird9/parity/` manifests — same idiom as python9's
  parity reports (99.4%) and the cc9 libc++ conformance sweep. The done-bar for each engine
  milestone is a parity percentage against host, not "it seems to work".
- **Pin + rebase discipline.** `fetch.sh` pins the exact master commit; every patch lives in
  `ladybird9/port/patches/` with an upstream-file-path-preserving layout so the fork can be
  rebased forward and the diff audited at a glance.

## Design

Three subsystems, designed and cross-verified (upstream master raw files + 9front kernel source
+ local cc9 sources, all spot-checked). Host toolchain already qualifies: clang 22.1.8 (need ≥21),
cmake 4.3.4 (need ≥3.30), ninja, rustup nightly-2026-06-30 (the rust9 toolchain).

### A. Shared memory — `cc9/runtime/shm9.c` over 9front `#g` (devsegment)

Verified 9front semantics: `#g` named global segments (create dir → write `va <addr> <len>` to ctl
once → segment lives at the **same VA in every process**; attach ≡ opening `data` ORDWR; persists
until all attachers exit; `remove()` blocks new attaches). Hard kernel cap: **100 named segments
system-wide** (`static Globalseg *globalseg[100]`). SEGDETACH=31 thunk is missing from cc9
(`n9_segattach` exists in `cc9/test/n9syscall.s`).

- New `cc9/runtime/shm9.c` + `include/sys/shm9.h`: `cc9_shm_create(size)` → fd of `#g/lb.<pid>.<seq>/data`;
  `cc9_shm_export(fd)` → `{name, offset, len}` wire triple (stateless — `fd2path` is the registry);
  `cc9_shm_import(name, offset, len)` → fd; `cc9_shm_sweep(prefix, grace)` GC.
- VA allocator: region base `0x0000300000000000` (48 TiB), per-creator 1 GiB slab keyed on
  `pid & 0xFFFF`, page-rounded bump + free-list. `// ponytail: 1 GiB live-VA per creator`.
- mmap routing patch in `posix_llvm.c`: `MAP_SHARED && fd → fd2path starts with "#g/"` →
  segattach (`SG_CEXEC`, refcounted per-process attach table); munmap consults the table first
  (segdetach), else `free()` as today. All other fds keep current behavior.
- Lifetime: nobody removes eagerly (A→B→C bitmap forwarding is real: ImageDecoder→WebContent→
  Compositor). Sweep removes `lb.*` segments attached nowhere (checked via `/proc/*/segment` VA
  match) after 60 s grace; driven by a 10 s timer in the chrome + a startup sweep (crash cleanup).
- **Phase B (built when the 100-segment cap bites, loudly):** same API + wire format; per-process
  pool segments `lbp.<pid>.<k>` (256 MiB, demand-paged), page-aligned carvings, header page with
  Plan 9 semaphore + refcounts + holder pids, `segfree()` returns pages. ~500 lines.
- Ladybird side: **explicit 4th branch** in `Core::System::anon_create` (`#elif AK_OS_PLAN9 →
  cc9_shm_create`), early-return (no ftruncate). Rejected: impersonating memfd_create — that's the
  documented silent-cfg-fallback trap.

### B. IPC — `Libraries/LibIPC/TransportPlan9.{h,cpp}` + /srv fd passing

Verified upstream: `IPC::Transport` is a pure include selector; attachments are a first-class
seam (`IPC::Attachment`, `IPC::HandleType{Generic,Socket}`, `IPC::TransportHandle`); the
fd-retention-until-ack machinery exists only for a macOS kernel bug (not protocol); Windows
backend serializes handles **in-band**. Verified 9front: `/srv` holds a reference to any posted
fd; an opener gets another reference to the **same channel** (shared offset) — exactly
SCM_RIGHTS open-file-description semantics.

- Clone TransportSocket's skeleton (IO thread, SendQueue, wakeup pipe, same public interface —
  zero call-site churn); byte stream = cc9 `socketpair()` (full-duplex Plan 9 pipe), raw
  read/write, O_NONBLOCK.
- Wire format (Windows-style): header `{payload_size, attachment_data_size, attachment_count}`;
  attachment records: `Segment{name, offset, len}` (when `fd2path` says `#g/`) or `Srv{name}`
  (sender posts fd to `/srv/lb.<pid>.<seq>` synchronously in `post_message`; receiver opens then
  removes the entry). No FdAck, no retained-fd queue, no peer pid needed. Multi-hop re-share
  works by construction (re-classification on re-encode).
- Spawn path (verified upstream): `Core::Process::spawn` = posix_spawn + file_actions; child
  socketpair end passed via env `SOCKET_TAKEOVER=<name>:<fd>`. So: **implement real posix_spawn
  in cc9** (fork + apply file_actions + execve; `_exit(127)` on exec failure) — fixes every
  future port at once, zero Ladybird call-site patches. One upstream patch:
  `SystemServerTakeover.cpp`'s `is_socket(fd)` check must accept pipe fds on plan9.
- cc9 caps/semantics (est. ~45 polled fds in the UI process at 3 tabs): `PFD_MAX 64→256`
  (+ execve's hardcoded 3..64 CLOEXEC scan raised to match), rings 8 KB→64 KB lazily allocated,
  reaper `rtab 4→8` / `ztab 32→128`. **One real semantic fix: write-side O_NONBLOCK rings** —
  today POLLOUT lies ("always ready") and write() blocks, which deadlocks two IO threads
  streaming large payloads at each other. Mirror the proven reader-ring design; partial-write
  returns are exactly what upstream SendQueue's `start_offset` logic consumes.

### C. Engine runtime — AsmInt, LibGC, seams, fonts

- **AsmInt: expected to work as-is** (audited upstream `AsmIntGen/src/codegen_x86_64.rs`): no TLS
  (%fs) access — VM ptr is a SysV arg; no red-zone use (explicit `sub rsp, 8` prologue); pinned
  state in callee-saved regs only; zero raw syscalls; ELF `.data.rel.ro` dispatch table (already
  placed by `cc9/test/plan9.ld:31`); CMake arch gate passes via the toolchain's
  `CMAKE_SYSTEM_NAME=Linux` trick. One real seam: `gen_asm_offsets` runs at build time as a
  host binary — add an **offsets gate** (rewrite its output into `static_assert(offsetof...)`
  TU compiled with cc9-c++; compile success = ABI-proven). Fallback (parity-breaking, emergency
  only, decided at end of M0): revert upstream PR #10099 (merge `a29e1f5`) at our pin.
- **LibGC**: upstream BlockAllocator munmaps head/tail slack of over-allocated chunks — on cc9
  that's `free()` of an interior pointer → heap corruption. Fix = the servo9 SpiderMonkey
  "malloc pages" precedent: `AK_OS_PLAN9` branch using cc9 `aligned_alloc` (16 KiB block
  alignment is all upstream needs) + hard no-op decommit. `AK::StackInfo` plan9 branch over
  `cc9_stack_bounds()` (`cc9/runtime/pthread.c`).
- **EventLoop: no work needed** — verified `EventLoopImplementationUnix.cpp` is poll(2)-based
  (no epoll/kqueue/timerfd/eventfd anywhere); maps 1:1 onto cc9's poll layer.
- **Platform**: `AK_OS_PLAN9` in `AK/Platform.h` keyed on `__plan9__` (cc9-cc already defines it,
  `servo9/host/cc9-cc:33`). Sandbox → `RendererSandboxUnimplemented.cpp` (zero code, one CMake
  branch). sysconf NPROCESSORS=1 → pools of 1, correct-first. WebDriver skipped; WebWorker built.
- **Fonts** (no fontconfig): `FontDatabase.cpp` plan9 dirs `/lib/ladybird/fonts` +
  `$home/lib/ladybird/fonts`; `TypefaceSkia.cpp` → `SkFontMgr_New_Custom_Directory` +
  FreeType scanner; pac9 ships DejaVu + Noto TTFs (stock 9front has no TTF at all).

### D. Build system + dependency pile

- **Toolchain**: `ladybird9/host/toolchain.cmake` = copy of proven `cc9/native/toolchain.cmake`
  (SYSTEM_NAME=Linux trick, STATIC_LIBRARY try-compile, cc9-link) + deps sysroot
  (`_out/deps/{include,lib,...}` via CMAKE_PREFIX_PATH/PKG_CONFIG_LIBDIR) +
  `CMAKE_PROJECT_INCLUDE=host/plan9-inject.cmake` for feature toggles without patching.
  Compilers = `servo9/host/cc9-cc`/`cc9-c++` verbatim. vcpkg bypass is automatic (root
  CMakeLists only engages vcpkg for its own toolchain file); drive cmake directly, not presets.
  elf2aout as post-build pass; `--gc-sections`.
- **Deps** (sysroot static libs; ssl9/gl9 harvest or direct-CMake per dep):
  - Done/proven: openssl (ssl9), zlib, sqlite (bump 3.46→3.52 amalgamation), freetype, harfbuzz.
  - New builds: **ICU 78.3** (autotools cross w/ host build; **archive data packaging** —
    `/lib/icu/icudt78l.dat` + locale filter, NOT a 30 MB .a in every process; small
    `u_setDataDirectory` patch), **Skia m148** (GN direct with cc9 wrappers; CPU-raster only:
    ganesh/graphite/GL/vulkan off, freetype fontmgr custom-dir, all codecs off — LibGfx decodes;
    found via our hand-written `skia.pc` — upstream has a pkg-config fallback), **curl 8.20**
    (CMake; openssl+nghttp2+brotli+zstd+websockets; HTTP/3 off, unix-sockets off), libpng(+apng
    patch), libjpeg-turbo (SIMD off first), libwebp, woff2+brotli, zstd, nghttp2, simdutf,
    simdjson (cpuid dispatch — fine), fmt, libxml2, libtommath, wuffs/fast-float (header-only).
  - Shim: **mimalloc** → honest `mi_*`→malloc static shim (cc9 mmap is malloc-backed; real
    mimalloc's reserve/commit assumptions are false).
  - Deferred via upstream-style gates + parity ledger: ffmpeg (no <video> initially; LibMedia
    stub), angle/vulkan (no WebGL), libavif/libjxl/tiff (fewer image formats), libpsl, libproxy,
    libedit (REPL line editing), cpptrace, fontconfig (replaced, see C). SDL3 consumer unknown —
    audit; dummy-build or falls away with GUI targets off.
- **Rust (10 crates)**: upstream `rust_crate.cmake` already parameterizes `RUST_TARGET_TRIPLE` —
  set `x86_64-unknown-plan9` + `RUST_TARGET_PATH=rust9/targets` + build-std env
  (`CARGO_UNSTABLE_BUILD_STD=std,panic_unwind`), `RUSTUP_TOOLCHAIN=nightly` (rust9's
  nightly-2026-06-30 overrides upstream's 1.96.1 pin; fallback RUSTC_BOOTSTRAP=1). Reuse servo9's
  `[patch.crates-io]` plan9 forks (getrandom, memmap2, inventory…). Per-crate silent-cfg-fallback
  audit + on-box gate (the servo9 lesson). AsmIntGen builds for the host triple.

## Milestones

| M | Deliverable | Gate (on 9front unless noted) | Size |
|---|---|---|---|
| **M0** | De-risk spikes | (a) host Lagom/full build of the pin on this Mac (reference behavior + parity baseline); (b) AsmInt: generate, assemble via cc9-c++, `%fs`/red-zone objdump audit, offsets static_assert gate (host), link+run smoke on box; (c) `segshm_gate.c` — two exec'd processes truly share a `#g` segment (+ SG_CEXEC, /proc format, cap probe); (d) `srvfd_gate.c` — fd passing via /srv; (e) poll caps + write-rings + `spawn_gate.c` (real posix_spawn) | M |
| **M1** | `js` REPL on box | test262 smoke subset **diffed against same-commit host run** + one Intl call (proves ICU archive data). Pulls AsmInt + ICU + 4 Rust crates through rust_crate.cmake — the highest-de-risking milestone | L |
| **M2** | Dep pile built | per-dep on-box gates: Skia raster→PNG, curl HTTPS GET over ssl9 (live TLS), harfbuzz shaping, simdutf/simdjson dispatch self-tests, codec goldens | L |
| **M3** | IPC substrate live | `transport_echo_gate` (incl. simultaneous 8 MB bidirectional deadlock probe); then two Ladybird processes exchange real IPC + a shared `Gfx::Bitmap` round-trips | M |
| **M4** | **Headless screenshot** | upstream `HeadlessWebView` **unmodified**, full 5-helper process stack; PNG of a live HTTPS page rendered on 9front; LibWeb text/layout test subset vs host parity ledger | XL |
| **M5** | **Interactive chrome** | new `UI/Plan9/` (ViewImplementation subclass) + extended `gl9win2` presenter (new `GL9B` BGRA frame op; input records back); browse ladybird.org on bare-metal cirno — click, type, scroll | L |
| **M6** | Release | `pac9 install ladybird9` on a clean image (fonts, icudt, pdfjs, certs packaged); WPT smoke subset in parity ledger | S |

## Work items (dependency order)

**cc9 substrate** (each lands with its gate test, before any Ladybird code):
1. `cc9/test/n9syscall.s`: add `n9_segdetach` (31), `n9_segfree` (32).
2. `cc9/runtime/shm9.c` + header; mmap/munmap MAP_SHARED routing in `posix_llvm.c` → `segshm_gate`.
3. `srvfd_gate` (no new runtime code expected — /srv semantics probe).
4. `cc9/runtime/poll.c`: PFD_MAX 256, lazy 64 KB rings, **write-side rings** (POLLOUT honesty);
   `posix_llvm.c`: reaper caps 8/128, execve CLOEXEC-scan bump.
5. `posix_llvm.c`: real posix_spawn + file_actions → `spawn_gate`.
6. `transport_echo_gate.c` (standalone C mirror of the wire protocol incl. deadlock probe).

**ladybird9 port** (dir per repo convention: `fetch.sh` pin → gitignored `vendor/`; committed
`host/` scripts, `port/patches/` single-purpose diffs, `test/` gates, `parity/` ledger):
7. `fetch.sh` (LADYBIRD_REV, SKIA_REV m148, ICU/dep pins); M0a host reference build.
8. AsmInt spike A1–A6 (generator → assemble → audit → offsets gate → on-box link smoke).
9. `AK/Platform.h` AK_OS_PLAN9; `AK/StackInfo.cpp` + `LibGC/BlockAllocator.cpp` plan9 branches.
10. `host/toolchain.cmake`, `plan9-inject.cmake`, `build-deps.py` (ssl9-recipe style), sysroot.
11. Tier-1 deps (M1 set): ICU cross + archive data, sqlite bump, fmt, simdjson/simdutf, mimalloc
    shim; patches: libedit-optional, fontconfig-off, ffmpeg-stub, cpptrace-off.
12. Rust plumbing (target json, build-std env, servo9 crate forks, per-crate audit) → **M1 js gate**.
13. Tier-2 deps: brotli/zstd/nghttp2/curl, libpng/jpeg-turbo/webp/wuffs, woff2, libxml2,
    libtommath, freetype/harfbuzz rebuild against sysroot → per-dep gates.
14. Skia GN cross + `skia.pc` + raster gate → **M2 done**.
15. `Libraries/LibIPC/TransportPlan9.{h,cpp}` + `Transport.h` selector; `System.cpp` anon_create
    branch; `SystemServerTakeover.cpp` pipe acceptance → **M3 gates**.
16. Fonts (`FontDatabase.cpp`, `TypefaceSkia.cpp`, DejaVu/Noto payload); GC sweep timer +
    startup sweep in chrome; SDL3 audit resolution.
17. Full link + elf2aout + ship.py (disk, NOT /tmp — RAMFS wedge on record) → **M4 headless gate**.
18. `ladybird9/win/` gl9win2 fork + `GL9B` op; `UI/Plan9/{main,ApplicationPlan9,Plan9WebView}` →
    **M5 on cirno**.
19. pac9 packaging + release; parity ledger finalized → **M6**.
20. (When the 100-segment cap bites) Phase B pool allocator.

## Critical files

Modify (cc9): `cc9/runtime/posix_llvm.c` (mmap routing, posix_spawn, caps), `cc9/runtime/poll.c`
(caps + write rings), `cc9/test/n9syscall.s` (segdetach/segfree).
Create (cc9): `cc9/runtime/shm9.c`, `cc9/runtime/include/sys/shm9.h`, gates in `cc9/test/`.
Patch (pinned Ladybird tree, via `ladybird9/port/patches/`): `AK/Platform.h`, `AK/StackInfo.cpp`,
`Libraries/LibGC/BlockAllocator.cpp`, `Libraries/LibCore/System.cpp`,
`Libraries/LibCore/SystemServerTakeover.cpp`, `Libraries/LibIPC/Transport.h`,
`Libraries/LibGfx/Font/{FontDatabase,TypefaceSkia}.cpp`, LibUnicode ICU-data-dir, LibMedia stub,
Services CMake sandbox branch.
Create (Ladybird tree additions): `Libraries/LibIPC/TransportPlan9.{h,cpp}`, `UI/Plan9/*`.
Reuse: `servo9/host/cc9-cc`, `cc9/native/toolchain.cmake` (copy), `ssl9/build.py` (recipe
template), `ssl9/_out/*.a`, `rust9/targets/x86_64-unknown-plan9.json` +
`rust9/stdhello/.cargo/config.toml` (build-std flags), servo9 crate forks,
`alacritty9/win/gl9win2.c` + `alacritty9/PROTOCOL.md` (presenter), `servo9/host/ship.py`,
`servo9/_work/mozjs_sys/.../gc/Memory.cpp` (malloc-pages precedent).

## Risks

- **100 global segments system-wide** (kernel cap, verified) — top structural risk; Phase A fits
  the screenshot milestone, Phase B pool is the designed exit; failure is loud, not corrupting.
- **Binary size**: static WebContent w/ Skia est. 80–150 MB; ICU data externalized (biggest win),
  gc-sections, ship to disk; if elf2aout/loader chokes on huge text, surface early with a fat
  dummy link (M2).
- **Write-ring correctness** (ordering/partials) — dedicated deadlock probe in transport gate.
- **Rust silent-cfg fallbacks** — per-crate audit + on-box gates; still budget one "renders
  nothing, no error" incident (servo9's memmap2 story).
- **AsmInt offsets skew** (host-generated offsets vs cc9 layout) — caught at compile time by the
  static_assert gate; last-resort fallback = bounded revert of PR #10099 at pin.
- **Skia ganesh-off compile breaks in LibGfx** — fallback: ganesh on with all backends off.
- **OpenSSL 3.0 (ours) vs 3.5 (upstream's pin)** — EVP-level usage, low risk; ssl9 bump is a
  known-cost recipe rerun if a symbol is missing.
- **Thread-per-fd proc counts** (~100–300 Plan 9 procs at 3 tabs) — within 9front defaults;
  documented, revisit only if real.
- **Upstream drift** — permanent pinned fork; patches kept single-purpose/rebasable; parity
  ledger measures the gap.

## Verification

Every layer has an executable gate before the next layer builds on it (repo convention):
1. cc9 gates on VM + cirno via listen1: `segshm_gate`, `srvfd_gate`, `spawn_gate`,
   `transport_echo_gate` (incl. 8 MB bidirectional deadlock probe, 10k-message soak).
2. AsmInt: objdump audit + offsets static_assert (host), link smoke (box), then M1 `js` running
   arithmetic/closures/exceptions + test262 smoke **diffed against the same-commit host Lagom
   run** — parity measured, not asserted.
3. Per-dep on-box gates (M2), incl. live TLS fetch through curl+ssl9.
4. M4: headless PNG of a live HTTPS page on 9front; LibWeb text/layout subset vs host ledger.
5. M5: interactive session on bare-metal cirno (QMP screendump / photos for the VM leg);
   13 ms-class frame pacing sanity vs alacritty9 numbers.
6. M6: `pac9 install ladybird9` on a clean image; WPT smoke subset recorded in `parity/`.

## Open items (tracked, non-blocking)

- SDL3's actual consumer on master (audio vs chrome) → audit at item 16.
- `ENABLE_GUI_TARGETS=OFF` + headless interaction detail → resolved at M4 wiring.
- mimalloc `mi_*` surface Ladybird actually calls → enumerate when building the shim.
- Whether angle/ffmpeg/avif/jxl absence is toggle-clean or needs the budgeted patches.
- Compositor: one-per-browser vs per-tab (affects only caps arithmetic).
- `LocalSocket::adopt_fd` socket-ness VERIFY (gate will catch; patch site identified).
