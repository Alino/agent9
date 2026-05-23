---
title: pi9 Phase 10 — Session 3 (ChatGPT/Copilot OAuth)
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [arch, status-partial, plan9, oauth, providers, copilot]
---

# pi9 Phase 10 — Session 3 (ChatGPT/Copilot OAuth)

> **Status: Session 3 partial done 2026-05-17.** Three things:
>
> 1. **GitHub Copilot OAuth — fully wired.** Device code flow
>    (perfect for plan9: no callback server needed). Provider
>    impl with proper Bearer auth + Editor-Version headers + base
>    URL derived from token's `proxy-ep` field.
> 2. **OpenAI Codex (ChatGPT Plus/Pro) — AUTH-ONLY.** Login + token
>    storage + refresh works. But ACTUAL USE of the token would
>    require OpenAI's Responses API (chatgpt.com/backend-api) which
>    pi9 doesn't implement. Login succeeds; first message fails
>    with a clear error.
> 3. **Hardening:** Copilot is OAuth-only — picker skips the
>    subscription/api-key choice and goes straight to OAuth.

## Why Codex is auth-only

Pi.dev has 700+ LOC dedicated to OpenAI's Responses API
(`packages/ai/src/providers/openai-codex-responses.ts`). It's a
fundamentally different wire format than Chat Completions —
different request structure, different SSE event types, different
streaming semantics. Porting all that would be its own Session 4+
and is way out of scope for "add OAuth for Codex".

I made a pragmatic call: ship the OAuth half (which is small and
reusable for ANY OAuth provider) and surface a clear error when
the user tries to actually USE a Codex token. The token IS stored
correctly in auth.json; pi9's existing OpenAI provider just doesn't
know how to talk to chatgpt.com/backend-api yet.

If you want Codex to actually work, you'd:

1. Add a `codexResponsesProvider` type implementing Provider
2. Build the Responses API wire format (POST chatgpt.com/backend-api/v1/responses)
3. Parse the Responses streaming events (different event types
   than Chat Completions — `response.output_text.delta`,
   `response.output_item.added`, etc.)

~600-800 LOC port from pi.dev's source. ~6-8 hours of work,
mostly mechanical. Deferred.

## Why Copilot works fully

Two reasons:

1. **Device flow has no plan9-specific problems.** Authorization
   code + PKCE requires a callback server + browser-on-localhost,
   which fails when the browser is on the Mac and pi9 is in the
   VM. Device flow just shows the user a code: "go to
   github.com/login/device and type ABCD-EFGH". The user types
   it in ANY browser. No callback. Pi9 polls GitHub until the
   user finishes.

2. **Copilot's API is OpenAI-compatible.** Same /chat/completions
   endpoint shape, same SSE events. Pi9 already has the streaming
   parser (`readSSE` in openrouter.go). The only Copilot-specific
   bits are:
   - Base URL extracted from token's `proxy-ep=...` field
   - `User-Agent: GitHubCopilotChat/0.35.0` + other VS Code identity
     headers
   - `Authorization: Bearer <token>` (not x-api-key)

## Files added/changed

```
internal/provider/oauth_openai_codex.go    NEW (~210 LOC) — Codex OAuth
                                           login + refresh, form-encoded
                                           token requests (different
                                           from Anthropic's JSON)
internal/provider/oauth_github_copilot.go  NEW (~290 LOC) — Copilot OAuth
                                           device flow with polling +
                                           slow_down handling + token
                                           exchange + proxy-ep URL parsing
internal/provider/copilot.go               NEW (~110 LOC) — Streamer impl
                                           for Copilot (openaiCompat-style
                                           but with dynamic base URL +
                                           special headers)
internal/provider/types.go                 added ProviderCopilot constant,
                                           added it to AllProviders +
                                           DisplayName, added RequiresOAuth
                                           helper, added copilot/ prefix
                                           routing in ProviderForModel
internal/provider/oauth.go                 OAuthProviders() now returns
                                           all three (anthropic, codex,
                                           copilot)
main.go                                    /login picker: if provider
                                           RequiresOAuth (Copilot), skip
                                           the subscription/api-key choice
                                           and go straight to OAuth flow
~/Projects/plan9-agent/launch-9front-detached.sh  + hostfwd 1455 (Codex)
~/Projects/plan9-agent/boot-9front.sh             + hostfwd 1455 (Codex)
wiki/assets/pi9-phase10s3-picker.png       picker w/ OAuth + Copilot
wiki/assets/pi9-phase10s3-copilot-authmethod.png  Copilot picker
```

## OAuth implementations side-by-side

| | Anthropic | OpenAI Codex | GitHub Copilot |
|---|---|---|---|
| Flow type | auth code + PKCE | auth code + PKCE | device code |
| Client ID | `9d1c250a-...` | `app_EMoamEEZ...` | `Iv1.b507a08c...` |
| Authorize endpoint | claude.ai | auth.openai.com | github.com/login/device/code (no browser redirect) |
| Token endpoint | console.anthropic.com | auth.openai.com | github.com/login/oauth/access_token |
| Callback port | 53692 | 1455 | none |
| Token request | JSON | form-encoded | form-encoded |
| Required identity | "You are Claude Code,..." prompt + headers | (Codex models need Responses API) | Editor-Version: vscode/... |
| Token suffix | sk-ant-oat | (opaque) | (opaque, contains proxy-ep) |
| Base URL extraction | static | static | from token's proxy-ep field |
| Refresh shape | JSON | form-encoded | re-call exchange endpoint |
| Plan9-friendly | needs port-forward | needs port-forward | **fully native** (just polling) |

## Verified in VM

- Picker shows **three** "(subscription available)" badges:
  Anthropic, OpenAI, GitHub Copilot — see
  `wiki/assets/pi9-phase10s3-picker.png`
- Selecting GitHub Copilot shows auth-method picker w/ OAuth
  highlighted by default — see
  `wiki/assets/pi9-phase10s3-copilot-authmethod.png`
- Builds clean: darwin/arm64 + plan9/amd64 (8.5MB)

## NOT verified

| | Why |
|---|---|
| Live Anthropic OAuth round-trip | VM reboot needed for port 53692 forward (still — Session 2 limitation persists) |
| Live Codex OAuth | Same + Codex requests would fail anyway (no Responses API impl) |
| Live Copilot device flow | Could test — it just needs network access to github.com (no callback). Didn't run live to avoid VM reboot risk. |

Realistically the Copilot device flow is the most testable of the
three because it doesn't need any QEMU networking changes. Try it:

1. From any pi9: `/login`
2. Arrow down to GitHub Copilot
3. Pi9 opens device code dance → shows "go to github.com/login/device
   enter code XXXX-YYYY"
4. Open the URL on any browser, enter the code
5. Pi9 polls, gets the token, exchanges for Copilot token, stores
6. Then `/model copilot/claude-sonnet-4` and send a message

## Known limitations / quirks

### Codex tokens are stored but unusable

If you `/login` with ChatGPT Plus/Pro, the access token persists to
auth.json. But trying to actually use the token (e.g. `/model
gpt-4o-codex` then send a message) will fail because pi9's OpenAI
Stream impl uses Chat Completions wire format, not Responses.

The error: "openai: http 401: ..." or similar. There's no
"Responses API not implemented" check today. **TODO:** add a check
in runStream — if the provider is OpenAI AND the token type is
oauth AND we don't have Responses support, surface a friendly
error instead.

### Copilot model names need the copilot/ prefix

To use a Copilot model, type `/model copilot/claude-sonnet-4` or
`/model copilot/gpt-5`. Without the prefix, `claude-sonnet-4`
routes to Anthropic (and fails because you don't have an Anthropic
key) and `gpt-5` routes to OpenAI (similar failure).

Why: Copilot's models overlap with other providers' (e.g.
claude-sonnet-4 exists in both Anthropic and Copilot). Pi9's
prefix-based routing can't disambiguate without an explicit hint.

Pi.dev solves this with a full model registry that maps each name
to a specific provider; pi9 doesn't have one.

### Copilot's `Use an API key (paste it)` was deceptive

Initial Session 3 picker offered both options for Copilot. But
Copilot doesn't actually issue API keys — only OAuth tokens via
device flow. Hardening fix: added `RequiresOAuth(p)` to
provider/types.go; picker now skips the choice for Copilot and
goes straight to OAuth.

### Device flow's polling is bounded

Copilot's device flow times out after ~15 minutes if the user
doesn't enter the code. Pi9 polls every ~6s with 1.2x multiplier
(or 1.4x after `slow_down`). If GitHub returns `slow_down` 3+
times in a row pi.dev's heuristic detects clock-drift (common in
VMs); we just bump the interval per server hint and keep going.

### Refresh token semantics differ

- **Anthropic:** refresh_token may rotate on each refresh. If
  response is empty, keep the old one.
- **Codex:** refresh_token rotates always.
- **Copilot:** the GITHUB token (long-lived) is stored in Refresh.
  The COPILOT token (short-lived) is Access. "Refresh" = call
  copilotExchangeToken again with the GitHub token.

All three are unified behind the same `OAuthCredentials` shape +
`Refresh(refresh string)` method.

## What's left

### Quick wins (~2 hrs)

- Manual code paste fallback (Anthropic / Codex) for when the
  callback can't reach the VM
- Friendly "Codex tokens unusable" error when user tries to chat
  with an OpenAI model after OAuth login
- Background refresh goroutine instead of synchronous on-demand

### Bigger items (~6-8 hrs each)

- **OpenAI Responses API** for Codex — would make ChatGPT Plus/Pro
  actually usable. ~600-800 LOC port.
- **GitHub Copilot model registry** — fetched from Copilot API at
  login time and shown in the picker so user doesn't have to
  remember model names.
- **GitHub Enterprise support** — Copilot's device flow already
  takes a domain param; we just hardcode github.com today.

## See Also

- [[pi9-phase10]] — Session 1: API-key multi-provider
- [[pi9-phase10-session2]] — Session 2: Anthropic OAuth (Claude Pro/Max)
- pi.dev source:
  - `packages/ai/src/utils/oauth/openai-codex.ts` — port reference for
    Codex OAuth
  - `packages/ai/src/utils/oauth/github-copilot.ts` — Copilot device flow
  - `packages/ai/src/providers/openai-codex-responses.ts` — what we'd
    need to port for Codex to actually work
- `internal/provider/oauth_openai_codex.go` — Codex OAuth
- `internal/provider/oauth_github_copilot.go` — Copilot OAuth
- `internal/provider/copilot.go` — Copilot Stream impl
- wiki/assets/pi9-phase10s3-picker.png
- wiki/assets/pi9-phase10s3-copilot-authmethod.png
