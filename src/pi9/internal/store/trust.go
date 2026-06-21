package store

// Project trust store.
//
// Project-local resources (skills, prompts, SYSTEM.md, AGENTS.md, etc.) can
// instruct the model to run arbitrary commands, so they are gated behind an
// explicit trust decision per project directory. Decisions live in
// Home()/trust.json, a flat JSON object mapping an absolute project directory
// to "always" or "never":
//
//	{
//	  "/usr/glenda/work/proj": "always",
//	  "/tmp/sketchy":          "never"
//	}
//
// IsTrusted resolves the nearest ancestor decision (a "never" on an ancestor
// blocks descendants until one of them is explicitly set "always").

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
)

// configDirName is the per-project pi9 config directory (mirrors pi's `.pi`).
const configDirName = ".pi"

// Trust decision values stored in trust.json.
const (
	TrustAlways = "always"
	TrustNever  = "never"
)

// TrustPath returns the path to the trust store file.
func TrustPath() string {
	return filepath.Join(Home(), "trust.json")
}

// loadTrustFile reads and parses trust.json. A missing file yields an empty
// map and no error.
func loadTrustFile() (map[string]string, error) {
	b, err := os.ReadFile(TrustPath())
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return map[string]string{}, nil
		}
		return nil, err
	}
	m := map[string]string{}
	if len(b) == 0 {
		return m, nil
	}
	if err := json.Unmarshal(b, &m); err != nil {
		return nil, fmt.Errorf("parse %s: %w", TrustPath(), err)
	}
	return m, nil
}

// writeTrustFile writes m to trust.json with sorted keys, atomically.
func writeTrustFile(m map[string]string) error {
	if err := EnsureHome(); err != nil {
		return err
	}
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	// Marshal manually to preserve sorted key order deterministically.
	ordered := make(map[string]string, len(m))
	for _, k := range keys {
		ordered[k] = m[k]
	}
	b, err := json.MarshalIndent(ordered, "", "  ")
	if err != nil {
		return err
	}
	b = append(b, '\n')
	tmp := TrustPath() + ".tmp"
	if err := os.WriteFile(tmp, b, 0600); err != nil {
		return err
	}
	if err := os.Rename(tmp, TrustPath()); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// IsTrusted reports whether cwd is trusted. It resolves the nearest ancestor
// directory (cwd first) that has an explicit decision: "always" -> true,
// "never" -> false. With no matching entry, the default is untrusted (false).
func IsTrusted(cwd string) bool {
	m, err := loadTrustFile()
	if err != nil {
		return false
	}
	abs, err := filepath.Abs(cwd)
	if err != nil {
		abs = cwd
	}
	dir := abs
	for {
		switch m[dir] {
		case TrustAlways:
			return true
		case TrustNever:
			return false
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return false
		}
		dir = parent
	}
}

// SetTrust records a trust decision for cwd. decision must be "always" or
// "never"; passing "" removes any explicit entry for cwd.
func SetTrust(cwd, decision string) error {
	if decision != TrustAlways && decision != TrustNever && decision != "" {
		return fmt.Errorf("invalid trust decision %q (want %q, %q, or empty)", decision, TrustAlways, TrustNever)
	}
	abs, err := filepath.Abs(cwd)
	if err != nil {
		abs = cwd
	}
	m, err := loadTrustFile()
	if err != nil {
		return err
	}
	if decision == "" {
		delete(m, abs)
	} else {
		m[abs] = decision
	}
	return writeTrustFile(m)
}
