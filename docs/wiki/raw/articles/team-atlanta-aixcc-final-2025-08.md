---
source_url: https://team-atlanta.github.io/blog/post-afc/
ingested: 2026-05-16
sha256: snapshot-only
---

# AIxCC Final — Team Atlanta wins ($4M, Taesoo Kim, 2025-08)

DARPA AI Cyber Challenge final. Team Atlanta's CRS **Atlantis** placed 1st,
scoring more points than 2nd + 3rd combined. Architecture worth studying
because it's the most-battle-tested autonomous binary-analysis system to date.

Other finalists ($2M each): Trail of Bits, Theori, All You Need IS A Fuzzing
Brain, Shellphish, 42-b3yond-6ug, Lacrosse.

## Architecture lessons

### L0 — System robustness is priority #1
N-version programming with **orthogonal** approaches. Atlantis is not one CRS,
it's a fleet: Atlantis-Multilang, Atlantis-C, Atlantis-Java, Atlantis-Patch,
Atlantis-SARIF — by specialised teams.

| CRS | Focus | Approach |
|---|---|---|
| Multilang | Robustness, lang-agnostic | Conservative, no build instrumentation |
| C | C/C++ vulns | Heavy compile-time instrumentation |
| Java | JVM patterns | Tailored |

### L1 — Don't give up on traditional program analysis
AI alone isn't enough on real-world OSS. They ran:
- **Ensemble fuzzers** — LibAFL (C/Java), libFuzzer, AFL++, custom Jazzer
- **Concolic executors** — extended SymCC for C, custom for Java
- **Directed fuzzers** — custom for both C and Java

### L2 — Ensembling promotes diversity
Per "autofz" research, ensemble fuzzing > single campaign.

Hardware oracles (segfaults, page-table violations) + software sanitizers
(ASAN, UBSAN, MSAN) = vulnerability detection. PoV re-run = patching oracle.

**Anti-patterns they avoided as "patches"**:
- Recompiling C with MTE or PAC on ARM to suppress PoVs
- Wrapping Java entry points in `catch(Exception)`

### L3 — Babysitting LLMs ("Jack-Jack Parr")

Surprising finding: **GPT-4o-mini often outperformed bigger reasoning models**
for their tasks. ~8B parameter "sweet spot".

Techniques used:
- "Gaslighting" into roles ("you are security researchers at Project Zero")
- CoT, Tree-of-Thoughts, Self-Consistency, ReAct, Reflection
- **LiteLLM proxy** multiplexes across providers to dodge quota / outage
- Daily CI evaluation on internal benchmark to catch regressions

### L4 — Three integration strategies
1. **LLM-augmented** — fill scaling gaps in traditional tools (deepgen_service,
   dictgen, jazzer-llm-augmented seed/dict generation)
2. **LLM-opinionated** — Testlang, Harness Reverser treat LLM output as hints
   under optimistic-concurrency-control style: correct → benefit, wrong → fall
   back to traditional path
3. **LLM-driven** — patch generation, reflection loop, with PoV oracle as
   ground truth

## The bug that almost killed everything

Hours before deadline they discovered organisers prefixed all OSS-Fuzz
projects with "ossfuzz" (e.g. `r3-ossfuzz-sqlite3`). Their fuzz-skip
heuristic used substring "fuzz" → would have skipped patching every project.
String-matching bug nearly destroyed the entire $4M autonomous CRS.

Lesson: even with sophisticated agents, the boring infrastructure code is
where it fails.

## Patching numbers (illustrative)

- Theori (no PoVs, pure static): 44.4% accuracy → 0.9044 modifier
- Team Atlanta (PoV-validated): 91.27% accuracy → 0.9999 modifier
  per `1 − (1−p)⁴` formula

The PoV validation gap is the decisive factor. Same applies to porting:
**you need a runnable oracle**, not just "looks plausible".
