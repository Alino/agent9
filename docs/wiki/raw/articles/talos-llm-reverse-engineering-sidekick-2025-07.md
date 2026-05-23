---
source_url: https://blog.talosintelligence.com/using-llm-as-a-reverse-engineering-sidekick/
ingested: 2026-05-16
sha256: snapshot-only
---

# Using LLMs as a Reverse Engineering Sidekick — Cisco Talos (Guilherme Venere, 2025-07-31)

Cisco Talos malware-analyst's field report. LLMs **complement**, not replace,
analysts. The actually-useful integration pattern is MCP servers in front of
IDA Pro / Ghidra.

## Stack

```
[VSCode + Cline / Roo Code / Copilot MCP]   ← MCP client
        ↓ MCP protocol
[Claude 3.7 Sonnet / Ollama Devstral:24b]   ← LLM
        ↓ tool calls
[ida-pro-mcp / GhidraMCP server]            ← MCP server
        ↓ HTTP / RPC
[IDA Pro 9.x or Ghidra]                     ← Disassembler
```

Components can be split across machines — GPU box runs LLM, isolated
sandbox runs the malware in IDA.

## MCP server options surveyed

- github.com/LaurieWired/GhidraMCP — most popular, 7k+ stars
- github.com/suidpit/ghidra-mcp
- github.com/mrexodia/ida-pro-mcp — used in Talos research
- plugins.hex-rays.com/mxiris-reverse-engineering/ida-mcp-server
- github.com/fdrechsler/mcp-server-idapro
- github.com/taida957789/ida-mcp-server-plugin
- github.com/JusticeRage/Gepetto (IDA, not MCP but similar role)

## Local vs cloud trade-off

| | Cloud (Claude/GPT) | Local (Devstral / Llama via Ollama) |
|---|---|---|
| Cost | Token charges balloon on large files | Hardware/energy upfront |
| Privacy | Data goes to provider — malware compliance risk | Fully private |
| Speed | Minutes | Hours (CPU fallback when GPU memory exceeded) |
| Quality | Higher | Lower; misses subtleties |

## Three effective prompt templates

### 1. Top-down analysis from entry point
Start at `StartAddress`, follow calls recursively, add summary comments,
rename functions with `VIBE_` prefix, rename variables, set pointer/array
types correctly, never convert number bases manually (use the
`convert_number` MCP tool — LLMs are terrible at it).

### 2. Deep-dive specific function
Drill into one function and its callees. For Windows API calls,
auto-comment with the API's parameter meanings, rename variables to match
API parameter names.

### 3. Cleanup pass on unknown functions
Process all `sub_*` functions in batch. Cheaper and good for triage.

## Lessons that translate beyond malware

- Number-base conversions: ALWAYS delegate to a tool. LLMs hallucinate
  `0x10 == 10 decimal` constantly.
- Pointer/array type recovery is the single biggest reason re-decompilation
  goes off the rails. Force the LLM to fix types early.
- Test sample was IcedID malware with ~37 functions. Cloud finished in
  minutes; local ran for hours on a Ryzen 9800X3D + 64GB + RTX 5070 Ti.
- For follow-up queries, the previous decompilation gets appended to
  context — input window pressure is the binding constraint.

## Citation context

This is the field report that established MCP-in-front-of-disassembler as
the standard 2025 RE workflow. Before this, AI-assisted RE was mostly
Gepetto-style single-shot rename helpers.
