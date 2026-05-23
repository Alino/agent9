---
source_url: https://projectzero.google/2024/06/project-naptime.html
ingested: 2026-05-16
sha256: snapshot-only
---

# Project Naptime — Google Project Zero (Glazunov + Brand, June 2024)

The framework that legitimised "agentic" LLM use for vulnerability research
and binary analysis. Set the architectural template that DARPA AIxCC winners
later refined.

## Headline numbers (CyberSecEval 2 benchmark)

- **Buffer Overflow**: 0.05 → 1.00 (20× improvement, GPT-4 Turbo, Naptime@20)
- **Advanced Memory Corruption**: 0.24 → 0.76 (GPT-4 Turbo, with ASan)
- Gemini 1.5 Pro reaches 0.76 with 32-step trajectories

## Five guiding principles (paraphrased)

1. **Space for reasoning.** Verbose chain-of-thought reliably beats
   straitjacketed answer-only prompts.
2. **Interactive environment.** Models must adjust and correct near-misses
   through actual tool feedback, not single-shot guesses.
3. **Specialised tools.** Mirror human researchers' workflow: debuggers,
   Python sandbox, code browsers. Interface matters.
4. **Perfect verification.** Vulnerability discovery is uniquely
   crash-or-no-crash — automatic, unambiguous. Critical for benchmarks
   and agent loops.
5. **Sampling strategy.** Multiple **independent trajectories** beat
   trying multiple hypotheses inside one trajectory.

## Architecture

| Tool | Purpose |
|---|---|
| Code Browser | Navigate codebase (Chromium Code Search-style); view source, find references |
| Python Tool | Sandboxed scripting for calculations and input generation |
| Debugger | Set breakpoints, evaluate expressions, observe runtime; uses AddressSanitizer |
| Reporter | Structured progress communication; signals completion or abort |

**Model-agnostic, backend-agnostic, self-contained.** Humans can use the
same toolset to generate fine-tuning trajectories.

## Why this matters for porting

The verification principle generalises perfectly to porting:
"compile-then-test-passes" is the porting equivalent of "crash-or-no-crash".
The /goal pattern (judge + executor + clear test criterion) maps
1-to-1 onto Naptime's architecture.

Naptime's lineage: → Big Sleep (Google DeepMind / Project Zero
collaboration) → multiple discovered real-world vulnerabilities including
the SQLite stack buffer underflow CVE-2024-7592 in late 2024.
