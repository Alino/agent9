// Package provider: Anthropic-specific OAuth implementation.
//
// Implements Claude Pro/Max OAuth. Mirrors pi.dev's
// packages/ai/src/utils/oauth/anthropic.ts.
//
// The client_id is the same one Claude Code itself uses
// (9d1c250a-e61b-44d9-88ed-5944d1962f5e). Pi.dev's version
// base64-decodes it to obscure it slightly; we just inline the plain
// value — anyone who can read the binary can read the literal too,
// and the scrambling doesn't add real security.
//
// Anthropic OAuth Bearer tokens look like `sk-ant-oat01-...`. The
// provider Stream code (anthropic.go) detects that prefix and uses
// `Authorization: Bearer ...` + Claude Code identity headers
// (anthropic-beta: claude-code-20250219,oauth-2025-04-20 + user-agent:
// claude-cli/X.Y.Z + x-app: cli) instead of the api_key header.
//
// CRITICAL: when using OAuth tokens, the FIRST system message MUST
// be "You are Claude Code, Anthropic's official CLI for Claude."
// Otherwise Anthropic refuses the request. Pi9's prompt assembly
// prepends this when the token is OAuth (see anthropic.go).
package provider

import (
	"context"
	"encoding/json"
	"fmt"
	"net/url"
	"strings"
	"time"
)

const (
	anthropicClientID     = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
	anthropicAuthorizeURL = "https://claude.ai/oauth/authorize"
	anthropicTokenURL     = "https://console.anthropic.com/v1/oauth/token"
	anthropicCallbackPort = 53692
	anthropicCallbackPath = "/callback"
	// Scopes ported from pi.dev. user:inference is what we need for
	// /v1/messages. The other scopes are requested for forward-compat
	// with Claude Code's full feature set even though pi9 doesn't
	// use them today.
	anthropicScopes = "org:create_api_key user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload"
)

func anthropicCallbackURL() string {
	return fmt.Sprintf("http://localhost:%d%s", anthropicCallbackPort, anthropicCallbackPath)
}

// anthropicOAuth implements OAuthProvider for Claude Pro/Max.
type anthropicOAuth struct{}

func (anthropicOAuth) ID() ProviderID       { return ProviderAnthropic }
func (anthropicOAuth) DisplayLabel() string { return "Anthropic (Claude Pro/Max)" }

// Login runs the full Anthropic OAuth flow. Steps mirror pi.dev's:
//
//  1. Generate PKCE verifier + SHA-256 challenge
//  2. Start the local callback server on port 53692
//  3. Build authorize URL with code_challenge=challenge, state=verifier
//     (state==verifier is pi.dev's convention; it's NOT a security
//     hole because the state is also signed via PKCE — the state's
//     job here is purely CSRF, the verifier's job is the binding)
//  4. Open the URL in the browser via openBrowser()
//  5. Wait for the callback to deliver (code, state)
//  6. POST code+verifier+client_id to the token endpoint
//  7. Return parsed OAuthCredentials with expiry adjusted (we shave
//     5 min off the expires_in to refresh before actual expiry,
//     matching pi.dev's safety margin)
func (anthropicOAuth) Login(ctx context.Context, cb OAuthCallbacks) (OAuthCredentials, error) {
	verifier, challenge, err := generatePKCE()
	if err != nil {
		return OAuthCredentials{}, err
	}

	// Start callback server BEFORE opening browser so we don't miss
	// the redirect if the user clicks through quickly.
	srv, err := startCallbackServer(anthropicCallbackPort, anthropicCallbackPath, verifier)
	if err != nil {
		return OAuthCredentials{}, fmt.Errorf("anthropic oauth: %w (is another pi9/claude-code instance running an OAuth flow?)", err)
	}
	defer srv.close()

	// Build authorize URL.
	q := url.Values{}
	q.Set("code", "true") // pi.dev sets this; tells Anthropic to use the copy/paste-friendly redirect (still works with localhost server)
	q.Set("client_id", anthropicClientID)
	q.Set("response_type", "code")
	q.Set("redirect_uri", anthropicCallbackURL())
	q.Set("scope", anthropicScopes)
	q.Set("code_challenge", challenge)
	q.Set("code_challenge_method", "S256")
	q.Set("state", verifier)

	authURL := anthropicAuthorizeURL + "?" + q.Encode()

	if cb.OnAuthURL != nil {
		cb.OnAuthURL(authURL)
	}
	if cb.OnProgress != nil {
		cb.OnProgress("waiting for browser callback...")
	}

	// Attempt browser launch (best-effort). If it fails the user can
	// still copy the URL from OnAuthURL.
	_ = openBrowser(authURL) // ignore error; UI shows URL anyway

	// Wait for EITHER the browser callback OR a manual paste,
	// whichever completes first. Plan9 case: when pi9 runs inside a
	// VM, the browser is typically on the host (Mac), so the OAuth
	// callback hits 127.0.0.1:53692 on the HOST — and only reaches
	// the VM via QEMU's hostfwd. If that's not set up or fails for
	// any reason, the user can paste the code directly into pi9
	// (Anthropic's consent page shows `<code>#<state>` for exactly
	// this purpose when `code=true` is set in the authorize URL).
	ctxWait, cancel := context.WithTimeout(ctx, 5*time.Minute)
	defer cancel()

	var code, gotState string

	type srvResult struct {
		code, state string
		err         error
	}
	srvCh := make(chan srvResult, 1)
	go func() {
		c, s, e := srv.waitForCode(ctxWait)
		srvCh <- srvResult{c, s, e}
	}()

	manualCh := cb.ManualCode
	if manualCh == nil {
		// Closed channel never fires in select — treat as disabled.
		c := make(chan string)
		close(c)
		manualCh = c
	}

	select {
	case r := <-srvCh:
		if r.err != nil {
			// Browser callback failed. Don't bail yet — maybe the
			// user is about to paste. Switch to manual-only mode and
			// keep waiting.
			if cb.OnProgress != nil {
				cb.OnProgress("browser callback unreachable — paste code from browser to continue")
			}
			select {
			case input, ok := <-manualCh:
				if !ok {
					return OAuthCredentials{}, fmt.Errorf("anthropic oauth: %w (no manual input either)", r.err)
				}
				pc, ps := ParseAuthorizationInput(input)
				if pc == "" {
					return OAuthCredentials{}, fmt.Errorf("anthropic oauth: pasted input had no code")
				}
				if ps != "" && ps != verifier {
					return OAuthCredentials{}, fmt.Errorf("anthropic oauth: state mismatch (got %q want %q)", ps, verifier)
				}
				code = pc
				gotState = ps
			case <-ctxWait.Done():
				return OAuthCredentials{}, fmt.Errorf("anthropic oauth: timeout waiting for paste after callback failed: %w", r.err)
			}
		} else {
			code = r.code
			gotState = r.state
		}
	case input, ok := <-manualCh:
		if !ok {
			// Channel closed without sending → manual-paste path
			// abandoned. Wait for the server.
			r := <-srvCh
			if r.err != nil {
				return OAuthCredentials{}, fmt.Errorf("anthropic oauth callback: %w", r.err)
			}
			code = r.code
			gotState = r.state
		} else {
			pc, ps := ParseAuthorizationInput(input)
			if pc == "" {
				return OAuthCredentials{}, fmt.Errorf("anthropic oauth: pasted input had no code")
			}
			if ps != "" && ps != verifier {
				return OAuthCredentials{}, fmt.Errorf("anthropic oauth: state mismatch")
			}
			code = pc
			gotState = ps
		}
	}

	_ = gotState // we don't currently send state in token exchange, just validate above

	if code == "" {
		return OAuthCredentials{}, fmt.Errorf("anthropic oauth: empty code after wait")
	}

	if cb.OnProgress != nil {
		cb.OnProgress("exchanging code for tokens...")
	}

	// Exchange authorization code for tokens.
	body := map[string]interface{}{
		"grant_type":    "authorization_code",
		"client_id":     anthropicClientID,
		"code":          code,
		"state":         verifier,
		"redirect_uri":  anthropicCallbackURL(),
		"code_verifier": verifier,
	}
	respBytes, err := postOAuthJSON(ctx, anthropicTokenURL, body)
	if err != nil {
		return OAuthCredentials{}, fmt.Errorf("anthropic token exchange: %w", err)
	}

	var tok struct {
		AccessToken  string `json:"access_token"`
		RefreshToken string `json:"refresh_token"`
		ExpiresIn    int    `json:"expires_in"`
	}
	if err := json.Unmarshal(respBytes, &tok); err != nil {
		return OAuthCredentials{}, fmt.Errorf("anthropic token: parse: %w (body=%s)", err, truncForErr(respBytes))
	}
	if tok.AccessToken == "" {
		return OAuthCredentials{}, fmt.Errorf("anthropic token: empty access_token in response (body=%s)", truncForErr(respBytes))
	}

	// Refresh 5 minutes before actual expiry so users don't hit a
	// "token expired mid-request" surprise. Matches pi.dev's safety
	// margin.
	expiresMs := time.Now().UnixMilli() + int64(tok.ExpiresIn)*1000 - 5*60*1000

	return OAuthCredentials{
		Access:    tok.AccessToken,
		Refresh:   tok.RefreshToken,
		ExpiresAt: expiresMs,
	}, nil
}

// Refresh exchanges a refresh token for a fresh access token. Pi.dev's
// refresh POST is the same shape as the initial exchange but with
// grant_type=refresh_token. Returns new credentials with the refresh
// token possibly rotated (Anthropic sometimes issues a new one).
func (anthropicOAuth) Refresh(ctx context.Context, refresh string) (OAuthCredentials, error) {
	body := map[string]interface{}{
		"grant_type":    "refresh_token",
		"client_id":     anthropicClientID,
		"refresh_token": refresh,
	}
	respBytes, err := postOAuthJSON(ctx, anthropicTokenURL, body)
	if err != nil {
		return OAuthCredentials{}, fmt.Errorf("anthropic refresh: %w", err)
	}

	var tok struct {
		AccessToken  string `json:"access_token"`
		RefreshToken string `json:"refresh_token"`
		ExpiresIn    int    `json:"expires_in"`
	}
	if err := json.Unmarshal(respBytes, &tok); err != nil {
		return OAuthCredentials{}, fmt.Errorf("anthropic refresh: parse: %w (body=%s)", err, truncForErr(respBytes))
	}

	// Some token endpoints return a new refresh token on refresh; if
	// not, keep the old one.
	newRefresh := tok.RefreshToken
	if newRefresh == "" {
		newRefresh = refresh
	}

	expiresMs := time.Now().UnixMilli() + int64(tok.ExpiresIn)*1000 - 5*60*1000

	return OAuthCredentials{
		Access:    tok.AccessToken,
		Refresh:   newRefresh,
		ExpiresAt: expiresMs,
	}, nil
}

// truncForErr keeps OAuth error messages bounded so we don't dump
// 4KB of HTML into a single status bar entry.
func truncForErr(b []byte) string {
	s := strings.TrimSpace(string(b))
	if len(s) > 200 {
		return s[:200] + "..."
	}
	return s
}
