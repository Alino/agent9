# qjsbench — is the engine slow because of kencc?

One benchmark, compiled by both toolchains, so the numbers compare like for like:

- `bench.c FILE [n]` — compile a file `n` times (**parser**), and round-trip its
  bytecode once (`JS_WriteObject` → `JS_ReadObject`).
- `bench.c -run FILE` — execute a file (**interpreter**).

No internal clock: wall time is measured outside with rc's `time`, so two different
`gettimeofday` implementations can't skew the comparison. A stub module loader hands back
an empty module for any specifier, because compiling a module still resolves its imports.

## Building

```sh
# cc9 (clang -O2 -> ELF -> Plan 9 a.out); needs cc9/lib/libcc9cxx.a + libcc9m.a
cp /path/to/quickjs-master/{quickjs,libregexp,libunicode,dtoa}.c .   # pristine sources
cp /path/to/quickjs-master/*.h .
./build-cc9.sh                       # -> qjsbench-cc9
```

```rc
# kencc, linked against the objects node9 is actually built from
cd /usr/glenda/node9/work3
pcc -c -I. -DPLAN9 -D__DJGPP -DNO_TM_GMTOFF '-DJS_NAN_BOXING=0' '-D_POSIX_SOURCE' \
    '-D_BSD_EXTENSION' '-D__STDC_NO_ATOMICS__=1' -DNDEBUG bench.c
pcc -o qjsbench-kencc bench.6 quickjs.6 libregexp.6 libunicode.6 dtoa.6
```

## Result on cirno (2026-07-25)

| Workload | kencc | cc9 (clang -O2) | Ratio |
|---|---|---|---|
| interpreter (`-run interp-bench.js`) | 1.32 s | **0.40 s** | **3.3x faster** |
| parser (`interactive-mode.js` x20, 232 KB) | 0.56 s | 0.62 s | 0.9x |

The interpreter gap is the expected one: `patch.sh` has to set `DIRECT_DISPATCH 0` because
kencc has no labels-as-values, so the bytecode loop runs on a `switch` instead of computed
goto, and pcc's register allocation is no match for `clang -O2` on that loop. The parser is
a wash, so **compilation is not what makes startup slow** — pi's whole module graph parses in
well under a second. What is left is interpreted work: module top-level code, and node9's own
JS (the resolver, the `fs` layer).

`-DJS_NAN_BOXING=0` is *not* a kencc penalty, contrary to first appearances: quickjs-ng only
NaN-boxes on 32-bit builds (`INTPTR_MAX < INT64_MAX`), so a 64-bit build uses the struct
either way. The flag exists because APE's 32-bit `intptr_t` made the engine think it was a
32-bit platform.

Bytecode round-trip (`JS_WriteObject` → `JS_ReadObject`) works under **both** toolchains, in
memory, on a 232 KB module (647 KB of bytecode). So the fault that killed the module cache
(`fault read addr=0x31600000004`) was in that cache's own file path or a race between
processes — not a broken reader.
