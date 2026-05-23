// Package store: auth.json — multi-provider credential storage.
//
// Phase 10 introduces per-provider auth, mirroring pi.dev's
// ~/.pi/agent/auth.json shape. Multiple providers can be active
// simultaneously; /model selects which one a request goes to (via
// provider.ProviderForModel from the model name).
//
// File layout (per-user, plan9 path $home/lib/pi9/auth.json,
// mode 0600):
//
//   {
//     "anthropic":  { "type": "api_key", "key": "sk-ant-..." },
//     "openrouter": { "type": "api_key", "key": "sk-or-v1-..." },
//     "openai":     { "type": "api_key", "key": "sk-..." }
//   }
//
// Future shapes (Session 2/3): OAuth tokens with type="oauth", with
// access_token/refresh_token/expires_at fields. Code paths already
// branch on "type" so adding oauth handling later is local.
//
// Migration from Phase 8b config: if auth.json doesn't exist BUT
// the legacy config has api_key set, we silently promote it to an
// auth.json entry for OpenRouter (the only provider pre-Phase-10
// pi9 spoke). The legacy api_key= field stays in config for now
// — code reads from auth.json first, falls back to config.
package store

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sort"
)

// AuthEntry is one provider's stored credentials. Keep the type
// field flexible so OAuth can land later without breaking the
// on-disk format.
type AuthEntry struct {
	// Type is "api_key" or "oauth". For api_key, only Key is used.
	// For oauth, AccessToken/RefreshToken/ExpiresAt are used.
	Type string `json:"type"`

	// Key is the literal API key for type=api_key. For OAuth, this
	// field is unused.
	//
	// Compatibility with pi.dev: their format also accepts shell
	// commands ("!security find-...") and env var names. We DON'T
	// implement those resolution shapes in Session 1 — the user
	// pastes literal keys. Session 3+ can add resolution.
	Key string `json:"key,omitempty"`

	// AccessToken et al. used for type=oauth (Session 2+).
	AccessToken  string `json:"access_token,omitempty"`
	RefreshToken string `json:"refresh_token,omitempty"`
	ExpiresAt    int64  `json:"expires_at,omitempty"` // unix seconds

	// AccountID is provider-specific OAuth metadata (Session 4+).
	// Codex needs chatgpt-account-id on every request; we extract
	// from the JWT at login time and stash here.
	AccountID string `json:"account_id,omitempty"`
}

// Auth is the parsed auth.json: provider id → entry.
type Auth map[string]AuthEntry

// AuthPath returns the path to auth.json.
func AuthPath() string {
	return filepath.Join(Home(), "auth.json")
}

// LoadAuth reads $home/lib/pi9/auth.json. Returns an empty Auth (not
// an error) if the file doesn't exist — auth.json is created on
// first /login.
func LoadAuth() (Auth, error) {
	b, err := os.ReadFile(AuthPath())
	if errors.Is(err, os.ErrNotExist) {
		return Auth{}, nil
	}
	if err != nil {
		return nil, err
	}
	var a Auth
	if err := json.Unmarshal(b, &a); err != nil {
		return nil, err
	}
	if a == nil {
		a = Auth{}
	}
	return a, nil
}

// SaveAuth writes auth.json atomically with mode 0600. Sorts the
// provider keys for stable diffs (the user may put auth.json in
// dotfiles).
func SaveAuth(a Auth) error {
	if err := EnsureHome(); err != nil {
		return err
	}

	// Sort keys for stable output.
	keys := make([]string, 0, len(a))
	for k := range a {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	// Use ordered marshaling (manual) so diffs are stable.
	var buf []byte
	buf = append(buf, '{', '\n')
	for i, k := range keys {
		kb, _ := json.Marshal(k)
		vb, _ := json.Marshal(a[k])
		buf = append(buf, ' ', ' ')
		buf = append(buf, kb...)
		buf = append(buf, ':', ' ')
		buf = append(buf, vb...)
		if i < len(keys)-1 {
			buf = append(buf, ',')
		}
		buf = append(buf, '\n')
	}
	buf = append(buf, '}', '\n')

	tmp := AuthPath() + ".tmp"
	if err := os.WriteFile(tmp, buf, 0600); err != nil {
		return err
	}
	if err := os.Rename(tmp, AuthPath()); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// SetAPIKey is a convenience for the common case: stash an API key
// for one provider, persist immediately. Loads, modifies, saves.
//
// On first call (no auth.json yet) creates the file. Subsequent calls
// overwrite the named provider's entry, leaving other providers
// untouched.
func SetAPIKey(provider, key string) error {
	a, err := LoadAuth()
	if err != nil {
		return err
	}
	a[provider] = AuthEntry{Type: "api_key", Key: key}
	return SaveAuth(a)
}

// ClearProvider removes one provider's entry. Used by /logout.
// No-op if the provider isn't present. Saves the (possibly smaller)
// auth.json back to disk.
func ClearProvider(provider string) error {
	a, err := LoadAuth()
	if err != nil {
		return err
	}
	if _, ok := a[provider]; !ok {
		return nil
	}
	delete(a, provider)
	return SaveAuth(a)
}

// LookupAPIKey returns the API key (or OAuth access token) for one
// provider, or "" if not configured.
//
// For OAuth providers (type=oauth):
//   - Returns AccessToken if still valid
//   - If expired, attempts to refresh synchronously (caller blocks
//     briefly while we hit the token endpoint) and returns the new
//     access token. The refreshed credentials are persisted back to
//     auth.json so the next launch starts with a valid token.
//   - On refresh failure, returns "" (caller surfaces the auth error
//     in the chat UI; user can /login again).
//
// Compatibility: if auth.json has no entry for the provider but it
// IS OpenRouter AND the legacy config has api_key set, returns
// that. Pre-Phase-10 setups keep working.
//
// The refresh function is passed in to avoid an import cycle
// (store can't import provider). Callers can pass nil to skip
// refresh and just return the raw access token (used by tests).
//
// In normal pi9 use, main.go wraps this:
//
//   provider.LookupAPIKeyWithRefresh(string(providerID))
//
// which threads the refresh callback for us.
func LookupAPIKey(provider string) string {
	a, err := LoadAuth()
	if err == nil {
		if e, ok := a[provider]; ok {
			switch e.Type {
			case "api_key":
				return e.Key
			case "oauth":
				return e.AccessToken
			}
		}
	}
	// Legacy fallback.
	if provider == "openrouter" {
		cfg, _ := LoadConfig()
		return cfg.APIKey
	}
	return ""
}

// LookupAuthEntry returns the full AuthEntry for a provider, or
// (zero, false) if not present. Used by code that needs to inspect
// type/refresh/expires (e.g. main.go's auto-refresh logic).
func LookupAuthEntry(provider string) (AuthEntry, bool) {
	a, err := LoadAuth()
	if err != nil {
		return AuthEntry{}, false
	}
	e, ok := a[provider]
	return e, ok
}

// SetOAuth stashes OAuth credentials for one provider. Sets type to
// "oauth". Persists immediately. Used by /login OAuth flow and
// background refresh.
//
// accountID is provider-specific metadata (currently only Codex
// uses it — pass "" for providers that don't need it).
func SetOAuth(provider, access, refresh string, expiresAt int64, accountID ...string) error {
	a, err := LoadAuth()
	if err != nil {
		return err
	}
	entry := AuthEntry{
		Type:         "oauth",
		AccessToken:  access,
		RefreshToken: refresh,
		ExpiresAt:    expiresAt,
	}
	if len(accountID) > 0 {
		entry.AccountID = accountID[0]
	}
	a[provider] = entry
	return SaveAuth(a)
}
