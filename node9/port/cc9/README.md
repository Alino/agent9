# node9 on cc9 (clang/LLVM) instead of kencc

A second build of the same runtime: identical `lib/boot.js`, identical native bindings,
different C toolchain. `port/plan9` builds `qjs` with kencc/APE; this builds `qjs-cc9`
with clang through cc9 (ELF -> Plan 9 a.out).

```sh
bash port/cc9/build.sh          # -> port/cc9/_out/qjs-cc9
```

Prereqs: `cc9/lib/libcc9cxx.a` + `libcc9m.a` (`cc9/host/build-runtime.sh`), OpenSSL for
cc9 (`ssl9/_out/lib{ssl,crypto}.a`), a cc9 `libz.a`, and a pristine quickjs-ng tree
(`QJS_SRC`).

## What it buys

| | kencc `qjs` | cc9 `qjs-cc9` |
|---|---|---|
| interpreter loop (`test/qjsbench`) | 1.32 s | **0.40 s** (3.3x) |
| JS compute in the full runtime | 1.43 s | **0.51 s** (2.8x) |
| parser | — | ~10% slower |
| `pi --version` | 7.68 s | 6.49 s |
| one `pi -p` turn | 9.02 s | 7.67 s |
| TLS certificates | not verified (libsec) | **verified** (OpenSSL, PKIX + hostname) |

The interpreter gap is `DIRECT_DISPATCH`: it needs labels-as-values, which kencc does not
have, so the kencc build runs the bytecode loop on a `switch`. pi gains far less than 3x
because its startup is module resolution, I/O and parsing — see `../../PI.md`.

## Differences from the kencc build

- **pristine engine sources** — none of `port/plan9/patch.sh`'s kencc workarounds are
  needed (no NaN-compare guards, no alloca shim, no `intptr_t` rewrite, no arg-hoist mask).
  build.sh does apply the *two upstream quickjs-libc event-loop fixes* patch.sh carries,
  because those are engine bugs, not toolchain workarounds. Skipping them cost a day:
  streamed responses were dispatched only when an unrelated fd went ready.
- **crypto/TLS/zlib from OpenSSL + zlib** (`n9_sec_ossl.c`) rather than Plan 9 libsec/libz.
  TLS runs over **memory BIOs**: `SSL_read` on a blocking Plan 9 fd would park the whole
  event loop, so the socket layer feeds ciphertext in (`tlsFeed`) and pulls it out
  (`tlsPull`) while every SSL call touches memory only. boot.js switches on
  `__n9native.tlsHandles`.
- **real `poll(2)` and threads** from the cc9 runtime, no APE `select` shim.

## Status

Side by side with the kencc build — `qjs` stays the default, `qjs-cc9` installs alongside.
Same test results on both: `runtime-test.js` 51/51, `esm-interop-test.mjs` 15/15,
`fetchtest.js` 48/48 (real HTTPS), plus npm install and a real pi turn.

Getting there turned up one cc9 **runtime** bug and three node9 socket bugs, all fixed:

- `cc9_poll_owned()` keyed ownership on a live reader thread, but that thread exits the
  moment the kernel hands it EOF — often with the tail of the transfer still in the ring.
  `read()` then went back to the kernel, got 0, and dropped those bytes: a 220 KB body
  arrived as 4 KB. Gate: `cc9/test/pollring_gate.c` case 3.
- `Socket.end()` wrote `hangup` to the TCP ctl fd as a "half-close". Plan 9 has no
  half-close — that tears the connection down, so ending the request raced the response.
- `Socket.destroy()` cleared `_fd` before the branch that removes the read handler, so an
  aborted TLS fetch left a handler armed on a closed fd and the process never exited.
- the response parser treated an unparseable chunk size and a mid-body close as a clean
  end of body, which is what let all of the above look like short reads instead of errors.
