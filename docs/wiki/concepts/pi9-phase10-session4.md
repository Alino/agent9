---
title: pi9 Phase 10 — Session 4 (Codex Responses MVP + arrow-key fix)
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [arch, status-partial, plan9, oauth, codex, vtwin]
---

# pi9 Phase 10 — Session 4 (Codex Responses MVP + arrow-key fix)

> **Status: Session 4 partial done 2026-05-17.** Two things shipped:
>
> 1. **Codex Responses API minimum viable port.** Login via Session
>    3's OAuth + chat in a single turn now works on paper. Text
>    streaming, no tools, no multi-turn-with-tool-history, no
>    reasoning blocks. Documented honestly as "MVP — breaks on
>    anything fancy".
> 2. **Arrow keys now work in the picker.** vtwin translates plan9's
>    private-use-area keyboard runes (Kup, Kdown, etc.) to xterm
>    CSI sequences (`\x1b[A`, `\x1b[B`) before forwarding to vts.
>    Pi9 (and any other bubbletea/ncurses/vim app inside vts) now
>    gets standard arrow keys.

## The arrow-key fix (the user-visible win)

User report: "make it also work with normal arrow buttons (when
picking provider or model for example)".

### Root cause

vtwin reads keyboard runes from libdraw's `Keyboardctl` channel.
Plan 9 puts arrow keys + special keys in the Unicode private-use
area (0xF000-0xF8FF) — see `/sys/include/keyboard.h`:

| Plan 9 rune | Code |
|---|---|
| `Kup` | `KF\|0x0E` = `0xF00E` |
| `Kdown` | `Kview` = `Spec\|0x00` = `0xF800` |
| `Kleft` | `KF\|0x11` = `0xF011` |
| `Kright` | `KF\|0x12` = `0xF012` |
| `Khome` | `KF\|0x0D` = `0xF00D` |
| `Kend` | `KF\|0x18` = `0xF018` |
| `Kpgup` | `KF\|0x0F` = `0xF00F` |
| `Kpgdown` | `KF\|0x13` = `0xF013` |
| `Kdel` | `KF\|0x14` = `0xF014` |
| `KF\|1..0xC` | F1-F12 |

vtwin was doing `runetochar(buf, &r); write(consfd, buf, n)` —
sending these runes as raw UTF-8 (Kup → `0xEF 0x80 0x8E`). Bubbletea
on the receiving side has no mapping for these; it treats them as
generic runes and either passes them to the rune-input handler
(which my picker's loginModePicker ignored) or drops them entirely.

### Fix

vtwin now has a switch statement that translates plan9 runes to
xterm CSI escape sequences before writing to cons:

```c
case Kup:     seq = "\x1b[A"; break;
case Kdown:   seq = "\x1b[B"; break;
case Kright:  seq = "\x1b[C"; break;
case Kleft:   seq = "\x1b[D"; break;
case Khome:   seq = "\x1b[H"; break;
case Kend:    seq = "\x1b[F"; break;
case Kpgup:   seq = "\x1b[5~"; break;
case Kpgdown: seq = "\x1b[6~"; break;
case Kdel:    seq = "\x1b[3~"; break;
case Kins:    seq = "\x1b[2~"; break;
/* F1-F12 */
case KF|1:    seq = "\x1bOP"; break;
case KF|2:    seq = "\x1bOQ"; break;
/* ... */
```

Falls through to runetochar for normal printable runes.

### Why this is the right layer

vtwin is the plan9↔xterm translation boundary. Fixing it here means:

- Pi9 gets arrow keys
- Bubbletea apps in general get arrow keys
- ncurses apps in vts get arrow keys (if anyone ports any)
- Vim/9vim inside vts now responds to arrow keys properly

Alternative places:

- **In bubbletea's plan9 shim**: would fix only bubbletea apps. Real
  apps like vim wouldn't benefit. Also requires a vendor-patches
  diff that's harder to upstream.
- **In pi9's handleLoginKey**: would only fix pi9. Other bubbletea
  apps still broken.

vtwin is one switch statement; rebuilds in <2s on plan9.

### Verified end-to-end

After rebuilding vtwin in the VM:

- screenshots `wiki/assets/pi9-phase10s4-arrows-down.png` and
  `pi9-phase10s4-arrows-up.png` show arrow-down moving selection
  OpenRouter → Anthropic → OpenAI, and arrow-up moving back
- No more "vim-style j/k only" workaround needed
- Old `ctrl+n/ctrl+p` paths still work (kept for accessibility)

### Quirk: existing vtwin windows don't get the fix

Plan 9 doesn't hot-reload binaries inside running processes. Old
vtwin procs keep using the pre-fix code. The fix only applies to
NEW vtwin windows opened after the rebuild. User flow:

1. `pi9-install` (or rebuild vtwin manually with `mk install`)
2. Close existing vtwin windows OR ignore them — they keep working,
   just without arrow keys until restarted
3. Open new pi9 via Start menu — that vtwin uses the new code

## Codex Responses MVP (the technical detail)

User picked option 1 from a clarification: "ship minimum viable
Codex Responses provider — text-only single-turn works, multi-turn/
tools brittle".

### What got built

`internal/provider/codex_responses.go` — new Provider impl that:

1. **Detects OAuth-authed OpenAI** in `runStream` (main.go). When
   the auth.json entry for `openai` is type=oauth, dispatch goes
   to `codexResponsesProvider` instead of the regular `openaiCompat`.
2. **Builds the Responses API request body**:
   ```json
   {
     "model": "gpt-5",
     "instructions": "<system prompt>",
     "input": [
       {"type": "message", "role": "user", "content": [{"type": "input_text", "text": "..."}]}
     ],
     "store": false,
     "stream": true,
     "tool_choice": "auto",
     "parallel_tool_calls": true,
     "text": {"verbosity": "low"}
   }
   ```
3. **POSTs to `chatgpt.com/backend-api/codex/responses`** with:
   - `Authorization: Bearer <oauth_access_token>`
   - `chatgpt-account-id: <from JWT>` — required header
   - `OpenAI-Beta: responses=experimental`
   - `originator: pi9`
   - `User-Agent: pi9 (plan9)`
4. **Parses Responses SSE events**:
   - `response.output_text.delta` → text chunk
   - `response.completed` → terminal
   - `response.failed` → terminal error
   - `error` → terminal error
   - All others (reasoning, refusal, tool calls, etc) ignored

### chatgpt-account-id wiring

The Responses API requires `chatgpt-account-id` on every request.
It's embedded in the OAuth access token (which is a JWT) in the
`https://api.openai.com/auth.chatgpt_account_id` claim.

Pi9's flow:

1. At login time (oauth_openai_codex.go), `extractCodexAccountID`
   decodes the JWT and pulls out the account ID
2. Stored in `OAuthCredentials.AccountID` (new field)
3. Persisted in `AuthEntry.AccountID` (new field on the JSON shape)
4. At request time (main.go runStream), read from `LookupAuthEntry`,
   smuggled to the provider via the `cfg.APIURL = "codex:" + accountID`
   trick (avoids adding a dedicated field on Config for one-provider
   metadata)

### Honest limitations

| Feature | Status |
|---|---|
| Single-turn text streaming | Implemented, untested live |
| Multi-turn text (no tools) | Probably works — we send full history each turn |
| System prompt | Implemented (`instructions` field, not in input array) |
| Tool calls (request) | Sent in body but model results discarded |
| Tool calls (response) | NOT parsed; lost |
| Tool results (round-trip) | NOT supported — would need fc_/call_id encoding |
| Reasoning blocks | NOT parsed; the model's reasoning is dropped |
| Refusals | NOT distinguished from errors |
| Previous_response_id | NOT used — full history sent each turn |
| Server-side conversation state | NOT used |

### NOT live-tested

Same blocker as Sessions 2+3: VM reboot needed for QEMU port 1455
forward (already in launch scripts but not active in current VM
session). Also needs a real ChatGPT Plus/Pro account to test
against. I built the request shape from pi.dev's source but
haven't seen a successful Codex completion myself.

## Files changed in Session 4

```
src/vtwin/main.c                              + plan9-rune to xterm-CSI translation
                                              (the arrow-key fix — affects
                                              ALL terminal apps in vts)
src/pi9/internal/provider/codex_responses.go  NEW: Responses API impl (~300 LOC)
src/pi9/internal/provider/oauth.go            + AccountID field on OAuthCredentials
src/pi9/internal/provider/oauth_openai_codex.go + extractCodexAccountID (JWT decode)
src/pi9/internal/provider/types.go            + GetCodexResponses()
src/pi9/internal/store/auth.go                + AccountID on AuthEntry + SetOAuth variadic
src/pi9/main.go                               + runStream OAuth-OpenAI → Codex dispatch
                                              + accountID smuggled via cfg.APIURL "codex:" prefix
wiki/concepts/pi9-phase10-session4.md         NEW (this file)
wiki/assets/pi9-phase10s4-arrows-down.png     arrow-key verified
wiki/assets/pi9-phase10s4-arrows-up.png       arrow-key verified
```

Build status: clean on darwin/arm64 + plan9/amd64 (8.5MB binary).

## What's left

- Live Codex test with a real ChatGPT Plus/Pro account
- Tool call support in Responses API (the `|`-separated `fc_` ID
  encoding pi.dev does)
- Reasoning block rendering (would show chain-of-thought from o1/
  o3/o4)
- Refusal blocks distinct from errors
- previous_response_id for server-side conversation state

## See Also

- [[pi9-phase10]] — Session 1: API-key multi-provider
- [[pi9-phase10-session2]] — Session 2: Anthropic OAuth
- [[pi9-phase10-session3]] — Session 3: Codex auth + Copilot full
- pi.dev source (cloned to /tmp/pi-reference):
  - `packages/ai/src/providers/openai-codex-responses.ts`
  - `packages/ai/src/providers/openai-responses-shared.ts`
- `src/vtwin/main.c` — the arrow-key translation
- `src/pi9/internal/provider/codex_responses.go` — MVP impl
- wiki/assets/pi9-phase10s4-arrows-*.png — verification screenshots
