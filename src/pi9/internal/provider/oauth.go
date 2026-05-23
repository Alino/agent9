// Package provider: OAuth scaffolding shared across subscription providers.
//
// Phase 10 Session 2 introduces OAuth so pi9 can use Claude Pro/Max
// (without consuming API credits — Claude Pro/Max usage doesn't bill
// per token but draws from the user's subscription quota). Future
// sessions will add ChatGPT Plus/Pro (Codex) + GitHub Copilot.
//
// Design ported from pi.dev (earendil-works/pi:
// packages/ai/src/utils/oauth/anthropic.ts). Pi.dev's flow:
//   1. Generate PKCE verifier + challenge
//   2. Start local HTTP server on a fixed port (53692 for Anthropic)
//      to catch the OAuth redirect
//   3. Build authorize URL with PKCE challenge + scopes + state
//   4. Open URL in user's browser (plumber on plan9, xdg-open/open on
//      unix). If no browser available, show URL for user to paste.
//   5. User authenticates in browser; redirect comes back to our
//      local server with `code` and `state` query params
//   6. Verify state matches (CSRF protection)
//   7. POST code+verifier to token endpoint, get access+refresh tokens
//   8. Persist to auth.json with expires timestamp
//
// Plan 9 specifics:
//   - Browser launch: `plumb -d web <url>` (uses default web handler
//     in plumber rules — usually mothra or netsurf)
//   - HTTP server: net.Listen on 127.0.0.1:53692 works fine on plan9
//   - PKCE: SHA-256 + base64url is in crypto/sha256 + encoding/base64
package provider

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os/exec"
	"runtime"
	"strings"
	"time"
)

// OAuthCredentials is what an OAuth provider returns after a
// successful login. Mirrors auth.json's OAuth shape. Refresh has the
// long-lived refresh token, Access is the short-lived bearer token,
// ExpiresAt is Unix milliseconds (matches pi.dev + Anthropic's
// convention).
//
// AccountID is provider-specific extra metadata that comes out of
// the OAuth flow and needs to live alongside the tokens. Currently
// only Codex uses it (chatgpt-account-id from JWT) but the field
// is generic so future providers can stash their own equivalents.
type OAuthCredentials struct {
	Access    string `json:"access"`
	Refresh   string `json:"refresh"`
	ExpiresAt int64  `json:"expires_at"` // unix milliseconds
	AccountID string `json:"account_id,omitempty"`
}

// IsExpired returns true if the access token has expired or is
// within `skew` of expiry. Pi.dev uses a 5-minute skew to refresh
// proactively. Use the same.
func (c OAuthCredentials) IsExpired(skew time.Duration) bool {
	if c.ExpiresAt == 0 {
		return false // no expiry info; assume good
	}
	nowMs := time.Now().UnixMilli()
	return nowMs+skew.Milliseconds() >= c.ExpiresAt
}

// OAuthProvider is implemented by providers that support OAuth login.
// Distinct from the Streamer Provider interface — OAuthProvider only
// covers the auth side. A Streamer provider can also be an
// OAuthProvider (Anthropic in Phase 10 is both).
type OAuthProvider interface {
	// ID is the same as the Streamer's ProviderID (e.g. "anthropic").
	ID() ProviderID

	// DisplayLabel is shown in the /login picker, with a subscription
	// hint ("Anthropic (Claude Pro/Max)").
	DisplayLabel() string

	// Login starts the OAuth flow and returns credentials on success.
	// Callbacks let the caller inject the browser-open + status-update
	// behavior. ctx cancellation stops the flow at the next safe point.
	Login(ctx context.Context, cb OAuthCallbacks) (OAuthCredentials, error)

	// Refresh exchanges an expired access token for a fresh one using
	// the refresh token. Returns new credentials with updated
	// expires_at. Should be called automatically before requests when
	// IsExpired returns true.
	Refresh(ctx context.Context, refresh string) (OAuthCredentials, error)
}

// OAuthCallbacks is the user-facing side of the OAuth dance, injected
// by the UI layer (main.go). Lets the picker reflect what's happening.
type OAuthCallbacks struct {
	// OnAuthURL is called once the URL is built and the server is
	// listening. The UI should open the URL in the browser AND show
	// it to the user (in case the auto-open fails). Pi9's
	// implementation calls openBrowser(url) here and updates the
	// status bar to "complete login in your browser...".
	OnAuthURL func(authURL string)

	// OnProgress reports milestone strings: "waiting for browser
	// callback...", "exchanging code...", etc. Optional.
	OnProgress func(msg string)

	// ManualCode is a channel the OAuth provider drains in parallel
	// with waiting for the browser callback. The UI layer can pump
	// user-pasted text (an authorization code, a full redirect URL,
	// or `code#state`) into this channel; whichever path completes
	// first wins.
	//
	// Pi.dev calls this "onManualCodeInput". Critical for plan9 where
	// the browser is typically on the Mac host while pi9 runs in the
	// VM — if QEMU's port forward fails, the browser callback never
	// reaches pi9, and manual paste is the only way through.
	//
	// Closing this channel signals "no manual input will come". The
	// provider treats that as "fall back to browser-only wait".
	//
	// Nil means manual-paste is disabled (back-compat).
	ManualCode <-chan string
}

// ParseAuthorizationInput accepts ANY of these shapes a user might
// paste after completing OAuth in a browser:
//
//   - Full redirect URL: `http://localhost:53692/callback?code=ABC&state=XYZ`
//   - Anthropic's copy-paste format: `<code>#<state>` (what their consent
//     page displays after click-through with `code=true` flag)
//   - Bare query string: `code=ABC&state=XYZ`
//   - Just the code: `ABC` (lossy — state can't be verified)
//
// Returns code and state separately. Empty if input is empty.
// Ported from pi.dev parseAuthorizationInput.
func ParseAuthorizationInput(input string) (code, state string) {
	v := strings.TrimSpace(input)
	if v == "" {
		return "", ""
	}

	// URL form
	if u, err := url.Parse(v); err == nil && u.Scheme != "" {
		q := u.Query()
		return q.Get("code"), q.Get("state")
	}

	// code#state form (Anthropic's copy-friendly format)
	if strings.Contains(v, "#") {
		parts := strings.SplitN(v, "#", 2)
		return parts[0], parts[1]
	}

	// bare query string
	if strings.Contains(v, "code=") {
		if q, err := url.ParseQuery(v); err == nil {
			return q.Get("code"), q.Get("state")
		}
	}

	// last resort: treat the whole thing as a code (no state)
	return v, ""
}

// ---------- PKCE ----------

// generatePKCE returns a fresh PKCE verifier (random 32 bytes,
// base64url-encoded) and its SHA-256 challenge (also base64url).
// Pi.dev uses these directly in the authorize URL.
func generatePKCE() (verifier, challenge string, err error) {
	b := make([]byte, 32)
	if _, err = rand.Read(b); err != nil {
		return "", "", fmt.Errorf("pkce: random: %w", err)
	}
	verifier = base64.RawURLEncoding.EncodeToString(b)
	h := sha256.Sum256([]byte(verifier))
	challenge = base64.RawURLEncoding.EncodeToString(h[:])
	return verifier, challenge, nil
}

// ---------- Callback server ----------

// callbackServer is a one-shot HTTP server that listens for the OAuth
// redirect. It captures the `code` and `state` query params, sends
// them to a result channel, then shuts down.
//
// Lifetime: start before opening the browser, wait for code via
// waitForCode(), call close() in defer to release the port even on
// error/timeout.
type callbackServer struct {
	srv      *http.Server
	listener net.Listener
	port     int
	path     string
	expState string
	result   chan callbackResult
}

type callbackResult struct {
	code  string
	state string
	err   error
}

// startCallbackServer binds to 127.0.0.1:<port>/<path> and waits for
// the OAuth redirect. The expected state is checked here so a CSRF
// attack returning a different state never reaches the token
// exchange.
//
// Returns an error if the port is in use (which usually means a
// previous failed OAuth flow didn't clean up — kill stragglers and
// retry). Pi.dev uses a fixed port (53692 for Anthropic) because the
// authorize endpoint must know the exact redirect_uri in advance.
//
// Bind address: ":<port>" instead of "127.0.0.1:<port>" because
// plan9's `net.Listen("tcp", "127.0.0.1:N")` fails with "not a local
// IP address" — plan9 has no implicit loopback alias and Go's plan9
// net stack validates the bind IP against /net interfaces. Using
// ":N" binds to all interfaces (equivalent to 0.0.0.0:N) which
// works on plan9 and is harmless on Linux/Mac (loopback gets bound
// too).
//
// The OAuth provider doesn't care which interface answers; what
// matters is the redirect URI's hostname (`http://localhost:N/...`)
// resolving correctly when the BROWSER hits it. Localhost resolves
// to 127.0.0.1 on every OS and Plan9's TCP stack accepts incoming
// connections to that IP regardless of bind address.
func startCallbackServer(port int, path, expectedState string) (*callbackServer, error) {
	addr := fmt.Sprintf(":%d", port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("oauth callback: listen %s: %w (is another pi9/claude-code instance running an OAuth flow?)", addr, err)
	}

	cs := &callbackServer{
		listener: ln,
		port:     port,
		path:     path,
		expState: expectedState,
		result:   make(chan callbackResult, 1),
	}

	mux := http.NewServeMux()
	mux.HandleFunc(path, cs.handle)
	cs.srv = &http.Server{Handler: mux}

	go func() {
		_ = cs.srv.Serve(ln)
	}()

	return cs, nil
}

func (cs *callbackServer) handle(w http.ResponseWriter, r *http.Request) {
	code := r.URL.Query().Get("code")
	state := r.URL.Query().Get("state")
	errParam := r.URL.Query().Get("error")

	if errParam != "" {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprintf(w, oauthErrorHTML, "Authentication did not complete: "+errParam)
		select {
		case cs.result <- callbackResult{err: fmt.Errorf("oauth: %s", errParam)}:
		default:
		}
		return
	}
	if code == "" || state == "" {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprintf(w, oauthErrorHTML, "Missing code or state parameter.")
		select {
		case cs.result <- callbackResult{err: fmt.Errorf("oauth: missing code/state")}:
		default:
		}
		return
	}
	if state != cs.expState {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprintf(w, oauthErrorHTML, "State mismatch (possible CSRF). Please try again.")
		select {
		case cs.result <- callbackResult{err: fmt.Errorf("oauth: state mismatch")}:
		default:
		}
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	fmt.Fprint(w, oauthSuccessHTML)
	select {
	case cs.result <- callbackResult{code: code, state: state}:
	default:
	}
}

// waitForCode blocks until the OAuth callback arrives or ctx is
// canceled. Returns the validated code/state pair or an error.
func (cs *callbackServer) waitForCode(ctx context.Context) (string, string, error) {
	select {
	case r := <-cs.result:
		if r.err != nil {
			return "", "", r.err
		}
		return r.code, r.state, nil
	case <-ctx.Done():
		return "", "", ctx.Err()
	}
}

func (cs *callbackServer) close() {
	// Brief shutdown grace so the success page reaches the browser.
	ctx, cancel := context.WithTimeout(context.Background(), 1*time.Second)
	defer cancel()
	_ = cs.srv.Shutdown(ctx)
}

// ---------- Browser launch ----------

// openBrowser attempts to open `url` in the user's default web
// browser. Best-effort: errors are returned but callers usually just
// log them and show the URL for manual paste.
//
// Plan 9 path: `plumb -d web <url>` — plumber routes web URLs to the
// configured handler (mothra by default on 9front, or NetSurf if the
// rules have been customized). plumb is in /bin on standard plan9.
//
// Other platforms:
//   - macOS:  open <url>
//   - linux:  xdg-open <url>
//   - windows: rundll32 url.dll,FileProtocolHandler <url>
//
// Returns nil if the command was at least dispatched (we don't wait
// for the browser to exit). Returns error if the command itself is
// missing or fails to exec.
func openBrowser(rawURL string) error {
	// Sanity-check the URL — `plumb -d web` will pass arbitrary
	// strings through, but we don't want a malformed URL to silently
	// route to nothing.
	if _, err := url.ParseRequestURI(rawURL); err != nil {
		return fmt.Errorf("openBrowser: invalid url: %w", err)
	}

	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "plan9":
		// plumb's -d flag specifies the destination port; "web" is
		// the conventional port for browser URLs.
		cmd = exec.Command("/bin/plumb", "-d", "web", rawURL)
	case "darwin":
		cmd = exec.Command("/usr/bin/open", rawURL)
	case "windows":
		cmd = exec.Command("rundll32", "url.dll,FileProtocolHandler", rawURL)
	default: // linux, freebsd, etc.
		cmd = exec.Command("xdg-open", rawURL)
	}

	// Detach: we don't care about the browser's exit code, and
	// blocking until the user closes the browser would hang the
	// login flow.
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("openBrowser: exec %s: %w", cmd.Path, err)
	}
	go func() { _ = cmd.Wait() }()
	return nil
}

// ---------- OAuth HTML pages ----------

// Small static HTML for the callback browser tab. Pi.dev's pages are
// more elaborate (logo, dark theme); pi9's are minimal but functional.
// The %s in oauthErrorHTML is replaced with the error message.
const oauthSuccessHTML = `<!doctype html>
<html><head><meta charset="utf-8"><title>pi9: login complete</title>
<style>body{font-family:system-ui;background:#0a0a0a;color:#fafafa;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}
.card{text-align:center;padding:3rem}
h1{font-size:1.5rem;margin:0 0 1rem}
p{color:#a1a1aa}</style></head>
<body><div class="card">
<h1>pi9: login complete</h1>
<p>You can close this window and return to pi9.</p>
</div></body></html>`

const oauthErrorHTML = `<!doctype html>
<html><head><meta charset="utf-8"><title>pi9: login error</title>
<style>body{font-family:system-ui;background:#0a0a0a;color:#fafafa;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}
.card{text-align:center;padding:3rem;max-width:600px}
h1{font-size:1.5rem;margin:0 0 1rem;color:#ef4444}
p{color:#a1a1aa}</style></head>
<body><div class="card">
<h1>pi9: login error</h1>
<p>%s</p>
<p style="margin-top:2rem;font-size:0.875rem">Close this window and try again in pi9.</p>
</div></body></html>`

// ---------- generic JSON POST helper ----------

// postOAuthJSON does a JSON POST to a token endpoint and returns the
// raw body. Used by Anthropic + (future) ChatGPT/Copilot during code
// exchange and refresh. Embeds standard headers and a 30s timeout.
func postOAuthJSON(ctx context.Context, url string, body map[string]interface{}) ([]byte, error) {
	buf, err := json.Marshal(body)
	if err != nil {
		return nil, fmt.Errorf("oauth: marshal body: %w", err)
	}

	req, err := http.NewRequestWithContext(ctx, "POST", url, strings.NewReader(string(buf)))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")

	client := httpClient()
	client.Timeout = 30 * time.Second
	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("oauth POST %s: %w", url, err)
	}
	defer resp.Body.Close()

	respBody, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("oauth POST %s: HTTP %d: %s", url, resp.StatusCode, string(respBody))
	}
	return respBody, nil
}

// ---------- Registry ----------

// OAuthProviders returns the list of providers that support OAuth.
// Used by /login picker to add the "(subscription)" badge and route
// selection through the OAuth flow instead of key entry.
//
// Session 2: Anthropic (Claude Pro/Max).
// Session 3: + OpenAI Codex (ChatGPT Plus/Pro) — auth-only, see
//            oauth_openai_codex.go for the Responses-API caveat.
//            + GitHub Copilot — device flow, fully wired.
func OAuthProviders() []OAuthProvider {
	return []OAuthProvider{
		anthropicOAuth{},
		openAICodexOAuth{},
		githubCopilotOAuth{},
	}
}

// GetOAuth returns the OAuth implementation for an id, or nil if the
// id doesn't have OAuth support (e.g. OpenRouter is API-key only).
func GetOAuth(id ProviderID) OAuthProvider {
	for _, p := range OAuthProviders() {
		if p.ID() == id {
			return p
		}
	}
	return nil
}
