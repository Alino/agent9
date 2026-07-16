# AVX / YMM on 9front (the rust9 target-features question)

**Question (from the rust9 parity audit, finding "AVX"):** is it safe to build any
Rust/cc9 code with `+avx2`? The worry was that autovectorized YMM code could be
silently corrupted if the kernel's FP-save path doesn't preserve YMM across a
context switch or a note.

**Answer: settled with hardware evidence — do NOT add `+avx2` anywhere for this
deployment.** Two independent facts:

## 1. The kernel WOULD preserve YMM (on an AVX-capable box)

`/sys/src/9/pc64/fpu.c` `fpuinit()`:

```c
m->xcr0 = 0;
cr4 = getcr4() | CR4Osfxsr|CR4Oxmmex;
if((m->cpuidcx & (Xsave|Avx)) == (Xsave|Avx) && getconf("*noavx") == nil){
    cr4 |= CR4Oxsave; putcr4(cr4);
    m->xcr0 = 7;              /* x87, sse, AVX  */
    putxcr0(m->xcr0);         /* XSETBV -> XCR0 = 0b111 */
    ... fpsave = fpxsaves|fpxsaveopt|fpxsave;  fprestore = fpxrestore(s);
} else {
    cr4 &= ~CR4Oxsave; putcr4(cr4);
    fpsave = fpssesave;  fprestore = fpsserestore;   /* FXSAVE — SSE only */
}
```

- When the CPU has **both** the `Xsave` and `Avx` CPUID bits and plan9.ini has no
  `*noavx`, 9front sets `CR4.OSXSAVE`, sets **`XCR0 = 7`** (x87|SSE|**AVX/YMM**),
  and points `fpsave`/`fprestore` at the XSAVE family — so the full YMM state is
  saved/restored on every FP save.
- The **note path** uses the same pointers: `notefpsave()` (fpu.c:461) is called
  from note delivery (fpu.c:~554, `fpsave(up->fpsave); … notefpsave(up); …
  fprestore(up->fpsave)`), so YMM survives note entry/return too. This directly
  answers the audit's "notes push a Ureg + re-enter — FP save must cover that path":
  it does, via the same XSAVE `fpsave`.
- Otherwise (no AVX, or `*noavx`) it falls back to FXSAVE — SSE/XMM only, YMM not
  tracked. Building `+avx2` code for *that* configuration WOULD corrupt.

So on an AVX-capable Broadwell+ machine with default config, YMM is preserved
across both context switch and notes. The mechanism is sound.

## 2. But cirno's CPU has no AVX at all

Ran a CPUID/XGETBV probe on cirno (scratchpad `cpuid2.c`, via `cc9 run`):

```
brand: Intel(R) Celeron(R) 3205U @ 1.50GHz
leaf1.ECX=0x45faebbf leaf1.EDX=0xbfebfbff
  SSE2=1 SSE=1                         (sanity: CPUID readout is correct)
  SSE4.2=1  AVX=0  OSXSAVE=0  XSAVE=1
leaf7.EBX=0x02042603  AVX2=0  BMI2=0
```

Intel **fuses AVX off on Celeron/Pentium Broadwell SKUs** — the 3205U has no AVX,
no AVX2, no BMI2. `XSAVE` the instruction exists, but `fpuinit`'s `(Xsave|Avx)`
test is false (Avx bit = 0), so the kernel took the **SSE-only FXSAVE branch**
(`OSXSAVE=0` at runtime confirms it). A `+avx2` build would emit VEX-encoded
instructions that **`#UD` (illegal instruction)** the moment they execute here.

## Decision

- **No `+avx2` (or any AVX) for the cirno deployment** — the hardware can't run it.
  The global-target-features flip was already correctly rejected; the *swgl-scoped*
  build is rejected too, because on this box it would crash, not just underperform.
- swgl's SSE path (`GALLIUM_NOSSE` is unrelated — that's a W^X/JIT fault, not a
  register issue) is the ceiling on a 3205U. SSE4.2 is available and already used.
- **If the deployment target ever changes** to a Core-series Broadwell+ (or any CPU
  reporting AVX/AVX2), fact #1 above makes a **CPUID-guarded, swgl-scoped** `+avx2`
  build valid and safe — the kernel preserves YMM. Gate it on a runtime
  `CPUID.1:ECX.28 (AVX) && CPUID.7:EBX.5 (AVX2)` check with an SSE fallback; never
  flip the global `features` in the target JSON (that would autovectorize the whole
  world and #UD on any AVX-less box).
