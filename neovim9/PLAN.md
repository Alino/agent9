# neovim9 — Neovim on 9front (inside alacritty9)

Full plan: ~/.claude/plans/can-you-port-neovim-joyful-puffin.md
Toolchain: cc9 (clang → x86_64-unknown-none → elf2aout). Lua: LuaJIT interpreter mode.
Bar: TUI editing + treesitter inside alacritty9 (VM + cirno). Jobs/:terminal/LSP = follow-ons.

## Status

- [x] **G0 recon + host reference build** (2026-07-06)
  - Version: **v0.12.4** (latest stable, released 2026-07-05). vendor/neovim (shallow clone).
  - Host build green: `make CMAKE_BUILD_TYPE=Release CMAKE_EXTRA_FLAGS=-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`
    → build/bin/nvim runs, build/compile_commands.json = 416 TUs, generated sources in
    build/src/nvim/auto/, dep sources unpacked in .deps/build/src/.
  - Deps to cross-compile: libuv 1.52.1, luajit (pinned commit, 2.1.1774638290), unibilium 2.1.2,
    luv 1.52.1-0, lpeg 1.1.0, lua-compat53, utf8proc 2.11.3, tree-sitter 0.26.7 + 6 parsers
    (c/lua/vim/vimdoc/query/markdown). Vendored in-tree: vterm, termkey, mpack, cjson, xdiff, klib.
    OFF: gettext, libiconv, wasmtime.
  - **TUI model is TWO-PROCESS** (0.12): foreground nvim = TUI client, spawns
    `$VV_PROGPATH --embed` as server over stdio pipes (src/nvim/main.c:356,
    src/nvim/ui_client.c:53). ⇒ uv_spawn + uv_exepath + msgpack-rpc pipes are on the
    G4 critical path (already in G2's gate). ui_client.c:73 re-opens the tty fd — needs
    a plan9 look (no /dev/tty; fd 0 IS the terminal pipe).
  - dlopen sites: lua/treesitter.c:138 (uv_dlopen for parsers → static table patch),
    os/dl.c (os_libcall, :libcall — can fail gracefully).
  - Harvest bridge (compile_commands → cc9) deferred to G3, adapting gl9/harvest.py.
- [x] **G1 LuaJIT interpreter on 9front** (2026-07-06) — **gate 12/12 PASS on dev VM**
  (fib, string.buffer, bit, pcall/xpcall, coroutines, gc, gsub/sort, io+read("*n"),
  os.time/date). Build: port/build-luajit.sh (pristine tarball + 1 patch
  [lj_prng /dev/random seed] + LuaJIT's own Makefile in cross mode: HOST_CC=clang,
  CC=port/n9cc, TARGET_SYS=Other → LUAJIT_OS_OTHER = no-POSIX/no-dlopen path;
  XCFLAGS=-DLUAJIT_DISABLE_JIT -DLUAJIT_USE_SYSMALLOC -DLUAJIT_NO_UNWIND).
  Artifacts: _out/luajit/{libluajit.a, lj9.aout, lua headers}. n9cc defines
  __plan9__ for all port patches. cc9 runtime GAINED: mktime/difftime
  (posix_llvm.c, Hinnant inverse of gmtime_r), tmpnam/tmpfile/fscanf("%lf")
  (stdio.c). GOTCHA: /tmp/libcxx-thr got wiped by the macOS /tmp cleaner —
  regen recipe now committed as cc9/host/regen-libcxx.sh. Note: lj9 bss=270MB
  (virtual, lazily committed — loads fine).
- [ ] **G2 poll() + libuv** — gate: uvgate.c (timer/pipe/spawn/async) on VM
- [ ] **G3 nvim headless** — gate: `nvim --headless "+lua print(...)" +q` on VM
- [ ] **G4 TUI in alacritty9** — acceptance: edit/:w/treesitter colors, VM + cirno, screenshots
- [ ] **G5 follow-ons**: jobs, :terminal, LSP, pac9 package

## Dev loop

Host build/deliver from this dir; run on dev VM 127.0.0.1:1717 (listen1), acceptance
also on bare-metal cirno 192.168.88.159:17010. Known listen1 gotchas: binary via cc9
deliver.py byte-writer; single inline commands; stderr goes to listener console.
