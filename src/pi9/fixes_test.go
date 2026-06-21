package main

import (
	"encoding/json"
	"strings"
	"testing"

	"github.com/alino/plan9-winxp/pi9/internal/chat"
	"github.com/alino/plan9-winxp/pi9/internal/store"
)

// writeTreeJSONL builds a one-branch tree from h and writes it as the
// session's .jsonl file. The stored System is taken from h so callers can
// prove loadSessionTree re-pins the System onto the passed value.
func writeTreeJSONL(t *testing.T, id string, h *chat.History) {
	t.Helper()
	tree := chat.FromHistory(id, "/cwd", h)
	data, err := tree.MarshalJSONL()
	if err != nil {
		t.Fatalf("MarshalJSONL: %v", err)
	}
	if err := store.SaveSessionTree(id, data); err != nil {
		t.Fatalf("SaveSessionTree: %v", err)
	}
}

// writeLegacyJSON writes a chat.History JSON snapshot as the session's
// legacy .json file (the format loadSessionTree migrates via FromHistory).
func writeLegacyJSON(t *testing.T, id string, h *chat.History) {
	t.Helper()
	data, err := json.Marshal(h)
	if err != nil {
		t.Fatalf("marshal legacy history: %v", err)
	}
	if err := store.SaveSession(id, data); err != nil {
		t.Fatalf("SaveSession: %v", err)
	}
}

// firstUserText returns the first non-synthetic, non-local user message in
// the materialized tree, so tests can tell the JSONL and legacy sources
// apart by content.
func firstUserText(tree *chat.SessionTree) string {
	if tree == nil {
		return ""
	}
	for _, turn := range tree.Materialize().Turns {
		if turn.Synthetic || turn.Local {
			continue
		}
		return turn.User
	}
	return ""
}

func TestLoadSessionTree_Matrix(t *testing.T) {
	const passedSystem = "SYSTEM-FROM-CALLER"

	t.Run("jsonl present wins over legacy", func(t *testing.T) {
		t.Setenv("PI9_HOME", t.TempDir())
		id := "sess-both"
		// JSONL stores a DIFFERENT system than the caller passes, so the
		// assertion below proves loadSessionTree re-pins System.
		writeTreeJSONL(t, id, &chat.History{System: "STALE-DISK-SYSTEM", Turns: []chat.Turn{
			{ID: "j1", User: "from-jsonl"},
		}})
		writeLegacyJSON(t, id, &chat.History{System: "legacy-sys", Turns: []chat.Turn{
			{ID: "l1", User: "from-legacy"},
		}})

		tree := loadSessionTree(id, "/cwd", passedSystem)
		if tree == nil {
			t.Fatal("loadSessionTree returned nil; expected the JSONL tree")
		}
		if got := firstUserText(tree); got != "from-jsonl" {
			t.Fatalf("JSONL should win: first user text = %q, want %q", got, "from-jsonl")
		}
		if tree.System != passedSystem {
			t.Fatalf("System = %q, want pinned to passed %q", tree.System, passedSystem)
		}
	})

	t.Run("only legacy json migrates", func(t *testing.T) {
		t.Setenv("PI9_HOME", t.TempDir())
		id := "sess-legacy"
		writeLegacyJSON(t, id, &chat.History{System: "legacy-sys", Turns: []chat.Turn{
			{ID: "l1", User: "from-legacy"},
		}})

		tree := loadSessionTree(id, "/cwd", passedSystem)
		if tree == nil {
			t.Fatal("loadSessionTree returned nil; expected a migrated legacy tree")
		}
		if got := firstUserText(tree); got != "from-legacy" {
			t.Fatalf("legacy should migrate: first user text = %q, want %q", got, "from-legacy")
		}
	})

	t.Run("corrupt jsonl falls through to legacy", func(t *testing.T) {
		t.Setenv("PI9_HOME", t.TempDir())
		id := "sess-corrupt-jsonl"
		// Unparseable JSONL (not merely empty) forces the fallthrough.
		if err := store.SaveSessionTree(id, []byte("{not valid json")); err != nil {
			t.Fatalf("SaveSessionTree: %v", err)
		}
		writeLegacyJSON(t, id, &chat.History{System: "legacy-sys", Turns: []chat.Turn{
			{ID: "l1", User: "from-legacy"},
		}})

		tree := loadSessionTree(id, "/cwd", passedSystem)
		if tree == nil {
			t.Fatal("loadSessionTree returned nil; expected the legacy fallback")
		}
		if got := firstUserText(tree); got != "from-legacy" {
			t.Fatalf("corrupt JSONL should fall back to legacy: first user text = %q, want %q", got, "from-legacy")
		}
	})

	t.Run("both corrupt returns nil", func(t *testing.T) {
		t.Setenv("PI9_HOME", t.TempDir())
		id := "sess-both-corrupt"
		if err := store.SaveSessionTree(id, []byte("{not valid json")); err != nil {
			t.Fatalf("SaveSessionTree: %v", err)
		}
		if err := store.SaveSession(id, []byte("{not valid json either")); err != nil {
			t.Fatalf("SaveSession: %v", err)
		}

		if tree := loadSessionTree(id, "/cwd", passedSystem); tree != nil {
			t.Fatalf("loadSessionTree = %+v, want nil when both sources are corrupt", tree)
		}
	})

	t.Run("nothing on disk returns nil", func(t *testing.T) {
		t.Setenv("PI9_HOME", t.TempDir())
		if tree := loadSessionTree("does-not-exist", "/cwd", passedSystem); tree != nil {
			t.Fatalf("loadSessionTree = %+v, want nil when no session exists", tree)
		}
	})
}

func TestRecordCompaction_FirstKeptAndFileOps(t *testing.T) {
	// Build a history where the summarized prefix (turns[:cut]) performed
	// read + edit/write tool calls, so the compaction body must carry the
	// file-op footer. The kept tail starts at turns[cut].
	turns := []chat.Turn{
		{ID: "t0", User: "explore", Assistant: "looking", Calls: []chat.ToolInvocation{
			{Name: "read", Args: `{"path":"a.go"}`},
			{Name: "edit", Args: `{"path":"b.go"}`},
		}},
		{ID: "t1", User: "more", Assistant: "writing", Calls: []chat.ToolInvocation{
			{Name: "write", Args: `{"path":"c.go"}`},
		}},
		{ID: "t2", User: "keep me", Assistant: "kept"},
		{ID: "t3", User: "and me", Assistant: "also kept"},
	}
	cut := 2 // turns[:2] summarized, turns[2:] kept

	m := &pi9Model{
		history: chat.History{System: "sys", Turns: turns},
		tree:    chat.NewSessionTree("rc", "/cwd", "sys"),
	}

	const summary = "short summary of earlier work"
	m.recordCompaction(cut, summary, 1234)

	// Locate the appended compaction entry (last entry in insertion order).
	var comp *chat.Entry
	for _, e := range m.tree.Tree() {
		if e.Kind == chat.KindCompaction {
			comp = e
		}
	}
	if comp == nil {
		t.Fatal("no compaction entry was appended")
	}

	wantFirstKept := turns[cut].ID
	if comp.FirstKeptID != wantFirstKept {
		t.Fatalf("FirstKeptID = %q, want id of history.Turns[%d] = %q", comp.FirstKeptID, cut, wantFirstKept)
	}

	if !strings.Contains(comp.Summary, summary) {
		t.Fatalf("compaction summary %q does not contain the original summary %q", comp.Summary, summary)
	}
	// The summarized prefix read a.go and modified b.go + c.go, so the
	// footer must surface both file-op blocks.
	if !strings.Contains(comp.Summary, "<read-files>") || !strings.Contains(comp.Summary, "a.go") {
		t.Fatalf("compaction body missing read-files footer: %q", comp.Summary)
	}
	if !strings.Contains(comp.Summary, "<modified-files>") ||
		!strings.Contains(comp.Summary, "b.go") || !strings.Contains(comp.Summary, "c.go") {
		t.Fatalf("compaction body missing modified-files footer: %q", comp.Summary)
	}
}
