---
title: LLM-Assisted Reverse Engineering — Landscape (2026-05)
created: 2026-05-16
updated: 2026-05-16
type: concept
tags: [arch, toolchain, decision]
sources:
  - raw/articles/llm4decompile-github-2026-05.md
  - raw/articles/ghidramcp-tool-reference-2026-05.md
  - raw/articles/talos-llm-reverse-engineering-sidekick-2025-07.md
  - raw/articles/project-naptime-google-zero-2024-06.md
  - raw/articles/team-atlanta-aixcc-final-2025-08.md
  - raw/articles/claude-share-llm-re-porting-asahi-2026-05.md
confidence: high
---

# LLM-Assisted Reverse Engineering — Landscape (May 2026)

State of the art for using LLMs to read and translate binaries. Companion
page [[llm-porting-workflow]] applies this to our concrete use case: porting
userland C software to Plan 9 / 9front.

---

## 1. Specialised decompilation models

Models fine-tuned specifically to turn assembly into source. Run locally;
no external API calls.

### LLM4Decompile (DeepSeek-based, Tan et al., EMNLP 2024)

- Repo: `albertan017/LLM4Decompile` (6.6k★). Models on HF: `LLM4Binary/*`.
- License: MIT + DeepSeek License.
- Sizes: 1.3B, 6.7B, 9B, 22B. **llm4decompile-9b-v2** is the SOTA leader at
  **64.9% re-executability** on HumanEval-Decompile.
- Two variants: **-End** (asm → C direct) and **-Ref** (refines Ghidra
  pseudocode). The Ref variants always beat End because Ghidra already
  recovered structure and CFG.
- Target: **Linux x86_64 only**, GCC O0–O3. No ARM, no MIPS, no RISC-V.
- Pipeline: `objdump -d` → extract function → strip opcodes/comments →
  feed to model. See [[draw-api]] for a stripped-asm example shape.
- Training reproducible in ~3.5h on single A100 40G, ~$20 total.
- 2025-10 addition: **SK²Decompile** — two-phase (Skeleton then Skin) for
  better structural recovery before naming.
- 2025-05: **decompile-bench** dataset — 2M binary↔source pairs for
  training + 70K eval.

### Nova (Jiang et al., NeurIPS 2024)

- arXiv:2311.13721. Generative foundation LLM **pre-trained on assembly**
  (not fine-tuned from a Python LLM).
- Hierarchical self-attention to handle low information density and
  long sequences of asm.
- Contrastive learning to learn assembly **optimisation equivalence** —
  knows that O0 and O3 binaries of the same source should embed nearby.
- Useful as a backbone for binary similarity / clone detection, not
  end-user decompilation.

### SLaDe (Armengol-Estapé et al., 2023-2024)

- arXiv:2305.12520. **Small** seq2seq Transformer (not decoder LLM).
- Cross-ISA: trained for both x86 and ARM, two optimisation levels.
- Pairs with **PsycheC** type-inference engine to clean up undefined types
  in output.
- Up to 6× more accurate than Ghidra alone, 4× more accurate than GPT (at
  the time) on ExeBench 4000+ functions.
- Smaller, portable, ISA-flexible — worth the look for non-x86 work.

### Cross-cutting limits of "decompilation models"

- All operate at **function granularity**. Whole-program structure is the
  caller's problem.
- All hallucinate function names — only the Ref-style variants benefit
  from Ghidra's already-recovered signatures.
- Calling-convention failures are common when input ABI differs from
  training distribution.
- None of these handle Plan 9's 9c output format. We will have to either
  (a) feed Plan 9 binaries through Ghidra first and use Ref-variants, or
  (b) skip binary-input entirely and work from source.

---

## 2. RE plugins (LLM-aware host tooling)

Plugins that turn IDA / Ghidra / Binary Ninja / r2 into AI-aware
analysts. Different from MCP servers — these are the IDE side, often with
their own model integration.

### Gepetto (IDA Pro) — `JusticeRage/Gepetto`
- 3.4k★, GPL-3.0. The original AI plugin for IDA, predates MCP era.
- Anthropic (Claude Opus 4.7 / Sonnet 4.6 / Haiku 4.5), OpenAI (GPT-5,
  gpt-5-mini, o3, o3-pro, GPT-4o, o4-mini), Gemini 2.5 Pro/Flash, Azure
  OpenAI, Ollama, Groq, Together, Novita AI, Kluster.ai, LM Studio.
- Hotkeys: Ctrl+Alt+G explain, Ctrl+Alt+K comment, Ctrl+Alt+R rename
  variables.
- Requires Hex-Rays decompiler. Does NOT support IDA Free.
- Tip from docs: rename works better **after** asking for an explanation
  first — the model uses its own comments to make better suggestions.

### aiDAPal (IDA, locally fine-tuned) — `atredispartners/aidapal`
- 390★. Ollama-backed, locally hosted.
- Custom 8k-context **fine-tuned model on Hex-Rays pseudocode**
  (`AverageBusinessUser/aidapal` on HF, Q4_K_M GGUF, 4.4GB).
- Setup: `ollama create aidapal -f aidapal.modelfile`.
- Right-click in Hex-Rays → analyse selection → accept/reject dialog →
  Hex-Rays output updates with comments and renames.
- Maintainer reports usable speeds on Apple Silicon (M-series MacBook).
- Privacy-preserving by design.

### Sidekick (Binary Ninja, Vector35 official, commercial)
- Subscription service. Requires Binary Ninja ≥ 4.0.4958.
- v2.0 (Aug 2024) introduced **Analysis Workbench**: dialogue-based
  refinement of generated scripts, coding assistant for the Binja API,
  Operator tab shows the LLM prompt for transparency.
- Public scripts repo: `Vector35/Sidekick-public`.
- Real-world demo blogs: Amadey malware string deobfuscation, firmware
  analysis.

### GhidrAssist / G3PT / ReVA / Ghidra-ChatGPT
- Older single-shot rename helpers for Ghidra, mostly pre-MCP. Use
  GhidraMCP instead in 2026.

### r2ai / decai (radare2 / Cutter) — `radareorg/r2ai`
- 433★, MIT, written in C+JS+TS+Python. Latest 1.3.8 (Apr 2026).
- Native r2 plugin + r2js decompilation-focused plugin `decai`.
- Install: `r2pm -Uci r2ai && r2pm -Uci decai`.
- Local (Ollama, LM Studio, llama.cpp) AND remote (OpenAI, Anthropic,
  Grok, Mistral, Gemini).
- **Native RAG** over markdown/code/text via built-in vector DB.
- ReAct mode using function-calling.
- Built-in prompts: `explain`, `devices`, `libs`, `varnames`,
  `autoname`, `vulns`, `signature`, `dlopen`, `decompile`.

---

## 3. MCP servers — the 2026 standard interface

MCP (Anthropic's Model Context Protocol) is now the lingua franca. The LLM
client (Claude Code / Cursor / Cline) speaks MCP; an MCP server exposes
disassembler tools as discrete tool calls.

### GhidraMCP — `LaurieWired/GhidraMCP` (7k+★, 24 tools)

The reference implementation. Tools split four ways:

- **Listing**: `list_methods`, `list_classes`, `list_segments`,
  `list_imports`, `list_exports`, `list_namespaces`, `list_data_items`,
  `search_functions_by_name`, `list_functions`
- **Decompilation**: `decompile_function`, `decompile_function_by_address`,
  `disassemble_function`, `get_function_by_address`
- **Modification**: `rename_function`, `rename_function_by_address`,
  `rename_data`, `rename_variable`, `set_decompiler_comment`,
  `set_disassembly_comment`, `set_function_prototype`,
  `set_local_variable_type`
- **Navigation**: `get_current_address`, `get_current_function`

The HN-noted 110-tool variant adds batch analysis, cross-referencing,
headless/Docker deployment — useful for whole-binary sweeps.

### IDA Pro MCP — `mrexodia/ida-pro-mcp` (8.6k★, 54 contributors)

Two operating modes:

1. **GUI mode** (legacy, being deprecated) — plugin inside running IDA.
2. **`idalib-mcp` headless mode** (new, recommended) — supervisor process
   keeps each open database in its own worker. CLI flags:
   - `--isolated-contexts` when multiple agents share one server
   - `--max-workers N` (default 4, env `IDA_MCP_MAX_WORKERS`)
   - Sessions: `idalib_open / idalib_switch / idalib_current /
     idalib_unbind / idalib_list`

Requires IDA Pro 8.3+ (9 recommended). **IDA Free is not supported.**
Python 3.11+ via `idapyswitch`. macOS install:

```bash
uv run "/Applications/IDA Professional 9.3.app/Contents/MacOS/idalib/python/py-activate-idalib.py"
claude plugin marketplace add mrexodia/claude-marketplace
claude plugin install ida-pro-mcp@mrexodia
```

Pitfall: LLMs are notoriously bad at integer↔bytes conversions. Use the
`int_convert` MCP tool, never let the model do base conversion in its head.

### Other MCP servers worth knowing

- `radare2/radare2-mcp` — r2mcp, official
- `darallium/r2-copilot` — r2copilot, CTF focus
- `suidpit/ghidra-mcp` — alternative Ghidra server
- `fdrechsler/mcp-server-idapro`, `taida957789/ida-mcp-server-plugin` —
  alt IDA servers
- `plugins.hex-rays.com/mxiris-reverse-engineering/ida-mcp-server` — IDA's
  official-ish

---

## 4. Autonomous harnesses & agent patterns

### Project Naptime (Google Project Zero, June 2024)
The architectural blueprint. Five principles that everyone since copies:
**reasoning space, interactive environment, specialised tools, perfect
verification, multi-trajectory sampling**. Tooling = Code Browser + Python
sandbox + Debugger (ASan) + Reporter. Showed 20× improvement on Meta's
CyberSecEval 2 buffer-overflow benchmark. See
[[../raw/articles/project-naptime-google-zero-2024-06]].

### Big Sleep (DeepMind + Project Zero, 2024+)
Naptime's successor, autonomous. Discovered real CVEs in production code
including SQLite stack-buffer-underflow CVE-2024-7592 — first known
public-CVE-discovered-by-LLM-agent.

### Atlantis / Team Atlanta — AIxCC winner 2025
$4M DARPA prize. **N-version programming with orthogonal CRSs**: separate
agents for C, Java, multilang, patch generation, SARIF reporting. Lessons:
- Surprising: 8B-param models (GPT-4o-mini) often beat reasoning models on
  their tasks — sweet spot for cost & latency.
- LiteLLM proxy multiplexes providers — dodge quota/outage.
- Daily CI on internal benchmark catches regressions.
- Three integration patterns: LLM-augmented (fill scaling gaps),
  LLM-opinionated (treat output as hints, fall back to traditional),
  LLM-driven (loop with oracle as ground truth).
- The PoV-validated patch oracle vs blind-static gap is the decisive
  factor: 91.27% vs 44.4% patch accuracy.
- See [[../raw/articles/team-atlanta-aixcc-final-2025-08]].

### CRUST-Bench — C-to-safe-Rust transpilation (COLM 2025)
arXiv:2504.15254. 100 real C repos, hand-written Rust interfaces, Rust
tests for validation. Best result (gpt-5 high reasoning, with test repair):
**70% test-pass rate** after 3 iterations. Pure pass@1 is much harder —
gpt-5 only solves 26/100 single-shot tests.

Headline: even SOTA models cannot one-shot multi-file C→Rust at safe-Rust
quality. **Iteration + test oracle is mandatory.** Same lesson applies to
C→Plan-9-C — see [[llm-porting-workflow]].

---

## 4b. The size-vs-context problem and the answers to it

A typical real binary has 50,000–500,000 functions and hundreds of MB of
code+data. A 200K-token context window holds at best a few hundred
decompiled functions. So **navigation is the hard problem**, not
decompilation. Three answers are converging:

### RAG over decompiled functions

Pre-process the whole binary once with Ghidra/IDA. Summarise each function
with an LLM (expensive but one-time). Embed summaries into a vector DB
(ChromaDB, Qdrant, Pinecone). Query at agent time with natural language
("find functions that check player position against wall geometry"),
retrieve top-K candidates, feed those into context.

Pairs naturally with:
- **String xref search** — strings narrow function space cheaply
- **Call graph** stored in a graph DB (Neo4j) — traverse from
  candidates outward
- **Function shape filters** — size, parameter count, return-style

Combined architecture:

```
Binary
  ↓
IDA/Ghidra decompiles all functions
  ↓
LLM summarises each function in batches     ← expensive, one-time
  ↓
Summaries → vector DB (similarity)
Pseudocode → document store
Call graph → graph DB
  ↓
Agent query: "find collision detection"
  ├ vector search on summaries
  ├ string-xref filter
  └ call-graph traversal from candidates
  ↓
Top-K functions in context → analysis + reimplementation
```

### The LLM Wiki pattern (what this page lives in)

Karpathy's compounding knowledge base. Persistent interlinked markdown.
Explicitly positioned as a **RAG alternative**: instead of
rediscovering knowledge per query, compile it once and keep cross-references
synthesised.

```
RAG:      query → embed → retrieve chunks → answer (re-do every time)
LLM Wiki: query → read index → read relevant pages → answer
                  (knowledge already cross-referenced, contradictions flagged)
```

For an RE harness, the wiki **solves the persistent-memory problem** —
the hardest unsolved piece of long-running autonomous agents:

| Without wiki | With wiki |
|---|---|
| Function 1000: agent has forgotten decisions made at function 50 | Every analysed function → wiki page |
| Architectural inconsistencies accumulate | Every architectural decision → logged |
| Same subsystem RE'd twice without realising | Every identified subsystem → concept page with cross-refs |
| Agent re-discovers from scratch | Agent orients at session start by reading index + log |

The skill that builds this lives in the Hermes Agent skills tree
Our own wiki you are reading is the implementation.

### Hybrid: vector store + wiki

In practice both layers live together. Vector store handles raw scale
(every function's pseudocode). Wiki holds the synthesised understanding
(what subsystems exist, which functions implement them, what decisions
were made about UB / divergence). Agent queries hit the wiki first for
known territory, fall back to vector store for exploration.

### The driver / hardware-register oracle (AsahiLinux as proof)

Drivers are the cleanest RE/porting target because:
- Well-defined interface (IOCTL, kernel APIs, interrupt handlers) —
  inputs and outputs are structured and enumerable.
- No rendering/audio ambiguity. Either the hardware does the right
  thing or it doesn't.
- Observable behaviour — instrument both original driver and
  reimplementation, compare register reads/writes directly.

**The hardware-register sequence is the perfect oracle**:

```
Same OS request → Original driver → Register-write sequence → HW response
Same OS request → Reimpl driver  → Register-write sequence → HW response

diff(register sequences) == 0  → correct
```

PCIe bus analysers, USB protocol analysers, MMIO tracing — these tools
already exist and feed straight into an automated agent loop.

**AsahiLinux did this manually for the Apple M1/M2 GPU**:
- Traced GPU command streams from the macOS driver
- Inferred command buffer format from observed patterns
- Tested re-implementation against real hardware

Existing Asahi tooling that would plug into an LLM-driven version:

| Asahi tool | Pipeline role |
|---|---|
| `AsahiLinux/gpu-re` | Manual RE notes → seeds the wiki |
| `AsahiLinux/agx-exploit` | GPU command-stream tracer → differential oracle |
| Piglit | Automated GPU validation → test runner |
| Vulkan CTS | Conformance tests → acceptance criteria |
| Mesa AGX driver | Target codebase for patches |

This is the cleanest known precedent for the agent pipeline pattern
we are sketching: oracle (HW register trace) + wiki (gpu-re notes) +
target (Mesa) + tests (Piglit/CTS). For Plan 9 the oracle is "code runs
on 9front and tests pass" rather than HW register traces, but the
architecture is identical.

---

## 5. Recommended starter kit (macOS, our box)

For Plan 9 porting and 9front native code reading:

1. **Ghidra + GhidraMCP** — free, runs on macOS Apple Silicon, mature.
   Install: download Ghidra 11.x, drop `GhidraMCP.zip` plugin, install
   `bridge_mcp_ghidra.py` as MCP server in Claude Code config.
2. **r2 + decai + r2mcp** — `brew install radare2 && r2pm -Uci r2ai
   decai`. Excellent for one-off triage and quick read of small Plan 9
   binaries. Decai's local-Ollama mode is the most private option.
3. **LLM4Decompile 9B Ref** — for x86_64 Linux binaries only. Run via
   Ollama once GGUF version is available, or directly via
   transformers+bfloat16 on a Mac with sufficient unified memory (≥32GB).
4. **Claude Code with /goal** — the orchestrator. The Hermes /goal
   feature gives us judge + executor + sub-goal + persistence across
   restarts — exactly Naptime's five-principle pattern, packaged. See
   [[llm-porting-workflow]] for the concrete pipeline.

For IDA users: ida-pro-mcp + Gepetto is the gold standard; aiDAPal for
private/local work.

For Binary Ninja users: Sidekick if you have the subscription.

---

## 6. Known pitfalls (cross-tool)

- **Integer ↔ byte / hex ↔ dec conversion**: LLMs are terrible. Always
  delegate to MCP tool.
- **Pointer and array types**: single biggest source of decompilation
  derailment. Force the model to fix types **early** before naming.
- **Calling-convention drift**: LLMs assume System V AMD64; struggle with
  Windows x64, fastcall, custom ABIs. Plan 9 has its own ABI; expect
  failures.
- **Hallucinated function names**: only Ref-variant models that see real
  symbols are safe. End-style decompilation invents plausible-but-wrong
  names constantly.
- **Control-flow flattening / opaque predicates**: defeats both decompiler
  and LLM. Need devirtualisation or symbolic execution (angr) first.
- **Context exhaustion**: large binaries blow the context window fast.
  Process function-by-function, summarise into a project memory.
- **Function granularity ≠ program understanding**: even perfect
  per-function decompilation doesn't give you the architecture. That's
  the human's job (or a higher-level agent's).

---

## See also

- [[llm-porting-workflow]] — the practical /goal-driven Plan 9 porting pipeline
- [[browser-webview-plan9]] — example of a porting target that benefits from this
- [[build-toolchain]] — our cross-compile setup; needed as the verification
  oracle in any porting loop
- [[testing-harness]] — QMP + screendump + vision_analyze; the equivalent
  of Naptime's "perfect verification" for UI work
