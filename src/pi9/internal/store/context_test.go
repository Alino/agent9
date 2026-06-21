package store

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// setHome points PI9_HOME at a fresh temp dir so the real home is untouched.
func setHome(t *testing.T) string {
	t.Helper()
	home := t.TempDir()
	t.Setenv("PI9_HOME", home)
	return home
}

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		t.Fatalf("write %s: %v", path, err)
	}
}

func TestLoadContextFiles_WalkUpAndWrap(t *testing.T) {
	home := setHome(t)
	// Global context file (always included, first).
	writeFile(t, filepath.Join(home, "AGENTS.md"), "global guidance")

	// Project tree: root (git) / mid / cwd.
	root := t.TempDir()
	writeFile(t, filepath.Join(root, ".git", "HEAD"), "ref: refs/heads/main\n")
	writeFile(t, filepath.Join(root, "AGENTS.md"), "root agents")
	mid := filepath.Join(root, "mid")
	cwd := filepath.Join(mid, "cwd")
	writeFile(t, filepath.Join(cwd, "CLAUDE.md"), "cwd claude")

	// pi parity: AGENTS.md/CLAUDE.md context files load regardless of trust,
	// so both trust values yield global + root + cwd in walk order.
	for _, trusted := range []bool{false, true} {
		got := LoadContextFiles(cwd, trusted)
		for _, want := range []string{"global guidance", "root agents", "cwd claude"} {
			if !strings.Contains(got, want) {
				t.Fatalf("trusted=%v: missing %q in:\n%s", trusted, want, got)
			}
		}
		iGlobal := strings.Index(got, "global guidance")
		iRoot := strings.Index(got, "root agents")
		iCwd := strings.Index(got, "cwd claude")
		if !(iGlobal < iRoot && iRoot < iCwd) {
			t.Fatalf("trusted=%v order wrong: global=%d root=%d cwd=%d\n%s", trusted, iGlobal, iRoot, iCwd, got)
		}
		// pi wrapping: a single outer <project_context> block, one
		// <project_instructions path="..."> per file.
		if strings.Count(got, "<project_context>") != 1 || strings.Count(got, "</project_context>") != 1 {
			t.Fatalf("trusted=%v want exactly one project_context block:\n%s", trusted, got)
		}
		if strings.Count(got, "</project_instructions>") != 3 {
			t.Fatalf("trusted=%v want 3 project_instructions, got:\n%s", trusted, got)
		}
		if !strings.Contains(got, `<project_instructions path="`+filepath.Join(cwd, "CLAUDE.md")+`">`) {
			t.Fatalf("trusted=%v missing wrapper for cwd file:\n%s", trusted, got)
		}
	}
}

func TestLoadContextFiles_DedupeWhenCwdIsHome(t *testing.T) {
	home := setHome(t)
	writeFile(t, filepath.Join(home, ".git", "HEAD"), "x")
	writeFile(t, filepath.Join(home, "AGENTS.md"), "shared")

	got := LoadContextFiles(home, true)
	if strings.Count(got, "</project_context>") != 1 {
		t.Fatalf("expected dedupe to 1 block, got:\n%s", got)
	}
}

func TestSystemOverride(t *testing.T) {
	home := setHome(t)
	cwd := t.TempDir()

	// Nothing yet.
	if _, ok := SystemOverride(cwd, true); ok {
		t.Fatal("expected no override")
	}

	// Global only.
	writeFile(t, filepath.Join(home, "SYSTEM.md"), "global system")
	s, ok := SystemOverride(cwd, true)
	if !ok || s != "global system" {
		t.Fatalf("global: got %q ok=%v", s, ok)
	}

	// Project wins when trusted.
	writeFile(t, filepath.Join(cwd, ".pi", "SYSTEM.md"), "project system")
	s, ok = SystemOverride(cwd, true)
	if !ok || s != "project system" {
		t.Fatalf("trusted project: got %q ok=%v", s, ok)
	}

	// Untrusted falls back to global.
	s, ok = SystemOverride(cwd, false)
	if !ok || s != "global system" {
		t.Fatalf("untrusted: got %q ok=%v", s, ok)
	}
}

func TestAppendSystem(t *testing.T) {
	home := setHome(t)
	cwd := t.TempDir()

	if got := AppendSystem(cwd, true); got != "" {
		t.Fatalf("expected empty, got %q", got)
	}

	writeFile(t, filepath.Join(home, "APPEND_SYSTEM.md"), "global append")
	writeFile(t, filepath.Join(cwd, ".pi", "APPEND_SYSTEM.md"), "project append")

	// Untrusted: global only.
	if got := AppendSystem(cwd, false); got != "global append" {
		t.Fatalf("untrusted: got %q", got)
	}
	// Trusted: both, global first.
	got := AppendSystem(cwd, true)
	if !strings.Contains(got, "global append") || !strings.Contains(got, "project append") {
		t.Fatalf("trusted: got %q", got)
	}
	if strings.Index(got, "global append") > strings.Index(got, "project append") {
		t.Fatalf("order wrong: %q", got)
	}
}

func TestHasProjectResources(t *testing.T) {
	cwd := t.TempDir()

	// Nothing yet.
	if HasProjectResources(cwd) {
		t.Fatal("expected no project resources in empty dir")
	}

	// An empty .pi dir still has nothing loadable.
	if err := os.MkdirAll(filepath.Join(cwd, ".pi"), 0755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	if HasProjectResources(cwd) {
		t.Fatal("empty .pi dir should not count as resources")
	}

	// A SYSTEM.md counts.
	writeFile(t, filepath.Join(cwd, ".pi", "SYSTEM.md"), "x")
	if !HasProjectResources(cwd) {
		t.Fatal("expected SYSTEM.md to count as a project resource")
	}

	// A prompt template counts too.
	cwd2 := t.TempDir()
	writeFile(t, filepath.Join(cwd2, ".pi", "prompts", "review.md"), "review")
	if !HasProjectResources(cwd2) {
		t.Fatal("expected a prompt template to count as a project resource")
	}
}
