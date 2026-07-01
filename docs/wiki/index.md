# Wiki Index

> plan9-winxp knowledge base. Every wiki page listed with one-line summary.
> Last updated: 2026-07-01 | Total pages: 41

## Concepts
- [[draw-api]] — Plan 9 /dev/draw interface: how programs get a canvas
- [[rio-architecture]] — Rio WM internals: wind.c, xfid.c, mouse/keyboard handling
- [[mxio-design]] — mxio design + verified status: titlebars, hit-testing, right-click menu
- [[winxp-visual-spec]] — WinXP Luna color values, dimensions, button shapes
- [[xena-panel-design]] — Taskbar daemon — verified v0.1 with Start, window list, clock + launcher
- [[build-toolchain]] — Cross-build Mac→9front: hget over HTTP, mk, rc shell gotchas
- [[python3-on-plan9]] — CPython 3.11 port strategy + parity harness: state of the art (jas 2.7, no Py3 today), APE env recon (no dlfcn → static modules, no pthread → Plan 9 thread backend, only 12 APE utils → bypass configure), reference baseline 38,259 testcases, Hermes Rust-dep blocker
- [[testing-harness]] — QMP mouse/keyboard, screendump+vision loop, listen1 pitfalls
- [[browser-webview-plan9]] — Browser engine + zero-native WebView runtime on Plan 9: phased build plan, engine comparison, WPT test strategy
- [[netsurf-install]] — How to build & install NetSurf on 9front: verified procedure, ~52 MB binary, what works (HTML5+CSS2+images+HTTPS) vs what doesn't (modern JS), pitfalls, screenshots
- [[9fans-ecosystem]] — 9fans GitHub org: plan9port, drawterm, go/draw, srv9p — relevance per component
- [[9front-release-status]] — Current 9front release **GEFS SP1 (2026-01-24, ID 11554)**: timeline, what changed, what's relevant for plan9-winxp (namespace perf fixes, libdraw cleanup, shell-as-#!), VM image source
- [[gefs]] — Good Enough File System: 9front's new default since 2025-01. B-tree CoW, crash-safe-without-fsck, timed snapshots, no separate venti. Comparison to cwfs64x/fossil+venti
- [[myrddin-language]] — Ori Bernstein's small typed systems language with Plan 9 amd64 target. ADTs + pattern matching + generics + no GC. Maintenance mode since 2022. When to reach for it (almost never) and why it's interesting (only modern typed lang on Plan 9)
- [[pac9]] — Package manager for 9front: `pac9 install <name-or-git-url>`. Thin rc wrapper over git9 + `mk install`; curated registry with short names, source and prebuilt-tarball package kinds (python9/node9/zig9 install prebuilt)
- [[git9]] — Plan 9 native git that ships in 9front. 9P file server at `$repo/.git/fs`, no index/staging, three states only. Daily-driver flow, install path, "state-as-9P-fs" design reference for [[xena-panel-design]] and pi9 tools
- [[oridb-ecosystem]] — Ori Bernstein's GitHub: which repos matter (git9, mc, plan9port fork), which don't (Myrddin libs, personal forks). Tier table by relevance to plan9-winxp. Complement to [[9fans-ecosystem]]
- [[vt-architecture]] — vt console daemon: collapses st+tmux+zsh into one Plan 9 9P service
- [[vt-9p-namespace]] — vt's wire interface: ctl, cons, cells, size, scroll, title
- [[vt100-parsing]] — Which VT100 escape sequences vt understands (and which it ignores)
- [[vtwin-client]] — libdraw frontend that mounts /srv/vts, paints cells under mxio's titlebar, forwards keystrokes
- [[vtwin-typography]] — Font story: vtwin honours `$font`, fontsrv synthesises TTFs (Inconsolata/JetBrains Mono), kills the "libdraw can't do nice fonts" myth
- [[llm-reverse-engineering]] — SOTA landscape May 2026: decompilation models (LLM4Decompile, Nova, SLaDe), RE plugins (Gepetto, aiDAPal, Sidekick, r2ai), MCP servers (GhidraMCP, ida-pro-mcp), autonomous harnesses (Naptime, AIxCC/Atlantis), pitfalls
- [[llm-porting-workflow]] — Practical /goal-driven pipeline for porting C software to Plan 9: **port the design not the API** (Plan-9 mindset checklist, redesign-beats-translate table, when-not-to-port heuristics), three modes (source / binary / spiritual reimpl), executor/judge/oracle wiring, first-port candidates, Plan-9-specific failure modes
- [[pi9-architecture]] — pi9: plan9-native LLM agent (Go + Bubble Tea, runs in vts/vtwin). Heir to tinyxena. Phasing, tool palette, on-disk layout, blocker analysis (charmbracelet/x/term needs plan9 shim — verified)
- [[pi9-phase1]] — pi9 Phase 1 **DONE**: Bubble Tea running natively in vts session on 9front amd64 with Luna palette, resize polling, clean quit-on-q. Full shim set (tty_plan9, signals_plan9, signals_other, tea.go patch) verified end-to-end via cellstream
- [[pi9-phase2]] — pi9 Phase 2 **DONE**: Chat UI (header + scrollback + input box + status), OpenRouter-compatible SSE streaming client, plan9-aware TLS, raw mode enabled in vts. Verified end-to-end with mock server in VM
- [[pi9-phase3]] — pi9 Phase 3 **DONE**: Tool calling (read_file, write_file, run_rc) with full streaming tool_call assembly, agent loop (stream→tools→stream until done), inline tool block rendering (cyan ▸ markers). Verified end-to-end: real `run_rc("ls /tmp")` executed against 9front filesystem
- [[pi9-phase4]] — pi9 Phase 4 **DONE**: Sessions (autosave JSON, resume on launch), skills (on-demand markdown via read_skill), memory (append via remember, re-injected into system prompt). On-disk layout $home/lib/pi9/{sessions,skills,memory.md}. Verified end-to-end: turn persisted, memory written, second launch resumed prior conversation
- [[pi9-phase5]] — pi9 Phase 5 **DONE**: Plan9-native tools (plumb, hget, walk, ns, bind, mount). Pi9 introspects its own namespace, walks file trees, fetches via hget, plumbs to handlers, reshapes its namespace via Bind/Mount syscalls. Verified: `ns({filter:"/srv"})` returned 13 real mount lines; `walk` and `hget` work against live 9front fs. This is where pi9 becomes plan9-native
- [[plan9-namespaces-for-agents]] — Why per-process namespaces fundamentally change what an LLM agent can do: self-diagnosis, capability scoping via construction (not restriction), service composition as file I/O, remote=local with no special tooling, reproducible environments. The killer-feature page for pi9.
- [[pi9-for-porting]] — Honest assessment: do pi9's namespace tools help when porting software to Plan 9? Sandboxed experiments + read-only-source-with-writable-overlay + APE stitching are real wins; conceptual translation work (epoll→channels, signal→note) is unchanged. Proposes sandbox/overlay_diff/srvssh_mount tools + port-c-to-plan9 skill. Status: speculative until we test on a real port.
- [[pi9-phase6]] — pi9 Phase 6 **DONE**: Polish. Stale-frame stacking from Phases 2-5 finally diagnosed (lipgloss border + bubbletea diff renderer + missing-glyph squares in libdraw font) and fixed via hand-drawn ASCII borders + fitRow/fitBlock width enforcement + tea.ClearScreen on turn boundaries. 9 slash commands wired (/help, /clear, /new, /save, /sessions, /memory, /skill, /model, /quit). Local turns excluded from LLM context.
- [[pi9-phase7]] — pi9 Phase 7 **DONE**: Desktop integration. Start menu → Pi9 → new vtwin window. `new-pi9` rc helper creates fresh vts session per launch (named `pi9-$pid` for concurrency). Multiple pi9 instances supported. Color profile pinned with `termenv.ANSI256` so Luna palette renders without `$TERM`. Verified: 2 concurrent pi9-* sessions visible on the desktop.
- [[pi9-phase8]] — pi9 Phase 8 **DONE**: Config file ($home/lib/pi9/config) + auto-run via new-pi9 + render fix. First launch writes template + helpful exit message. Env vars still override config. /config slash command (api_key masked). Width-1 trick fixes the "each keystroke creates new line" bug — root cause: vts auto-wraps on last cell, triggering scroll that desynchronizes bubbletea's diff renderer.
- [[pi9-phase9]] — pi9 Phase 9 **DONE**: DECAWM in vts, scrollback navigation, proper install path. Replaces the width-1 trick with a real CSI ?7 h/l implementation (vts side ~25 LOC). Pi9 emits DECAWM-off at startup. Scrollback nav via pgup/pgdn/shift+up/shift+down/ctrl+end with "scrolled N rows" status indicator. Install: `mkfile` with install/uninstall/clean/nuke rules + `pi9-install` one-shot script. Pi9 now lives at `/$cputype/bin/pi9`, discoverable via `whatis pi9`. Live OpenRouter test deferred — no key available.
- [[pi9-phase10]] — pi9 Phase 10 Session 1 **DONE**: Multi-provider auth (11 providers). Provider interface, ProviderForModel routing by model name, multi-provider auth.json (pi.dev-compatible shape). Anthropic native /v1/messages support. Interactive `/login` picker with masked key entry, deep-links to provider key-mint URLs. `/logout <provider>` or all. Sessions 2 (OAuth for Claude Pro/Max) + 3 (ChatGPT/Copilot OAuth) parked — each ~4-6 hrs because plan9 browser-launch via plumber is its own piece of work.
- [[pi9-phase10-session2]] — pi9 Phase 10 Session 2 **DONE**: Anthropic OAuth (Claude Pro/Max). PKCE + local callback server (port 53692) + browser launch via `plumb -d web`. Token auto-refresh (5min skew). Picker shows `(subscription available)` badges + two-step auth-method choice (subscription vs API key). Cloned pi.dev reference (earendil-works/pi) for exact OAuth constants — saved hours of guesswork. Anthropic provider auto-detects `sk-ant-oat` OAuth tokens and switches to Bearer + Claude Code identity headers. UI verified in VM; live OAuth flow needs VM reboot for QEMU port-forward (already applied to launch scripts).
- [[pi9-phase10-session3]] — pi9 Phase 10 Session 3 **PARTIAL DONE**: ChatGPT Plus/Pro (Codex) OAuth + GitHub Copilot OAuth. Copilot fully wired via device flow (no callback server needed — perfect for plan9). Codex auth-only — login persists tokens but using them would need OpenAI Responses API port (~600 LOC, deferred). Provider list now has 3 subscription badges: Anthropic + OpenAI + GitHub Copilot. Copilot uses dynamic base URL extracted from token's `proxy-ep` field + VS Code identity headers. Picker hardening: OAuth-only providers (Copilot) skip the subscription/api-key choice and go straight to OAuth.
- [[pi9-phase10-session4]] — pi9 Phase 10 Session 4 **PARTIAL DONE**: Codex Responses API MVP (text streaming works on paper, tool calls/multi-turn-with-tools/reasoning dropped — documented) + arrow-key fix in vtwin (plan9 private-use runes Kup/Kdown/etc translated to xterm `\x1b[A/B` so bubbletea/ncurses/vim apps inside vts now respond to arrows). The vtwin fix affects all terminal apps, not just pi9.
- [[pi9-phase11]] — pi9 Phase 11 **PARTIAL DONE**: Three user-reported quality bugs. Mouse-wheel scroll: vtwin translates rio wheel ticks (button mask 8/16) to xterm SGR sequences (ESC[<64/65;X;YM); pi9 enables mouse mode + handles MouseWheelUp/Down. Font flag: vtwin learned `-f <path>` to override $font (which is broken-by-design because /env is copy-on-write per process). Resize reflow: code path was already in place from Phase 1/2 — likely worked all along, user's prior report was against pre-vtwin-rebuild state. Mouse-wheel can't be verified from QMP (QEMU HMP doesn't simulate PS2 wheel cleanly); font flag verified with Lucida 9pt in VM.

## Entities
(none yet)

## Decisions
(none yet)

## References
(none yet)
