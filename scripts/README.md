# scripts — host-side helpers for driving the 9front VM

Tools to push code into the VM, build/test inside, and verify visually
without booting Jump Desktop. All assume `~/Projects/plan9-agent/` has
the VM running (boot-9front.sh) and `aux/listen1 -t tcp!*!17010 /bin/rc -i`
is up (started by `riostart`, listens on host :1717 via SLIRP fwd).

## VM-side execution

| Script              | Use                                                  |
|---------------------|------------------------------------------------------|
| `vm-rc 'cmd'`       | Run a single rc command in the VM, print stdout.     |
| `echo 'cmd' \| vm-rc -` | Read stdin (multi-line scripts via heredoc).      |

Quirks:
- `cmd1 && cmd2` returns only the LAST line's output. Use `;` to chain.
- rc redirect is `>[2]/dev/null`, NOT `2>/dev/null`. Easy slip from bash.
- Single-quote regex chars (`>`, `<`, `|`) so bash doesn't eat them
  before they reach rc.

## NetSurf install (one-time, manual)

These are designed to be `hget`-pulled into `/tmp/` then run inside the VM.
See `wiki/concepts/netsurf-install.md` for the full procedure.

| Script              | Purpose                                            |
|---------------------|----------------------------------------------------|
| `run-fetch.rc`      | `cd ~/nsport; fetch clone http` (clones 18 sub-libs) |
| `run-mk.rc`         | `cd ~/nsport; mk` (full build, 18-22 min on TCG)   |
| `run-install.rc`    | `cd ~/nsport; mk install` (cp binary + resources)  |
| `launch-netsurf.rc` | `window -r ... netsurf URL` (start a fresh window) |

## Progress polling

| Script               | Reads                                                 |
|----------------------|-------------------------------------------------------|
| `vm-fetch-status`    | `==> name` count from `/tmp/nsfetch.log` + done file. |
| `vm-build-status`    | `Building:` stages + `pcc -` calls + done sentinel.   |

## pi9

`build-pi9.sh` is the Mac-side cross-build for pi9. See pi9-phase1/2/3
wiki pages.
