package chat

// SessionTree is pi9's tree-structured session model. It owns ALL
// branches of a conversation, while chat.History remains the flat,
// in-memory ACTIVE BRANCH that rendering and the agent loop use.
//
// The design follows pi.dev's session format (see docs/session-format.md
// and docs/sessions.md): every entry has a stable string ID and a
// ParentID, forming a tree. The current position is the Leaf. Navigating
// to an earlier entry and appending new work creates a new branch without
// rewriting any existing entry.
//
// The split of responsibilities is deliberate and low-blast-radius:
//
//   - History (System, Name, Turns): the visible/active branch. Rendering,
//     streaming, and tool execution operate on it UNCHANGED.
//   - SessionTree: owns persistence + branching. After each turn, main.go
//     calls SyncFromHistory to fold new History turns into the tree, then
//     persists. To switch branches it calls SetLeaf/Fork/Clone and then
//     Materialize to rebuild the History the UI renders.
//
// Entry kinds (mirroring pi):
//
//   - KindMessage:        payload is one chat.Turn (the unit of render).
//   - KindModelChange:    a /model switch (Model).
//   - KindThinkingChange: a Shift+Tab thinking-level change (Level).
//   - KindCompaction:     summary + firstKeptID + tokensBefore.
//   - KindBranchSummary:  summary attached when /tree leaves a branch.
//   - KindSessionInfo:    a /name display-name change (Name).
//
// IDs are 8 hex chars. Message entries get a CONTENT-DERIVED id so a
// re-sync of the same History is idempotent (the same Turn maps to the
// same entry). Non-message entries get random ids.

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"strconv"
	"time"

	"github.com/alino/plan9-winxp/pi9/internal/provider"
)

// Entry kind constants. Stored verbatim in the JSONL `type` field so they
// must stay stable across versions.
const (
	KindMessage        = "message"
	KindModelChange    = "model_change"
	KindThinkingChange = "thinking_level_change"
	KindCompaction     = "compaction"
	KindBranchSummary  = "branch_summary"
	KindSessionInfo    = "session_info"
)

// SessionVersion is the JSONL header version pi9 writes (tree format).
const SessionVersion = 3

// Entry is a single node in the session tree. Exactly one of the payload
// fields is meaningful, selected by Kind. JSON tags match pi's on-disk
// session format so external .jsonl files round-trip.
//
// The struct is flat (not a tagged union) so the JSONL line for each
// entry is a single object; unused fields are omitted via omitempty.
type Entry struct {
	Kind     string    `json:"type"`
	ID       string    `json:"id"`
	ParentID string    `json:"parentId"`
	Ts       time.Time `json:"timestamp"`

	// KindMessage payload.
	Message *Turn `json:"message,omitempty"`

	// KindModelChange payload.
	Model string `json:"modelId,omitempty"`

	// KindThinkingChange payload.
	Level string `json:"thinkingLevel,omitempty"`

	// KindCompaction payload.
	Summary      string `json:"summary,omitempty"`
	FirstKeptID  string `json:"firstKeptEntryId,omitempty"`
	TokensBefore int    `json:"tokensBefore,omitempty"`

	// KindBranchSummary payload. Reuses Summary above; FromID records the
	// entry we branched from.
	FromID string `json:"fromId,omitempty"`

	// KindSessionInfo payload.
	Name string `json:"name,omitempty"`
}

// Header is the first JSONL line: session metadata, not part of the tree.
type Header struct {
	Type    string    `json:"type"` // always "session"
	Version int       `json:"version"`
	ID      string    `json:"id"`
	Created time.Time `json:"created"`
	Cwd     string    `json:"cwd,omitempty"`
	Name    string    `json:"name,omitempty"`
}

// leafLine is the trailing JSONL record pointing at the current leaf.
// Last-one-wins on load so re-persisting after a SetLeaf is cheap.
type leafLine struct {
	Type string `json:"type"` // always "leaf"
	ID   string `json:"id"`
}

// SessionTree is the in-memory tree of all branches for one session.
//
// System is the active system prompt; it is NOT persisted as a tree
// entry (it is rebuilt from current sources on every load, like History
// already does) but is carried so Materialize can populate History.System.
type SessionTree struct {
	ID      string
	Created time.Time
	Cwd     string
	System  string

	entries map[string]*Entry // id -> entry
	order   []string          // insertion order, for stable persistence
	leaf    string            // current leaf id ("" = empty tree / before root)
}

// NewSessionTree creates an empty tree with the given id, cwd, and system
// prompt. A zero id means the caller will set one before persisting.
func NewSessionTree(id, cwd, system string) *SessionTree {
	return &SessionTree{
		ID:      id,
		Created: time.Now().UTC(),
		Cwd:     cwd,
		System:  system,
		entries: map[string]*Entry{},
	}
}

// ----- ID generation -----

// newRandomID returns an 8-hex-char id with negligible collision odds for
// a single session's entry count. Derived from the entry count + a clock
// so it is unique without importing crypto/rand (which is unavailable on
// some plan9 builds); ties are broken by the running entry index.
func (s *SessionTree) newRandomID() string {
	for i := 0; ; i++ {
		seed := fmt.Sprintf("%s|%d|%d|%d", s.ID, len(s.order), time.Now().UnixNano(), i)
		sum := sha256.Sum256([]byte(seed))
		id := hex.EncodeToString(sum[:4])
		if _, exists := s.entries[id]; !exists {
			return id
		}
	}
}

// turnID derives a stable id for a message entry from its parent id and
// the turn's content. The same Turn under the same parent always yields
// the same id, which is what makes SyncFromHistory idempotent. A salt
// disambiguates the rare case of two identical turns under one parent.
func turnID(parentID string, t Turn, salt int) string {
	h := sha256.New()
	h.Write([]byte(parentID))
	h.Write([]byte{0})
	h.Write([]byte(t.User))
	h.Write([]byte{0})
	h.Write([]byte(t.Assistant))
	h.Write([]byte{0})
	h.Write([]byte(t.Reasoning))
	h.Write([]byte{0})
	if t.Local {
		h.Write([]byte("L"))
	}
	for _, c := range t.Calls {
		h.Write([]byte{0})
		h.Write([]byte(c.ID))
		h.Write([]byte(c.Name))
		h.Write([]byte(c.Args))
		h.Write([]byte(c.Output))
	}
	h.Write([]byte{0})
	h.Write([]byte(strconv.Itoa(salt)))
	sum := h.Sum(nil)
	return hex.EncodeToString(sum[:4])
}

// ----- low-level mutation -----

// add appends an entry to the tree, advancing the leaf to it. The entry's
// ParentID is set to the current leaf if not already set.
func (s *SessionTree) add(e *Entry) {
	if e.ID == "" {
		e.ID = s.newRandomID()
	}
	if e.Ts.IsZero() {
		e.Ts = time.Now().UTC()
	}
	s.entries[e.ID] = e
	s.order = append(s.order, e.ID)
	s.leaf = e.ID
}

// ----- appending typed entries -----

// AppendModelChange records a /model switch under the current leaf.
func (s *SessionTree) AppendModelChange(model string) string {
	e := &Entry{Kind: KindModelChange, ParentID: s.leaf, Model: model}
	s.add(e)
	return e.ID
}

// AppendThinkingChange records a thinking-level change under the leaf.
func (s *SessionTree) AppendThinkingChange(level string) string {
	e := &Entry{Kind: KindThinkingChange, ParentID: s.leaf, Level: level}
	s.add(e)
	return e.ID
}

// AppendCompaction records a compaction entry: the summary, the id of the
// first kept entry on the path, and the pre-compaction token estimate.
func (s *SessionTree) AppendCompaction(summary, firstKeptID string, tokensBefore int) string {
	e := &Entry{
		Kind:         KindCompaction,
		ParentID:     s.leaf,
		Summary:      summary,
		FirstKeptID:  firstKeptID,
		TokensBefore: tokensBefore,
	}
	s.add(e)
	return e.ID
}

// AppendBranchSummary records a branch-summary entry under the leaf,
// capturing context from the branch we navigated away from (fromID).
func (s *SessionTree) AppendBranchSummary(summary, fromID string) string {
	e := &Entry{Kind: KindBranchSummary, ParentID: s.leaf, Summary: summary, FromID: fromID}
	s.add(e)
	return e.ID
}

// AppendSessionInfo records a display-name change under the leaf.
func (s *SessionTree) AppendSessionInfo(name string) string {
	e := &Entry{Kind: KindSessionInfo, ParentID: s.leaf, Name: name}
	s.add(e)
	return e.ID
}

// ----- navigation -----

// Leaf returns the current leaf id ("" before any entries).
func (s *SessionTree) Leaf() string { return s.leaf }

// SetLeaf moves the current position to id. Returns false if id is not in
// the tree (the leaf is left unchanged). The empty string is accepted and
// resets the leaf to "before the root".
func (s *SessionTree) SetLeaf(id string) bool {
	if id == "" {
		s.leaf = ""
		return true
	}
	if _, ok := s.entries[id]; !ok {
		return false
	}
	s.leaf = id
	return true
}

// Get returns the entry with the given id, or nil.
func (s *SessionTree) Get(id string) *Entry { return s.entries[id] }

// Children returns the direct children of id (entries whose ParentID is
// id), in insertion order. Passing "" returns the roots.
func (s *SessionTree) Children(id string) []*Entry {
	var out []*Entry
	for _, eid := range s.order {
		e := s.entries[eid]
		if e.ParentID == id {
			out = append(out, e)
		}
	}
	return out
}

// Roots returns the top-level entries (ParentID == "").
func (s *SessionTree) Roots() []*Entry { return s.Children("") }

// Tree returns all entries in insertion order. Callers must not mutate
// the returned slice's entries.
func (s *SessionTree) Tree() []*Entry {
	out := make([]*Entry, 0, len(s.order))
	for _, id := range s.order {
		out = append(out, s.entries[id])
	}
	return out
}

// pathToLeaf walks from the given id up to a root, returning the entries
// ordered root..id. A missing id yields nil.
func (s *SessionTree) pathFrom(id string) []*Entry {
	var rev []*Entry
	seen := map[string]bool{}
	for cur := id; cur != ""; {
		e := s.entries[cur]
		if e == nil || seen[cur] {
			break // dangling parent or cycle guard
		}
		seen[cur] = true
		rev = append(rev, e)
		cur = e.ParentID
	}
	// Reverse into root..id order.
	for i, j := 0, len(rev)-1; i < j; i, j = i+1, j-1 {
		rev[i], rev[j] = rev[j], rev[i]
	}
	return rev
}

// activePath returns the entries on the path root..leaf.
func (s *SessionTree) activePath() []*Entry { return s.pathFrom(s.leaf) }

// MessageEntryIDs returns, in order, the ids of the message entries on the
// active path (root..leaf). Useful for mapping a History turn index back
// to its tree entry id (e.g. to set a compaction's firstKeptEntryId).
func (s *SessionTree) MessageEntryIDs() []string {
	var out []string
	for _, e := range s.activePath() {
		if e.Kind == KindMessage {
			out = append(out, e.ID)
		}
	}
	return out
}

// ----- name lookup -----

// Name returns the most recent session display name on the active path,
// or "" if none. The latest session_info entry wins.
func (s *SessionTree) Name() string {
	name := ""
	for _, e := range s.activePath() {
		if e.Kind == KindSessionInfo {
			name = e.Name
		}
	}
	return name
}

// ----- Materialize -----

// compactionView analyzes a root..leaf path for the governing compaction
// (the last compaction entry on the path) and the set of message-entry ids
// hidden by it (those before its firstKeptEntryId). Returns -1 and an empty
// set when there is no compaction on the path.
func compactionView(path []*Entry) (lastCompactionIdx int, hidden map[string]bool) {
	lastCompactionIdx = -1
	hidden = map[string]bool{}
	for i, e := range path {
		if e.Kind == KindCompaction {
			lastCompactionIdx = i
		}
	}
	if lastCompactionIdx < 0 {
		return lastCompactionIdx, hidden
	}
	comp := path[lastCompactionIdx]
	cutIdx := lastCompactionIdx // default: hide everything before the compaction
	if comp.FirstKeptID != "" {
		for i, e := range path {
			if e.ID == comp.FirstKeptID {
				cutIdx = i
				break
			}
		}
	}
	for i := 0; i < cutIdx; i++ {
		hidden[path[i].ID] = true
	}
	return lastCompactionIdx, hidden
}

// visibleMessageEntries returns the message entries on the active path that
// Materialize includes as Turns — i.e. excluding those hidden by the
// governing compaction. Order matches Materialize's turn order (after the
// synthetic compaction turn, when present).
func (s *SessionTree) visibleMessageEntries() []*Entry {
	path := s.activePath()
	_, hidden := compactionView(path)
	var out []*Entry
	for _, e := range path {
		if e.Kind == KindMessage && !hidden[e.ID] && e.Message != nil {
			out = append(out, e)
		}
	}
	return out
}

// Materialize rebuilds a flat *History for the active branch (root..leaf).
// Message entries become Turns in order. A compaction entry on the path
// becomes the synthetic summary turn that begins the visible branch:
// everything before firstKeptID is hidden, the summary is shown as one
// turn, then the kept message turns follow. A branch_summary entry becomes
// a synthetic (local) turn so the abandoned-branch context is visible.
//
// The returned History is independent of the tree (deep enough that
// rendering + streaming can mutate it freely); call SyncFromHistory to
// fold changes back in.
func (s *SessionTree) Materialize() *History {
	h := &History{System: s.System, Name: s.Name()}
	path := s.activePath()
	lastCompactionIdx, hidden := compactionView(path)

	// The governing compaction's summary represents the HEAD of the visible
	// branch, even though the compaction entry itself is the newest entry on
	// the path. Emit the synthetic summary turn just before the first
	// non-hidden message entry.
	compactionEmitted := lastCompactionIdx < 0
	summary := ""
	if lastCompactionIdx >= 0 {
		summary = path[lastCompactionIdx].Summary
	}

	for i, e := range path {
		switch e.Kind {
		case KindMessage:
			if hidden[e.ID] {
				continue
			}
			if !compactionEmitted {
				h.Turns = append(h.Turns, compactionTurn(summary))
				compactionEmitted = true
			}
			if e.Message != nil {
				t := *e.Message
				t.ID = e.ID // carry the entry id so a later SyncFromHistory matches it
				t.RestoreErr()
				h.Turns = append(h.Turns, t)
			}
		case KindCompaction:
			// The synthetic summary turn is emitted before the first kept
			// message (above), not at the compaction entry's position.
			_ = i
		case KindBranchSummary:
			h.Turns = append(h.Turns, branchSummaryTurn(e.Summary))
		case KindSessionInfo:
			// Folded into h.Name via Name(); no visible turn.
		case KindModelChange, KindThinkingChange:
			// Settings entries are not visible turns.
		}
	}
	// If a compaction governs the path but every kept message was also
	// hidden (degenerate), still surface the summary so the branch isn't
	// empty.
	if !compactionEmitted {
		h.Turns = append(h.Turns, compactionTurn(summary))
	}
	return h
}

// compactionTurn builds the synthetic assistant turn that represents a
// compaction summary at the head of the visible branch. It mirrors the
// shape applyCompaction (in main) produces so rendering is consistent.
func compactionTurn(summary string) Turn {
	now := time.Now()
	return Turn{
		User:      "(earlier conversation compacted)",
		Assistant: "Summary of earlier conversation:\n\n" + summary,
		Synthetic: true,
		Started:   now,
		Finished:  now,
	}
}

// branchSummaryTurn builds the synthetic (local) turn shown when a branch
// summary is present, so the abandoned-branch context is visible but never
// re-sent verbatim to the model as a user/assistant exchange.
func branchSummaryTurn(summary string) Turn {
	now := time.Now()
	return Turn{
		User:      "(summary of a previous branch)",
		Assistant: summary,
		Local:     true,
		Synthetic: true,
		Started:   now,
		Finished:  now,
	}
}

// RestoreErr rehydrates the Err interfaces on a single Turn from ErrText.
// Mirrors History.RestoreErrs for a standalone turn.
func (t *Turn) RestoreErr() {
	if t.ErrText != "" && t.Err == nil {
		t.Err = fmt.Errorf("%s", t.ErrText)
	}
	for j := range t.Calls {
		c := &t.Calls[j]
		if c.ErrText != "" && c.Err == nil {
			c.Err = fmt.Errorf("%s", c.ErrText)
		}
	}
}

// ----- SyncFromHistory -----

// SyncFromHistory folds the History's active branch back into the tree:
// every Turn that is not already represented as a message entry on the
// active path (in order) is appended as a new message entry chained under
// the leaf, advancing the leaf. It is IDEMPOTENT — calling it twice with
// the same History adds nothing the second time — because message ids are
// content-derived (see turnID).
//
// Synthetic turns produced by Materialize (the compaction summary turn and
// branch-summary turns) are skipped: they already exist as their own entry
// kinds and must not be re-appended as messages.
//
// h.Name changes are recorded as a session_info entry when they differ
// from the tree's current name. h.System is copied into the tree.
func (s *SessionTree) SyncFromHistory(h *History) {
	if h == nil {
		return
	}
	s.System = h.System

	// Each Turn carries a stable ID (assigned at creation by AppendUser/
	// AppendLocal, or copied from its entry id by Materialize). We match on
	// that ID rather than on content position: a turn already on the active
	// path is either skipped (unchanged) or UPDATED IN PLACE (the in-flight
	// streaming turn is saved several times as its content fills, and finally
	// when it errors/finishes); a turn whose ID is not on the path is appended
	// under the current leaf. Matching by ID is what keeps this idempotent —
	// the growing leaf no longer re-appends a new partial entry on every save,
	// and skipping a synthetic turn no longer shifts the alignment of the
	// turns after it.
	//
	// We only update entries on the ACTIVE PATH: a History materialized from
	// this leaf can only carry on-path (or fresh) ids, so restricting the
	// match prevents a stray id collision from mutating a sibling branch.
	onPath := make(map[string]bool)
	for _, e := range s.activePath() {
		onPath[e.ID] = true
	}
	for i := range h.Turns {
		t := h.Turns[i]
		if isSyntheticTurn(t) {
			continue
		}
		if t.ID != "" && onPath[t.ID] {
			if e := s.entries[t.ID]; e != nil && e.Kind == KindMessage {
				if e.Message == nil || !turnStoredEqual(*e.Message, t) {
					stored := sanitizeTurn(t)
					e.Message = &stored
				}
				continue // already represented; leaf unchanged
			}
		}
		// New turn: append under the current leaf (uses t.ID as the entry id,
		// or a fresh random id if it had none). Write the id back into the
		// History so a turn created without AppendUser still becomes stable —
		// the next sync matches it instead of appending a duplicate.
		h.Turns[i].ID = s.appendMessage(t)
	}

	// Record a name change if the History's name diverged.
	if h.Name != "" && h.Name != s.Name() {
		s.AppendSessionInfo(h.Name)
	}
}

// sanitizeTurn returns a persistable copy of t with the non-serializable
// Err interfaces cleared (only ErrText is stored).
func sanitizeTurn(t Turn) Turn {
	stored := t
	stored.Err = nil
	for j := range stored.Calls {
		stored.Calls[j].Err = nil
	}
	return stored
}

// appendMessage appends one Turn as a message entry under the current
// leaf, using a content-derived id (salted to avoid collisions when an
// identical turn already sits under the same parent).
func (s *SessionTree) appendMessage(t Turn) string {
	parent := s.leaf
	// Prefer the turn's stable id as the entry id. Fall back to a random id
	// only when the turn has none (legacy migration) or it would collide.
	id := t.ID
	if id == "" {
		id = s.newRandomID()
	} else if _, exists := s.entries[id]; exists {
		id = s.newRandomID()
	}
	stored := sanitizeTurn(t)
	stored.ID = id // keep Message.ID == entry.ID so Materialize round-trips
	e := &Entry{Kind: KindMessage, ID: id, ParentID: parent, Ts: time.Now().UTC(), Message: &stored}
	s.add(e)
	return id
}

// isSyntheticTurn reports whether a Turn was injected by Materialize for a
// compaction or branch summary, so SyncFromHistory skips it.
func isSyntheticTurn(t Turn) bool {
	if t.Synthetic {
		return true
	}
	// Legacy fallback for turns that predate the Synthetic flag (older
	// in-memory histories / migrated .json), detected by the sentinel text
	// the synthetic turns carry.
	if t.User == "(earlier conversation compacted)" {
		return true
	}
	if t.Local && t.User == "(summary of a previous branch)" {
		return true
	}
	return false
}

// turnsEqual compares two turns by the same content turnID hashes over,
// so a turn matches its stored entry regardless of timestamps / Err
// interface restoration.
func turnsEqual(a, b Turn) bool {
	return turnID("", a, 0) == turnID("", b, 0)
}

// turnStoredEqual reports whether the stored entry already reflects the live
// turn for persistence. It extends content equality (turnsEqual) with the
// two fields turnID intentionally ignores but that still must be saved on a
// re-sync: the error text and the in-flight->finished transition. Without
// this, a turn that finishes with an ERROR but no content change (e.g. a
// stream that fails before emitting any text) would be judged equal, so its
// error + Finished timestamp would never persist and on reload it would
// render as a successful, perpetually in-flight turn.
func turnStoredEqual(stored, live Turn) bool {
	if !turnsEqual(stored, live) {
		return false
	}
	if stored.ErrText != live.ErrText {
		return false
	}
	return stored.Finished.IsZero() == live.Finished.IsZero()
}

// ----- Fork / Clone -----

// Fork moves the leaf to the PARENT of the message entry with id
// fromTurnID, so the next user input starts a NEW branch from before that
// turn. Returns false if fromTurnID is not a known entry.
//
// This is the /fork primitive: pick a prior user message, fork from that
// point. The selected turn and everything after it on the old branch are
// preserved as a sibling branch.
func (s *SessionTree) Fork(fromTurnID string) bool {
	e := s.entries[fromTurnID]
	if e == nil {
		return false
	}
	s.leaf = e.ParentID
	return true
}

// Clone duplicates the active branch (root..leaf) into a fresh tree with a
// new id. Entry ids are preserved so the cloned History materializes
// identically; the new tree's leaf is the cloned leaf. The new id is left
// for the caller to assign if newID is empty.
func (s *SessionTree) Clone(newID string) *SessionTree {
	clone := NewSessionTree(newID, s.Cwd, s.System)
	path := s.activePath()
	for _, e := range path {
		cp := *e
		if cp.Message != nil {
			m := *cp.Message
			cp.Message = &m
		}
		clone.entries[cp.ID] = &cp
		clone.order = append(clone.order, cp.ID)
	}
	clone.leaf = s.leaf
	return clone
}

// ----- ToProviderMessages -----

// ToProviderMessages builds the provider message list for the active
// branch, honoring the most recent compaction on the path: it starts from
// that compaction's firstKeptID with the summary prepended, dropping the
// summarized prefix. Branch summaries become user-context messages. Model
// and thinking-level change entries are settings, not messages.
//
// This is the tree-native equivalent of History.ToProviderMessages and
// produces an identical list when no compaction/branch-summary entries are
// present, so the agent loop is unaffected on the simple path.
func (s *SessionTree) ToProviderMessages() []provider.Message {
	path := s.activePath()

	// Governing compaction = last on the path.
	lastCompactionIdx := -1
	for i, e := range path {
		if e.Kind == KindCompaction {
			lastCompactionIdx = i
		}
	}

	out := []provider.Message{{Role: provider.RoleSystem, Content: s.System}}

	startIdx := 0
	if lastCompactionIdx >= 0 {
		comp := path[lastCompactionIdx]
		// Prepend the summary as a synthetic assistant turn's content.
		out = append(out,
			provider.Message{Role: provider.RoleUser, Content: "(earlier conversation compacted)"},
			provider.Message{Role: provider.RoleAssistant, Content: "Summary of earlier conversation:\n\n" + comp.Summary},
		)
		// Begin emitting messages from firstKeptID (or just after the
		// compaction entry when unset/unfound).
		startIdx = lastCompactionIdx + 1
		if comp.FirstKeptID != "" {
			for i, e := range path {
				if e.ID == comp.FirstKeptID {
					startIdx = i
					break
				}
			}
		}
	}

	for i := startIdx; i < len(path); i++ {
		e := path[i]
		switch e.Kind {
		case KindMessage:
			if e.Message == nil || e.Message.Local {
				continue
			}
			appendTurnMessages(&out, *e.Message)
		case KindBranchSummary:
			out = append(out, provider.Message{
				Role:    provider.RoleUser,
				Content: "Context from a previous branch:\n\n" + e.Summary,
			})
		case KindCompaction:
			// A non-governing compaction on the path: skip (its summarized
			// span is already represented by the governing one).
		case KindModelChange, KindThinkingChange, KindSessionInfo:
			// Settings / metadata: not part of the message stream.
		}
	}
	return out
}

// appendTurnMessages emits the provider messages for one non-local turn:
// the user message, an assistant message with finished tool calls, and one
// tool message per finished call. Mirrors History.ToProviderMessages so
// the wire shape is byte-identical for the simple (no-compaction) path.
func appendTurnMessages(out *[]provider.Message, t Turn) {
	*out = append(*out, provider.Message{Role: provider.RoleUser, Content: t.User})

	var finishedCalls []provider.ToolCall
	for _, c := range t.Calls {
		if c.Finished.IsZero() {
			continue
		}
		finishedCalls = append(finishedCalls, provider.ToolCall{
			ID:   c.ID,
			Type: "function",
			Function: provider.ToolCallFn{
				Name:      c.Name,
				Arguments: c.Args,
			},
		})
	}
	if len(finishedCalls) > 0 || t.Assistant != "" {
		*out = append(*out, provider.Message{
			Role:      provider.RoleAssistant,
			Content:   t.Assistant,
			ToolCalls: finishedCalls,
		})
	}
	for _, c := range t.Calls {
		if c.Finished.IsZero() {
			continue
		}
		body := c.Output
		if c.Err != nil {
			body = "ERROR: " + c.Err.Error() + "\n" + body
		} else if c.ErrText != "" {
			body = "ERROR: " + c.ErrText + "\n" + body
		}
		*out = append(*out, provider.Message{
			Role:       provider.RoleTool,
			ToolCallID: c.ID,
			Name:       c.Name,
			Content:    body,
		})
	}
}

// ----- JSONL serialization -----

// MarshalJSONL serializes the tree to JSONL bytes: a header line, one line
// per entry (in insertion order), then a trailing leaf line. The result is
// what store.SaveSessionTree persists.
func (s *SessionTree) MarshalJSONL() ([]byte, error) {
	var buf []byte
	enc := func(v any) error {
		b, err := json.Marshal(v)
		if err != nil {
			return err
		}
		buf = append(buf, b...)
		buf = append(buf, '\n')
		return nil
	}

	hdr := Header{
		Type:    "session",
		Version: SessionVersion,
		ID:      s.ID,
		Created: s.Created,
		Cwd:     s.Cwd,
		Name:    s.Name(),
	}
	if err := enc(hdr); err != nil {
		return nil, err
	}
	for _, id := range s.order {
		if err := enc(s.entries[id]); err != nil {
			return nil, err
		}
	}
	if err := enc(leafLine{Type: "leaf", ID: s.leaf}); err != nil {
		return nil, err
	}
	return buf, nil
}

// UnmarshalJSONL parses JSONL bytes (header + entries + optional trailing
// leaf lines, last-one-wins) into a SessionTree. Unknown entry types are
// preserved in insertion order but otherwise ignored by the typed
// accessors, so forward-compatible files don't lose data on rewrite.
//
// system is overlaid onto the result (the system prompt is rebuilt from
// current sources on load, never trusted from disk).
func UnmarshalJSONL(data []byte, system string) (*SessionTree, error) {
	s := &SessionTree{entries: map[string]*Entry{}, System: system}
	lines := splitJSONLines(data)
	for i, line := range lines {
		if len(line) == 0 {
			continue
		}
		// Peek at the type field.
		var probe struct {
			Type string `json:"type"`
		}
		if err := json.Unmarshal(line, &probe); err != nil {
			return nil, fmt.Errorf("session line %d: %w", i+1, err)
		}
		switch probe.Type {
		case "session":
			var hdr Header
			if err := json.Unmarshal(line, &hdr); err != nil {
				return nil, fmt.Errorf("session header: %w", err)
			}
			s.ID = hdr.ID
			s.Created = hdr.Created
			s.Cwd = hdr.Cwd
		case "leaf":
			var lf leafLine
			if err := json.Unmarshal(line, &lf); err != nil {
				return nil, fmt.Errorf("session leaf: %w", err)
			}
			s.leaf = lf.ID // last-one-wins
		case "":
			// No type: skip (blank/garbage line).
		default:
			var e Entry
			if err := json.Unmarshal(line, &e); err != nil {
				return nil, fmt.Errorf("session entry line %d: %w", i+1, err)
			}
			if e.ID == "" {
				continue
			}
			if _, dup := s.entries[e.ID]; dup {
				continue
			}
			s.entries[e.ID] = &e
			s.order = append(s.order, e.ID)
		}
	}
	// If the leaf points nowhere valid, default to the last entry so a
	// file without a leaf line (or a stale one) still materializes the
	// full linear chain.
	if s.leaf != "" {
		if _, ok := s.entries[s.leaf]; !ok {
			s.leaf = lastEntryID(s)
		}
	} else if len(s.order) > 0 {
		s.leaf = lastEntryID(s)
	}
	if s.Created.IsZero() {
		s.Created = time.Now().UTC()
	}
	return s, nil
}

// lastEntryID returns the id of the most recently added entry, or "".
func lastEntryID(s *SessionTree) string {
	if len(s.order) == 0 {
		return ""
	}
	return s.order[len(s.order)-1]
}

// splitJSONLines splits data on '\n' and trims trailing '\r', returning
// non-trimmed JSON byte slices (empty lines preserved as nil so indices
// stay meaningful for error messages).
func splitJSONLines(data []byte) [][]byte {
	var out [][]byte
	start := 0
	for i := 0; i <= len(data); i++ {
		if i == len(data) || data[i] == '\n' {
			line := data[start:i]
			// Trim a trailing CR.
			if n := len(line); n > 0 && line[n-1] == '\r' {
				line = line[:n-1]
			}
			out = append(out, trimSpaceBytes(line))
			start = i + 1
		}
	}
	return out
}

// trimSpaceBytes trims ASCII whitespace from both ends of b without
// allocating when nothing changes.
func trimSpaceBytes(b []byte) []byte {
	lo, hi := 0, len(b)
	for lo < hi && isASCIISpace(b[lo]) {
		lo++
	}
	for hi > lo && isASCIISpace(b[hi-1]) {
		hi--
	}
	return b[lo:hi]
}

func isASCIISpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'
}

// ----- legacy migration -----

// FromHistory builds a fresh single-branch tree from a flat History. This
// is the migration path for legacy sessions/<id>.json files: the linear
// Turns become a chain of message entries, the name (if set) becomes a
// session_info entry, and the leaf ends at the last turn.
//
// Synthetic compaction/branch-summary turns in a legacy History are stored
// as ordinary message entries (legacy files never carried separate entry
// kinds), which preserves their visible text on reload.
func FromHistory(id, cwd string, h *History) *SessionTree {
	s := NewSessionTree(id, cwd, h.System)
	for _, t := range h.Turns {
		s.appendMessage(t)
	}
	if h.Name != "" {
		s.AppendSessionInfo(h.Name)
	}
	return s
}
