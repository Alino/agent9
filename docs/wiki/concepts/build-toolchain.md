---
title: Build Toolchain
created: 2026-05-15
updated: 2026-05-16
type: reference
tags: [toolchain, cross-compile, qemu, plan9, build]
status: done
---

# Build Toolchain

How to compile C code for 9front amd64 from macOS Apple Silicon.

## Workflow (verified)

We compile DIRECTLY inside the VM. macOS cross-compiling Plan 9 C is painful due
to Plan 9 ABI and include headers. Working flow:

```
Mac Studio
  ├─ python3 -m http.server 8765 --bind 0.0.0.0   ← serve sources
  └─ QEMU VM (9front amd64, TCG emulation)
       ├─ hget http://10.0.2.2:8765/FILE          ← pull sources
       └─ /sys/src/cmd/mxio/
            └─ mk install                          ← Plan 9 make
```

`10.0.2.2` = QEMU SLIRP gateway, always points to the host.

### CRITICAL: HTTP server must bind to 0.0.0.0

```bash
python3 -m http.server 8765 --bind 0.0.0.0   # ✅ IPv4
python3 -m http.server 8765                  # ❌ default on macOS binds ONLY to ::
```

Without `--bind 0.0.0.0` the server binds only to IPv6 (`::`) and `hget` from the VM
(IPv4 via SLIRP) gets connection refused. Fails SILENTLY — `hget` creates a
**0-byte destination file** which then gives a confusing build error.

Symptom: `/bin/hget:92: < can't open: /mnt/web/0/body: 0 No status`

### Pushing files Mac → VM

Via QMP `typeln` into a window in rio (or via aux/listen1 TCP shell):
```bash
python3 qmp.py typeln "hget http://10.0.2.2:8765/wind.c > /sys/src/cmd/mxio/wind.c"
```

Batch push of all changed files:
```python
for f in ["wind.c", "rio.c", "xfid.c", "data.c"]:
    subprocess.run(["python3", "qmp.py", "typeln",
                    f"hget http://10.0.2.2:8765/{f} > /sys/src/cmd/mxio/{f}"])
    time.sleep(1.5)
```

### Booting the VM

```bash
cd ~/Projects/plan9-agent
BOOT=c ./boot-9front.sh &
# Wait ~20s for boot
python3 qmp.py screendump /tmp/screen.ppm
```

BOOT=c = boot from disk. At bootargs/user prompts send Enter via `qmp.py key ret`.

## Phase 0: Install 9front to disk

The disk starts empty (196KB). Requires a single install session:

```bash
# Boot from ISO
BOOT=d ./boot-9front.sh &
sleep 30
python3 qmp.py screendump /tmp/s0.ppm   # check boot screen

# Drive the install
python3 qmp.py typeln "inst/start"
sleep 5
python3 qmp.py screendump /tmp/s1.ppm

# inst/start interactive wizard:
# sdN = select disk (virtio = sd00)
# fs = hjfs (simpler)
# copydist = waits 10-20 min (469MB ISO → disk)
```

Alternative: fully automated install via qmp.py sendkey.

## Plan 9 Build System (mk)

Plan 9 uses `mk` (not make). Similar syntax, but different.

```mkfile
# /sys/src/cmd/mxio/mkfile
</$objtype/mkfile

TARG=mxio
OFILES=\
    rio.$O\
    wind.$O\
    xfid.$O\
    rioaux.$O\

HFILES=dat.h fns.h

</sys/src/cmd/mkone
```

`$objtype` = `amd64` on 64-bit 9front.
`$O` = object suffix (`.6` for amd64, `.8` for 386).

```rc
mk             # compile
mk install     # copy binary to /bin/
mk clean       # clean up
```

## Plan 9 C vs ANSI C

Key differences:
- `#include <u.h>` must be the FIRST include (defines ulong, uchar, etc.)
- `#include <libc.h>` instead of `<stdlib.h>` + `<string.h>`
- `#include <draw.h>` for libdraw
- No `printf` — use `print()` or `fprint(2, ...)`
- No `malloc/free` — use `mallocz()`, `free()` from libc.h
- `nil` instead of `NULL`
- Strings: Plan 9 uses UTF-8, `Rune` for Unicode codepoints
- Thread library: `<thread.h>` + `proccreate()` instead of pthreads

## plan9port (alternative for prototyping)

On Mac (or Linux) you can install plan9port:
```bash
brew install plan9port
```

Then compile and test Plan 9 C code natively:
```bash
9c wind.c && 9l -o wind wind.o
```

**Limitation:** plan9port graphics go via X11/Quartz/devdraw — not through the 9front kernel.
Good for testing draw logic, but production builds must be done in the VM.

## rc shell gotchas (Plan 9 != bash)

Plan 9 `rc` shell has DIFFERENT syntax from bash. Important differences when building:

| bash | rc | notes |
|------|----|-------|
| `cmd 2>&1` | `cmd >[2=1]` | merge stderr into stdout |
| `cmd > out 2>&1` | `cmd >out >[2=1]` | two redirects |
| `cmd > out 2>> log` | `cmd >out >>[2]log` | append stderr |
| `cmd1 && cmd2` | `cmd1 && cmd2` | works |
| `cmd1 \|\| cmd2` | `cmd1 \|\| cmd2` | works |
| `$VAR` | `$var` | case-sensitive, no quoting magic |
| `==` (string compare) | `~ a b` (match operator) | different syntax |
| `kill -9 PID` | `echo kill > /proc/PID/note` | no `kill -9` |

**COMMON BUG:** Via QMP `typeln "cmd 2>&1"` rc parses `2` as a target for `mk`
and throws: `mk: don't know how to make '2'`. Always use `>[2=1]`.

### Killing processes

```rc
kill listen1 | rc           # kill by name; pipe to rc = runs echo>note for each PID
echo kill > /proc/SOMEPID/note
```

## Sync Script for Build Pipeline

Real build cycle after an edit:
```python
# Push changed sources
for f in ["wind.c", "rio.c"]:
    typeln(f"hget http://10.0.2.2:8765/{f} > /sys/src/cmd/mxio/{f}")
    sleep(1.5)

# Build (note rc redirect syntax!)
typeln("cd /sys/src/cmd/mxio && mk install >/tmp/bld.log >[2=1]")
sleep(60)   # build takes ~30-40s, give it margin

# Verify
typeln("tail -10 /tmp/bld.log")
typeln("ls -l /amd64/bin/mxio")
typeln("date")
# screendump + vision_analyze to verify the timestamp is fresh
```

## Editors in the VM

9front includes **acme** and **sam** by default. Vim is not part of the base install.

### 9vim (Vim 7.4 on Plan 9)

Most current port: https://git.sr.ht/~shurizzle/9vim (active 2024)
Fork chain: stefanha (2008, Vim 7.1) → telephil9/vim (plumbing, mouse) → shurizzle (custom event loop, Vim 7.4.2367)

Install inside the VM:

```rc
git clone https://git.sr.ht/~shurizzle/9vim
cd 9vim/src
mk -f mkfile install
```

Config: `$home/lib/vimrc` (not ~/.vimrc)

Recommended vimrc for Plan 9 C dev:

```vim
set grepprg=g\ $*              " Plan 9 grep (command 'g')
set errorformat+=%f:%l\ %m     " mk error format
set keywordprg=:Man            " K opens Plan 9 man page
```

Mk quickfix — `:make` works, errorformat parses Plan 9 compiler output.

Known limitations: non-ASCII input is partial (leader key and some combinations don't work).

### acme (native)

Recommended for longer work on Plan 9 — integrated with the plumber, mouse-driven.
9fans/go acme-lsp + clangd gives LSP (autocomplete, go-to-definition) directly in acme.

## See Also

- [[mxio-design]] — what we compile
- [[rio-architecture]] — original Rio build (reference for mkfile)
- [[draw-api]] — include headers and linking
- [[testing-harness]] — QMP/screendump/vision loop to verify after build
- [[9fans-ecosystem]] — acme-lsp for LSP in acme
