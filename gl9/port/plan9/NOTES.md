# gl9 — porting notes / dev log

Running log of what was tried, what broke, and why. Newest at top. The plan is
`docs/plans/2026-07-01-gl9-opengl-mesa.md` (mirror of the approved plan).

## Headline status

**gl9 is complete — all planned phases done. Real OpenGL 3.3 (Mesa 24.0.9
softpipe) compiles, links, renders pixel-perfect, presents to a live 9front
window, and is reachable through both OSMesa and an EGL API.**

- **Phase 0** — all ~800 softpipe+OSMesa+GLSL/NIR TUs compile for `x86_64-plan9`
  with cc9 (0 failures), link to a 13 MB a.out; `00_clear_probe` prints
  `GL_VERSION=3.3 (Compatibility Profile) Mesa 24.0.9` + `R=255 G=0 B=0`.
- **Phase 1 + 3** — the 6-program modern-GL corpus (VBO/VAO/GLSL, textures,
  instanced quads, depth+blend) is **byte-identical** to the host Mesa softpipe
  golden: `test/parity/manifests/qemu.json` = **6/6 PASS, every maxΔ=0**.
- **Phase 2** — `port/plan9/win/gl9win.c` (native kencc/libdraw) blits rendered
  frames to a 9front window over the two-process pipe seam. Proof:
  `screenshots/phase2-shaded-triangle.png` (a Gouraud RGB triangle on-screen).
- **Phase 4** — `port/plan9/egl/gl9egl.c` (a minimal libEGL over OSMesa) drives
  the full glutin-style sequence (getDisplay→initialize→chooseConfig→createContext
  →createWindowSurface→makeCurrent→render→swapBuffers). Proof:
  `screenshots/phase4-egl-triangle.png`. This IS the Alacritty seam.
- **Demo** — `test/corpus/cube_demo.c`: an animated, perspective-projected,
  depth-tested, per-pixel Phong-lit spinning cube (mat4 pipeline + specular),
  streamed frame-by-frame to gl9win. The strong "real 3D OpenGL on 9front" proof:
  `screenshots/cube-3d-lit.png` (lit face bright, shadowed face dark — Phong works).

### Phase 2 — gl9win (the presentation seam)
cc9's System-V a.out can't link kencc libdraw (the ABI wall), so the Mesa process
writes framed RGBA to a pipe and a separate native `gl9win` blits it (the
`src/vtwin` pattern). Frame: `"GL9F" | u32be w | u32be h | w*h*4 RGBA`. OSMesa's
RGBA byte order == Plan 9 **ABGR32** (`a8b8g8r8` little-endian), so no repack —
verified on-screen (red/green/blue triangle corners correct). Build `gl9win` on
the VM: `mk` in `port/plan9/win/` (`<draw.h>` auto-links libdraw via `#pragma
lib`). Run: `glapp | gl9win`. Verify with QMP `screendump` (the VM runs mxio, so
initdraw from the listen1 terminal gets a window). gl9win draws fullscreen-centered
and holds the last frame; a real interactive one would add resize + a dismiss
event loop.

### Phase 4 — gl9egl (the EGL seam, the lazy-correct way)
Porting Mesa's egl_dri2 (DRI loader / platform backends / dlopen) is a mountain
stock 9front can't support. Instead `gl9egl.c` implements the ~20 EGL entrypoints
glutin binds, over OSMesa: `eglCreateContext`→`OSMesaCreateContextExt`,
`eglMakeCurrent`→`OSMesaMakeCurrent`, `eglSwapBuffers`→the gl9_present frame
protocol, `eglGetProcAddress`→`OSMesaGetProcAddress`. Same softpipe that passed the
parity suite; EGL is just the binding. Mesa's `EGL/eglplatform.h` #errors on an
unrecognized platform → shimmed by `port/plan9/egl/EGL/eglplatform.h` (generic
opaque native types + EGL_CAST), placed before Mesa's include. Build/run:
`build-gl9.py link test/corpus/egl_demo.c port/plan9/egl/gl9egl.c`, then
`egl_demo | gl9win` with `GALLIUM_NOSSE=1`.

### Out of scope (beyond gl9, per the plan): a **winit Plan 9 backend** (window +
input events over the gl9win seam) and a **Rust-on-9front toolchain** (doesn't
exist; conflicts with the "no Rust" rule) — the remaining prerequisites for
Alacritty *itself*, separate programs of work. gl9 gives them a working, tested
OpenGL to build on.

### THE fix that made draw calls work: `GALLIUM_NOSSE=1`
`glDrawArrays` faulted with `trap: fault read addr=PC` (jump-to-heap = executing
non-exec memory). Cause: Mesa's `translate_sse` (gallium/auxiliary/translate) JITs
x86 for vertex fetch via `rtasm/rtasm_execmem.c`, and 9front NX-enforces all
writable memory (cc9's mmap is a non-exec malloc stub) — so the generated code
faults. Mesa has a built-in escape: `u_cpu_detect.c` honors **`GALLIUM_NOSSE=1`**
(and `GALLIUM_OVERRIDE_CPU_CAPS=nosse`) → reports no SSE → `translate_sse2_create`
returns NULL → pure-C `translate_generic`. **Every gl9 run must set
`GALLIUM_NOSSE=1`** (rc: `GALLIUM_NOSSE=1; prog` in the same connection, so it lands
in /env for cc9 getenv). The GLSL compiler + NIR are pure C and were never the
problem — compile/link/VAO/VBO all worked; only the JIT'd vertex-fetch faulted.
(Reachable-JIT alternative: the wxallow W^X kernel patch — but NOSSE is free.)

### Phase 1 corpus + harness (this is what "parity suite" means)
`test/corpus/{00_clear_probe,01_clear_color,02_triangle,03_shaded_triangle,
04_textured_quad,05_instanced_quads,06_depth_blend}.c` share `glharness.h`
(OSMesa 3.3+depth ctx, shader compile/link, P3-PPM writer at 64x64; P3 ASCII so it
cats back over listen1). Goldens: `host/build-host-refs.sh` compiles the SAME
source with gcc against the build-gen oracle (same Mesa 24.0.9 softpipe) → the only
divergence is openlibm-vs-host-libm (so far: none — maxΔ=0). Diff: `test/ppmdiff.py`
(per-channel tol, pass%). Runner: `test/run_gl9.py` (link → serve → hget → run
NOSSE → fetch PPM → diff → `test/parity/manifests/qemu.json`).

### Running on the VM — the transfer dance (listen1 is flaky!)
The 13 MB a.out is too big for cc9/host/deliver.py (it inlines bytes as a C array).
Use HTTP: `python3 -m http.server 8099 --bind 0.0.0.0` in `_out/`, then on the VM
`hget http://10.0.2.2:8099/<bin> >/tmp/gl9bin`. listen1 (`nc … 127.0.0.1:1717`)
returns empty intermittently and a single-shot `nc` times out on a slow softpipe
run — so: (1) launch hget in the background on the VM (`… >[2]/dev/null &`) and
poll `ls -l` for the full size; (2) run in the background to a file
(`/tmp/gl9bin >/tmp/out >[2=1] &`) and poll `cat /tmp/out`. The binary runs slow
under emulation (~seconds to init the GLSL compiler + softpipe). This matches the
memory's "listen1 flaky, lean into files" note — a 9P mount would be cleaner.

### Compile-all: the shim gaps (all fixed in shim/, zero Mesa source edits except
2 tiny os_* patches)

The `enumerate` pass drove this: 72 initial failures → 0. Root causes and fixes:
- **libc++ header order** (~30 "false" C++ failures): cc9's `-isystem INC` (has a C
  `stdlib.h`) must come AFTER libc++'s `-isystem`, else `<cstdlib>` finds the wrong
  `stdlib.h`. Fixed in build-gl9.py `cmd_for`.
- **`-DMAPI_ABI_HEADER="/work/..."`** (7 mapi files): a container path baked into a
  `-D` value. harvest.py now remaps `/work` inside define values too.
- **`alloca`** → `#define alloca __builtin_alloca` (clang has it).
- **missing errno** (EDEADLK, E2BIG, socket errnos) + **SIGSYS** + **TIME_MONOTONIC**
  → defined in gl9_pre.h (standard Linux values, guarded).
- **libc gaps** strchrnul/strcasecmp/strncasecmp/strtok_r/open_memstream +
  **pthread** sigmask/mutex_timedlock/getcpuclockid → prototypes in gl9_pre.h,
  impls in gl9_os_extra.c (open_memstream = fixed 1 MB fmemopen, ponytail ceiling).
- **`<strings.h>`** cc9 lacks → shim header `shim/include/strings.h`.
- **os_misc.c / os_time.c "unexpected platform"** `#error`s → 2 minimal patches
  (`patches/01-*`, `02-*`, applied by apply.sh) with Plan 9 fallbacks
  (return-unknown memory, page size 4096, usleep, clock via /dev/bintime).
- **blake3 SIMD `.S`** can't assemble → excluded; `BLAKE3_NO_*` forces portable C.

### Excludes (build-gl9.py): TUs meson configures but OSMesa+softpipe doesn't link
`.S` asm, the glsl `standalone` tool lib (duplicate `_mesa_*` stubs, matched by obj
path), gtest, dynamic loaders (`/loader/`, `/pipe-loader/`), `/virtio/`,
disk-cache (`mesa_cache_db.c`, `shader_cache.cpp`), driconf (`xmlconfig.c`, needs
expat), and tool `main()`s (glcpp.c, glsl main/standalone/test_optpass, spirv2nir.c).
SPIR-V *lib* and driver_ddebug ARE included (referenced by the link; they compile).

## The build bridge (how it works)

`meson generates on host → cc9 compiles`, joined by `compile_commands.json`:

1. `host/Dockerfile` → `gl9build:bookworm` (debian + meson/ninja/flex/bison/mako).
   **Built `--platform=linux/amd64`** so meson emits x86-64 arch defines/dispatch
   matching cc9's target — NOT the host arm64.
2. `meson setup` (options in `host/linux-configure.sh`, TBD file — currently run by
   hand; see below) with `gallium-drivers=swrast` (softpipe when `llvm=disabled`),
   `osmesa=true`, `gles2=enabled`, `shared-glapi=enabled`, everything else off.
   **softpipe is selected via `swrast`, not `-Dgallium-drivers=softpipe`** (that
   value doesn't exist). Configure succeeds; `compile_commands.json` = 834 TUs.
3. `ninja` in the container materializes generated C + builds the amd64 oracle
   (reused later for parity goldens). **Runs `-j4`** — emulated amd64 `-j$(nproc)`
   OOM'd Docker Desktop's VM mid-build (container "unexpected EOF"). Incremental, so
   just resume.
4. `host/harvest.py` parses `compile_commands.json` → `harvest.json`: per-TU
   `{src, lang, std, defines, includes, arch, forced}`, paths remapped
   container `/work` → host repo, and **Linux/glibc/asm feature `-D`s scrubbed**
   (32 of them: `HAVE_ENDIAN_H`, `HAVE_DLFCN_H`, `HAVE_LINUX_FUTEX_H`,
   `HAS_SCHED_*`, `HAVE_MEMFD_CREATE`, `HAVE_ZLIB`, `USE_DRICONF`, `HAVE_OPENMP`,
   `USE_X86_64_ASM`, …). Dropping these makes Mesa compile its portable branches.
5. `host/build-gl9.py` compiles every TU with cc9 clang (`--target=x86_64-unknown-none
   -femulated-tls -funwind-tables -fno-pic -O2 -isystem cc9/runtime/include
   -include shim/gl9_pre.h`; C++ adds `-nostdinc++ -isystem $CC9_LIBCXX`). Modes:
   `enumerate` (compile-all, collect the aggregate gap list), `build` (archive),
   `link` (TODO).

## Shim gaps found & fixed (`shim/gl9_pre.h`, `shim/gl9_os_extra.c`)

All four were a single force-included header away — no Mesa source edits:

- **endian** — with `HAVE_ENDIAN_H` scrubbed and `--target=…-none` defining no OS
  macro, `util/u_endian.h` `#error`s ("UTIL_ARCH_* unset"). Fixed by defining
  `UTIL_ARCH_LITTLE_ENDIAN=1 / BIG_ENDIAN=0` (amd64 is LE). This `#error` cascades
  into bogus syntax/`undeclared identifier` errors elsewhere (e.g. `PIPE_FORMAT_
  RG88_UNORM`, which is actually defined) — fix endian first, most "errors" vanish.
- **`static_assert`** — cc9's `<assert.h>` lacks the C11 macro → `#define
  static_assert _Static_assert` (C only; C++ has the keyword).
- **`pthread_barrier_t`** — cc9 pthreads have no barriers, but `util/u_thread.h`
  declares `util_barrier` over them. Provide the type + a correct phase-flip barrier
  over cc9's mutex+cond (`shim/gl9_os_extra.c`). softpipe is single-threaded for now
  so it rarely fires, but must link.
- **`M_PI` / `M_LOG2E` / …** — cc9's `<math.h>` omits the POSIX math constants
  Mesa uses directly (`nir_builtin_builder.h`, `glsl_to_nir`). Defined the standard
  set (guarded on `M_PI`).

## Gotcha: the cc9 freestanding libc++ header tree is in /tmp (ephemeral)

`CC9_LIBCXX` defaults to `/tmp/libcxx-thr/include/c++/v1`. That tree gets cleaned
(it's `/tmp`; cc9 was last fully built 2026-06-28), and here it had lost its
top-level umbrella headers (`<new>`, `<vector>`, …) while keeping the target-correct
`__config_site` + `__*` internals. Symptom: `fatal error: 'new' file not found`.
Do NOT substitute brew's hosted `include/c++/v1` — its `__config_site` triggers
`"No thread API"` / vendor-availability errors for the bare target. Fix used:
`cp -Rn <brew>/include/c++/v1/ <freestanding>/` — fills only the missing
(config-independent) umbrella headers, keeps cc9's `__config_site`
(`_LIBCPP_HAS_THREAD_API_PTHREAD 1`, `_LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0`).
Both are LLVM 22.1.8 so the umbrellas match. **Durability TODO:** stop depending on
`/tmp` — have gl9 (or cc9) provision a complete freestanding tree in a stable path.

## Environment (verified 2026-07-01)

- cc9 archives built: `cc9/lib/{libcc9cxx.a (271M), libcc9m.a}`. clang = brew LLVM
  22.1.8 at `/opt/homebrew/opt/llvm/bin`. `cc9/test/plan9.ld` present.
- 9front qemu VM reachable on listen1 `127.0.0.1:1717` (deliver+run via
  `cc9/host/deliver.py`). cirno bare-metal at `192.168.88.159:17010` (Phase 3).
- Host has no `meson` (use the container); has `docker`, `ninja`, `clang`, `ld.lld`.

## Next

1. Finish `ninja -j4` (materialize all generated sources; complete the oracle).
2. `build-gl9.py enumerate` → aggregate remaining shim gaps in one pass; extend
   `gl9_pre.h`/`gl9_os_extra.c` until compile-all is green.
3. `build-gl9.py build` → `libgl9mesa.a`.
4. Link a minimal `OSMesaCreateContext` + `glClear` + read-pixel main → a.out,
   deliver to the VM, confirm `R=255 G=0 B=0` (the plan's minimal first step).
5. Then PPM + parity harness (Phase 1), `gl9win` blit (Phase 2), EGL (Phase 4).
