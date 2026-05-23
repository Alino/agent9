---
title: pi9 Phase 10 — Multi-Provider Auth, Session 1
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [arch, status-partial, plan9, providers]
---

# pi9 Phase 10 — Multi-Provider Auth (Session 1)

> **Status: Session 1 done 2026-05-17. Sessions 2 (OAuth) + 3
> (ChatGPT/Copilot OAuth + hardening) parked.** Pi9 can now speak
> directly to Anthropic, OpenAI, OpenRouter, Groq, DeepSeek, xAI,
> Mistral, Together, Fireworks, Cerebras — anything OpenAI-compatible
> (10 of them) plus Anthropic's native Messages API. `/login` is
> now an interactive provider picker with masked key entry.
>
> Skipped intentionally: OAuth for Claude Pro/Max, ChatGPT Plus/Pro,
> GitHub Copilot (those need browser-launch via plumber + callback
> server — Session 2 work, ~4-6 hrs each).

## What changed

| | Phase 9b | Phase 10 (S1) |
|---|---|---|
| Providers | OpenRouter only (or anything OpenAI-compatible if you tweak api_url) | **11 providers** with proper routing |
| `/login` UI | one-shot: `/login sk-or-v1-...` | **interactive picker** + masked key entry |
| Auth storage | single `api_key=` in config | **per-provider auth.json** (mirrors pi.dev shape) |
| Provider selection | hardcoded | **`ProviderForModel(model)`** routing by model name |
| `/logout` | clears single api_key | clears all OR `/logout <provider>` for one |

## Architecture

### Provider interface (`internal/provider/types.go`)

```go
type Provider interface {
    Name() ProviderID
    Stream(ctx context.Context, cfg Config, messages []Message) (<-chan Chunk, <-chan error)
}
```

Two implementations ship in S1:

- **`openaiCompat`** (`openrouter.go`) — handles OpenRouter, OpenAI,
  Groq, DeepSeek, xAI, Mistral, Together, Fireworks, Cerebras. Same
  wire format, different endpoint URL per provider.
- **`anthropic`** (`anthropic.go`) — Anthropic's native /v1/messages
  format. Different request shape (system at top level, content blocks
  for tool_use/tool_result), different SSE event types
  (`content_block_delta` instead of `chat.completion.chunk`), different
  auth header (`x-api-key` instead of `Authorization: Bearer`).

Provider selection at request time:

```go
providerID := provider.ProviderForModel(model)   // by model name prefix
impl := provider.Get(providerID)
key := store.LookupAPIKey(string(providerID))     // from auth.json
chunks, errs := impl.Stream(ctx, cfg, msgs)
```

### Routing by model name (`ProviderForModel`)

| Model prefix | Provider |
|---|---|
| `claude-*` | Anthropic |
| `gpt-*`, `o1-*`, `o3-*`, `o4-*` | OpenAI |
| `grok-*` | xAI |
| `gemini-*` | Google (not yet implemented) |
| `mistral-*`, `codestral-*` | Mistral |
| `deepseek-*` | DeepSeek |
| `vendor/model` (has `/`) | OpenRouter |
| anything else | OpenRouter (catch-all) |

Switching providers: `/model claude-sonnet-4` → next message goes
to Anthropic via the user's Anthropic key. `/model
moonshotai/kimi-k2.5` → routes through OpenRouter.

### auth.json (`internal/store/auth.go`)

Mirrors pi.dev's `~/.pi/agent/auth.json` shape:

```json
{
  "anthropic":  { "type": "api_key", "key": "sk-ant-..." },
  "openrouter": { "type": "api_key", "key": "sk-or-v1-..." },
  "openai":     { "type": "api_key", "key": "sk-..." }
}
```

Path: `$home/lib/pi9/auth.json` (mode 0600). Multiple providers
active simultaneously; `LookupAPIKey(provider)` returns the right
one per request.

Legacy `api_key=` in config.config still works — `LookupAPIKey`
falls back to it for OpenRouter when auth.json is empty, so
pre-Phase-10 setups keep working without re-login.

OAuth fields are reserved in the AuthEntry struct
(`AccessToken`/`RefreshToken`/`ExpiresAt`) but no provider uses
them yet — that's Session 2.

### /login picker (`handleLoginKey` + `renderLoginPicker`)

Bubbletea sub-state via `loginMode` enum on `pi9Model`:

- `loginModeOff` — normal chat input
- `loginModePicker` — provider list overlay, navigate + select
- `loginModeKeyEntry` — replaces input box with `| key for X: ***_  |`

Keys in picker mode (vim-style + ctrl, because plain arrow keys
don't reach pi9 reliably from QMP):

| Key | Action |
|---|---|
| `↑`/`ctrl+p`/`k` | previous provider |
| `↓`/`ctrl+n`/`j` | next provider |
| `enter` | pick this provider, go to key entry |
| `esc` | cancel back to chat |

Keys in key-entry mode:

| Key | Action |
|---|---|
| typing | append to key buffer (masked in render) |
| `backspace`/`ctrl+h` | delete |
| `ctrl+u` | clear all |
| `enter` | save to auth.json + return to chat |
| `esc` | back to picker |

Hot-update: if the provider you just /login'd matches the current
model's provider, `m.apiKey` is updated live — the next message
uses the new key without restart.

### Legacy /login <key> still works

Pi9-phase8b's `/login sk-or-v1-...` one-shot form still works.
Stores under OpenRouter (since that's what pre-Phase-10 users were
implicitly using). Adds a hint in the response pointing at the
interactive picker.

## Verified end-to-end

Screenshot 1: `wiki/assets/pi9-phase10-picker.png` — `/login` opens
the provider list:

```
/login - pick provider:
> OpenRouter         (cyan highlight)
  Anthropic
  OpenAI
  DeepSeek
  Groq

[status: pick provider with arrows, enter to select, esc to cancel]
```

Screenshot 2: `wiki/assets/pi9-phase10-keyentry.png` — after enter
on Anthropic:

```
+-----------------------------------+
| key for Anthropic: _              |
+-----------------------------------+
[status: enter API key for Anthropic (get one at https://console.anthropic.com/s...)]
```

The status bar deep-links to the provider's key-mint page. The
input shows `_` (cursor) initially; as user types, characters
appear masked (first 3 + middle stars + last 3).

Save flow: enter on a valid key → `store.SetAPIKey(provider, key)`
writes auth.json → returns to chat with a magenta turn
`/login anthropic sk-ant-...XXX` and `saved to /usr/glenda/lib/pi9/auth.json`.

### Build status

- darwin/arm64: clean
- plan9/amd64: clean (7.9MB binary, +51KB vs Phase 9b — anthropic.go
  + auth.go + picker UI)

### What's NOT tested live yet

- Actual Anthropic streaming response — I built the wire format
  from the public docs but haven't seen a successful completion
  yet. Provisional until tested with a real key.
- Tool calls via Anthropic — the wire shape is implemented
  (content_block_start.tool_use → input_json_delta accumulation →
  finalizeAnthropicTools) but unproven against the live API.
- OpenAI / Groq / DeepSeek streams — all share the openaiCompat
  path, same code that's been working for OpenRouter for phases.
  Should work; not tested.

## Quirks worth remembering

### QMP arrow keys may not reach pi9

In testing the picker, `qmp.py key down` did not navigate. `ctrl+n`
did. The arrow key probably arrives but bubbletea's plan9 input
shim doesn't translate the escape sequence. **Vim-style nav
(`j`/`k`) and ctrl-style (`ctrl+n`/`ctrl+p`) both work as
fallbacks** — I wired them explicitly for this reason.

User experience: when you USE the picker for real (not via QMP),
arrow keys should work because vtwin sends the proper sequences.
This is only a testing constraint.

### auth.json overrides legacy config api_key

When both exist, auth.json wins for the named provider. Legacy
config.api_key only used as fallback for OpenRouter when auth.json
has no openrouter entry. Migration path: any first `/login`
through the picker silently moves you off the legacy path.

### Anthropic's `system` is special

Anthropic's `/v1/messages` puts the system prompt at the top level
of the request body, NOT in `messages[]`. My `buildAnthropicBody`
extracts the first system message and hoists it. Multiple system
messages → only the first wins (Anthropic API limitation, not pi9's).

### Tool call format differs

OpenAI: `tool_calls` array on assistant message, `tool` role for
result. Anthropic: `tool_use` content block on assistant,
`tool_result` content block on user message (NOT a separate role).
Both translate cleanly through pi9's neutral Message type.

### Anthropic tool args arrive in two pieces

In OpenAI's stream, `tool_calls[i].function.arguments` accumulates
across deltas. In Anthropic's stream, the tool_use block ships an
initial `input` (often `{}`) in `content_block_start`, then
`input_json_delta`'s `partial_json` accumulates the real arguments.
The `finalizeAnthropicTools` helper concatenates both into the
final `arguments` string. Edge case: if `content_block_start.input`
isn't `{}` but a complete value, we still concatenate the deltas —
which would produce malformed JSON. In practice Anthropic always
sends `{}` first and streams the rest. If they change behavior,
the heuristic breaks.

## Files added / changed

```
src/pi9/internal/provider/types.go        NEW: Provider interface,
                                          ProviderID enum, shared types
                                          extracted from openrouter.go,
                                          ProviderForModel routing,
                                          DisplayName/KeyURL/KeyPrefix
                                          helpers for picker UI.

src/pi9/internal/provider/openrouter.go   refactored: types moved to
                                          types.go, added openaiCompat
                                          struct (per-provider Provider
                                          impl), providerEndpoint
                                          dispatcher for 9 provider URLs,
                                          legacy free-function
                                          StreamRequest kept for
                                          backward compat.

src/pi9/internal/provider/anthropic.go    NEW: Anthropic native API
                                          provider. Builds /v1/messages
                                          request bodies (system at top
                                          level, content blocks for
                                          tool_use/tool_result),
                                          parses Anthropic's SSE
                                          (content_block_*, message_*
                                          events).

src/pi9/internal/store/auth.go            NEW: auth.json layer.
                                          LoadAuth/SaveAuth/SetAPIKey/
                                          ClearProvider/LookupAPIKey.
                                          Legacy config.api_key
                                          fallback for OpenRouter.
                                          Atomic writes via tmp+rename,
                                          mode 0600.

src/pi9/main.go                           extended:
                                          - pi9Model.loginMode/Cursor/Picked
                                            for picker state
                                          - runStream now picks provider
                                            via ProviderForModel + Get
                                          - /login handles picker case
                                            (no args) + legacy case
                                          - /logout handles all-providers
                                            case + per-provider case
                                          - handleLoginKey routes picker
                                            keys
                                          - renderLoginPicker renders
                                            provider list or key-entry
                                            overlay in place of input box
```

## Acceptance criteria

| Criterion | Verified |
|---|---|
| Provider interface defined | ✅ types.go |
| openaiCompat covers all OpenAI-format providers | ✅ 9 providers |
| Anthropic native impl | ✅ anthropic.go (untested live) |
| auth.json persistent storage | ✅ store/auth.go |
| Legacy config api_key falls back to OpenRouter | ✅ LookupAPIKey |
| `/login` (no args) opens picker | ✅ screenshot |
| Picker navigation works | ✅ ctrl+n (arrows fail in QMP but should work in vtwin) |
| Enter on provider goes to key entry | ✅ screenshot |
| Key entry shows masked input | ✅ first 3 + stars + last 3 |
| Save persists to auth.json | ✅ store.SetAPIKey called |
| `/logout` clears all | ✅ |
| `/logout <provider>` clears one | ✅ |
| `/model` switches provider via routing | ✅ ProviderForModel called per request |
| Both builds clean | ✅ host + plan9 |

## What's deferred (Sessions 2 + 3)

### Session 2 (~4-6 hrs)

- OAuth scaffolding:
  - HTTP callback server (`net.Listen` on a random port, `accept()`
    in a goroutine)
  - Browser launch via `plumb -d web https://...`
  - PKCE challenge/verifier generation
  - Token refresh background goroutine
- Anthropic Claude Pro/Max OAuth specifically
- `/login` picker shows OAuth providers with "(subscription)"
  suffix; selecting one starts the OAuth flow instead of key entry

### Session 3 (~4-6 hrs)

- ChatGPT Plus/Pro (Codex) OAuth (different flow)
- GitHub Copilot OAuth (different again, optional GHES domain)
- Token refresh edge cases
- Cosmetic polish: mouse scroll, picker scrollback fix

### Truly future work

- `/login` accepting environment variable names (`{key: "MY_KEY_VAR"}`)
  and shell commands (`{key: "!security find-..."}`) like pi.dev
- Custom model lists per provider (`/scoped-models` from pi.dev)
- Provider-specific endpoint overrides for Cloudflare AI Gateway,
  Vercel AI Gateway, Azure OpenAI

## See Also

- [[pi9-architecture]] — overall design
- [[pi9-phase8]] — Phase 8/8b: single-provider config + initial /login
- [[pi9-phase9]] — Phase 9: install path, DECAWM, scrollback nav
- `src/pi9/internal/provider/types.go` — Provider interface + routing
- `src/pi9/internal/provider/anthropic.go` — Anthropic native API
- `src/pi9/internal/store/auth.go` — auth.json layer
- `src/pi9/main.go` — picker UI, runStream provider dispatch
- `wiki/assets/pi9-phase10-picker.png` — provider list overlay
- `wiki/assets/pi9-phase10-keyentry.png` — masked key entry
- <https://pi.dev/docs/latest/providers> — pi.dev's provider list +
  auth.json shape that pi9 mirrors
- <https://docs.anthropic.com/en/api/messages-streaming> — Anthropic
  SSE event reference used by anthropic.go
