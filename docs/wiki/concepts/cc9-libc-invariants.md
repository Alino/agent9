---
title: cc9 libc invariants
created: 2026-10-08
updated: 2026-10-08
type: concept
tags: [toolchain, plan9, rc, fs, done]
---

# cc9 libc invariants

Rules the cc9 runtime (`cc9/runtime/`) depends on across files. See
[[build-toolchain]] for how the runtime is built and [[pac9]] for how it
ships.

## Allocator: free a block by its own header

`n9libc.c` has three layers. Blocks up to 2048 bytes come from per-thread
size-class caches and `g_class[]`. Larger blocks come from the power-of-two
lists in `g_big[]`. Both carve fresh memory from a 1 MB bump region. The
K&R free list (`malloc_u`, `free_u`) is only the fallback for requests the
bump region cannot serve.

Two facts follow. The K&R list head is `NULL` until `malloc_u` runs once, so
`free_u` on a block before then crashes. And `malloc()` never reads the K&R
list on its fast paths, so a block given to `free_u` is not reused by
ordinary allocations.

So every block must go back through `free()` using its own header, which
puts it on its class or `g_big` list. An `aligned_alloc` block has a
sentinel at `p[-1]` and its malloc base at `p[-2]`. `free()` and `realloc()`
recover the base and call `free(base)`. Do not call `free_u` directly.

## /env values: a terminating NUL means the value is exact

Plan 9 keeps the environment as files in `/env`. rc writes a variable as
NUL-terminated words, `"w\0"` or `"w1\0w2\0"`. Raw tools write bytes as is,
so `echo x > /env/X` stores `"x\n"`.

`getenv()` in `fs.c` reads the value and looks at the last byte.

- Ends in NUL: the value is exact. Return it verbatim up to that NUL,
  newlines included.
- Does not end in NUL: a raw writer produced it. Strip trailing newlines.
- Empty file: return `""`. The defaults for `PATH` and `SHELL` apply only
  when the file does not exist.

`setenv()` and the envp writer in `execve()` write value plus NUL. Any new
`/env` writer in the runtime must do the same, or the child will see its
trailing newlines stripped.

`env_path` accepts only plain file names: not empty, not `.` or `..`, no
`/`, no `=`. Anything else is `EINVAL`, and a name longer than the path
buffer is `ENAMETOOLONG`.

The kernel caps a value at `Maxenvsize` (about 16 KB). Larger writes fail
at the syscall.

## Exit status: `cc9exit=N`

Plan 9 processes exit with a string. `cc9_exitstr(n)` encodes a nonzero
code as `"cc9exit=N"`, and `waitpid()` in `posix_llvm.c` scans the child's
status for that marker. A bare number from a native program (rc's `exit 3`)
is also accepted. Any other non-empty status is reported as a signal death.
Both `exit(n)` and a `return` from `main` must go through `cc9_exitstr`.

## errno: one errstr table for every wrapper

`cc9_errno_from_errstr_or(dflt)` in `fs.c` maps Plan 9 errstr substrings to
errno values. Every syscall wrapper in the runtime uses it, so a new entry
affects all of them. Order matters: `"does not exist"` must come before
`"exists"`. Strings with no entry get the caller's default.

## Notes interrupt syscalls

The note handler in `crt0.c` runs the registered signal handler and
resumes the process. The interrupted syscall returns `-1` with errstr
`"interrupted"`. `poll()` reports this as `EINTR`; `read` paths retry. A
`SIG_IGN` signal is still a delivered note, so it still interrupts.

## fork() keeps the malloc lock state

`fork()` is `rfork(RFPROC|RFFDG|RFENVG)` with no atfork hooks, so the child
inherits `malloc_held` as it was. If another thread held the lock at fork
time, the child's first `malloc` blocks forever. Code between `fork` and
`exec` should avoid allocating where it can.
