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
//
// The extra frontmatter fields mirror the Agent Skills spec (see
// docs/skills.md): License, Compatibility, AllowedTools (the
// space-delimited `allowed-tools` list), and DisableModelInvocation
// (the `disable-model-invocation` flag). Disabled skills are excluded
// from the model-facing index (FormatSkillsXML) but remain loadable by
// name via ReadSkillBody.
type Skill struct {
	Name        string
	Description string
	Path        string

	License                string
	Compatibility          string
	AllowedTools           []string
	DisableModelInvocation bool
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
		s, _, err := parseSkill(p)
		if err != nil {
			continue
		}
		if s.Name == "" {
			// Fall back to filename without extension.
			s.Name = strings.TrimSuffix(e.Name(), ".md")
		}
		skills = append(skills, s)
	}
	sort.Slice(skills, func(i, j int) bool {
		return skills[i].Name < skills[j].Name
	})
	return skills, nil
}

// ListSkillsFor returns the union of global skills (Home()/skills) and, when
// trusted, project skills discovered by walking from cwd UP to the git (or
// filesystem) root looking for ./.pi/skills and ./.agents/skills directories.
//
// Direct *.md files in each skills directory are loaded as individual skills.
// Skills are deduped by name (first found wins; global skills are added first
// so they take precedence) and the result is sorted by name. A skill with
// DisableModelInvocation set is still returned here — callers decide whether
// to hide it (FormatSkillsXML does).
func ListSkillsFor(cwd string, trusted bool) []Skill {
	seen := make(map[string]bool)
	var out []Skill

	add := func(s Skill) {
		if s.Name == "" || seen[s.Name] {
			return
		}
		seen[s.Name] = true
		out = append(out, s)
	}

	// Global skills first (highest precedence on name collisions).
	if global, err := ListSkills(); err == nil {
		for _, s := range global {
			add(s)
		}
	}

	if trusted {
		if abs, err := filepath.Abs(cwd); err == nil {
			root := gitOrFSRoot(abs)
			dir := abs
			for {
				add2 := func(skillsDir string) {
					for _, s := range loadSkillsFromDir(skillsDir) {
						add(s)
					}
				}
				add2(filepath.Join(dir, configDirName, "skills"))
				add2(filepath.Join(dir, ".agents", "skills"))
				if dir == root {
					break
				}
				parent := filepath.Dir(dir)
				if parent == dir {
					break
				}
				dir = parent
			}
		}
	}

	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out
}

// loadSkillsFromDir loads direct *.md files in dir as individual skills.
// A missing directory yields no skills. The name falls back to the file
// stem when frontmatter has no name.
func loadSkillsFromDir(dir string) []Skill {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []Skill
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".md") {
			continue
		}
		p := filepath.Join(dir, e.Name())
		s, _, err := parseSkill(p)
		if err != nil {
			continue
		}
		if s.Name == "" {
			s.Name = strings.TrimSuffix(e.Name(), ".md")
		}
		out = append(out, s)
	}
	return out
}

// FormatSkillsXML renders skills as the model-facing index:
//
//	<available_skills>
//	  <skill>
//	    <name>...</name>
//	    <description>...</description>
//	    <location>...</location>
//	  </skill>
//	  ...
//	</available_skills>
//
// Skills with DisableModelInvocation set are omitted. Returns "" when no
// visible skills remain. XML-significant characters are escaped.
func FormatSkillsXML(skills []Skill) string {
	var visible []Skill
	for _, s := range skills {
		if !s.DisableModelInvocation {
			visible = append(visible, s)
		}
	}
	if len(visible) == 0 {
		return ""
	}
	var b strings.Builder
	b.WriteString("<available_skills>\n")
	for _, s := range visible {
		b.WriteString("  <skill>\n")
		b.WriteString("    <name>" + escapeXML(s.Name) + "</name>\n")
		b.WriteString("    <description>" + escapeXML(s.Description) + "</description>\n")
		b.WriteString("    <location>" + escapeXML(s.Path) + "</location>\n")
		b.WriteString("  </skill>\n")
	}
	b.WriteString("</available_skills>")
	return b.String()
}

// escapeXML escapes the five XML predefined entities.
func escapeXML(s string) string {
	r := strings.NewReplacer(
		"&", "&amp;",
		"<", "&lt;",
		">", "&gt;",
		`"`, "&quot;",
		"'", "&apos;",
	)
	return r.Replace(s)
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
// It is a thin wrapper over parseSkill that keeps the original
// three-value shape used by ReadSkillBody and ListSkills.
func parseSkillFile(path string) (name, desc, body string, err error) {
	s, body, err := parseSkill(path)
	if err != nil {
		return "", "", "", err
	}
	return s.Name, s.Description, body, nil
}

// parseSkill reads a skill markdown file and returns the parsed Skill
// (with Path set) plus the body (frontmatter stripped). Frontmatter
// keys recognized: name, description, license, compatibility,
// allowed-tools, disable-model-invocation. Unknown keys are ignored.
func parseSkill(path string) (Skill, string, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return Skill{}, "", err
	}
	front, body := splitFrontmatter(string(b))
	s := Skill{
		Name:                   front["name"],
		Description:            front["description"],
		Path:                   path,
		License:                front["license"],
		Compatibility:          front["compatibility"],
		DisableModelInvocation: parseBoolFrontmatter(front["disable-model-invocation"]),
	}
	if at := strings.TrimSpace(front["allowed-tools"]); at != "" {
		s.AllowedTools = strings.Fields(at)
	}
	return s, body, nil
}

// splitFrontmatter splits leading YAML-ish frontmatter (delimited by ---
// lines) from the body. Returns a key->value map (values quote-trimmed)
// and the remaining body with leading newlines stripped. When no
// frontmatter is present the map is empty and the whole text is the body.
//
// The parser is intentionally minimal (line-based key:value) — no yaml
// dependency. It tolerates "---\n" or "---\r\n" delimiters.
func splitFrontmatter(text string) (map[string]string, string) {
	front := map[string]string{}

	// Normalize the opening delimiter detection without rewriting CRLFs
	// throughout the body.
	trimmed := strings.TrimPrefix(text, "\ufeff") // strip UTF-8 BOM if present
	if !strings.HasPrefix(trimmed, "---\n") && !strings.HasPrefix(trimmed, "---\r\n") {
		return front, trimmed
	}
	// Position just past the opening "---" line.
	nl := strings.IndexByte(trimmed, '\n')
	rest := trimmed[nl+1:]

	// Find the closing delimiter line.
	var frontText, body string
	if i := strings.Index(rest, "\n---\n"); i >= 0 {
		frontText = rest[:i]
		body = rest[i+len("\n---\n"):]
	} else if i := strings.Index(rest, "\n---\r\n"); i >= 0 {
		frontText = rest[:i]
		body = rest[i+len("\n---\r\n"):]
	} else if strings.HasPrefix(rest, "---\n") || strings.HasPrefix(rest, "---\r\n") {
		// Empty frontmatter block: opening immediately followed by closing.
		frontText = ""
		body = rest[strings.IndexByte(rest, '\n')+1:]
	} else {
		// No closing delimiter; treat the whole thing as body.
		return front, trimmed
	}

	for _, line := range strings.Split(frontText, "\n") {
		line = strings.TrimRight(line, "\r")
		colon := strings.Index(line, ":")
		if colon < 0 {
			continue
		}
		k := strings.TrimSpace(line[:colon])
		v := strings.TrimSpace(line[colon+1:])
		v = strings.Trim(v, `"'`)
		if k != "" {
			front[k] = v
		}
	}
	body = strings.TrimLeft(body, "\n")
	return front, body
}

// parseBoolFrontmatter interprets a frontmatter scalar as a boolean.
// Only an explicit "true" (case-insensitive) is true.
func parseBoolFrontmatter(v string) bool {
	return strings.EqualFold(strings.TrimSpace(v), "true")
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

// SessionPath returns the path to a specific session's legacy JSON file.
//
// New sessions are written as JSONL (see SessionTreePath); SessionPath is
// retained for migration + the legacy chat.History save path.
func SessionPath(id string) string {
	return filepath.Join(SessionsDir(), id+".json")
}

// SessionTreePath returns the path to a session's tree-format JSONL file.
func SessionTreePath(id string) string {
	return filepath.Join(SessionsDir(), id+".jsonl")
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

// SaveSessionTree writes the tree-format JSONL bytes to
// sessions/<id>.jsonl. Atomic via write-to-tmp + rename, same as
// SaveSession. The caller marshals the tree (chat.SessionTree.MarshalJSONL).
func SaveSessionTree(id string, jsonl []byte) error {
	if err := EnsureHome(); err != nil {
		return err
	}
	final := SessionTreePath(id)
	tmp := final + ".tmp"
	if err := os.WriteFile(tmp, jsonl, 0644); err != nil {
		return err
	}
	if err := os.Rename(tmp, final); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// LoadSessionTree reads the JSONL bytes for a session. Callers parse them
// via chat.UnmarshalJSONL. Returns os.ErrNotExist if absent.
func LoadSessionTree(id string) ([]byte, error) {
	return os.ReadFile(SessionTreePath(id))
}

// HasSessionTree reports whether a JSONL session file exists for id.
func HasSessionTree(id string) bool {
	_, err := os.Stat(SessionTreePath(id))
	return err == nil
}

// ImportSessionTree copies an external JSONL session file at path into the
// store under id (deriving id from the file's stem when id is ""). Returns
// the id used. The bytes are written verbatim; callers should validate
// parseability with chat.UnmarshalJSONL first.
func ImportSessionTree(id, path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	if id == "" {
		base := filepath.Base(path)
		base = strings.TrimSuffix(base, ".jsonl")
		base = strings.TrimSuffix(base, ".json")
		id = base
	}
	if id == "" {
		return "", fmt.Errorf("could not derive session id from %q", path)
	}
	if err := SaveSessionTree(id, data); err != nil {
		return "", err
	}
	return id, nil
}

// ListSessions returns all session ids, newest first by mtime, merging
// both the new tree format (sessions/<id>.jsonl) and legacy snapshots
// (sessions/<id>.json). When an id has both files, the newer mtime wins
// for ordering and the id appears once (the loader prefers .jsonl).
func ListSessions() ([]string, error) {
	dir := SessionsDir()
	entries, err := os.ReadDir(dir)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, nil
		}
		return nil, err
	}
	// id -> newest mtime seen across .json / .jsonl.
	best := map[string]time.Time{}
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		name := e.Name()
		var id string
		switch {
		case strings.HasSuffix(name, ".jsonl"):
			id = strings.TrimSuffix(name, ".jsonl")
		case strings.HasSuffix(name, ".json"):
			id = strings.TrimSuffix(name, ".json")
		default:
			continue
		}
		if id == "" || id == "current" {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		if prev, ok := best[id]; !ok || info.ModTime().After(prev) {
			best[id] = info.ModTime()
		}
	}
	type entry struct {
		id    string
		mtime time.Time
	}
	list := make([]entry, 0, len(best))
	for id, mt := range best {
		list = append(list, entry{id: id, mtime: mt})
	}
	sort.Slice(list, func(i, j int) bool {
		if list[i].mtime.Equal(list[j].mtime) {
			return list[i].id > list[j].id
		}
		return list[i].mtime.After(list[j].mtime)
	})
	out := make([]string, len(list))
	for i, e := range list {
		out[i] = e.id
	}
	return out, nil
}
