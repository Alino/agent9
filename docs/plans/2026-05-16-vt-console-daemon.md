# vt — Plan 9 Console Daemon Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task.

**Goal:** Build a single 9P file server (`vt`) that collapses st (VT100 rendering), tmux (session persistence + multiplexing), and zsh (line editing + completion) into one coherent Plan 9 service.

**Architecture:** vt serves a 9P namespace where each session exposes `cons`, `consctl`, `cells`, `scroll`, `size`, `title`. Programs running inside a session see `/dev/cons` as today and emit VT100 escape codes; vt parses them into an internal cell buffer. Two client modes consume the cell buffer: (a) mxio windows mount the session and render diffs natively with libdraw; (b) a `vt-attach` binary serializes the cell buffer back to VT100 escape codes over a dumb pipe (SSH, drawterm dumb). Sessions outlive clients — disconnect = unmount, reconnect = remount.

**Tech Stack:**
- C, K&R style, 8-space tabs (Plan 9 convention)
- `lib9p` for 9P server protocol
- `libdraw` for mxio client rendering
- `libthread` for concurrency (channels, alt, threads)
- `mk` for builds (Mkfile per binary)
- Test inside 9front VM (`~/Projects/plan9-agent/`)

**Out of scope:**
- PTYs (not building any). isatty()-gated Unix programs (htop, lazygit, etc.) will NOT work — that's an APE-level kernel problem and not what vt solves.
- Replacing rc. vt runs rc as the session shell; no shell port.
- Encrypted/authenticated 9P. We rely on existing factotum/sshd auth.

---

## Reference reading before starting

Required before Task 1:
- `wiki/concepts/rio-architecture.md` — how rio currently serves /dev/cons via wind.c
- `wiki/concepts/draw-api.md` — libdraw drawing primitives mxio client will use
- `wiki/concepts/mxio-design.md` — current mxio internals we'll modify
- 9front manpages: `lib9p(2)`, `thread(2)`, `srv(4)`, `cons(3)`
- st source for VT100 parser reference: https://git.suckless.org/st (read `st.c` `tputc` / `csihandle` / `eschandle`)

---

## Phase 0 — Scaffolding & Wiki Decision Record

### Task 0.1: Create wiki design pages

**Files:**
- Create: `wiki/concepts/vt-architecture.md`
- Create: `wiki/concepts/vt-9p-namespace.md`
- Create: `wiki/concepts/vt100-parsing.md`
- Modify: `wiki/index.md` — add three new entries, bump page count 9 → 12
- Modify: `wiki/log.md` — prepend dated entry

1. Write `vt-architecture.md`: goals, two-client-mode design, why-not-PTYs rationale, components diagram
2. Write `vt-9p-namespace.md`: full directory layout, file semantics, message formats
3. Write `vt100-parsing.md`: which escape sequences we support (CSI subset), which we ignore
4. Update index + log
5. Commit: `wiki: add vt design pages`

**Verify:** `read_file wiki/index.md` shows 12 pages.

### Task 0.2: Create source tree skeleton

**Files:**
- Create: `src/vt/mkfile`
- Create: `src/vt/dat.h` (shared types)
- Create: `src/vt/fns.h` (function declarations)
- Create: `src/vt/main.c` (stub: prints "vt: hello\n" and exits)
- Create: `src/vt-attach/mkfile`
- Create: `src/vt-attach/main.c` (stub: prints "vt-attach: hello\n" and exits)

1. Skeleton `vt/mkfile` (use mxio's Mkfile as template):
   ```
   </$objtype/mkfile
   TARG=vt
   BIN=/$objtype/bin
   OFILES=main.$O
   HFILES=dat.h fns.h
   </sys/src/cmd/mkone
   ```
2. `main.c`:
   ```c
   #include <u.h>
   #include <libc.h>
   void main(int, char**) { print("vt: hello\n"); exits(nil); }
   ```
3. Push to VM via HTTP transfer (per `wiki/concepts/build-toolchain.md`)
4. In VM: `cd /sys/src/cmd/vt && mk install`
5. Run `vt` — expect `vt: hello`
6. Same for `vt-attach`
7. Commit: `vt: scaffold source tree`

**Verify:** Both binaries print their hello message in VM.

---

## Phase 1 — Minimal 9P File Server

Goal: vt serves a 9P namespace with one hardcoded session that you can mount and write to. No VT100 yet, no shell yet — just prove the 9P plumbing works.

### Task 1.1: Stand up lib9p server with `ctl` and `cons`

**Files:**
- Modify: `src/vt/main.c`
- Create: `src/vt/srv.c` (9P file server logic)
- Modify: `src/vt/dat.h`, `src/vt/fns.h`, `src/vt/mkfile`

1. **Failing test:** in VM, try `9p ls /srv/vt` → expect error (no service yet)
2. Implement `srv.c` using lib9p's `Tree` API:
   - Root has one entry: `1` (session id, hardcoded for now)
   - Session dir `1/` has: `cons` (rw), `ctl` (rw)
   - Walk/open/read/write/clunk dispatched via lib9p's Srv struct
3. In `main.c`: post the service at `/srv/vt` using `postsrv()` or via `srv` command
4. Build & run: `vt &`
5. **Verify pass:** in another window: `9p ls /srv/vt` → `1`
6. `9p ls /srv/vt/1` → `cons ctl`
7. `9p read /srv/vt/1/ctl` → returns "session 1 idle\n"
8. Commit: `vt: minimal 9p server with one session`

### Task 1.2: Echo loop on `cons`

**Files:**
- Modify: `src/vt/srv.c`

1. **Failing test:** `9p write /srv/vt/1/cons` with "hello" — should accept; `9p read /srv/vt/1/cons` should block then return "hello"
2. Implement: writes to `cons` push to a per-session buffer (Channel); reads pop from it. Use `libthread` channel for the queue.
3. **Verify pass:** two terminal windows:
   - Window A: `9p read /srv/vt/1/cons` (blocks)
   - Window B: `echo hi | 9p write /srv/vt/1/cons`
   - Window A unblocks, prints `hi`
4. Commit: `vt: cons echo loop`

### Task 1.3: Spawn rc inside the session

**Files:**
- Create: `src/vt/session.c`
- Modify: `src/vt/main.c`, `src/vt/dat.h`, `src/vt/fns.h`

1. **Failing test:** session 1 should run `rc` such that its stdin = our cons-write-buffer, stdout/stderr = our cons-read-buffer
2. Implement: on vt startup, `rfork(RFPROC|RFNAMEG|RFFDG)`, in child: bind our cons file as `/dev/cons` (via `srv`/`mount`), `exec("/bin/rc")`
3. Parent keeps the 9P server running
4. **Verify pass:** mount `/srv/vt/1` somewhere, `cat cons` → see rc prompt `term%`; `echo "echo hi" >> cons` → see `hi` come back out
5. Commit: `vt: spawn rc inside session 1`

**Pitfall:** Plan 9's `bind` semantics — the rc child must have its own namespace (RFNAMEG). The bind of our cons file replaces /dev/cons only in the child's namespace, not vt's.

---

## Phase 2 — VT100 Parser & Cell Buffer

### Task 2.1: Cell buffer data structure

**Files:**
- Create: `src/vt/cells.c`
- Modify: `src/vt/dat.h`, `src/vt/fns.h`, `src/vt/mkfile`

1. Define `Cell` struct: `Rune r; uchar fg, bg, attrs;` (attrs = bold/underline/reverse bits)
2. Define `Buffer`: 2D grid of cells, cursor x/y, size rows/cols, scrollback ring buffer of lines
3. Functions: `bufinit(rows, cols)`, `bufput(b, rune, attrs)`, `bufnewline(b)`, `bufclear(b)`, `bufscroll(b, n)`
4. **Test:** unit test in `src/vt/test/test_cells.c`:
   ```c
   Buffer *b = bufinit(24, 80);
   bufput(b, L'a', 0);
   assert(b->cells[0][0].r == L'a');
   ```
5. Compile test with `9c test_cells.c cells.c && 9l -o test_cells *.o && ./test_cells`
6. Commit: `vt: cell buffer with cursor tracking`

### Task 2.2: VT100 state machine — printable + basic control

**Files:**
- Create: `src/vt/parser.c`
- Modify: `src/vt/dat.h`, `src/vt/fns.h`, `src/vt/mkfile`

1. Define states: `Ground, Escape, CsiEntry, CsiParam, CsiIntermediate, OscString`
2. Reference: st's `tputc` function in st.c — same state machine, port to Plan 9 idioms (Rune not wchar_t, no realloc tricks)
3. Handle in this task:
   - Printable runes → `bufput`
   - `\n` → newline
   - `\r` → cursor x=0
   - `\b` → cursor x--
   - `\t` → tab to next 8-col stop
4. **Test:** `test_parser.c`:
   ```c
   Parser *p = parserinit(b);
   parserfeed(p, "hello\nworld", 11);
   /* assert row 0 = "hello", row 1 = "world" */
   ```
5. Commit: `vt: parser handles printable + basic control chars`

### Task 2.3: VT100 CSI sequences (color, cursor, erase)

**Files:**
- Modify: `src/vt/parser.c`

1. Support these CSI sequences (st reference for exact behavior):
   - `ESC[<n>m` — SGR (set graphics rendition): colors 30-37, 40-47, 90-97, 100-107; bold (1), underline (4), reset (0)
   - `ESC[<r>;<c>H` — cursor position
   - `ESC[<n>A/B/C/D` — cursor up/down/right/left
   - `ESC[<n>J` — erase display (0=below, 1=above, 2=all)
   - `ESC[<n>K` — erase line
   - `ESC[?25h/l` — show/hide cursor (track in cursor.visible)
2. Ignore (but consume cleanly): all other CSI, OSC except OSC 0 (window title), DCS, PM, APC
3. **Test:** feed `"\033[31mred\033[0m"` → row 0 cols 0-2 should have fg=red
4. **Test:** feed `"\033[2;5Hhi"` → cursor at row 1 col 4, then "hi" at those cells
5. Commit: `vt: parser handles SGR + cursor + erase`

### Task 2.4: Wire parser into cons write path

**Files:**
- Modify: `src/vt/srv.c`, `src/vt/session.c`

1. When a write arrives on `cons` from a program (rc's stdout): feed bytes through parser before storing
2. When a read arrives on `cons` from rc (rc's stdin): pass through raw (keyboard input doesn't get parsed)
3. **Verify pass:** start vt, mount session 1, `echo -e "\033[31mhello\033[0m" >> cons` — read the cell buffer (we'll expose it next task) and confirm cells contain red 'h','e','l','l','o'
4. Commit: `vt: parser wired into cons pipeline`

### Task 2.5: Expose `cells` file for clients

**Files:**
- Modify: `src/vt/srv.c`
- Create: `src/vt/celldiff.c` (diff serialization)

1. `cells` file semantics: reads return a binary protocol describing changes since the last read. Walk-clunk = full repaint.
2. Wire format (define in `dat.h`):
   ```
   uint32 magic = 0x76746331  // 'vtc1'
   uint16 rows, cols
   uint16 ncells_changed
   for each changed cell:
     uint16 row, col
     uint32 rune
     uint8 fg, bg, attrs
   uint16 cursor_row, cursor_col, cursor_visible
   ```
3. Maintain a dirty bitmap per session, cleared on read
4. **Test:** write "hi" to cons, read cells → ncells_changed=2, cells (0,0)='h' and (0,1)='i'
5. Commit: `vt: cells diff protocol exposed via 9p`

---

## Phase 3 — mxio Renderer Client

Goal: mxio windows stop owning /dev/cons internally; they mount a vt session and render its cell buffer.

### Task 3.1: New mxio mode flag

**Files:**
- Modify: `src/mxio/wind.c`
- Modify: `src/mxio/dat.h`

1. Add `int vtmode` to Window struct — 0 = legacy (current rio behavior), 1 = vt-attached
2. Command line flag: `mxio -V` enables vt mode globally for new windows
3. When `vtmode == 0`, behavior is unchanged (regression safety net)
4. Commit: `mxio: vtmode flag (no behavior change yet)`

### Task 3.2: Renderer reads cells, draws with libdraw

**Files:**
- Create: `src/mxio/vtclient.c`
- Modify: `src/mxio/wind.c`, `src/mxio/mkfile`

1. When window created in vtmode: spawn helper thread that
   - Mounts `/srv/vt` onto window's namespace
   - Opens `/srv/vt/<id>/cells`
   - Loops: read cell diffs, apply to local screen via libdraw (use existing font from `display->defaultfont`)
2. ANSI color → libdraw `Image*` lookup table (16 colors × allocimage)
3. **Verify pass:**
   - Start vt manually with one session
   - Launch `mxio -V`
   - In one terminal: `echo -e "\033[31mhello\033[0m" | 9p write /srv/vt/1/cons`
   - Should see red "hello" appear in the mxio vt window
4. Commit: `mxio: vt cells renderer`

### Task 3.3: Keyboard input → cons

**Files:**
- Modify: `src/mxio/vtclient.c`

1. mxio keyboard events (already wired in wind.c) → write to `/srv/vt/<id>/cons` (the rc-stdin direction)
2. **Verify pass:** type `echo hi` <Enter> in the vt window — see `hi` echoed back
3. Commit: `mxio: vt keyboard input`

### Task 3.4: Size sync via `size` file

**Files:**
- Modify: `src/vt/srv.c`, `src/mxio/vtclient.c`

1. Expose `size` rw file in session dir: read returns "<rows> <cols>\n", write accepts same format
2. mxio on resize: compute rows/cols from window pixel dims and font metrics, write to `size`
3. vt on size change: resize cell buffer, send SIGWINCH-equivalent to rc (Plan 9: `postnote(PNGROUP, pid, "sys: window size change")`)
4. **Verify pass:** resize mxio window, run `stty size` (if available) or check that rc reflows
5. Commit: `vt: size sync between mxio and session`

---

## Phase 4 — Remote Attach (vt-attach)

Goal: SSH into 9front, run `vt-attach 1`, see the same session rendered as VT100 escape codes over the dumb pipe.

### Task 4.1: vt-attach reads cells, emits VT100

**Files:**
- Modify: `src/vt-attach/main.c`
- Create: `src/vt-attach/term.c` (VT100 reserializer)

1. Open `/srv/vt/<id>/cells`, loop reading diffs
2. For each diff: emit `ESC[<r>;<c>H` to position, then SGR for color, then the rune as UTF-8
3. Track previous SGR to avoid redundant escape codes
4. Write to stdout (the SSH pipe)
5. **Verify pass:**
   - In 9front VM: `vt &` (start daemon)
   - `vt-attach 1` (foreground)
   - Run any color-emitting command — see VT100 output appear
   - But also: from Mac via ssh, `ssh -p 2222 glenda@localhost vt-attach 1` should render correctly in iTerm2
6. Commit: `vt-attach: VT100 reserializer`

### Task 4.2: vt-attach reads stdin → cons

**Files:**
- Modify: `src/vt-attach/main.c`

1. Spawn thread reading stdin (keyboard from SSH pipe)
2. Write each byte to `/srv/vt/<id>/cons`
3. Need raw mode on the Mac SSH side — vt-attach emits `\033[?1049h` etc. on startup (alternate screen), restores on exit
4. Handle SIGINT/SIGHUP cleanly — detach not kill the session
5. **Verify pass:** from Mac iTerm2: `ssh -p 2222 glenda@localhost vt-attach 1` → fully interactive rc session, colors work, disconnect with Ctrl-Q (or whatever we pick), session persists
6. Commit: `vt-attach: bidirectional terminal mode`

### Task 4.3: QEMU SSH port forward

**Files:**
- Modify: `~/Projects/plan9-agent/boot-9front.sh`

1. Add `-hostfwd tcp::2222-:22` to qemu args
2. Configure 9front sshd: `aux/listen1 -t tcp!*!22 /bin/service/tcp22 &` from `/rc/bin/cpurc` if not already
3. Add Mac public key to factotum: in VM, `echo 'key proto=ssh-rsa service=ssh user=glenda !secret=...' >> /usr/glenda/lib/keys`
4. From Mac: `ssh -p 2222 glenda@localhost` should land in rc
5. Commit: `infra: ssh port forward for vt-attach testing`

---

## Phase 5 — Line Editing & Completion (zsh features)

Goal: smart input handling in vt itself, so rc users get tab-complete, history, vi-mode regardless of which client they attached from.

### Task 5.1: Raw mode + line editor scaffold

**Files:**
- Create: `src/vt/lineedit.c`
- Modify: `src/vt/srv.c`, `src/vt/session.c`

1. Add `consctl` write commands: `rawon`, `rawoff`, `edit on`, `edit off`
2. When `edit on`: vt intercepts keystrokes before they reach rc. Buffers a line locally. Renders the partial line into the cell buffer (so all clients see it). On Enter, sends the completed line as one chunk to rc.
3. Default: `edit on` for interactive sessions
4. Keys handled: left/right arrow, backspace, home/end, Ctrl-A/E/U/W/K, Enter
5. **Verify pass:** type `hello`, press Left arrow 3 times, type `X` → buffer shows `heXllo`, Enter sends `heXllo\n` to rc
6. Commit: `vt: line editor`

### Task 5.2: History

**Files:**
- Create: `src/vt/history.c`
- Modify: `src/vt/lineedit.c`, `src/vt/srv.c`

1. Per-session history ring buffer (default 1000 lines)
2. Up/Down arrows scroll through it
3. Expose `history` file (read = full history, append-write = add entry)
4. Persist to `$home/lib/vt/history-<sessionname>` on session close
5. **Verify pass:** type and run 3 commands, Up arrow cycles through them
6. Commit: `vt: command history with persistence`

### Task 5.3: Tab completion

**Files:**
- Create: `src/vt/complete.c`
- Modify: `src/vt/lineedit.c`

1. On Tab: parse current line, identify last word
2. Completion sources (in order):
   - Filename completion (relative to session's pwd — we need to track that; either via `pwd` file periodically or by sniffing `cd` commands client-side… punt: filename completion against `$home`)
   - `/bin` and `/$objtype/bin` for first-word command completion
3. If 1 match: insert it. If N: display below prompt, cycle on repeated Tab.
4. **Verify pass:** type `ec<Tab>` → expands to `echo `
5. Commit: `vt: tab completion`

### Task 5.4: Prompt rendering with color

**Files:**
- Create: `lib/vt/promptrc` (rc snippet user sources)

1. Document how to make rc's prompt colorful now that vt parses SGR:
   ```rc
   prompt=(`{echo -n "\033[32mterm%\033[0m "} '	')
   ```
2. Add to wiki: `wiki/concepts/vt-architecture.md` "user setup" section
3. Commit: `vt: colorful rc prompt recipe`

---

## Phase 6 — Multi-Session & Persistence

### Task 6.1: Session lifecycle commands

**Files:**
- Modify: `src/vt/srv.c`
- Create: `src/vt/sessmgr.c`

1. `ctl` file at root (not per-session) accepts:
   - `new <name>` — create new session, spawn rc
   - `kill <name>` — terminate session, kill rc
   - `rename <old> <new>`
   - `list` — write to ctl is rejected; read returns list
2. Sessions identified by name (string) not just numeric id
3. **Verify pass:**
   - `echo "new work" | 9p write /srv/vt/ctl`
   - `9p ls /srv/vt` → `main work`
4. Commit: `vt: multi-session management`

### Task 6.2: vt-attach session picker

**Files:**
- Modify: `src/vt-attach/main.c`

1. `vt-attach` (no args) lists sessions and prompts
2. `vt-attach <name>` attaches directly
3. `vt-attach -n <name>` creates if missing, then attaches
4. Commit: `vt-attach: session picker`

### Task 6.3: Restart-survival via /srv

**Files:**
- Modify: `src/vt/main.c`

1. vt posts itself to `/srv/vt` so the namespace survives independent of who started it
2. If vt crashes: child rc processes die (we don't try to inherit them across restarts — that's Plan 9 hard mode, skip for v1)
3. Document this limitation in wiki
4. Commit: `vt: /srv mount survival`

---

## Phase 7 — Scrollback as a File

### Task 7.1: `scroll` file exposes ring buffer

**Files:**
- Modify: `src/vt/cells.c`, `src/vt/srv.c`

1. Ring buffer holds last N lines (default 10000) of historical cell data
2. `scroll` file: read returns all scrollback as UTF-8 text (one line per buffer row)
3. `scroll/raw` (binary): same but with full cell attributes
4. **Verify pass:** run `for(i in `{seq 1 100}) echo $i` → `9p read /srv/vt/main/scroll | wc -l` → 100
5. Commit: `vt: scrollback file`

### Task 7.2: mxio scrollback UI

**Files:**
- Modify: `src/mxio/vtclient.c`

1. Mouse wheel up/down: scroll the visible viewport over the scrollback
2. Any keypress: jump back to live tail
3. Status indicator on titlebar when scrolled away from live
4. Commit: `mxio: vt scrollback navigation`

---

## Phase 8 — Polish & Documentation

### Task 8.1: README + man pages

**Files:**
- Create: `src/vt/vt.man` (man page in troff for `man(1)`)
- Create: `src/vt-attach/vt-attach.man`
- Modify: `README.md`

1. Standard 9front man page format — NAME, SYNOPSIS, DESCRIPTION, FILES, EXAMPLES, SEE ALSO, BUGS
2. Install to `/sys/man/1/vt` and `/sys/man/1/vt-attach`
3. README section in repo root explaining vt's role
4. Commit: `vt: man pages and README`

### Task 8.2: Autostart integration

**Files:**
- Modify: `~/lib/profile` (in VM) — document
- Modify: `wiki/concepts/build-toolchain.md`

1. Add `vt &` to riostart so it's always running
2. Mxio launches with `-V` by default
3. Document the user-level autostart recipe in wiki
4. Commit: `vt: autostart wiring`

### Task 8.3: Final wiki update

**Files:**
- Modify: `wiki/concepts/vt-architecture.md` — mark status: done
- Modify: `wiki/concepts/mxio-design.md` — note vt integration
- Modify: `wiki/index.md`, `wiki/log.md`

1. Mark vt as v1.0 in wiki
2. List known limitations: no PTYs (so no htop), single-host (no remote 9P), no encryption layer
3. Commit: `wiki: vt v1.0 release notes`

---

## Verification Milestones

After each phase, the system should be observably better:

| Phase | Verifiable demo |
|-------|----------------|
| 1 | `9p ls /srv/vt/1` shows files, echo loop works |
| 2 | `echo -e "\033[31mhi\033[0m" | 9p write /srv/vt/1/cons` results in red cells in buffer |
| 3 | mxio -V window renders rc output in color |
| 4 | SSH from Mac, `vt-attach main` gives full color rc session in iTerm2 |
| 5 | Tab completion works in rc, Up arrow shows history |
| 6 | Multiple named sessions, detach/reattach preserves state |
| 7 | Scroll wheel in mxio reveals scrollback |
| 8 | Fresh boot of VM gives working vt out of the box |

---

## Risks & Mitigations

**Risk: lib9p complexity**
The Tree API has gotchas (qid management, clunk semantics). Mitigation: Task 1 is a full standalone milestone; if lib9p turns out to be wrong tool, fall back to raw 9P via libthread + lib9p's lower-level `Srv` struct.

**Risk: rc's stdin/stdout don't behave like a TTY**
rc doesn't check isatty(), but some Plan 9 programs do (`/dev/cons` is a known file, they read from it). Mitigation: ensure our bind of cons replaces /dev/cons cleanly; test with `read` and `sam -d`.

**Risk: VT100 parser bugs from incomplete CSI handling**
Some programs emit sequences we don't recognize and we might consume bytes incorrectly. Mitigation: parser bails to Ground state on any unrecognized byte sequence; log unknown sequences to a debug channel for inspection.

**Risk: Performance**
Naive cell-diff might thrash with full-screen redraws (e.g. `clear`). Mitigation: batch diffs, coalesce same-cell writes within a 16ms window before emitting.

**Risk: SSH dumb-mode rendering glitches**
Mac terminals (iTerm2, Terminal.app, Ghostty) all interpret VT100 slightly differently. Mitigation: test against iTerm2 (primary), Terminal.app (fallback). Document known glitches.

**Risk: Scope creep into a full terminal emulator**
This is a real risk — we'd love mouse reporting, sixel graphics, 256-color, true color, etc. Mitigation: v1 is 16 colors + cursor + erase. Everything else gets a `wiki/concepts/vt-future.md` page and waits for v2.

---

## Estimated effort

| Phase | LOC | Effort |
|-------|-----|--------|
| 0 | ~500 (wiki + scaffold) | 2-3h |
| 1 | ~600 | 1 day |
| 2 | ~1500 (the parser is the big one) | 2-3 days |
| 3 | ~400 | 1 day |
| 4 | ~600 | 1 day |
| 5 | ~1100 | 2 days |
| 6 | ~300 | half day |
| 7 | ~300 | half day |
| 8 | ~200 (docs) | half day |
| **Total** | **~5500** | **~10 days focused work** |

---

## Handoff Options

After this plan is saved, you can:

1. **Subagent-driven (same session)** — I delegate each task to a subagent that implements + commits + reports back. Sequential, takes hours.
2. **Parallel session (new chat)** — open a fresh session, point it at this plan + the `superpowers:executing-plans` skill, work tasks one at a time with full attention.
3. **Mix** — you do Phase 0-1 manually to feel out the lib9p ergonomics, hand off Phase 2 onward to subagents once the foundation is solid.

My recommendation: option 3. Phase 0-1 is small but exposes whether lib9p's Tree API fits or whether we need to drop to lower-level 9P. Once that's known, the rest parallelizes well.
