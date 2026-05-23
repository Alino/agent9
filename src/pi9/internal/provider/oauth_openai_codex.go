// Package provider: OpenAI Codex (ChatGPT Plus/Pro) OAuth implementation.
//
// Ported from pi.dev's packages/ai/src/utils/oauth/openai-codex.ts.
//
// KEY LIMITATION (Phase 10 Session 3): the OAuth flow itself works
// (login + token storage + refresh), but ACTUALLY USING the token
// requires OpenAI's Responses API at chatgpt.com/backend-api, which
// is a different wire format than Chat Completions. Pi9 doesn't
// implement Responses API today.
//
// Effect: a user can /login with ChatGPT Plus/Pro and get tokens
// stored. But /model gpt-4o (or any OpenAI Codex model) will fail
// at request time because the openaiCompat provider sends Chat
// Completions format, not Responses. We surface a clear error
// rather than silently failing. Sessions 4+ could implement the
// Responses API; out of scope for S3.
//
// Differences from Anthropic OAuth:
//   - Endpoint: auth.openai.com (not console.anthropic.com)
//   - Callback port: 1455 (not 53692), path /auth/callback
//   - Token request: application/x-www-form-urlencoded (Anthropic
//     uses JSON)
//   - Extra authorize params: id_token_add_organizations=true,
//     codex_cli_simplified_flow=true, originator=pi
//   - State is independent random bytes (pi.dev) not state=verifier
//     (Anthropic)
//   - JWT account ID extraction (we skip this — not needed for
//     refresh, and we don't use the token for requests yet)
package provider

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

const (
	codexClientID     = "app_EMoamEEZ73f0CkXaXp7hrann"
	codexAuthorizeURL = "https://auth.openai.com/oauth/authorize"
	codexTokenURL     = "https://auth.openai.com/oauth/token"
	codexCallbackPort = 1455
	codexCallbackPath = "/auth/callback"
	codexScope        = "openid profile email offline_access"
	codexOriginator   = "pi9"
)

func codexCallbackURL() string {
	return fmt.Sprintf("http://localhost:%d%s", codexCallbackPort, codexCallbackPath)
}

// openAICodexOAuth implements OAuthProvider for ChatGPT Plus/Pro.
type openAICodexOAuth struct{}

func (openAICodexOAuth) ID() ProviderID       { return ProviderOpenAI }
func (openAICodexOAuth) DisplayLabel() string { return "OpenAI (ChatGPT Plus/Pro)" }

// codexState generates a fresh anti-CSRF state token. Pi.dev uses
// 16 random bytes hex-encoded (32 chars). State is independent of
// the PKCE verifier here (unlike Anthropic which conflates them).
func codexState() (string, error) {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

func (openAICodexOAuth) Login(ctx context.Context, cb OAuthCallbacks) (OAuthCredentials, error) {
	verifier, challenge, err := generatePKCE()
	if err != nil {
		return OAuthCredentials{}, err
	}
	state, err := codexState()
	if err != nil {
		return OAuthCredentials{}, fmt.Errorf("codex oauth: state: %w", err)
	}

	srv, err := startCallbackServer(codexCallbackPort, codexCallbackPath, state)
	if err != nil {
		return OAuthCredentials{}, fmt.Errorf("codex oauth: %w (is another OAuth flow running? port %d in use)", err, codexCallbackPort)
	}
	defer srv.close()

	q := url.Values{}
	q.Set("response_type", "code")
	q.Set("client_id", codexClientID)
	q.Set("redirect_uri", codexCallbackURL())
	q.Set("scope", codexScope)
	q.Set("code_challenge", challenge)
	q.Set("code_challenge_method", "S256")
	q.Set("state", state)
	q.Set("id_token_add_organizations", "true")
	q.Set("codex_cli_simplified_flow", "true")
	q.Set("originator", codexOriginator)

	authURL := codexAuthorizeURL + "?" + q.Encode()

	if cb.OnAuthURL != nil {
		cb.OnAuthURL(authURL)
	}
	if cb.OnProgress != nil {
		cb.OnProgress("waiting for browser callback...")
	}

	_ = openBrowser(authURL)

	ctxWait, cancel := context.WithTimeout(ctx, 5*time.Minute)
	defer cancel()
	code, _, err := srv.waitForCode(ctxWait)
	if err != nil {
		return OAuthCredentials{}, fmt.Errorf("codex oauth callback: %w", err)
	}

	if cb.OnProgress != nil {
		cb.OnProgress("exchanging code for tokens...")
	}

	tok, err := codexExchangeCode(ctx, code, verifier)
	if err != nil {
		return OAuthCredentials{}, err
	}

	// Codex uses the chatgpt-account-id in every API request. It's
	// embedded in the JWT access token. We extract it now and stash
	// it on the credentials so codex_responses.go can read it
	// later. The AccountID field flows through AuthEntry to disk.
	accountID := extractCodexAccountID(tok.AccessToken)

	expiresMs := time.Now().UnixMilli() + int64(tok.ExpiresIn)*1000 - 5*60*1000
	return OAuthCredentials{
		Access:    tok.AccessToken,
		Refresh:   tok.RefreshToken,
		ExpiresAt: expiresMs,
		AccountID: accountID,
	}, nil
}

// extractCodexAccountID decodes the OAuth access token (a JWT) and
// pulls the chatgpt_account_id from the
// `https://api.openai.com/auth` claim. Returns "" on any error —
// pi.dev errors out in this case but we tolerate it (the API will
// 400 later with a clear message if the header is missing, and we
// don't want to fail login over a parsing edge case).
func extractCodexAccountID(token string) string {
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return ""
	}
	// JWT payload is base64url-encoded.
	payload, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		// Some tokens use padded base64. Try standard.
		payload, err = base64.StdEncoding.DecodeString(parts[1])
		if err != nil {
			return ""
		}
	}
	var claims map[string]interface{}
	if err := json.Unmarshal(payload, &claims); err != nil {
		return ""
	}
	authClaim, _ := claims["https://api.openai.com/auth"].(map[string]interface{})
	if authClaim == nil {
		return ""
	}
	accountID, _ := authClaim["chatgpt_account_id"].(string)
	return accountID
}

// codexExchangeCode does the authorization-code exchange. Differs
// from Anthropic in TWO ways:
//   - body is form-encoded (application/x-www-form-urlencoded)
//   - we use a dedicated POST helper because postOAuthJSON in
//     oauth.go assumes JSON
func codexExchangeCode(ctx context.Context, code, verifier string) (codexTokenResp, error) {
	body := url.Values{}
	body.Set("grant_type", "authorization_code")
	body.Set("client_id", codexClientID)
	body.Set("code", code)
	body.Set("code_verifier", verifier)
	body.Set("redirect_uri", codexCallbackURL())

	return codexPostForm(ctx, codexTokenURL, body)
}

// Refresh exchanges a refresh token. Form-encoded like exchange.
func (openAICodexOAuth) Refresh(ctx context.Context, refresh string) (OAuthCredentials, error) {
	body := url.Values{}
	body.Set("grant_type", "refresh_token")
	body.Set("refresh_token", refresh)
	body.Set("client_id", codexClientID)

	tok, err := codexPostForm(ctx, codexTokenURL, body)
	if err != nil {
		return OAuthCredentials{}, fmt.Errorf("codex refresh: %w", err)
	}

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

type codexTokenResp struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	ExpiresIn    int    `json:"expires_in"`
}

// codexPostForm POSTs form-encoded body, parses JSON response.
// Variant of postOAuthJSON for the form-encoded case.
func codexPostForm(ctx context.Context, urlStr string, body url.Values) (codexTokenResp, error) {
	req, err := http.NewRequestWithContext(ctx, "POST", urlStr, strings.NewReader(body.Encode()))
	if err != nil {
		return codexTokenResp{}, err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("Accept", "application/json")

	client := httpClient()
	client.Timeout = 30 * time.Second
	resp, err := client.Do(req)
	if err != nil {
		return codexTokenResp{}, fmt.Errorf("codex POST: %w", err)
	}
	defer resp.Body.Close()

	respBody, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != 200 {
		return codexTokenResp{}, fmt.Errorf("codex POST: HTTP %d: %s", resp.StatusCode, truncForErr(respBody))
	}

	var tok codexTokenResp
	if err := json.Unmarshal(respBody, &tok); err != nil {
		return codexTokenResp{}, fmt.Errorf("codex POST: parse: %w (body=%s)", err, truncForErr(respBody))
	}
	if tok.AccessToken == "" {
		return codexTokenResp{}, fmt.Errorf("codex POST: empty access_token (body=%s)", truncForErr(respBody))
	}
	return tok, nil
}
