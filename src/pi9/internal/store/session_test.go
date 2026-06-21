package store

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestSaveLoadSessionTree(t *testing.T) {
	setHome(t)
	jsonl := []byte(`{"type":"session","version":3,"id":"x"}` + "\n" +
		`{"type":"message","id":"a1","parentId":"","message":{"user":"hi"}}` + "\n" +
		`{"type":"leaf","id":"a1"}` + "\n")
	if err := SaveSessionTree("x", jsonl); err != nil {
		t.Fatalf("SaveSessionTree: %v", err)
	}
	if !HasSessionTree("x") {
		t.Fatal("HasSessionTree should be true after save")
	}
	got, err := LoadSessionTree("x")
	if err != nil {
		t.Fatalf("LoadSessionTree: %v", err)
	}
	if string(got) != string(jsonl) {
		t.Fatalf("round-trip mismatch:\n got=%q\nwant=%q", got, jsonl)
	}
	// Missing session is os.ErrNotExist.
	if _, err := LoadSessionTree("nope"); !os.IsNotExist(err) {
		t.Fatalf("missing tree should be ErrNotExist, got %v", err)
	}
}

func TestListSessions_MergesJSONandJSONL(t *testing.T) {
	home := setHome(t)
	dir := filepath.Join(home, "sessions")
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}

	// Two legacy .json and two new .jsonl, plus one id present in both
	// formats (must appear once). Set mtimes so ordering is deterministic.
	writeAt := func(name string, mtime time.Time) {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte("{}"), 0644); err != nil {
			t.Fatalf("write %s: %v", name, err)
		}
		if err := os.Chtimes(p, mtime, mtime); err != nil {
			t.Fatalf("chtimes %s: %v", name, err)
		}
	}
	base := time.Now()
	writeAt("old-legacy.json", base.Add(-3*time.Hour))
	writeAt("mid-tree.jsonl", base.Add(-2*time.Hour))
	writeAt("dup.json", base.Add(-4*time.Hour))
	writeAt("dup.jsonl", base.Add(-1*time.Hour)) // newer of the pair
	writeAt("new-tree.jsonl", base)
	// The "current" pointer file must be ignored even though it has no ext.
	if err := os.WriteFile(filepath.Join(dir, "current"), []byte("new-tree\n"), 0644); err != nil {
		t.Fatalf("write current: %v", err)
	}

	ids, err := ListSessions()
	if err != nil {
		t.Fatalf("ListSessions: %v", err)
	}
	// Expect 4 unique ids (dup collapsed), newest-first by mtime.
	want := []string{"new-tree", "dup", "mid-tree", "old-legacy"}
	if len(ids) != len(want) {
		t.Fatalf("want %d ids, got %d: %v", len(want), len(ids), ids)
	}
	for i := range want {
		if ids[i] != want[i] {
			t.Fatalf("order mismatch at %d: got %q want %q (full %v)", i, ids[i], want[i], ids)
		}
	}
	// "current" must not be listed as a session.
	for _, id := range ids {
		if id == "current" {
			t.Fatal("'current' pointer should not appear as a session")
		}
	}
}

func TestImportSessionTree(t *testing.T) {
	setHome(t)
	// Write an external file outside the store.
	ext := filepath.Join(t.TempDir(), "2030-01-01T00-00-00.jsonl")
	body := []byte(`{"type":"session","version":3,"id":"imp"}` + "\n" + `{"type":"leaf","id":""}` + "\n")
	if err := os.WriteFile(ext, body, 0644); err != nil {
		t.Fatalf("write ext: %v", err)
	}
	id, err := ImportSessionTree("", ext)
	if err != nil {
		t.Fatalf("ImportSessionTree: %v", err)
	}
	if id != "2030-01-01T00-00-00" {
		t.Fatalf("derived id wrong: %q", id)
	}
	if !HasSessionTree(id) {
		t.Fatal("imported tree should exist in store")
	}
	got, _ := LoadSessionTree(id)
	if !strings.Contains(string(got), `"id":"imp"`) {
		t.Fatalf("imported content wrong: %s", got)
	}
}

func TestSessionTreePath(t *testing.T) {
	setHome(t)
	if got := SessionTreePath("abc"); !strings.HasSuffix(got, filepath.Join("sessions", "abc.jsonl")) {
		t.Fatalf("SessionTreePath: %q", got)
	}
	if got := SessionPath("abc"); !strings.HasSuffix(got, filepath.Join("sessions", "abc.json")) {
		t.Fatalf("SessionPath: %q", got)
	}
}
