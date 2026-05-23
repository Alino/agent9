---
title: pi9 Phase 10 — Session 2 (OAuth scaffolding + Claude Pro/Max)
created: 2026-05-17
updated: 2026-05-17
type: concept
tags: [arch, status-done, plan9, oauth, providers]
---

# pi9 Phase 10 — Session 2 (OAuth scaffolding + Claude Pro/Max)

> **Status: Session 2 done 2026-05-17.** Pi9 can now do the full
> OAuth dance for Anthropic Claude Pro/Max — PKCE, browser launch,
> local callback server, token exchange, auto-refresh. The UI
> shows `(subscription available)` badges and a two-step auth-method
> picker (subscription vs API key). Builds clean, picker UI verified
> in VM, **live OAuth flow not yet end-to-end tested** because it
> needs a VM reboot to pick up the QEMU port-forward change.

## Why this exists

Pi.dev (earendil-works/pi) supports Claude Pro/Max OAuth out of the
box. Phase 10 Session 1 shipped multi-provider API keys but couldn't
log in via the user's subscription. Session 2 closes that gap for
Anthropic; Sessions 3+ will add ChatGPT Plus/Pro (Codex) and GitHub
Copilot.

## Key insight: cloned the reference

For Session 2 I cloned `earendil-works/pi` at /tmp/pi-reference and
ported the OAuth implementation from
`packages/ai/src/utils/oauth/anthropic.ts`. Specifically:

- The client_id (`9d1c250a-e61b-44d9-88ed-5944d1962f5e`) — same one
  Claude Code itself uses
- The endpoints (`claude.ai/oauth/authorize`,
  `console.anthropic.com/v1/oauth/token`)
- The callback port (53692) — fixed because the authorize endpoint
  must know the exact redirect_uri in advance
- The scope string (`org:create_api_key user:profile user:inference
  user:sessions:claude_code user:mcp_servers user:file_upload`)
- The "You are Claude Code, Anthropic's official CLI for Claude."
  system prompt **prepended automatically** for OAuth requests
- The Claude Code identity headers:
  - `Authorization: Bearer <access>`
  - `anthropic-beta: claude-code-20250219,oauth-2025-04-20`
  - `user-agent: claude-cli/2.1.75`
  - `x-app: cli`

Anthropic explicitly checks these headers + the system prompt; OAuth
tokens with stock headers get rejected. Without the reference repo
I'd have spent hours guessing.

## Files added/changed

```
internal/provider/oauth.go              NEW: PKCE, callback server,
                                          browser launch, HTML pages,
                                          OAuthProvider interface, registry
internal/provider/oauth_anthropic.go    NEW: Anthropic-specific Login +
                                          Refresh, constants from pi.dev
internal/provider/anthropic.go          modified: detect sk-ant-oat tokens,
                                          switch to Bearer + Claude Code
                                          identity, prepend system prompt
internal/store/auth.go                  added: SetOAuth, LookupAuthEntry
                                          (refresh logic stays in main.go to
                                          avoid import cycle)
main.go                                 added: oauthURLMsg, oauthDoneMsg,
                                          loginModeAuthMethod,
                                          loginModeOAuthRunning,
                                          runOAuthLogin tea.Cmd,
                                          refreshIfNeeded for auto-refresh,
                                          picker UI two-step flow
                                          (provider → auth-method →
                                          OAuth | key entry)
~/Projects/plan9-agent/launch-9front-detached.sh   QEMU hostfwd 53692
~/Projects/plan9-agent/boot-9front.sh              QEMU hostfwd 53692
wiki/assets/pi9-phase10s2-picker-subscription.png  picker w/ badge
wiki/assets/pi9-phase10s2-authmethod.png           auth-method picker
```

## Architecture

### OAuth flow (Anthropic)

1. User picks "Anthropic" → "Use subscription"
2. `runOAuthLogin(ProviderAnthropic)` tea.Cmd fires
3. Goroutine in the cmd:
   - `generatePKCE()` returns (verifier, challenge)
   - `startCallbackServer(53692, "/callback", verifier)` binds the port
   - Builds authorize URL with PKCE challenge + state=verifier
   - `OnAuthURL(url)` callback → `teaSendFn(oauthURLMsg{url})` →
     UI shows URL in status bar
   - `openBrowser(url)` runs `plumb -d web <url>` on plan9
   - `srv.waitForCode(ctx)` blocks until the redirect arrives
   - `exchangeAuthorizationCode(code, state, verifier, redirectURI)`
     POSTs to `console.anthropic.com/v1/oauth/token`
   - Returns OAuthCredentials{Access, Refresh, ExpiresAt}
4. `store.SetOAuth(provider, access, refresh, expiresAt)` persists to
   auth.json
5. `teaSendFn(oauthDoneMsg{provider})` returns UI to chat mode

### auth.json shape (with OAuth)

```json
{
  "anthropic": {
    "type": "oauth",
    "access_token": "sk-ant-oat01-...",
    "refresh_token": "sk-ant-ort01-...",
    "expires_at": 1763392800000
  },
  "openrouter": {
    "type": "api_key",
    "key": "sk-or-v1-..."
  }
}
```

`expires_at` is Unix milliseconds (matches pi.dev + Anthropic's
convention). `LookupAPIKey(provider)` returns AccessToken for OAuth
entries; auto-refresh kicks in via `refreshIfNeeded` in main.go.

### Auto-refresh

```go
// main.go runStream
providerID := provider.ProviderForModel(model)
key := refreshIfNeeded(ctx, providerID)   // refreshes if expired
if key == "" {
    key = store.LookupAPIKey(string(providerID))
}
```

`refreshIfNeeded` is synchronous (blocks ~1-2s on the first message
after expiry). Background-refresh goroutine would be cleaner but
needs more state management; deferred.

5-minute pre-emptive refresh (matches pi.dev): tokens are refreshed
when within 5 minutes of expiry, so users don't hit "token expired
mid-request" surprises.

### Browser launch on plan9

```go
case "plan9":
    cmd = exec.Command("/bin/plumb", "-d", "web", rawURL)
```

`plumb -d web` is plan9's "open in default browser" — routes to
mothra/netsurf via the plumber rules. Best-effort: errors are swallowed
because the user can also copy the URL from the status bar manually.

### Picker UI flow

```
loginModeOff (chat)
  /login
  ↓
loginModePicker (provider list)
  Anthropic (subscription available) ← enter
  ↓
loginModeAuthMethod (Anthropic-specific choice)
  > Use Anthropic subscription (browser OAuth)  ← enter
  ↓
loginModeOAuthRunning (browser opens, callback awaited)
  oauth in progress... ctrl+c to cancel
  ↓
loginModeOff (back to chat, /login (oauth) turn in scrollback)
```

For providers WITHOUT OAuth (OpenAI, Groq, etc.), the picker skips
the auth-method step and goes straight to key entry.

## Verified

| | Verified |
|---|---|
| Provider list shows "(subscription available)" badge for Anthropic | ✅ pi9-phase10s2-picker-subscription.png |
| Selecting Anthropic shows auth-method picker | ✅ pi9-phase10s2-authmethod.png |
| Both builds clean | ✅ darwin/arm64 + plan9/amd64 (8.4MB) |

## NOT verified

| | Why |
|---|---|
| Live OAuth flow end-to-end | QEMU port-forward for 53692 needs VM reboot, which has bricked the disk once already. Deferred to next session. |
| Token refresh | Code path is in place; needs real expired token to exercise |
| Tool calls via OAuth | The Anthropic provider's tool wiring is the same regardless of auth type, but the Claude Code identity headers might affect what tools the model offers |

## Quirks logged

### The browser is on a different machine than the callback server

This is the central tension. The callback server binds to
`127.0.0.1:53692` **inside the plan9 VM**. The user's actual browser
is on the **Mac**. Solutions:

- **Port-forward 53692 from Mac to VM** (QEMU hostfwd). Now the
  Mac's `localhost:53692` reaches the VM's callback server.
  Done — both launch scripts updated.
- **Manual code paste fallback**: pi.dev's flow supports pasting
  the full redirect URL as a fallback. Pi9 has the scaffolding
  (`onManualCodeInput` style) but doesn't wire it yet. If the
  callback doesn't arrive, the user is stuck. Should add a "paste
  URL here" prompt as Session 3 polish.

### Anthropic OAuth rejects non-Claude-Code requests

Tokens that go to `/v1/messages` MUST include:
- `Authorization: Bearer <access>` (NOT `x-api-key`)
- `anthropic-beta: claude-code-20250219,oauth-2025-04-20`
- `user-agent: claude-cli/X.Y.Z`
- `x-app: cli`
- First system message MUST be "You are Claude Code, Anthropic's
  official CLI for Claude."

Without these, Anthropic returns errors. Pi9's anthropic provider
detects OAuth tokens by `sk-ant-oat` prefix and switches modes
automatically.

### refresh_token may rotate

Some Anthropic refresh responses return a new refresh_token; some
don't. `oauth_anthropic.go` keeps the old one if the response is
empty.

### Cloning the reference repo was crucial

Pre-Session-2 I was reverse-engineering from docs. Cloning
earendil-works/pi gave me exact client_ids, scope strings, header
combinations, and the magic system prompt — saved hours of
guesswork and probably ruled out several failure modes (wrong scopes,
wrong betas, missing identity prompt).

## See Also

- [[pi9-phase10]] — Session 1: API-key multi-provider auth
- pi.dev source: <https://github.com/earendil-works/pi> cloned to
  /tmp/pi-reference (gitignored)
- `internal/provider/oauth.go` — generic PKCE + callback + browser
- `internal/provider/oauth_anthropic.go` — Anthropic-specific
- `internal/provider/anthropic.go` — token-type detection in Stream
- wiki/assets/pi9-phase10s2-picker-subscription.png
- wiki/assets/pi9-phase10s2-authmethod.png
