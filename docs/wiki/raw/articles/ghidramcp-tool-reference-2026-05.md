---
source_url: https://deepwiki.com/LaurieWired/GhidraMCP/7.2-mcp-tool-reference
ingested: 2026-05-16
sha256: snapshot-only
---

# GhidraMCP Tool Reference (LaurieWired, 7k+ stars)

MCP server for Ghidra. Pairs a Ghidra plugin (Java) with a Python MCP bridge
(`bridge_mcp_ghidra.py`). LLM clients (Claude Desktop, Claude Code, Cursor,
Cline, VS Code copilot etc.) call tools that translate into HTTP requests to
the Ghidra plugin's REST endpoints.

Two implementation halves: Ghidra plugin exposes REST endpoints; Python bridge
exposes those as MCP tools with `@mcp.tool()`.

## Listing / Query Tools

| Tool | Signature | Returns |
|---|---|---|
| `list_methods` | `(offset=0, limit=100)` | function names |
| `list_classes` | `(offset=0, limit=100)` | namespace/class names |
| `list_segments` | `(offset=0, limit=100)` | memory segments |
| `list_imports` | `(offset=0, limit=100)` | imported symbols |
| `list_exports` | `(offset=0, limit=100)` | exported funcs/symbols |
| `list_namespaces` | `(offset=0, limit=100)` | non-global namespaces |
| `list_data_items` | `(offset=0, limit=100)` | defined data labels+values |
| `search_functions_by_name` | `(query, offset, limit)` | substring search |
| `list_functions` | `()` | all functions (no pagination) |

## Decompilation & Disassembly

| Tool | Signature | Returns |
|---|---|---|
| `decompile_function` | `(name)` | C pseudocode |
| `decompile_function_by_address` | `(address)` | C pseudocode |
| `disassemble_function` | `(address)` | (address, instruction, comment) tuples |
| `get_function_by_address` | `(address)` | function info |

## Program Modification

| Tool | Signature |
|---|---|
| `rename_function` | `(old_name, new_name)` |
| `rename_function_by_address` | `(address, new_name)` |
| `rename_data` | `(address, new_name)` |
| `rename_variable` | `(function_name, old_name, new_name)` |
| `set_decompiler_comment` | `(address, comment)` |
| `set_disassembly_comment` | `(address, comment)` |
| `set_function_prototype` | `(address, prototype)` |
| `set_local_variable_type` | `(address, variable_name, new_type)` |

## Navigation

| Tool | Signature |
|---|---|
| `get_current_address` | `()` |
| `get_current_function` | `()` |

## Why it matters

This 24-tool surface is the de-facto contract that any agentic
binary-analysis pipeline targets. Other MCP servers (110+ tool variants
mentioned on HN) extend this; the LaurieWired set is the lowest-common
denominator.

Crucially: **the LLM doesn't see the binary**. It sees high-level requests
("decompile main", "rename sub_4012a0 to parse_header"). Ghidra does the
heavy lifting; the LLM brings naming, summarisation, and cross-function
reasoning.
