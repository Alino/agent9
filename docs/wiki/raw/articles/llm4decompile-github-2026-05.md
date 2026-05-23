---
source_url: https://github.com/albertan017/LLM4Decompile
ingested: 2026-05-16
sha256: snapshot-only
---

# LLM4Decompile — Reverse Engineering with LLMs

Repo: github.com/albertan017/LLM4Decompile (6.6k stars). Paper: arXiv:2403.05286
(EMNLP 2024). License: MIT + DeepSeek License. Models live on HuggingFace under
LLM4Binary/*.

## What it does

Pioneering open-source LLM dedicated to decompilation. Decompiles **Linux x86_64**
binaries from GCC O0–O3 into human-readable C source code. Two variants:

- **LLM4Decompile-End** — decompiles binary directly (assembly → C)
- **LLM4Decompile-Ref** — refines pseudo-code that Ghidra already decompiled

Sizes: 1.3B / 6.7B / 9B / 22B parameters.

## Benchmark numbers (re-executability rate)

| Model | Size | HumanEval-Decompile re-exec |
|---|---|---|
| llm4decompile-1.3b-v1.5 | 1.3B | 27.3% |
| llm4decompile-6.7b-v1.5 | 6.7B | 45.4% |
| llm4decompile-1.3b-v2 (Ref) | 1.3B | 46.0% |
| llm4decompile-6.7b-v2 (Ref) | 6.7B | 52.7% |
| **llm4decompile-9b-v2** | 9B | **64.9%** — best |
| llm4decompile-22b-v2 | 22B | 63.6% |

Re-executability = decompiled function compiles and passes original test
assertions. Significantly outperforms GPT-4o and raw Ghidra by >100%.

Training script reproduces from scratch in ~3.5h on a single A100 40G, total
cost under $20.

## Recent updates

- 2025-10-04 — **SK²Decompile**: two-phase decompilation
  (Skeleton: structure recovery → Skin: identifier naming)
- 2025-05-20 — **decompile-bench** released: 2M binary-source function pairs
  for training + 70K for evaluation
- 2024-09-23 — llm4decompile-9B-v2 hits 0.6494 re-executability

## Pipeline

Raw binary cannot go to LLM directly. Workflow:

1. `objdump -d binary > binary.s` produces assembly
2. Extract single function block (between `<funcname>:` and next `\n\n`)
3. Strip binary opcode columns, strip comments
4. Wrap with prompt `# This is the assembly code:\n...\n# What is the source code?\n`
5. Feed to model

```python
from transformers import AutoTokenizer, AutoModelForCausalLM
import torch
model = AutoModelForCausalLM.from_pretrained(
    'LLM4Binary/llm4decompile-6.7b-v1.5',
    torch_dtype=torch.bfloat16
).cuda()
```

## Limitations

- x86_64 only. No ARM64, no MIPS, no RISC-V.
- Single-function decompilation. No whole-program structure recovery.
- Trained on GCC output — clang/MSVC variations untested.
- Function names hallucinated unless V2-Ref + Ghidra signatures.
