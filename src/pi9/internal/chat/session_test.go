package chat

import (
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/alino/plan9-winxp/pi9/internal/provider"
)

// turn builds a non-local user->assistant turn with finished timestamps so
// it survives ToProviderMessages (which emits assistant content) and the
// turnID hash is stable.
func turn(user, asst string) Turn {
	now := time.Unix(0, 0)
	return Turn{User: user, Assistant: asst, Started: now, Finished: now}
}

// turnWithCall builds a turn carrying one finished tool call.
func turnWithCall(user, asst, callID, name, args, out string) Turn {
	t := turn(user, asst)
	t.Calls = []ToolInvocation{{
		ID:       callID,
		Name:     name,
		Args:     args,
		Output:   out,
		Started:  t.Started,
		Finished: t.Finished,
	}}
	return t
}

func TestSyncFromHistory_Idempotent(t *testing.T) {
	tree := NewSessionTree("s1", "/tmp", "SYS")
	h := &History{System: "SYS", Turns: []Turn{
		turn("hello", "hi there"),
		turn("how are you", "great"),
	}}

	tree.SyncFromHistory(h)
	first := len(tree.Tree())
	if first != 2 {
		t.Fatalf("after first sync want 2 entries, got %d", first)
	}
	leafAfterFirst := tree.Leaf()

	// Re-syncing the same history must add nothing and keep the leaf put.
	tree.SyncFromHistory(h)
	if got := len(tree.Tree()); got != first {
		t.Fatalf("second sync changed entry count: %d -> %d", first, got)
	}
	if tree.Leaf() != leafAfterFirst {
		t.Fatalf("second sync moved the leaf: %q -> %q", leafAfterFirst, tree.Leaf())
	}

	// Appending one more turn and re-syncing adds exactly one entry.
	h.Turns = append(h.Turns, turn("bye", "see ya"))
	tree.SyncFromHistory(h)
	if got := len(tree.Tree()); got != first+1 {
		t.Fatalf("after appending one turn want %d entries, got %d", first+1, got)
	}
	// And re-sync is still idempotent.
	tree.SyncFromHistory(h)
	if got := len(tree.Tree()); got != first+1 {
		t.Fatalf("idempotency broke after append: got %d", got)
	}
}

func TestSyncFromHistory_DuplicateTurnsGetDistinctIDs(t *testing.T) {
	tree := NewSessionTree("s", "/tmp", "SYS")
	// Two identical turns must still become two entries (salted ids).
	h := &History{System: "SYS", Turns: []Turn{turn("ping", "pong"), turn("ping", "pong")}}
	tree.SyncFromHistory(h)
	if got := len(tree.Tree()); got != 2 {
		t.Fatalf("want 2 distinct entries for identical turns, got %d", got)
	}
}

func TestMaterialize_RoundTrip(t *testing.T) {
	tree := NewSessionTree("s", "/tmp", "SYS")
	orig := &History{System: "SYS", Name: "my task", Turns: []Turn{
		turn("a", "1"),
		turnWithCall("b", "2", "call_1", "read", `{"path":"x"}`, "filebody"),
		turn("c", "3"),
	}}
	tree.SyncFromHistory(orig)

	got := tree.Materialize()
	if got.System != "SYS" {
		t.Fatalf("system not preserved: %q", got.System)
	}
	if got.Name != "my task" {
		t.Fatalf("name not preserved: %q", got.Name)
	}
	if len(got.Turns) != 3 {
		t.Fatalf("want 3 turns, got %d", len(got.Turns))
	}
	for i := range orig.Turns {
		if got.Turns[i].User != orig.Turns[i].User || got.Turns[i].Assistant != orig.Turns[i].Assistant {
			t.Fatalf("turn %d mismatch: %+v vs %+v", i, got.Turns[i], orig.Turns[i])
		}
	}
	if len(got.Turns[1].Calls) != 1 || got.Turns[1].Calls[0].Output != "filebody" {
		t.Fatalf("tool call not round-tripped: %+v", got.Turns[1].Calls)
	}

	// Re-syncing the materialized history must be a no-op (round-trip
	// stable): same number of message entries.
	before := len(tree.visibleMessageEntries())
	tree.SyncFromHistory(got)
	if after := len(tree.visibleMessageEntries()); after != before {
		t.Fatalf("materialize->sync round-trip not stable: %d -> %d", before, after)
	}
}

func TestFork_StartsNewBranch(t *testing.T) {
	tree := NewSessionTree("s", "/tmp", "SYS")
	h := &History{System: "SYS", Turns: []Turn{
		turn("u1", "a1"),
		turn("u2", "a2"),
		turn("u3", "a3"),
	}}
	tree.SyncFromHistory(h)

	// Identify the second user message entry id.
	ids := tree.MessageEntryIDs()
	if len(ids) != 3 {
		t.Fatalf("want 3 message entries, got %d", len(ids))
	}
	second := ids[1]

	// Fork from the 2nd message: leaf moves to its PARENT (the 1st).
	if !tree.Fork(second) {
		t.Fatal("Fork returned false")
	}
	if tree.Leaf() != ids[0] {
		t.Fatalf("after fork leaf want %q (first msg), got %q", ids[0], tree.Leaf())
	}

	// Materialize now shows only the first turn.
	mat := tree.Materialize()
	if len(mat.Turns) != 1 || mat.Turns[0].User != "u1" {
		t.Fatalf("forked branch should show only u1, got %+v", mat.Turns)
	}

	// Append a new turn on the new branch and sync: total entries grow by
	// one, old branch still reachable.
	mat.Turns = append(mat.Turns, turn("u2b", "a2b"))
	tree.SyncFromHistory(mat)
	if got := len(tree.Tree()); got != 4 {
		t.Fatalf("after forked append want 4 entries, got %d", got)
	}
	// The original second message must still exist (not overwritten).
	if tree.Get(second) == nil {
		t.Fatal("original branch entry lost after fork")
	}
	// First message now has two children (old branch + new branch).
	if got := len(tree.Children(ids[0])); got != 2 {
		t.Fatalf("first message should have 2 children after fork, got %d", got)
	}
}

func TestClone_DuplicatesActiveBranch(t *testing.T) {
	tree := NewSessionTree("orig", "/tmp", "SYS")
	h := &History{System: "SYS", Turns: []Turn{turn("u1", "a1"), turn("u2", "a2")}}
	tree.SyncFromHistory(h)

	clone := tree.Clone("clone1")
	if clone.ID != "clone1" {
		t.Fatalf("clone id: %q", clone.ID)
	}
	if clone.Leaf() != tree.Leaf() {
		t.Fatalf("clone leaf %q != orig leaf %q", clone.Leaf(), tree.Leaf())
	}
	if got := clone.Materialize(); len(got.Turns) != 2 {
		t.Fatalf("clone should materialize 2 turns, got %d", len(got.Turns))
	}
	// Mutating the clone must not affect the original tree.
	h2 := clone.Materialize()
	h2.Turns = append(h2.Turns, turn("u3", "a3"))
	clone.SyncFromHistory(h2)
	if got := len(tree.Tree()); got != 2 {
		t.Fatalf("original tree mutated by clone append: %d entries", got)
	}
}

func TestSetLeaf_AndNavigation(t *testing.T) {
	tree := NewSessionTree("s", "/tmp", "SYS")
	h := &History{System: "SYS", Turns: []Turn{turn("u1", "a1"), turn("u2", "a2")}}
	tree.SyncFromHistory(h)
	ids := tree.MessageEntryIDs()

	if !tree.SetLeaf(ids[0]) {
		t.Fatal("SetLeaf(first) returned false")
	}
	if tree.Leaf() != ids[0] {
		t.Fatalf("leaf not set: %q", tree.Leaf())
	}
	if got := tree.Materialize(); len(got.Turns) != 1 {
		t.Fatalf("after SetLeaf(first) want 1 turn, got %d", len(got.Turns))
	}
	// Unknown id is rejected; leaf unchanged.
	if tree.SetLeaf("deadbeef") {
		t.Fatal("SetLeaf(unknown) should return false")
	}
	if tree.Leaf() != ids[0] {
		t.Fatalf("leaf changed on rejected SetLeaf: %q", tree.Leaf())
	}
	// Empty resets to before-root.
	if !tree.SetLeaf("") {
		t.Fatal("SetLeaf(\"\") should succeed")
	}
	if got := tree.Materialize(); len(got.Turns) != 0 {
		t.Fatalf("after reset want 0 turns, got %d", len(got.Turns))
	}
	// Roots + Children.
	if len(tree.Roots()) != 1 {
		t.Fatalf("want 1 root, got %d", len(tree.Roots()))
	}
	if len(tree.Children(ids[0])) != 1 {
		t.Fatalf("first message should have 1 child, got %d", len(tree.Children(ids[0])))
	}
}

func TestToProviderMessages_NoCompaction_MatchesHistory(t *testing.T) {
	h := &History{System: "SYS", Turns: []Turn{
		turn("u1", "a1"),
		turnWithCall("u2", "a2", "c1", "read", `{"path":"p"}`, "out"),
	}}
	tree := NewSessionTree("s", "/tmp", "SYS")
	tree.SyncFromHistory(h)

	hMsgs := h.ToProviderMessages()
	tMsgs := tree.ToProviderMessages()
	if len(hMsgs) != len(tMsgs) {
		t.Fatalf("message count differs: history=%d tree=%d", len(hMsgs), len(tMsgs))
	}
	for i := range hMsgs {
		if hMsgs[i].Role != tMsgs[i].Role || hMsgs[i].Content != tMsgs[i].Content {
			t.Fatalf("message %d differs:\n history=%+v\n tree=   %+v", i, hMsgs[i], tMsgs[i])
		}
		if len(hMsgs[i].ToolCalls) != len(tMsgs[i].ToolCalls) {
			t.Fatalf("message %d tool-call count differs", i)
		}
	}
}

func TestToProviderMessages_HonorsCompaction(t *testing.T) {
	tree := NewSessionTree("s", "/tmp", "SYS")
	h := &History{System: "SYS", Turns: []Turn{
		turn("old1", "oa1"),
		turn("old2", "oa2"),
		turn("keep1", "ka1"),
		turn("keep2", "ka2"),
	}}
	tree.SyncFromHistory(h)
	ids := tree.MessageEntryIDs()

	// Compact: keep the last two turns; firstKept = ids[2].
	tree.AppendCompaction("SUMMARY-TEXT", ids[2], 12345)

	msgs := tree.ToProviderMessages()
	// Expect: system, (compacted-user, summary-assistant), then keep1/keep2.
	if msgs[0].Role != provider.RoleSystem {
		t.Fatalf("first message should be system, got %v", msgs[0].Role)
	}
	joined := ""
	for _, m := range msgs {
		joined += string(m.Role) + ":" + m.Content + "\n"
	}
	if !strings.Contains(joined, "SUMMARY-TEXT") {
		t.Fatalf("summary not present in provider messages:\n%s", joined)
	}
	// Summarized-away content must NOT appear.
	if strings.Contains(joined, "old1") || strings.Contains(joined, "old2") {
		t.Fatalf("compacted-away content leaked into provider messages:\n%s", joined)
	}
	// Kept content must appear.
	if !strings.Contains(joined, "keep1") || !strings.Contains(joined, "keep2") {
		t.Fatalf("kept content missing from provider messages:\n%s", joined)
	}

	// Materialize also hides the summarized prefix and shows the synthetic
	// compaction turn + kept turns.
	mat := tree.Materialize()
	// synthetic compaction turn + 2 kept turns = 3.
	if len(mat.Turns) != 3 {
		t.Fatalf("materialized compacted branch want 3 turns, got %d", len(mat.Turns))
	}
	if mat.Turns[0].User != "(earlier conversation compacted)" {
		t.Fatalf("first turn should be the synthetic compaction turn, got %q", mat.Turns[0].User)
	}
	if mat.Turns[1].User != "keep1" {
		t.Fatalf("kept turn missing, got %q", mat.Turns[1].User)
	}

	// Re-syncing the materialized (post-compaction) history must add no new
	// message entries — the synthetic turn is skipped and the kept turns
	// already exist.
	before := len(tree.Tree())
	tree.SyncFromHistory(mat)
	if after := len(tree.Tree()); after != before {
		t.Fatalf("sync after compaction added entries: %d -> %d", before, after)
	}
}

func TestJSONL_RoundTrip(t *testing.T) {
	tree := NewSessionTree("sid", "/work/dir", "SYS")
	h := &History{System: "SYS", Name: "named", Turns: []Turn{
		turn("u1", "a1"),
		turnWithCall("u2", "a2", "c1", "edit", `{"path":"f"}`, "ok"),
	}}
	tree.SyncFromHistory(h)
	tree.AppendModelChange("anthropic/claude")
	tree.AppendThinkingChange("high")

	data, err := tree.MarshalJSONL()
	if err != nil {
		t.Fatalf("MarshalJSONL: %v", err)
	}
	// Header is the first line, leaf is the last.
	lines := strings.Split(strings.TrimRight(string(data), "\n"), "\n")
	if !strings.Contains(lines[0], `"type":"session"`) || !strings.Contains(lines[0], `"version":3`) {
		t.Fatalf("first line should be a v3 session header: %s", lines[0])
	}
	if !strings.Contains(lines[len(lines)-1], `"type":"leaf"`) {
		t.Fatalf("last line should be a leaf record: %s", lines[len(lines)-1])
	}

	loaded, err := UnmarshalJSONL(data, "FRESH-SYS")
	if err != nil {
		t.Fatalf("UnmarshalJSONL: %v", err)
	}
	if loaded.ID != "sid" || loaded.Cwd != "/work/dir" {
		t.Fatalf("header not parsed: id=%q cwd=%q", loaded.ID, loaded.Cwd)
	}
	if loaded.System != "FRESH-SYS" {
		t.Fatalf("system should be overlaid from arg, got %q", loaded.System)
	}
	if loaded.Leaf() != tree.Leaf() {
		t.Fatalf("leaf not preserved: %q vs %q", loaded.Leaf(), tree.Leaf())
	}
	if loaded.Name() != "named" {
		t.Fatalf("name not preserved: %q", loaded.Name())
	}
	mat := loaded.Materialize()
	if len(mat.Turns) != 2 {
		t.Fatalf("loaded tree should materialize 2 turns, got %d", len(mat.Turns))
	}
	if len(mat.Turns[1].Calls) != 1 || mat.Turns[1].Calls[0].Name != "edit" {
		t.Fatalf("tool call lost on round-trip: %+v", mat.Turns[1].Calls)
	}
}

func TestUnmarshalJSONL_LeafLastOneWins(t *testing.T) {
	tree := NewSessionTree("s", "/d", "SYS")
	h := &History{System: "SYS", Turns: []Turn{turn("u1", "a1"), turn("u2", "a2")}}
	tree.SyncFromHistory(h)
	ids := tree.MessageEntryIDs()

	data, err := tree.MarshalJSONL()
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	// Append a second, older leaf pointer; the FIRST-written leaf is the
	// real one but last-one-wins means our appended line should govern.
	extra := []byte(`{"type":"leaf","id":"` + ids[0] + `"}` + "\n")
	data = append(data, extra...)

	loaded, err := UnmarshalJSONL(data, "SYS")
	if err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if loaded.Leaf() != ids[0] {
		t.Fatalf("last-one-wins failed: leaf=%q want %q", loaded.Leaf(), ids[0])
	}
}

func TestFromHistory_LinearMigration(t *testing.T) {
	h := &History{System: "SYS", Name: "legacy", Turns: []Turn{
		turn("u1", "a1"),
		{User: "/help", Assistant: "help text", Local: true},
		turn("u2", "a2"),
	}}
	tree := FromHistory("legacy-id", "/cwd", h)
	if tree.ID != "legacy-id" {
		t.Fatalf("id: %q", tree.ID)
	}
	if tree.Name() != "legacy" {
		t.Fatalf("name not migrated: %q", tree.Name())
	}
	mat := tree.Materialize()
	if len(mat.Turns) != 3 {
		t.Fatalf("migrated history should have 3 turns (incl local), got %d", len(mat.Turns))
	}
	if !mat.Turns[1].Local {
		t.Fatalf("local turn should survive migration as local")
	}
	// The chain should be linear: each entry has at most one child.
	for _, e := range tree.Tree() {
		if got := len(tree.Children(e.ID)); got > 1 {
			t.Fatalf("migrated tree is not linear: entry %s has %d children", e.ID, got)
		}
	}
}

func TestBranchSummary_VisibleAndInContext(t *testing.T) {
	tree := NewSessionTree("s", "/d", "SYS")
	h := &History{System: "SYS", Turns: []Turn{turn("u1", "a1")}}
	tree.SyncFromHistory(h)
	tree.AppendBranchSummary("explored approach A", "someFromID")

	mat := tree.Materialize()
	// 1 real turn + 1 synthetic branch-summary (local) turn.
	if len(mat.Turns) != 2 {
		t.Fatalf("want 2 turns (turn + branch summary), got %d", len(mat.Turns))
	}
	if !mat.Turns[1].Local || !strings.Contains(mat.Turns[1].Assistant, "approach A") {
		t.Fatalf("branch summary turn wrong: %+v", mat.Turns[1])
	}
	// Branch summary appears in provider context as a user-context message.
	msgs := tree.ToProviderMessages()
	found := false
	for _, m := range msgs {
		if strings.Contains(m.Content, "approach A") {
			found = true
		}
	}
	if !found {
		t.Fatal("branch summary not present in provider messages")
	}
	// Re-syncing must not duplicate the branch summary as a message entry.
	before := len(tree.Tree())
	tree.SyncFromHistory(mat)
	if after := len(tree.Tree()); after != before {
		t.Fatalf("branch-summary sync added entries: %d -> %d", before, after)
	}
}

// TestSyncFromHistory_GrowingTurnUpdatedInPlace is the H1 regression: an
// in-flight turn is saved repeatedly as its content streams in. Each save
// must UPDATE the same entry in place, never append a new partial entry.
func TestSyncFromHistory_GrowingTurnUpdatedInPlace(t *testing.T) {
	tree := NewSessionTree("s1", "/tmp", "SYS")
	h := &History{System: "SYS"}
	h.AppendUser("do the thing") // assigns a stable ID

	h.Turns[0].Assistant = "working"
	tree.SyncFromHistory(h)
	if got := len(tree.MessageEntryIDs()); got != 1 {
		t.Fatalf("after first save want 1 message entry, got %d", got)
	}
	leaf := tree.Leaf()

	// Stream grows across several saves: more text, a tool call, final text.
	h.Turns[0].Assistant = "working on it"
	tree.SyncFromHistory(h)
	h.Turns[0].Calls = []ToolInvocation{{ID: "c1", Name: "read", Args: "{}", Output: "data"}}
	h.Turns[0].Assistant = "working on it... done"
	tree.SyncFromHistory(h)

	if got := len(tree.MessageEntryIDs()); got != 1 {
		t.Fatalf("H1 regression: growing in-flight turn must stay 1 entry, got %d", got)
	}
	if tree.Leaf() != leaf {
		t.Fatalf("growing turn moved the leaf: %q -> %q", leaf, tree.Leaf())
	}
	mh := tree.Materialize()
	if len(mh.Turns) != 1 || !strings.Contains(mh.Turns[0].Assistant, "done") || len(mh.Turns[0].Calls) != 1 {
		t.Fatalf("entry not updated in place to final content: %+v", mh.Turns)
	}
}

// TestFromHistory_LegacyCompactedNoDuplication is the H2 regression: a
// migrated legacy .json carrying a synthetic compaction turn must not
// duplicate its post-summary turns on subsequent autosaves.
func TestFromHistory_LegacyCompactedNoDuplication(t *testing.T) {
	legacy := &History{System: "SYS", Turns: []Turn{
		{User: "(earlier conversation compacted)", Assistant: "Summary of earlier conversation:\n\nstuff"},
		turn("q1", "a1"),
		turn("q2", "a2"),
	}}
	tree := FromHistory("s1", "/tmp", legacy)

	// Load path: Materialize for rendering, then autosave (twice).
	h := tree.Materialize()
	base := len(tree.Tree())
	tree.SyncFromHistory(h)
	if got := len(tree.Tree()); got != base {
		t.Fatalf("H2 regression: first autosave duplicated entries %d -> %d", base, got)
	}
	tree.SyncFromHistory(h)
	if got := len(tree.Tree()); got != base {
		t.Fatalf("H2 regression: second autosave duplicated entries %d -> %d", base, got)
	}
	if got := len(tree.Materialize().Turns); got != 3 {
		t.Fatalf("want 3 turns after migration+saves, got %d", got)
	}
}

// TestSyncFromHistory_PersistsErrorWithoutContentChange covers the re-review
// finding: a turn that finishes with an error but no content change between
// saves must still have its error + Finished persisted (turnsEqual ignores
// both, so the in-place update would otherwise be skipped).
func TestSyncFromHistory_PersistsErrorWithoutContentChange(t *testing.T) {
	tree := NewSessionTree("s1", "/tmp", "SYS")
	h := &History{System: "SYS"}
	h.AppendUser("do it")   // empty assistant, Finished zero
	tree.SyncFromHistory(h) // first save: in-flight, no error

	// Stream fails immediately with no assistant text / tool calls.
	h.FinishTurn(fmt.Errorf("boom"))
	tree.SyncFromHistory(h) // second save: same content, now errored + finished

	if got := len(tree.MessageEntryIDs()); got != 1 {
		t.Fatalf("want 1 entry, got %d", got)
	}
	mh := tree.Materialize()
	if len(mh.Turns) != 1 {
		t.Fatalf("want 1 turn, got %d", len(mh.Turns))
	}
	if mh.Turns[0].ErrText != "boom" {
		t.Fatalf("error not persisted: ErrText=%q", mh.Turns[0].ErrText)
	}
	if mh.Turns[0].Finished.IsZero() {
		t.Fatal("Finished timestamp not persisted (turn would render as in-flight on reload)")
	}
}
