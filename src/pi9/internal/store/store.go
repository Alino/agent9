// Package store handles pi9's on-disk state: sessions, skills, memory.
//
// Layout (plan9):
//
//	$home/lib/pi9/
//	├── memory.md
//	├── skills/
//	│   ├── plan9-rc.md
//	│   └── ...
//	└── sessions/
//	    ├── current                       — session id of active conv
//	    ├── 2026-05-16T22-46-30.json      — session snapshot
//	    └── ...
//
// On unix, replace `$home/lib/pi9` with `$HOME/.pi9`. Both can be
// overridden by $PI9_HOME.
//
// Skill markdown files have YAML-ish frontmatter:
//
//	---
//	name: plan9-rc
//	description: Plan 9 rc shell idioms
//	---
//
//	# Plan 9 rc
//	...
//
// The frontmatter parser is hand-rolled (lines until ---) — no yaml dep.
package store

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"time"
)

// Home returns the pi9 home directory, honoring $PI9_HOME, and falling
// back to $home/lib/pi9 on plan9 or $HOME/.pi9 on unix.
//
// MkdirAll-equivalent ensure is the caller's responsibility — Home()
// returns a path string, nothing more.
func Home() string {
	if h := os.Getenv("PI9_HOME"); h != "" {
		return h
	}
	if runtime.GOOS == "plan9" {
		// On plan9 `home` is the env var (not HOME).
		if h := os.Getenv("home"); h != "" {
			return filepath.Join(h, "lib", "pi9")
		}
		return "/lib/pi9"
	}
	if h := os.Getenv("HOME"); h != "" {
		return filepath.Join(h, ".pi9")
	}
	return ".pi9"
}

// EnsureHome creates the pi9 home directory hierarchy if missing.
func EnsureHome() error {
	root := Home()
	for _, sub := range []string{"", "skills", "sessions"} {
		p := filepath.Join(root, sub)
		if err := os.MkdirAll(p, 0755); err != nil {
			return fmt.Errorf("mkdir %s: %w", p, err)
		}
	}
	return nil
}

// ---------- Memory ----------

// MemoryPath returns the path to the memory file.
func MemoryPath() string {
	return filepath.Join(Home(), "memory.md")
}

// LoadMemory reads memory.md. Returns ("", nil) if the file is missing.
func LoadMemory() (string, error) {
	b, err := os.ReadFile(MemoryPath())
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return "", nil
		}
		return "", err
	}
	return string(b), nil
}

// AppendMemory appends a new entry to memory.md, prefixed with "- "
// if not already. Creates the file if missing.
func AppendMemory(content string) error {
	if err := EnsureHome(); err != nil {
		return err
	}
	content = strings.TrimSpace(content)
	if content == "" {
		return fmt.Errorf("empty content")
	}
	if !strings.HasPrefix(content, "-") && !strings.HasPrefix(content, "#") {
		content = "- " + content
	}
	f, err := os.OpenFile(MemoryPath(), os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		return err
	}
	defer f.Close()
	if _, err := f.WriteString(content + "\n"); err != nil {
		return err
	}
	return nil
}

// ---------- Skills ----------

// Skill is one available skill: name, short description, and its
// file path. The full body is loaded on demand via ReadSkillBody.
type Skill struct {
	Name        string
	Description string
	Path        string
}

// SkillsDir returns the skills directory path.
func SkillsDir() string {
	return filepath.Join(Home(), "skills")
}

// ListSkills walks the skills directory, parses each .md file's
// frontmatter, and returns the sorted set. Missing directory = empty
// list, no error.
func ListSkills() ([]Skill, error) {
	dir := SkillsDir()
	entries, err := os.ReadDir(dir)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, nil
		}
		return nil, err
	}
	var skills []Skill
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		if !strings.HasSuffix(e.Name(), ".md") {
			continue
		}
		p := filepath.Join(dir, e.Name())
		name, desc, _, err := parseSkillFile(p)
		if err != nil {
			continue
		}
		if name == "" {
			// Fall back to filename without extension.
			name = strings.TrimSuffix(e.Name(), ".md")
		}
		skills = append(skills, Skill{
			Name:        name,
			Description: desc,
			Path:        p,
		})
	}
	sort.Slice(skills, func(i, j int) bool {
		return skills[i].Name < skills[j].Name
	})
	return skills, nil
}

// ReadSkillBody returns the full markdown body of the named skill
// (with frontmatter stripped). Returns os.ErrNotExist if not found.
func ReadSkillBody(name string) (string, error) {
	dir := SkillsDir()
	// Try direct filename match first.
	candidates := []string{
		filepath.Join(dir, name+".md"),
		filepath.Join(dir, name),
	}
	for _, p := range candidates {
		if _, err := os.Stat(p); err == nil {
			_, _, body, err := parseSkillFile(p)
			return body, err
		}
	}
	// Fall back to scanning every skill file for matching `name:` field.
	skills, err := ListSkills()
	if err != nil {
		return "", err
	}
	for _, s := range skills {
		if s.Name == name {
			_, _, body, err := parseSkillFile(s.Path)
			return body, err
		}
	}
	return "", os.ErrNotExist
}

// parseSkillFile reads a skill markdown file and returns name,
// description (from frontmatter) and body (everything else).
//
// Frontmatter format:
//
//	---
//	name: foo
//	description: ...
//	---
//	<body...>
//
// Frontmatter lines are key:value. We only care about name + description;
// other fields are silently ignored.
func parseSkillFile(path string) (name, desc, body string, err error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return "", "", "", err
	}
	text := string(b)

	// Detect frontmatter.
	const marker = "---\n"
	if !strings.HasPrefix(text, marker) {
		// No frontmatter; entire file is body.
		return "", "", text, nil
	}
	rest := text[len(marker):]
	end := strings.Index(rest, "\n"+marker)
	if end < 0 {
		// Malformed: no closing marker.
		return "", "", text, nil
	}
	front := rest[:end]
	body = rest[end+len("\n"+marker):]
	for _, line := range strings.Split(front, "\n") {
		colon := strings.Index(line, ":")
		if colon < 0 {
			continue
		}
		k := strings.TrimSpace(line[:colon])
		v := strings.TrimSpace(line[colon+1:])
		// Trim surrounding quotes if present.
		v = strings.Trim(v, `"'`)
		switch k {
		case "name":
			name = v
		case "description":
			desc = v
		}
	}
	body = strings.TrimLeft(body, "\n")
	return name, desc, body, nil
}

// ---------- Config ----------

// Config is pi9's persisted configuration. Stored as a plain
// key=value file at $home/lib/pi9/config (mode 0600 — it contains
// the API key).
//
// Precedence at load time: env var > config file > zero value.
// CLI flags can further override after Load returns.
//
// Format:
//
//	# pi9 config — see wiki/concepts/pi9-phase8.md
//	api_key=sk-or-v1-...
//	model=moonshotai/kimi-k2.5
//	api_url=https://openrouter.ai/api/v1/chat/completions
//	ssl_cert_file=/sys/lib/tls/ca.pem
//
// Blank lines and lines starting with # are comments.
type Config struct {
	APIKey      string
	Model       string
	APIURL      string
	SSLCertFile string
}

// ConfigPath returns the path to the config file.
func ConfigPath() string {
	return filepath.Join(Home(), "config")
}

// LoadConfig reads $home/lib/pi9/config and overlays env vars on top.
// Missing file is not an error — returns a zero Config.
//
// Env vars (override file):
//
//	OPENROUTER_API_KEY   → APIKey
//	OPENROUTER_MODEL     → Model
//	OPENROUTER_API_URL   → APIURL
//	SSL_CERT_FILE        → SSLCertFile
func LoadConfig() (Config, error) {
	var c Config

	// Parse file if it exists.
	if b, err := os.ReadFile(ConfigPath()); err == nil {
		for _, line := range strings.Split(string(b), "\n") {
			line = strings.TrimSpace(line)
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			eq := strings.Index(line, "=")
			if eq < 0 {
				continue
			}
			k := strings.TrimSpace(line[:eq])
			v := strings.TrimSpace(line[eq+1:])
			v = strings.Trim(v, `"'`)
			switch k {
			case "api_key":
				c.APIKey = v
			case "model":
				c.Model = v
			case "api_url":
				c.APIURL = v
			case "ssl_cert_file":
				c.SSLCertFile = v
			}
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return c, err
	}

	// Env vars override.
	if v := os.Getenv("OPENROUTER_API_KEY"); v != "" {
		c.APIKey = v
	}
	if v := os.Getenv("OPENROUTER_MODEL"); v != "" {
		c.Model = v
	}
	if v := os.Getenv("OPENROUTER_API_URL"); v != "" {
		c.APIURL = v
	}
	if v := os.Getenv("SSL_CERT_FILE"); v != "" {
		c.SSLCertFile = v
	}

	return c, nil
}

// WriteTemplate writes a commented template config file if one does
// not already exist. Returns true if a template was written. Used
// on first launch to give the user something to edit.
//
// Mode is 0600 — config holds the API key.
func WriteTemplate() (bool, error) {
	if _, err := os.Stat(ConfigPath()); err == nil {
		return false, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return false, err
	}
	if err := EnsureHome(); err != nil {
		return false, err
	}
	tmpl := `# pi9 config
# Generated by pi9 on first launch. Edit and save.
# See wiki/concepts/pi9-phase8.md for details.
#
# api_key is required. Get one at https://openrouter.ai/keys
# Other fields are optional and have sensible defaults.

api_key=
model=moonshotai/kimi-k2.5
api_url=https://openrouter.ai/api/v1/chat/completions
ssl_cert_file=/sys/lib/tls/ca.pem
`
	if err := os.WriteFile(ConfigPath(), []byte(tmpl), 0600); err != nil {
		return false, err
	}
	return true, nil
}

// MaskedAPIKey returns the api_key with the middle masked, suitable
// for display in /config slash command. "sk-or-v1-abc...xyz" style.
func MaskedAPIKey(key string) string {
	if key == "" {
		return "(not set)"
	}
	if len(key) <= 12 {
		return strings.Repeat("*", len(key))
	}
	return key[:9] + "..." + key[len(key)-4:]
}

// SaveConfig writes c back to ConfigPath() preserving the comment
// header from the template. Mode 0600. Used by /login + /logout.
//
// We intentionally do NOT preserve user-added comments — round-trip
// is lossy. Anyone who wants to edit the file by hand should edit
// it directly. /login is for "I'm pasting a key, do the boring part."
func SaveConfig(c Config) error {
	if err := EnsureHome(); err != nil {
		return err
	}
	body := fmt.Sprintf(`# pi9 config
# See wiki/concepts/pi9-phase8.md for details.

api_key=%s
model=%s
api_url=%s
ssl_cert_file=%s
`, c.APIKey, c.Model, c.APIURL, c.SSLCertFile)
	tmp := ConfigPath() + ".tmp"
	if err := os.WriteFile(tmp, []byte(body), 0600); err != nil {
		return err
	}
	if err := os.Rename(tmp, ConfigPath()); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// ---------- Sessions ----------

// SessionsDir returns the sessions directory path.
func SessionsDir() string {
	return filepath.Join(Home(), "sessions")
}

// CurrentPath returns the path to the "current" pointer file.
func CurrentPath() string {
	return filepath.Join(SessionsDir(), "current")
}

// SessionPath returns the path to a specific session's JSON file.
func SessionPath(id string) string {
	return filepath.Join(SessionsDir(), id+".json")
}

// NewSessionID returns a fresh session identifier based on the
// current time. Format: YYYY-MM-DDTHH-MM-SS (no colons — plan9
// file system handles them but they're awkward in shell).
func NewSessionID() string {
	return time.Now().UTC().Format("2006-01-02T15-04-05")
}

// CurrentSessionID returns the contents of the current pointer file,
// or "" if there is no active session.
func CurrentSessionID() string {
	b, err := os.ReadFile(CurrentPath())
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

// SetCurrentSession writes the session id into the current pointer file.
func SetCurrentSession(id string) error {
	if err := EnsureHome(); err != nil {
		return err
	}
	return os.WriteFile(CurrentPath(), []byte(id+"\n"), 0644)
}

// LoadSession reads the JSON for a session and returns the raw bytes.
// Callers unmarshal into chat.History (or whatever shape is current).
// Returns os.ErrNotExist if absent.
func LoadSession(id string) ([]byte, error) {
	return os.ReadFile(SessionPath(id))
}

// SaveSession writes the given JSON to the session file. Atomic via
// rename: write to .tmp first, then rename. Plan 9's rename is
// atomic on the same filesystem.
func SaveSession(id string, data []byte) error {
	if err := EnsureHome(); err != nil {
		return err
	}
	final := SessionPath(id)
	tmp := final + ".tmp"
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	if err := os.Rename(tmp, final); err != nil {
		// Best-effort cleanup of tmp on failure.
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// SaveAny is a JSON-encode-and-save helper. Most callers want this.
func SaveAny(id string, v any) error {
	data, err := json.MarshalIndent(v, "", "  ")
	if err != nil {
		return err
	}
	return SaveSession(id, data)
}

// ListSessions returns all session ids (filename without .json), newest
// first by mtime.
func ListSessions() ([]string, error) {
	dir := SessionsDir()
	entries, err := os.ReadDir(dir)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, nil
		}
		return nil, err
	}
	type entry struct {
		id    string
		mtime time.Time
	}
	var list []entry
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		name := e.Name()
		if !strings.HasSuffix(name, ".json") {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		list = append(list, entry{
			id:    strings.TrimSuffix(name, ".json"),
			mtime: info.ModTime(),
		})
	}
	sort.Slice(list, func(i, j int) bool {
		return list[i].mtime.After(list[j].mtime)
	})
	out := make([]string, len(list))
	for i, e := range list {
		out[i] = e.id
	}
	return out, nil
}
