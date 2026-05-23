// Package provider: GitHub Copilot OAuth implementation.
//
// Ported from pi.dev's packages/ai/src/utils/oauth/github-copilot.ts.
//
// Copilot uses **device code flow** (RFC 8628), NOT authorization
// code + PKCE like Anthropic/Codex. This is GREAT for plan9:
//
//   - No callback server (port to bind, redirect to receive)
//   - No "browser on Mac, callback in VM" cross-machine problem
//   - Just: show the user a code and a URL, poll until they enter
//     it
//
// Flow:
//   1. POST github.com/login/device/code → device_code + user_code +
//      verification_uri + interval + expires_in
//   2. Show user: "go to https://github.com/login/device and enter
//      ABCD-EFGH"
//   3. Poll github.com/login/oauth/access_token every N seconds
//      until success (or expiry). Server returns
//      `authorization_pending` until user completes, then
//      `access_token`.
//   4. Exchange GitHub access_token for the Copilot token via
//      api.github.com/copilot_internal/v2/token (different
//      endpoint, returns a token with embedded expiry +
//      `proxy-ep=...` for the API base URL).
//
// We DO NOT call the model-policy enablement endpoint pi.dev does
// — pi9 uses whatever models GitHub gives us by default. Adding
// policy acceptance would be a 30-LOC addition; deferred until
// someone hits "this model isn't enabled" errors.
//
// Token usage in API requests (handled by copilotProvider, not
// here): Bearer auth + special Editor-Version + Copilot-
// Integration-Id headers + base URL extracted from the token's
// `proxy-ep` field. Pi9's openaiCompat does most of this; copilot
// gets its own provider type to override the URL + headers.
package provider

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"regexp"
	"strings"
	"time"
)

const (
	copilotClientID = "Iv1.b507a08c87ecfe98"
	// Domains: github.com for personal/free, ghe.com for enterprise.
	// We only support github.com for v1. Enterprise = future work.
	copilotDeviceCodeURL  = "https://github.com/login/device/code"
	copilotAccessTokenURL = "https://github.com/login/oauth/access_token"
	copilotTokenExchange  = "https://api.github.com/copilot_internal/v2/token"
	copilotScope          = "read:user"
)

// Identity headers Copilot's API requires. Pi.dev pins VS Code's
// version strings; we copy them verbatim. If GitHub tightens checks,
// we'd need to bump these to match a newer VS Code release.
var copilotHeaders = map[string]string{
	"User-Agent":             "GitHubCopilotChat/0.35.0",
	"Editor-Version":         "vscode/1.107.0",
	"Editor-Plugin-Version":  "copilot-chat/0.35.0",
	"Copilot-Integration-Id": "vscode-chat",
}

// githubCopilotOAuth implements OAuthProvider for GitHub Copilot.
type githubCopilotOAuth struct{}

func (githubCopilotOAuth) ID() ProviderID {
	// We use a dedicated ProviderID rather than reusing one of the
	// existing ones because Copilot's API path is materially
	// different (its own URL, special headers). Defined in types.go.
	return ProviderCopilot
}

func (githubCopilotOAuth) DisplayLabel() string {
	return "GitHub Copilot (subscription)"
}

// Login runs the device-code flow. Unlike Anthropic/Codex this
// flow doesn't need a callback server — the user types the code
// into a browser and we poll until GitHub says "logged in".
func (githubCopilotOAuth) Login(ctx context.Context, cb OAuthCallbacks) (OAuthCredentials, error) {
	// Step 1: start device flow.
	device, err := copilotStartDevice(ctx)
	if err != nil {
		return OAuthCredentials{}, fmt.Errorf("copilot device flow: %w", err)
	}

	// Step 2: tell the user where to go + the code to enter.
	// We piggyback on OnAuthURL since it's already wired through to
	// the status bar. The "url" we send is the verification URL +
	// user_code formatted as one string so it's all visible.
	if cb.OnAuthURL != nil {
		cb.OnAuthURL(fmt.Sprintf("Open %s and enter code: %s", device.VerificationURI, device.UserCode))
	}
	if cb.OnProgress != nil {
		cb.OnProgress(fmt.Sprintf("enter code %s at %s — polling...", device.UserCode, device.VerificationURI))
	}

	// Also try to open the verification page automatically.
	_ = openBrowser(device.VerificationURI)

	// Step 3: poll for completion. Returns the GitHub access token,
	// which is then exchanged for the Copilot token.
	githubToken, err := copilotPollForToken(ctx, device, cb)
	if err != nil {
		return OAuthCredentials{}, err
	}

	if cb.OnProgress != nil {
		cb.OnProgress("exchanging for Copilot token...")
	}

	// Step 4: exchange GitHub token for Copilot-specific token.
	copilotCreds, err := copilotExchangeToken(ctx, githubToken)
	if err != nil {
		return OAuthCredentials{}, fmt.Errorf("copilot token exchange: %w", err)
	}

	return copilotCreds, nil
}

// Refresh: Copilot's refresh story is unusual. The GitHub access
// token (returned by polling) is the long-lived one; the Copilot
// token (returned by the exchange endpoint) is short-lived but can
// be re-fetched at any time by calling copilotExchangeToken with
// the GitHub token again.
//
// So: pi9 stores the GitHub token in the Refresh field and the
// Copilot token in Access. Refresh re-calls the exchange.
func (githubCopilotOAuth) Refresh(ctx context.Context, refresh string) (OAuthCredentials, error) {
	return copilotExchangeToken(ctx, refresh)
}

// ----- Device flow helpers -----

type copilotDeviceCode struct {
	DeviceCode      string `json:"device_code"`
	UserCode        string `json:"user_code"`
	VerificationURI string `json:"verification_uri"`
	Interval        int    `json:"interval"`
	ExpiresIn       int    `json:"expires_in"`
}

func copilotStartDevice(ctx context.Context) (*copilotDeviceCode, error) {
	body := url.Values{}
	body.Set("client_id", copilotClientID)
	body.Set("scope", copilotScope)

	req, err := http.NewRequestWithContext(ctx, "POST", copilotDeviceCodeURL, strings.NewReader(body.Encode()))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("User-Agent", "GitHubCopilotChat/0.35.0")

	client := httpClient()
	client.Timeout = 30 * time.Second
	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("device code POST: %w", err)
	}
	defer resp.Body.Close()

	respBody, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("device code POST: HTTP %d: %s", resp.StatusCode, truncForErr(respBody))
	}
	var dc copilotDeviceCode
	if err := json.Unmarshal(respBody, &dc); err != nil {
		return nil, fmt.Errorf("device code: parse: %w (body=%s)", err, truncForErr(respBody))
	}
	if dc.DeviceCode == "" || dc.UserCode == "" || dc.VerificationURI == "" {
		return nil, fmt.Errorf("device code: missing fields (body=%s)", truncForErr(respBody))
	}
	if dc.Interval == 0 {
		dc.Interval = 5 // GitHub default
	}
	return &dc, nil
}

// copilotPollForToken polls the access_token endpoint until success
// or expiry. Handles:
//   - authorization_pending (still waiting for user — sleep, retry)
//   - slow_down (poll less aggressively — bump interval)
//   - any other error (terminal)
//
// Pi.dev uses an exponential-ish backoff (1.2x or 1.4x multiplier
// on slow_down). We do the same. ctx cancellation stops the poll.
func copilotPollForToken(ctx context.Context, device *copilotDeviceCode, cb OAuthCallbacks) (string, error) {
	deadline := time.Now().Add(time.Duration(device.ExpiresIn) * time.Second)
	intervalMs := device.Interval * 1000
	if intervalMs < 1000 {
		intervalMs = 1000
	}
	// Multiplier: start 1.2x normal interval; on slow_down bump to 1.4x.
	multiplier := 1.2

	for time.Now().Before(deadline) {
		if ctx.Err() != nil {
			return "", ctx.Err()
		}

		// Sleep first (per RFC 8628: client polls AFTER waiting interval).
		waitMs := int(float64(intervalMs) * multiplier)
		select {
		case <-time.After(time.Duration(waitMs) * time.Millisecond):
		case <-ctx.Done():
			return "", ctx.Err()
		}

		// Poll.
		body := url.Values{}
		body.Set("client_id", copilotClientID)
		body.Set("device_code", device.DeviceCode)
		body.Set("grant_type", "urn:ietf:params:oauth:grant-type:device_code")

		req, err := http.NewRequestWithContext(ctx, "POST", copilotAccessTokenURL, strings.NewReader(body.Encode()))
		if err != nil {
			return "", err
		}
		req.Header.Set("Accept", "application/json")
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
		req.Header.Set("User-Agent", "GitHubCopilotChat/0.35.0")

		client := httpClient()
		client.Timeout = 30 * time.Second
		resp, err := client.Do(req)
		if err != nil {
			// Transient network error — keep polling.
			continue
		}
		respBody, _ := io.ReadAll(resp.Body)
		resp.Body.Close()

		var success struct {
			AccessToken string `json:"access_token"`
		}
		var fail struct {
			Error            string `json:"error"`
			ErrorDescription string `json:"error_description"`
			Interval         int    `json:"interval"`
		}

		// Try success first.
		if err := json.Unmarshal(respBody, &success); err == nil && success.AccessToken != "" {
			return success.AccessToken, nil
		}
		// Try error response.
		if err := json.Unmarshal(respBody, &fail); err == nil && fail.Error != "" {
			switch fail.Error {
			case "authorization_pending":
				// User hasn't entered code yet. Keep polling at
				// current rate.
				continue
			case "slow_down":
				// GitHub asked us to back off. Bump interval per
				// their hint or +5s.
				if fail.Interval > 0 {
					intervalMs = fail.Interval * 1000
				} else {
					intervalMs += 5000
				}
				multiplier = 1.4
				continue
			case "expired_token", "access_denied":
				return "", fmt.Errorf("copilot device flow: %s — %s", fail.Error, fail.ErrorDescription)
			default:
				return "", fmt.Errorf("copilot device flow: %s — %s", fail.Error, fail.ErrorDescription)
			}
		}
		// Unparseable response — log + retry.
		if cb.OnProgress != nil {
			cb.OnProgress("unexpected response, retrying...")
		}
	}
	return "", fmt.Errorf("copilot device flow: timed out waiting for user to enter code")
}

// copilotExchangeToken trades a GitHub access token for a Copilot
// API token. The Copilot token has its own expiry (in seconds since
// epoch) and embedded `proxy-ep=...` that pi9's copilotProvider
// uses to derive the API base URL.
func copilotExchangeToken(ctx context.Context, githubToken string) (OAuthCredentials, error) {
	req, err := http.NewRequestWithContext(ctx, "GET", copilotTokenExchange, nil)
	if err != nil {
		return OAuthCredentials{}, err
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("Authorization", "Bearer "+githubToken)
	for k, v := range copilotHeaders {
		req.Header.Set(k, v)
	}

	client := httpClient()
	client.Timeout = 30 * time.Second
	resp, err := client.Do(req)
	if err != nil {
		return OAuthCredentials{}, fmt.Errorf("copilot exchange GET: %w", err)
	}
	defer resp.Body.Close()
	respBody, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != 200 {
		return OAuthCredentials{}, fmt.Errorf("copilot exchange GET: HTTP %d: %s", resp.StatusCode, truncForErr(respBody))
	}

	var ex struct {
		Token     string `json:"token"`
		ExpiresAt int64  `json:"expires_at"` // unix seconds
	}
	if err := json.Unmarshal(respBody, &ex); err != nil {
		return OAuthCredentials{}, fmt.Errorf("copilot exchange: parse: %w (body=%s)", err, truncForErr(respBody))
	}
	if ex.Token == "" || ex.ExpiresAt == 0 {
		return OAuthCredentials{}, fmt.Errorf("copilot exchange: missing fields (body=%s)", truncForErr(respBody))
	}

	// Refresh comes from the original GitHub token (long-lived).
	// Access is the Copilot token (short-lived, contains proxy-ep).
	// Expires shaved by 5min for safety. Multiply by 1000 to match
	// our unix-ms convention.
	return OAuthCredentials{
		Access:    ex.Token,
		Refresh:   githubToken,
		ExpiresAt: ex.ExpiresAt*1000 - 5*60*1000,
	}, nil
}

// CopilotBaseURL extracts the API base URL from a Copilot token's
// `proxy-ep=...` field. The token is a semicolon-separated key=value
// string; one of the keys is `proxy-ep=proxy.individual.githubcopilot.com`.
// We convert `proxy.X` to `api.X` (Copilot routes API requests through
// api.* hostnames even though the token mentions proxy.*).
//
// Falls back to `https://api.individual.githubcopilot.com` if the token
// doesn't have proxy-ep (older format).
//
// Exported because copilot.go (the Provider impl) uses this for each
// request.
var proxyEPRegex = regexp.MustCompile(`proxy-ep=([^;]+)`)

func CopilotBaseURL(token string) string {
	m := proxyEPRegex.FindStringSubmatch(token)
	if len(m) >= 2 {
		host := strings.Replace(m[1], "proxy.", "api.", 1)
		return "https://" + host
	}
	return "https://api.individual.githubcopilot.com"
}
