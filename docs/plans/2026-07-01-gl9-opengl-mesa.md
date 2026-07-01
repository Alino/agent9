# gl9 — real OpenGL on 9front via Mesa softpipe

> Status: **plan / not started.** Captured 2026-07-01. Destined for
> `docs/plans/2026-07-01-gl9-opengl-mesa.md` on execution (repo plan-doc home).
> North star: this is the graphics substrate for eventually running **Alacritty**
> on 9front.

## Context — why this exists

9front has **no OpenGL and no 3D**. Native graphics is 2D only: `libdraw` →
`/dev/draw`, off-screen via `libmemdraw`. There is zero GL/GLES/Mesa/shader code
anywhere in the repo (verified). The user wants OpenGL with "1:1 parity as much
as possible" — meaning *real* OpenGL, measured against a real reference — not a
toy. The concrete downstream motivation is **Alacritty**, whose entire renderer
is modern GL: GLSL shaders, VBO/VAO, a glyph-atlas texture, and instanced quad
drawing. That immediately disqualifies TinyGL (fixed-function GL 1.x, no shaders)
and fixes the choice on **Mesa's softpipe** — the pure-C gallium software
rasterizer, which is GL 3.3 / GLES 3.x complete (exactly Alacritty's floor).
llvmpipe is out: it JITs through LLVM-as-a-runtime-library, which does not exist
on 9front and is blocked by NX/W^X.

## Goal

Stand up `gl9`: **Mesa 24.0.x softpipe + OSMesa + libEGL**, cross-compiled with
**cc9** (the repo's clang→9front a.out toolchain), rendering into a memory buffer
that a native `libdraw` helper blits to a rio window. Prove it with a **parity
suite diffed against host Mesa softpipe** (zig9/python9-style pass%). Architect
every seam so Alacritty is reachable, but keep the Alacritty-specific layers
(EGL platform detail, a winit backend, a Rust-on-9front toolchain) out of scope.

## Locked decisions (already agreed)

| Axis | Decision |
|---|---|
| Upstream | **Mesa softpipe** (pure-C gallium). Not TinyGL, not llvmpipe. |
| Compiler | **cc9** (clang→9front a.out). kencc can't build Mesa (mixed C/C++, mmap, etc.). |
| First proof | **OSMesa wedge** — render off-screen, blit to a rio window. |
| Done bar | **Parity suite vs host Mesa softpipe** — measured pass%. |
| Scope | **gl9 only, Alacritty-ready** (build EGL + GLES2 + GL 3.3). |

## The one fact that shapes the architecture — the cc9 ABI wall

`cc9/README.md:245` is explicit: **cc9 code is internally System V and cannot
link Plan 9's own C libraries** (kencc's stack-args ABI is incompatible; cc9
links statically before `elf2aout`). It reaches the kernel *only* through syscall
thunks. Confirmed by reading the runtime.

**Consequence:** Mesa-in-cc9 cannot call `initdraw`/`allocimage`/`draw`. The
presentation seam must be a **two-process pipe**:

```
 [ glapp + Mesa softpipe + OSMesa/EGL ]      cc9 a.out (System V)
        │  gl9_present_frame(fd, w,h, rgba)  →  write() a framed blob
        ▼
   GL9F | w | h | chan(fourcc) | stride | rows…      (a pipe / fd)
        │
        ▼
 [ gl9win ]   native kencc a.out (libdraw + libthread, K&R 8-space)
   initdraw → allocimage(chan) → flip+loadimage → draw → flushimage
   resize/keyboard/mouse events ──back-channel fd──▶ glapp
```

This is **not a new pattern** — it is exactly how `src/vtwin/main.c` already
reads frames from `vts` and blits them (`initdraw`/`loadimage`/`draw`/
`flushimage`/resize). `gl9win` is `vtwin` re-pointed at RGBA frames. The
`write(framefd, …)` call *is* the reusable SwapBuffers seam that OSMesa uses in
Phase 2 and `eglSwapBuffers` reuses in Phase 4.

Phase 1 needs no draw at all — it dumps a PPM through cc9's `fs.c`, cleanly
isolating "does softpipe produce correct pixels" from any windowing.

## Architecture

### Build bridge: "meson generates on host → cc9 compiles"

Mesa generates a lot of C at configure time (GL dispatch from XML, format
tables, NIR opcodes, the GLSL flex/bison lexer+parser) via Python/Mako. We
materialize those on a Linux host, then compile with cc9. The bridge is
**`compile_commands.json`**, which meson emits with the exact per-file
`-D`/`-I`/`-std` — far more reliable than reconstructing commands by hand.

1. `host/linux-configure.sh` — in a Linux container (reuse the
   `zig9/port/plan9/linux-build.sh` docker pattern): `meson setup build-gen …`
   + `ninja`. This (a) generates every `.c/.h`, (b) emits
   `compile_commands.json`, and (c) **produces a working native Mesa+softpipe we
   reuse as the parity golden oracle** — one build, two jobs.
2. `host/harvest.py` — parse `compile_commands.json`; for each object that links
   into our targets, extract `{source, defines, includes}`; **scrub Linux-only
   `-D`** (`HAVE_LINUX_FUTEX`, `HAVE_PTHREAD_SETNAME_NP`, `HAVE_SCHED_*`,
   `HAVE_MEMFD_CREATE`, file-backed `HAVE_SYS_MMAN_H` paths) so portable branches
   compile. Keep `HAVE_PTHREAD`, endian, `PIPE_ARCH_LITTLE_ENDIAN`.
3. `host/build-gl9.sh` — for each harvested source, invoke cc9's clang with the
   *same* `-D/-I` retargeted (`--target=x86_64-unknown-none -nostdlib`, C as
   c11, C++ as c++17, matching Mesa's `-fno-exceptions -fno-rtti`); link with
   `ld.lld` + `--start-group libcc9cxx.a libcc9m.a --end-group`; `elf2aout`.
   This mirrors `cc9/host/cc9` over a *file set* instead of one source. Reuse
   `cc9/host/elf2aout.py` and `cc9/host/deliver.py` verbatim.

Minimal meson option set for the host generate/oracle build:

```
meson setup build-gen \
  -Dgallium-drivers=softpipe -Dvulkan-drivers=[] -Dllvm=disabled \
  -Dglx=disabled -Degl=enabled -Dgbm=disabled -Ddri3=disabled \
  -Dopengl=true -Dgles2=enabled -Dosmesa=true -Dshared-glapi=enabled \
  -Dplatforms=[] -Dshader-cache=disabled -Dzstd=disabled \
  -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled \
  -Dgallium-nine=false -Dgallium-rusticl=false -Dgallium-opencl=disabled \
  -Dbuild-tests=false -Dbuildtype=release -Db_ndebug=true \
  -Dc_std=c11 -Dcpp_std=c++17
```

Option *names* drift across Mesa versions; `harvest.py` doesn't care — it reads
`compile_commands.json`. Pin **Mesa 24.0.9** (softpipe long-complete; ≤24.0.x
keeps Rust entirely out of the build — matters given the "no Rust" house rule;
"boring"/settled, predates newer hard deps). Fallback 23.1.x. `fetch.sh` pulls
`https://archive.mesa3d.org/mesa-24.0.9.tar.xz` + sha256 into gitignored
`vendor/mesa/` (same convention as `zig9/fetch.sh`).

**Source subtrees compiled:** `src/util` (+`format`), `src/mapi/{glapi,es2api,
shared-glapi}`, `src/compiler/{glsl_types,shader_enums,nir_types}` +
`src/compiler/glsl` (**C++**) + `glsl/glcpp` (C, flex/bison) + `src/compiler/nir`
(C), `src/mesa/{main,program,vbo,state_tracker,math}`, `src/gallium/auxiliary`
(util, draw, tgsi, nir_to_tgsi, target-helpers), `src/gallium/drivers/softpipe`,
`src/gallium/frontends/osmesa`, `src/gallium/winsys/sw/{null,wrapper}`, and
`src/egl` (Phase 4). SPIR-V is **not** needed (no `ARB_gl_spirv`). The only large
C++ subtree is `glsl` — precisely what cc9 exists for.

### POSIX/OS shim surface — cc9 already covers almost all of it

Verified against `cc9/runtime/{posix_llvm.c,pthread.c,n9libc.c,crt0.c}`:

| Mesa need | cc9 status |
|---|---|
| pthreads, C11 `<threads.h>`/`<stdatomic.h>` | **real** (`pthread.c`; select via `HAVE_PTHREAD`) |
| `clock_gettime`/`gettimeofday`/`getenv` | **real** (`/dev/bintime`, `/env`) |
| `mmap`/`munmap`/`mprotect`/`madvise` | **stub** (`posix_llvm.c:23` anon⇒zeroed malloc; softpipe mostly `malloc`s) |
| `dlopen`/`dlsym` | **stub→0** (forces static pipe path) |
| `getpagesize`/`sysconf`/`getpid` | **real** (`_SC_NPROCESSORS_ONLN`=1 → single-thread default) |
| FP-exception mask | **automatic** (`crt0.c` `cc9_fpmask` at every process/thread start) |
| Linux **futex** | **must scrub** — undefine so `simple_mtx`/`u_thread` take the pthread path |
| `pthread_setname_np`, `sched_*affinity` | **compile out / no-op** in `port/plan9/shim/gl9_os_extra.c` |
| disk shader cache, dynamic pipe loader | **disabled** (`-Dshader-cache=disabled`; static softpipe screen) |

Real work (all bounded, none blockers): thorough `-D` scrubbing; confirming no
*live* file-backed `mmap` after cache-disable (grep the harvested set); wiring a
**static** softpipe `pipe_screen` (`target-helpers` `sw_screen_wrap` +
`softpipe_create_screen`, no dynamic loader). Start **single-threaded**
(`num_threads=1`) to remove a variable; flip to N pthreads once parity is green.

### Presentation glue (two files, two ABI worlds)

- **cc9 side** `gl9_present_frame(fd,w,h,rgba)` (~30 lines, `write()` only, no
  libdraw): frame the OSMesa buffer (default **bottom-up**, `OSMESA_BGRA`) as
  `GL9F|w|h|chan|stride|rows`. Phase 4 `eglSwapBuffers` calls the same function.
- **native side** `port/plan9/win/gl9win.c` (kencc/libdraw/libthread): `initdraw`
  → `allocimage(chan)` matching the incoming fourcc (`x8r8g8b8`/`a8b8g8r8`) →
  **vertical flip** while `loadimage`/`writeimage` → `draw` → `flushimage`; an
  `alt` loop for `Eresize` that writes new WxH back to glapp (which reallocs the
  surface). Keyboard/mouse flow back the same channel (Alacritty later). Wire as
  `glapp | gl9win`.

### Parity harness (the done bar) — modeled on `zig9/test/`

Corpus (each a self-contained OSMesa program, dumps a PPM via `fs.c`, **modern
GL throughout**):

1. `01_clear_color` — `glClear` (compile+link+run+pixels)
2. `02_triangle` — VBO+VAO, GLSL 330 (first real C++ GLSL compiler + NIR pass)
3. `03_shaded_triangle` — varying color + `mat4`/`vec4` uniform
4. `04_textured_quad` — checkerboard texture + sampler
5. `05_instanced_quads` — `glDrawArraysInstanced` + `gl_InstanceID`, textured
   (**Alacritty's exact shape**: glyph-atlas instanced quads)
6. `06_depth_blend` — overlapping quads, depth test + alpha blend

Shaders kept arithmetic-simple (avoid `sin/cos/pow`) so libm divergence stays
small. `host/build-host-refs.sh` links the *same* corpus against the *same* Mesa
24.0.9 softpipe (§build oracle) → `test/goldens/*.ppm`. `test/run_gl9.py` builds
via `build-gl9.sh`, `deliver.py`s to qemu (`127.0.0.1:1717`) / cirno
(`192.168.88.159:17010`), captures the PPM over listen1, and `test/ppmdiff.py`
does a tolerance diff (start per-channel `|Δ|≤2`, pass if ≥99.5% pixels within
tolerance), reporting pass% into `test/parity/manifests/{qemu,cirno}.json`.
Because it's identical Mesa+softpipe on both sides, the only divergence source is
openlibm vs host libm rounding — hence the tolerance.

## Directory scaffold (matches the zig9/cc9 big-port template)

```
gl9/
  README.md  .gitignore                     # vendor/ build-gen/ _out/ *.o *.aout
  fetch.sh                                   # Mesa 24.0.9 + sha256 → vendor/mesa (gitignored)
  host/
    linux-configure.sh   # meson+ninja in container → generated srcs + compile_commands.json + host refs
    harvest.py           # compile_commands.json → per-target {srcs,defines,includes}; scrub Linuxisms
    build-gl9.sh         # cc9/clang over harvested set → .o → lld link → elf2aout (mirrors cc9/host/cc9)
    build-host-refs.sh   # link corpus vs host Mesa softpipe → goldens
    # elf2aout.py / deliver.py reused from cc9/host/
  port/plan9/
    README.md  NOTES.md  apply.sh
    patches/   01-host-build-coax.patch  02-force-pthread-path.patch  03-egl-plan9-platform.patch
    shim/  gl9_os_extra.c                    # true cc9 gaps only (pthread_setname_np no-op, …)
    win/   mkfile  gl9win.c                  # native libdraw blitter (vtwin re-pointed at RGBA)
  test/
    corpus/  glharness.h  01_…c … 06_…c
    goldens/*.ppm                            # committed host references (small)
    run_gl9.py  ppmdiff.py
    parity/manifests/{qemu,cirno}.json
  vendor/mesa/                               # gitignored; fetch.sh
```

## Phased task breakdown (each phase ends in a runnable proof)

**Minimal first step** (smallest possible proof): a cc9 a.out that
`OSMesaCreateContext` + `OSMesaMakeCurrent` on a 16×16 buffer, `glClearColor`
red + `glClear`, reads pixel(0,0), prints `R=255 G=0 B=0`, exits 0. Proves
compile+link+run+softpipe-pixels with zero PPM/draw/parity machinery.

- **Phase 0 — scaffold + host-generate + COMPILE+LINK.** `fetch.sh`,
  `linux-configure.sh`, `harvest.py`, `build-gl9.sh`; link one a.out that prints
  `glGetString(GL_VERSION)` on 9front. **Gate:** runs on qemu, prints a GL
  version. **Blocker:** harvest fidelity — a Linux-only header/`-D` slipping in.
  *Spike:* compile-all-files-once, aggregate every missing-symbol/header error to
  enumerate the shim gap in one shot (no whack-a-mole).
- **Phase 1 — OSMesa → PPM, pixel-match host.** `01`+`02` render to buffer, dump
  PPM (no draw), diff vs golden. **Gate:** `02_triangle` within tolerance.
  **Blocker:** the **C++ GLSL compiler under cc9 libc++** — first real C++ path
  exercise. *Spike:* compile just `src/compiler/glsl` into a standalone
  "GLSL-string → NIR" harness, prove it runs on 9front before the full context.
- **Phase 2 — live rio window via `gl9win`.** Pipe frames; run the full
  shader+VBO+VAO+texture+`05_instanced_quads` wedge interactively; resize works.
  **Gate:** instanced textured quads visible in a rio window; resize redraws.
  **Blocker:** the cc9↔libdraw frame seam (chan/fourcc, BGRA↔RGBA, flip, resize
  handshake).
- **Phase 3 — parity suite green.** All 6 tests, `run_gl9.py`+`ppmdiff.py`, on
  qemu **and** cirno; publish manifests. **Gate:** ≥99.5% pixels within `T=2`
  across all 6, both targets. **Blocker:** FP nondeterminism (openlibm vs host
  libm) → tune tolerance from measured `03` deltas; keep shaders simple.
- **Phase 4 — libEGL + Plan 9 EGL platform stub (Alacritty seam).** Add
  `src/egl`; write a minimal surfaceless/swrast **Plan 9 EGL platform** (static
  direct-to-softpipe `pipe_screen`, no gbm/dri3/dlopen); `eglSwapBuffers` calls
  the present seam. **Gate:** an EGL (not OSMesa) program renders `02` to a window
  via `eglGetDisplay`/`eglCreateContext`/`eglMakeCurrent`/`eglSwapBuffers`.
  **Blocker:** wiring a software EGL platform with no drm/gbm/dlopen — bypass
  Mesa's surfaceless-via-drm path with a custom static screen. *Highest-effort
  unknown; may slip. It is explicitly the seam, not full Alacritty.*

## Top risks (ranked)

1. **meson→cc9 command-extraction fidelity** — Linuxism blocklist in
   `harvest.py` + the compile-all-once enumeration spike.
2. **C++ GLSL compiler under cc9 libc++** — standalone GLSL→NIR spike before
   Phase 1; match `-fno-exceptions -fno-rtti`.
3. **EGL surfaceless platform effort (Phase 4)** — prototype `platform_plan9.c`
   returning a fixed softpipe screen; validate `eglSwapBuffers`→present early.
4. **mmap/dlopen residue** — grep harvested set for live callers post
   cache/loader-disable; confirm each is dead or hits the cc9 stub.
5. **FP exactness (openlibm vs host libm)** — measure `03` max-Δ, set tolerance
   from data; `cc9_fpmask` already pins round-nearest on both sides.
6. **Data model** — *reassuring:* 9front amd64 is LP64 little-endian, `long`=8,
   ptr=8, identical to Linux x86_64, so Mesa's assumptions hold; `long double`
   80-bit is provided by openlibm. Watch only for pointer-into-32-bit truncation.

## Verification (end-to-end)

- **Unit-of-proof per phase** is the phase Gate above — each is a real binary run
  on qemu (and cirno for Phase 3).
- **The measured result** is `test/parity/manifests/{qemu,cirno}.json` — a
  pass% over the 6-test corpus, diffed against host Mesa softpipe goldens. This
  is the "1:1 parity" number, reported like zig9/python9.
- **The compelling demo** is Phase 2: `05_instanced_quads` (Alacritty-shaped)
  drawn live in a rio window with working resize — screenshot into
  `gl9/screenshots/` per repo convention.

## Road to Alacritty (out of scope here — follow-ons, not tasks)

- Flesh out the Plan 9 EGL platform (input/event plumbing through `gl9win`'s
  resize/keyboard channels).
- A **winit Plan 9 backend** (window/input over the `gl9win` seam) for glutin.
- A **Rust-on-9front toolchain** — *does not exist* (no rust9) and conflicts with
  the `AGENTS.md`/`docs/development.md` "no Rust" rule. This is the true gating
  dependency for Alacritty itself and is a separate program of work, larger than
  gl9. Decide it consciously before starting Alacritty.

## Out of scope (this plan)

llvmpipe/JIT, GLX, Vulkan, hardware accel, X11/Wayland/gbm/dri3, disk shader
cache, gallium video (va/vdpau/xa/nine), rusticl/OpenCL, zink, any Rust
component. Multi-threaded softpipe is deferred (single-thread first, flip after
parity is green).

## Reference reading before starting

- `cc9/README.md` + `cc9/host/cc9` — the exact clang→lld→elf2aout invocation
  `build-gl9.sh` mirrors over a file set (flags, `--start-group libcc9cxx.a
  libcc9m.a`, target triple). Note the **ABI wall** (§Limitations) — the reason
  for the two-process seam.
- `cc9/runtime/posix_llvm.c` + `pthread.c` — the POSIX/OS shim Mesa's util/os
  layer lands on (mmap/dlopen/sysconf stubs; real pthread mutex+FIFO-condvar;
  why we avoid futex).
- `zig9/test/run_corpus.py` + `zig9/test/parity/manifests/*.json` +
  `zig9/port/plan9/{apply.sh,linux-build.sh}` — the parity-harness and
  patch/container templates `test/run_gl9.py` and `host/` copy.
- `src/vtwin/main.c` — the two-process libdraw frame-blit precedent
  (`initdraw`/`loadimage`/`draw`/`flushimage`/resize) that `gl9win.c` re-points
  at RGBA frames.
- `docs/wiki/concepts/draw-api.md` — libdraw chan/`bytesperline` reference for
  the `gl9win` blitter.
- `docs/development.md` — the host↔qemu↔cirno build/deliver loop (`deliver.py`
  over listen1 `1717`/`17010`).
