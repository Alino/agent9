// pi9 — a plan9-native LLM coding agent.
//
// Phase 6: polish.
//   - Render fix: each row in View is padded/truncated to exact
//     width, each section to exact height. Kills the "stale input
//     box frames stack" bug from Phase 2-5.
//   - Slash commands: /help, /clear, /new, /save, /sessions,
//     /memory, /skill, /model, /quit. Run locally; not sent to LLM.
//   - Input box truncates display when typed text exceeds the box
//     width, with a window centered on the cursor.
//
// Env / flags / on-disk layout unchanged from Phase 4 — see
// pi9-phase4.md.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"runtime"
	"strings"
	"time"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"
	"github.com/muesli/termenv"

	"github.com/alino/plan9-winxp/pi9/internal/chat"
	"github.com/alino/plan9-winxp/pi9/internal/provider"
	"github.com/alino/plan9-winxp/pi9/internal/store"
	"github.com/alino/plan9-winxp/pi9/internal/tools"
)

const (
	baseSystemPrompt = `You are pi9, a tiny LLM agent running natively on Plan 9 (9front).
The user runs you inside a vts terminal session inside rio.
Be concise; this is a TUI with limited screen space.

Tools (portable):
  read(path,offset?,limit?) read a file (offset/limit are 1-indexed lines)
  write(path,content)     write a whole file (overwrites)
  edit(path,edits[])      replace exact text regions; each oldText must be unique
  run_rc(command)         run a shell command (rc on plan9, sh elsewhere)
  grep(pattern,...)       search file contents (regex or literal)
  find(pattern,...)       find files by glob ('**' recurses)
  ls(path?)               list a directory
  remember(content)       save a durable fact to long-term memory
  read_skill(name)        load detailed instructions for a named skill

Tools (Plan 9-native - use these when relevant, no other agent can):
  plumb(port,content)     route text via plumber (port: edit/web/image/...)
  hget(url)               fetch URL via plan9's native HTTP client
  walk(path,depth)        recursive directory listing
  ns(filter)              dump current namespace (per-process!)
  bind(src,dst,flag)      bind path into your own namespace
  mount(srv,mountpoint)   mount /srv/X file into your namespace

When the user asks you to do something concrete, just do it: call
tools, observe results, report back briefly. When the user just
chats, chat back.

On Plan 9 the shell is rc - use rc syntax, not bash. e.g. ` + "`" + `for(f in *)` + "`" + ` not ` + "`" + `for f in *` + "`" + `, no $() or backticks for command substitution.`

	defaultModel = "moonshotai/kimi-k2.5"
	maxTurnLoops = 10
)

// thinkingLevels is the cycle order for Shift+Tab. "off" disables
// extended thinking / reasoning effort; the rest map to per-provider
// wire values inside the provider package (see provider.Config.
// ThinkingLevel). Cycling wraps back to "off" after "xhigh".
var thinkingLevels = []string{"off", "minimal", "low", "medium", "high", "xhigh"}

// nextThinkingLevel returns the level after cur in thinkingLevels,
// wrapping around. An unknown cur (including "") is treated as "off",
// so the first cycle lands on "minimal".
func nextThinkingLevel(cur string) string {
	for i, l := range thinkingLevels {
		if l == cur {
			return thinkingLevels[(i+1)%len(thinkingLevels)]
		}
	}
	return thinkingLevels[1] // off -> minimal
}

// isThinkingLevel reports whether s is one of the recognized thinking
// levels (off/minimal/low/medium/high/xhigh). Used to detect a trailing
// ":LEVEL" shorthand on --model (e.g. "sonnet:high").
func isThinkingLevel(s string) bool {
	for _, l := range thinkingLevels {
		if l == s {
			return true
		}
	}
	return false
}

// ---------- Bubble Tea model ----------

type pi9Model struct {
	width, height int
	inVts         string

	history chat.History

	// tree is the tree-structured session that owns ALL branches. The
	// active branch is mirrored into history (which rendering + the agent
	// loop use UNCHANGED). After each turn saveSession() folds history
	// back into the tree and persists it as JSONL. Branch navigation
	// (/fork, /clone, /tree) mutates the tree's leaf and re-materializes
	// history. Never nil after main() builds the model.
	tree *chat.SessionTree

	input        []rune
	inputCursor  int
	streaming    bool
	streamCancel context.CancelFunc

	// Task 2.2 steering + follow-up queues (pi parity). While a run is in
	// flight the user can keep typing: Enter queues a STEERING message
	// (delivered at the safe point between tool rounds, mid-run) and
	// Alt+Enter queues a FOLLOW-UP message (delivered only once the whole
	// run finishes). Alt+Up dequeues the most recent pending message back
	// into the editor. The queues are ephemeral — nothing extra persists.
	//
	// steeringMode / followUpMode mirror pi's settings: drainOneAtATime
	// (default) delivers one queued message per drain point; drainAll
	// delivers everything queued at once.
	steerQueue    msgQueue
	followUpQueue msgQueue
	steeringMode  drainMode
	followUpMode  drainMode

	statusMsg string

	apiKey    string
	model     string
	tools     []provider.Tool
	sessionID string

	// thinkingLevel is the current extended-thinking / reasoning level
	// ("off", "minimal", "low", "medium", "high", "xhigh"). Cycled by
	// Shift+Tab and passed into provider.Config.ThinkingLevel per request.
	// hideThinking, toggled by Ctrl+T, hides streamed reasoning text in
	// the assistant turn without changing what we request from the model.
	thinkingLevel string
	hideThinking  bool

	scrollOffset int

	// Phase 10 login picker state. inputMode flips between normal
	// chat input and provider/key picker overlays for /login.
	loginMode   loginMode
	loginCursor int                 // which provider is highlighted
	loginPicked provider.ProviderID // chosen provider; we're now in key-entry

	// Model picker state (Phase 12). Activated by /model with no
	// args. modelList is the merged curated+live list; modelQuery
	// is the fuzzy search input; modelCursor is which row is
	// highlighted (within the filtered subset).
	modelPickerOpen bool
	modelList       []provider.ModelInfo
	modelQuery      []rune
	modelQueryCur   int
	modelCursor     int

	// Token / context tracking. lastTotalTokens is the provider-
	// reported total from the most recent terminal chunk (0 until the
	// provider reports usage). When it's 0 we fall back to a byte-based
	// estimate of the whole history for the "NN% ctx" footer indicator.
	lastTotalTokens int

	// Compaction settings + state. autoCompact toggles auto-compaction
	// (default on); reserveTokens/keepRecentTokens mirror pi's defaults.
	// compacting is set while a summarization request is in flight so we
	// don't fire two at once.
	autoCompact      bool
	reserveTokens    int
	keepRecentTokens int
	compacting       bool

	// trusted caches whether the project cwd is trusted, so the /trust
	// hint and prompt-template/project-resource gating don't re-stat on
	// every keystroke.
	trusted bool

	// noSession is set by --no-session: the run is ephemeral, so
	// saveSession() and the startup/exit persistence are skipped. State
	// still lives in memory for the duration of the run.
	noSession bool

	// Task 3.3 flag-driven startup actions, consumed once on startupMsg.
	//   startupResume — -r/--resume: open the /resume picker immediately.
	//   startupPrompt — the raw initial @file/positional prompt to
	//                   auto-submit (submitInput @file-expands it); empty
	//                   when none was given.
	startupResume bool
	startupPrompt string

	// Settings overlay state (/settings). When open it replaces the
	// scrollback like the model picker. settingsCursor selects a row.
	settingsOpen   bool
	settingsCursor int

	// Task 2.1 tree-session overlays. Each replaces the scrollback like
	// the model picker and has its own key handler. Only one is open at a
	// time.
	//
	//   resumeOpen  — /resume: pick a session to switch to.
	//   forkOpen    — /fork:   pick a prior user message to fork from.
	//   treeOpen    — /tree:   navigate the branch tree, set the leaf.
	resumeOpen   bool
	resumeList   []sessionRow
	resumeCursor int

	forkOpen   bool
	forkList   []forkRow
	forkCursor int

	treeOpen   bool
	treeList   []treeRow
	treeCursor int

	// Task 2.4 scoped models. enabledModels is the ordered set Ctrl+P /
	// Shift+Ctrl+P cycle the active model through (seeded by --models,
	// edited via /scoped-models). The scopedModels* fields back the
	// /scoped-models overlay (search box + cursor), reusing the model
	// picker overlay pattern.
	enabledModels    []string
	scopedModelsOpen bool
	scopedQuery      []rune
	scopedQueryCur   int
	scopedCursor     int

	// Task 2.3 autocomplete dropdown. Non-intrusive completion shown only
	// while a "/" or "@" token is being typed. completeKind selects which
	// list (slash commands / files); completeList is the current
	// candidates; completeStart is the rune offset where the active token
	// begins; completeCursor is the highlighted row.
	completeKind   completionKind
	completeList   []completion
	completeStart  int
	completeCursor int
}

// loginMode controls what the input area does. In picker modes the
// chat input is replaced by a list/prompt UI overlay.
type loginMode int

const (
	loginModeOff          loginMode = iota // normal chat input
	loginModePicker                        // showing provider list
	loginModeAuthMethod                    // showing "subscription / api key" choice
	loginModeKeyEntry                      // showing "enter key for X:" prompt
	loginModeOAuthRunning                  // OAuth in progress; status bar shows browser URL
)

// Init fires once when the program starts. We use it to dispatch the
// startup actions requested by CLI flags: -r/--resume opens the /resume
// picker, and an initial @file/positional prompt is auto-submitted. Both
// are delivered via a startupMsg so they run inside the Update loop with a
// fully-sized model (after the first WindowSizeMsg would be ideal, but the
// picker and submit paths don't need the size, and submit re-renders).
func (m pi9Model) Init() tea.Cmd {
	if !m.startupResume && m.startupPrompt == "" {
		return nil
	}
	return func() tea.Msg { return startupMsg{} }
}

// startupMsg triggers the flag-driven startup actions (see Init).
type startupMsg struct{}

// ---------- streaming plumbing ----------

type chunkMsg struct{ delta string }

// reasoningMsg carries a streamed thinking/reasoning delta. Rendered as
// dimmed "thinking" text in the assistant turn unless hideThinking is on.
type reasoningMsg struct{ delta string }

// Phase 10 S2 OAuth messages. Sent from the OAuth login goroutine
// back to the bubbletea Update loop so the picker UI can react.
//
//	oauthURLMsg     - URL is built + callback server is up; show URL in status
//	oauthDoneMsg    - login completed (success or error). UI returns to chat
//	                  and shows result as a local turn.
type oauthURLMsg struct{ url string }

// modelsLoadedMsg arrives when the async live-model fetch finishes.
// merged is the combined curated+live list, sorted with recent first.
// err is the fetch error (we still display the curated list on error).
type modelsLoadedMsg struct {
	merged []provider.ModelInfo
	err    error
}

// fetchModelsCmd returns a tea.Cmd that fetches the live OpenRouter
// model list and merges it with the curated set. currentID is used
// to prioritize the user's current model in the merged output.
//
// Also fetches live GitHub Copilot models if the user has a Copilot
// OAuth token saved — that way the picker shows the actual model
// IDs available to their account (which can differ between Free /
// Pro / Business / Enterprise tiers).
func fetchModelsCmd(currentID string) tea.Cmd {
	return func() tea.Msg {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		// If user is logged into Copilot, pull their live model list too.
		var copilotToken, copilotURL string
		if entry, ok := store.LookupAuthEntry("copilot"); ok {
			copilotToken = entry.AccessToken
			// The Copilot OAuth flow stores the proxy-ep URL in the
			// AccountID field (because we don't have a separate slot
			// for it in AuthEntry yet).
			copilotURL = entry.AccountID
		}

		merged := provider.MergedModels(ctx, []string{currentID}, copilotToken, copilotURL)
		return modelsLoadedMsg{merged: merged}
	}
}

type oauthDoneMsg struct {
	provider provider.ProviderID
	err      error
}

type streamDoneMsg struct {
	err       error
	toolCalls []provider.ToolCall
	usage     *provider.Usage // provider-reported token accounting, if any
}

type toolResultMsg struct {
	idx    int
	output string
	err    error
}

type loopContinueMsg struct{}

var teaSendFn func(tea.Msg)

// oauthManualCh is the channel for plumbing user-pasted authorization
// codes from the main UI loop into a running OAuth goroutine. Set
// (along with oauthCancel) by runOAuthLogin and consumed by the
// OAuth provider's Login function.
//
// Lifecycle: created when OAuth starts, closed when OAuth ends.
// During loginModeOAuthRunning, pressing enter on the input box
// sends m.input as text into this channel. If the user pastes a
// valid code/URL, OAuth completes via the manual path.
var (
	oauthManualCh chan string
)

// runOAuthLogin returns a tea.Cmd that runs the OAuth flow for one
// provider in a goroutine. It posts oauthURLMsg when the auth URL
// is built (so the UI can show it / open the browser), then
// oauthDoneMsg when complete (success or error).
//
// Implementation: bubbletea's `tea.Cmd` is a function returning one
// tea.Msg. Since OAuth has TWO milestones (URL ready, login done),
// we use teaSendFn to push the URL message asynchronously from
// inside the callback, and return the final done message from the
// Cmd. This pattern matches how runStream emits chunkMsg + final
// streamDoneMsg.
//
// Pi.dev parity: the goroutine also accepts user-pasted codes via
// oauthManualCh, racing them against the browser callback. This
// matters on plan9 where the browser is typically on the host and
// the callback may not reach the VM.
func runOAuthLogin(providerID provider.ProviderID) tea.Cmd {
	return func() tea.Msg {
		op := provider.GetOAuth(providerID)
		if op == nil {
			return oauthDoneMsg{
				provider: providerID,
				err:      fmt.Errorf("provider %q has no OAuth support in this build", providerID),
			}
		}

		// 5-minute outer deadline. The login flow has its own
		// callback-wait deadline inside (also 5min), so this is
		// belt-and-suspenders.
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
		defer cancel()

		// Provision the manual-input channel for this OAuth run.
		// Closed in the deferred cleanup so any leftover paste
		// senders don't block.
		manualCh := make(chan string, 1)
		oauthManualCh = manualCh
		defer func() {
			oauthManualCh = nil
			close(manualCh)
		}()

		creds, err := op.Login(ctx, provider.OAuthCallbacks{
			OnAuthURL: func(authURL string) {
				teaSendFn(oauthURLMsg{url: authURL})
			},
			OnProgress: func(msg string) {
				// Reuse status bar by piping through oauthURLMsg
				// with a slight abuse — the status bar shows
				// whatever the latest message is. Cleaner would be
				// a separate oauthProgressMsg; the URL one is
				// fine for now since progress and URL are
				// mutually exclusive in time.
				teaSendFn(oauthURLMsg{url: msg})
			},
			ManualCode: manualCh,
		})
		if err != nil {
			return oauthDoneMsg{provider: providerID, err: err}
		}

		// Persist credentials.
		if err := store.SetOAuth(string(providerID), creds.Access, creds.Refresh, creds.ExpiresAt, creds.AccountID); err != nil {
			return oauthDoneMsg{provider: providerID, err: fmt.Errorf("save oauth creds: %w", err)}
		}

		return oauthDoneMsg{provider: providerID}
	}
}

// resolveProvider performs the shared provider-selection + credential
// resolution used by both runStream and streamOnce. It returns the
// chosen Provider implementation and the per-request Config (with key,
// model, tools, and any Codex-Responses smuggling already applied), or
// an error when no credentials / no implementation exist.
func resolveProvider(ctx context.Context, apiKey, model, thinkingLevel string, toolList []provider.Tool) (provider.Provider, provider.Config, error) {
	// Phase 10: pick the provider for this model. If the user has a
	// key for it in auth.json, use that — otherwise fall back to the
	// apiKey passed in (legacy config.api_key, set at startup from the
	// OpenRouter slot).
	providerID := provider.ProviderForModel(model)

	// Phase 10 S2: if this provider has an OAuth entry and the token
	// is expired (or near-expired), refresh BEFORE making the request.
	key := refreshIfNeeded(ctx, providerID)
	if key == "" {
		key = store.LookupAPIKey(string(providerID))
	}
	if key == "" {
		key = apiKey // legacy fallback
	}
	if key == "" {
		return nil, provider.Config{}, fmt.Errorf("no credentials for %q. run /login to add", providerID)
	}

	// Phase 10 S4: OAuth-authed OpenAI (ChatGPT Plus/Pro) dispatches to
	// the Codex Responses API; the account id is smuggled through
	// Config.APIURL with a "codex:" prefix.
	var impl provider.Provider
	cfg := provider.Config{
		APIKey:        key,
		Model:         model,
		MaxTokens:     4096,
		Tools:         toolList,
		ThinkingLevel: thinkingLevel,
	}
	if providerID == provider.ProviderOpenAI {
		if entry, ok := store.LookupAuthEntry(string(provider.ProviderOpenAI)); ok && entry.Type == "oauth" {
			impl = provider.GetCodexResponses()
			cfg.APIURL = "codex:" + entry.AccountID
		}
	}
	if impl == nil {
		impl = provider.Get(providerID)
	}
	if impl == nil {
		return nil, provider.Config{}, fmt.Errorf("provider %q is not implemented in this build", providerID)
	}
	return impl, cfg, nil
}

// streamOnce runs a one-shot, tool-less streaming request and returns
// the fully-assembled assistant text (reasoning is ignored). Used by
// summarizeCmd for compaction. Blocks until the stream completes or ctx
// is cancelled.
func streamOnce(ctx context.Context, apiKey, model, thinkingLevel string, msgs []provider.Message) (string, error) {
	impl, cfg, err := resolveProvider(ctx, apiKey, model, thinkingLevel, nil)
	if err != nil {
		return "", err
	}
	chunks, errs := impl.Stream(ctx, cfg, msgs)
	var b strings.Builder
	for {
		select {
		case <-ctx.Done():
			return b.String(), ctx.Err()
		case c, ok := <-chunks:
			if !ok {
				select {
				case e := <-errs:
					return b.String(), e
				default:
					return b.String(), nil
				}
			}
			if c.Delta != "" {
				b.WriteString(c.Delta)
			}
		case e := <-errs:
			if e != nil {
				return b.String(), e
			}
		}
	}
}

func runStream(ctx context.Context, apiKey, model, thinkingLevel string, toolList []provider.Tool, msgs []provider.Message) tea.Cmd {
	return func() tea.Msg {
		impl, cfg, err := resolveProvider(ctx, apiKey, model, thinkingLevel, toolList)
		if err != nil {
			return streamDoneMsg{err: err}
		}

		chunks, errs := impl.Stream(ctx, cfg, msgs)
		var toolCalls []provider.ToolCall
		var usage *provider.Usage
		for {
			select {
			case <-ctx.Done():
				return streamDoneMsg{err: ctx.Err(), usage: usage}
			case c, ok := <-chunks:
				if !ok {
					select {
					case e := <-errs:
						return streamDoneMsg{err: e, toolCalls: toolCalls, usage: usage}
					default:
						return streamDoneMsg{toolCalls: toolCalls, usage: usage}
					}
				}
				if c.Reasoning != "" {
					teaSendFn(reasoningMsg{delta: c.Reasoning})
				}
				if c.Delta != "" {
					teaSendFn(chunkMsg{delta: c.Delta})
				}
				if c.Done {
					toolCalls = c.ToolCalls
					if c.Usage != nil {
						usage = c.Usage
					}
				}
			case e := <-errs:
				if e != nil {
					return streamDoneMsg{err: e, toolCalls: toolCalls, usage: usage}
				}
			}
		}
	}
}

// refreshIfNeeded checks if the provider has OAuth credentials, and
// if those credentials are expired, refreshes them via the provider's
// OAuth implementation. Returns the (refreshed) access token, or ""
// if no OAuth entry exists OR if refresh failed (caller falls back
// to LookupAPIKey).
//
// Synchronous: blocks the caller until the refresh completes. This
// is intentional — the alternative (background goroutine) would
// require complex state management and we'd risk sending stale
// tokens. The refresh is fast (single HTTPS POST) so 1-2s of latency
// on the first message after expiry is acceptable.
func refreshIfNeeded(ctx context.Context, providerID provider.ProviderID) string {
	entry, ok := store.LookupAuthEntry(string(providerID))
	if !ok || entry.Type != "oauth" {
		return ""
	}

	creds := provider.OAuthCredentials{
		Access:    entry.AccessToken,
		Refresh:   entry.RefreshToken,
		ExpiresAt: entry.ExpiresAt,
		AccountID: entry.AccountID,
	}
	if !creds.IsExpired(5 * time.Minute) {
		return entry.AccessToken
	}

	// Token expired or near-expired; refresh.
	op := provider.GetOAuth(providerID)
	if op == nil {
		return entry.AccessToken // can't refresh; try anyway, might still work
	}
	refreshCtx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	newCreds, err := op.Refresh(refreshCtx, entry.RefreshToken)
	if err != nil {
		// Refresh failed (network, expired refresh token, etc).
		// Return "" so caller surfaces "no credentials" — user runs
		// /login to re-auth.
		return ""
	}
	_ = store.SetOAuth(string(providerID), newCreds.Access, newCreds.Refresh, newCreds.ExpiresAt, newCreds.AccountID)
	return newCreds.Access
}

func runTool(idx int, name, args string) tea.Cmd {
	return func() tea.Msg {
		out, err := tools.Run(name, args)
		return toolResultMsg{idx: idx, output: out, err: err}
	}
}

// ---------- session persistence ----------

// saveSession folds the active branch (m.history) back into the session
// tree and persists it as JSONL. Best-effort: errors are swallowed because
// we don't want autosave failures crashing the agent loop. The user sees
// them via the status bar instead.
//
// SyncFromHistory is idempotent, so calling saveSession repeatedly between
// turns (as the existing handlers do) adds nothing the second time.
func (m *pi9Model) saveSession() {
	// --no-session: ephemeral run. Keep all state in memory but never
	// touch the session store.
	if m.noSession {
		return
	}
	if m.sessionID == "" {
		return
	}
	if m.tree == nil {
		// Defensive: should never happen post-main, but never panic the
		// agent loop. Fall back to the legacy snapshot save.
		_ = store.SaveAny(m.sessionID, m.history)
		return
	}
	m.tree.ID = m.sessionID
	m.tree.SyncFromHistory(&m.history)
	jsonl, err := m.tree.MarshalJSONL()
	if err != nil {
		m.statusMsg = "save failed: " + err.Error()
		return
	}
	if err := store.SaveSessionTree(m.sessionID, jsonl); err != nil {
		m.statusMsg = "save failed: " + err.Error()
		return
	}
}

// loadSessionTree loads the session tree for id, preferring the new JSONL
// tree format and migrating a legacy chat.History JSON snapshot when only
// that exists. Returns nil when nothing loadable is on disk (the caller
// then starts a fresh tree). system is pinned onto the result.
func loadSessionTree(id, cwd, system string) *chat.SessionTree {
	// Prefer the tree format.
	if data, err := store.LoadSessionTree(id); err == nil {
		if tree, err := chat.UnmarshalJSONL(data, system); err == nil {
			return tree
		} else {
			fmt.Fprintf(os.Stderr, "pi9: parse session tree %s: %v\n", id, err)
		}
	} else if !os.IsNotExist(err) {
		fmt.Fprintf(os.Stderr, "pi9: load session tree %s: %v\n", id, err)
	}
	// Legacy migration: a chat.History JSON snapshot becomes a single
	// linear branch.
	if data, err := store.LoadSession(id); err == nil {
		var loaded chat.History
		if err := json.Unmarshal(data, &loaded); err == nil {
			loaded.RestoreErrs()
			return chat.FromHistory(id, cwd, &loaded)
		}
	} else if !os.IsNotExist(err) {
		fmt.Fprintf(os.Stderr, "pi9: load session %s: %v\n", id, err)
	}
	return nil
}

// recordCompaction folds the (pre-compaction) history into the tree and
// appends a compaction entry whose firstKeptEntryId points at the first
// kept turn's entry, so the branch reloads as summary + kept tail and the
// tree's ToProviderMessages honors the compaction. cut is the index of the
// first kept turn in m.history (turns[:cut] are summarized).
//
// Must be called BEFORE applyCompaction mutates m.history.
func (m *pi9Model) recordCompaction(cut int, summary string, tokensBefore int) {
	if m.tree == nil {
		return
	}
	// Sync the full pre-compaction history so every turn is an entry and
	// the leaf sits on the last turn.
	m.tree.SyncFromHistory(&m.history)
	// firstKept = the stable id of the first KEPT turn. Use the turn's own ID
	// (post-sync every turn has one) rather than indexing MessageEntryIDs by
	// position, which can misalign if the active path holds non-1:1 entries.
	firstKept := ""
	if cut >= 0 && cut < len(m.history.Turns) {
		firstKept = m.history.Turns[cut].ID
	}
	if firstKept == "" { // defensive fallback to positional mapping
		ids := m.tree.MessageEntryIDs()
		if cut >= 0 && cut < len(ids) {
			firstKept = ids[cut]
		}
	}
	// Persist the SAME body the live applyCompaction shows (summary + the
	// <read-files>/<modified-files> footer), so a reloaded compacted session
	// keeps the file-ops context that was sent to the model live.
	body := summary
	if cut >= 0 && cut <= len(m.history.Turns) {
		readFiles, modifiedFiles := extractFileOps(m.history.Turns[:cut])
		body = strings.TrimSpace(summary) + formatFileOps(readFiles, modifiedFiles)
	}
	m.tree.AppendCompaction(body, firstKept, tokensBefore)
}

// reMaterialize rebuilds m.history from the tree's current leaf after a
// branch-navigation operation (/fork, /clone, /tree). The system prompt is
// re-pinned from the current sources, never trusted from disk.
func (m *pi9Model) reMaterialize() {
	if m.tree == nil {
		return
	}
	m.tree.System = m.history.System
	h := m.tree.Materialize()
	m.history = *h
	// Reset provider-reported usage: the active branch changed, so the
	// last total no longer reflects the context.
	m.lastTotalTokens = 0
}

// ---------- input handling ----------

func (m *pi9Model) submitInput() (tea.Model, tea.Cmd) {
	text := strings.TrimSpace(string(m.input))
	if text == "" {
		return m, nil
	}
	// While a run is in flight, Enter QUEUES a steering message rather than
	// dropping the input. It is delivered at the safe point between tool
	// rounds (see drainSteering). Slash commands can't be queued (they're
	// local UI actions), so they're ignored mid-run — the user can run
	// them once the stream finishes.
	if m.streaming {
		if strings.HasPrefix(text, "/") {
			m.statusMsg = "slash commands can't be queued - wait for the run to finish"
			return m, nil
		}
		m.steerQueue.push(text)
		m.input = m.input[:0]
		m.inputCursor = 0
		m.statusMsg = m.queueStatus()
		return m, nil
	}
	m.input = m.input[:0]
	m.inputCursor = 0
	m.clearCompletions()

	// Slash command? Handle locally without bothering the LLM.
	if strings.HasPrefix(text, "/") {
		return m.handleSlash(text)
	}

	// Task 2.3 inline shell. "!cmd" runs cmd and SENDS its output to the
	// model as context; "!!cmd" runs it but does NOT send (local-only).
	if kind, cmd := parseInlineShell(text); kind != shellNone {
		out, _ := tools.RunShellCommand(cmd)
		switch kind {
		case shellLocal:
			// Local-only: render as a Local turn, never sent to the model.
			m.history.AppendLocal(text, out)
			m.saveSession()
			return m, nil
		case shellSend:
			// Send the command + its output to the model as a user turn.
			msg := fmt.Sprintf("$ %s\n%s", cmd, out)
			m.history.AppendUser(msg)
			m.saveSession()
			return m.beginStream()
		}
	}

	// Task 2.3 @file include: expand any @path tokens by appending the
	// referenced files' contents (clearly delimited) to the outgoing
	// message.
	out := expandAtFiles(text, nil)
	m.history.AppendUser(out)
	m.saveSession()
	return m.beginStream()
}

// queueFollowUp queues the current input as a FOLLOW-UP message (Alt+Enter
// while a run is in flight): delivered only after ALL current work
// finishes. When idle it behaves like a normal submit so Alt+Enter is
// never a dead key. Slash commands can't be queued.
func (m *pi9Model) queueFollowUp() (tea.Model, tea.Cmd) {
	text := strings.TrimSpace(string(m.input))
	if text == "" {
		return m, nil
	}
	if !m.streaming {
		return m.submitInput()
	}
	if strings.HasPrefix(text, "/") {
		m.statusMsg = "slash commands can't be queued - wait for the run to finish"
		return m, nil
	}
	m.followUpQueue.push(text)
	m.input = m.input[:0]
	m.inputCursor = 0
	m.statusMsg = m.queueStatus()
	return m, nil
}

// dequeueToEditor pulls the most recently queued (not-yet-sent) message
// back into the editor for editing (Alt+Up). Follow-up messages dequeue
// before steering ones (most-recently-queued first within each), matching
// pi's "restore queued messages" affordance. Returns false when nothing
// was queued so the caller can fall back to its prior Alt+Up behavior
// (scroll up).
func (m *pi9Model) dequeueToEditor() bool {
	text, ok := m.followUpQueue.popLast()
	if !ok {
		text, ok = m.steerQueue.popLast()
	}
	if !ok {
		return false
	}
	// Prepend the restored text to whatever is currently in the editor so
	// nothing the user already typed is lost.
	cur := string(m.input)
	combined := text
	if strings.TrimSpace(cur) != "" {
		combined = text + "\n\n" + cur
	}
	m.input = []rune(combined)
	m.inputCursor = len(m.input)
	m.statusMsg = "restored queued message - " + m.queueStatus()
	return true
}

// drainSteering is called at the SAFE POINT between tool rounds (after all
// tool calls of a turn finish, before the next LLM request). If steering
// messages are queued it delivers them as a new user turn — respecting
// steeringMode — and begins the next stream from that turn. Returns false
// (and leaves the queue untouched) when nothing is queued, so the caller
// continues the loop normally.
func (m *pi9Model) drainSteering() (tea.Model, tea.Cmd, bool) {
	text, ok := m.steerQueue.drain(m.steeringMode)
	if !ok {
		return m, nil, false
	}
	// Close out the in-flight assistant turn that issued the tool calls before
	// starting the steering turn; otherwise it renders/persists as "running..."
	// forever (the loop never returns to it).
	m.history.FinishTurn(nil)
	m.history.AppendUser(text)
	m.statusMsg = "steering: " + firstLine(text)
	m.saveSession()
	model, cmd := m.beginStream()
	return model, cmd, true
}

// drainFollowUp is called once a run FULLY finishes. If follow-up messages
// are queued it delivers them as the next user turn — respecting
// followUpMode — and begins a fresh stream. Returns false when nothing is
// queued. Steering takes precedence: any steering still queued when a run
// ends is delivered first (as a steering turn) before follow-ups.
func (m *pi9Model) drainFollowUp() (tea.Model, tea.Cmd, bool) {
	if !m.steerQueue.empty() {
		return m.drainSteering()
	}
	text, ok := m.followUpQueue.drain(m.followUpMode)
	if !ok {
		return m, nil, false
	}
	m.history.AppendUser(text)
	m.statusMsg = "follow-up: " + firstLine(text)
	m.saveSession()
	model, cmd := m.beginStream()
	return model, cmd, true
}

// queueStatus renders a short indicator of how many messages are queued,
// e.g. "2 steering, 1 follow-up queued (alt+up to edit)". Returns "" when
// both queues are empty.
func (m pi9Model) queueStatus() string {
	s, f := m.steerQueue.len(), m.followUpQueue.len()
	if s == 0 && f == 0 {
		return ""
	}
	var parts []string
	if s > 0 {
		parts = append(parts, fmt.Sprintf("%d steering", s))
	}
	if f > 0 {
		parts = append(parts, fmt.Sprintf("%d follow-up", f))
	}
	return strings.Join(parts, ", ") + " queued (alt+up to edit)"
}

// firstLine returns the first line of s, trimmed and clipped to a short
// length, for compact status messages.
func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		s = s[:i]
	}
	s = strings.TrimSpace(s)
	if len(s) > 48 {
		s = s[:45] + "..." // ASCII only; vtwin lacks U+2026
	}
	return s
}

// handleSlash dispatches a /command. Returns a tea.Cmd; the command
// itself is rendered as a Local turn in the history.
//
// Convention: every slash command appends a Local turn so the user
// has a record of what they ran and what came back. Local turns are
// excluded from ToProviderMessages.
// splitArgs tokenizes a slash-command line on whitespace while honoring
// single and double quotes (matching pi's parseCommandArgs), so
// `/cmd "foo bar"` yields ["/cmd", "foo bar"]. Quote characters are
// stripped; inside double quotes a backslash escapes the next character.
func splitArgs(s string) []string {
	var args []string
	var cur strings.Builder
	inWord := false
	quote := byte(0) // 0, '\'' or '"'
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case quote != 0:
			if c == quote {
				quote = 0
			} else if quote == '"' && c == '\\' && i+1 < len(s) {
				i++
				cur.WriteByte(s[i])
			} else {
				cur.WriteByte(c)
			}
		case c == '\'' || c == '"':
			quote = c
			inWord = true
		case c == ' ' || c == '\t' || c == '\n' || c == '\r':
			if inWord {
				args = append(args, cur.String())
				cur.Reset()
				inWord = false
			}
		default:
			cur.WriteByte(c)
			inWord = true
		}
	}
	if inWord {
		args = append(args, cur.String())
	}
	return args
}

// syncAPIKeyForModel refreshes the cached m.apiKey to the credential
// stored for the CURRENT model's provider. Must be called after any
// model switch: the send-guard (and the next request) read m.apiKey,
// which otherwise only gets updated at /login time and only when the
// active model already maps to the logged-in provider. Without this,
// "/login MiniMax" while Kimi is active, then "/model MiniMax-M3",
// leaves m.apiKey stale ("" -> bogus "no API key set"), even though
// auth.json holds the minimax key. Only overwrites when a key exists
// for the new provider, preserving the legacy config-key fallback.
func (m *pi9Model) syncAPIKeyForModel() {
	if k := store.LookupAPIKey(string(provider.ProviderForModel(m.model))); k != "" {
		m.apiKey = k
	}
}

// persistModel writes the current model back to the config file so the
// last explicitly selected model becomes the default on the next launch.
// Startup reads cfg.Model; without this, /model and the picker only
// change the model in memory + the session tree, so a restart reverts to
// the configured default (surprising — the user expects their last pick
// to stick). LoadConfig+SaveConfig preserves the api_key, comments, etc.
func (m *pi9Model) persistModel() {
	cfg, err := store.LoadConfig()
	if err != nil {
		return
	}
	if cfg.Model == m.model {
		return
	}
	cfg.Model = m.model
	_ = store.SaveConfig(cfg)
}

func (m *pi9Model) handleSlash(text string) (tea.Model, tea.Cmd) {
	parts := splitArgs(text)
	if len(parts) == 0 {
		return m, nil
	}
	cmd := strings.TrimPrefix(parts[0], "/")
	args := parts[1:]

	switch cmd {
	case "help", "?":
		m.history.AppendLocal(text, slashHelp())
		m.saveSession()
		return m, nil

	case "hotkeys", "keys":
		// Task 5: dedicated key-binding reference (was aliased to /help).
		m.history.AppendLocal(text, hotkeysHelp())
		m.saveSession()
		return m, nil

	case "login":
		// Two modes:
		//   /login              — open interactive provider picker
		//   /login <key>        — legacy single-shot: paste a key,
		//                         stored under OpenRouter for backward
		//                         compat with pre-Phase-10 docs
		//
		// The picker UI is bubbletea sub-state: m.loginMode flips
		// into loginModePicker; handleKey dispatches arrows/enter/
		// escape; View() renders the picker overlay instead of the
		// normal input box.
		if len(args) == 0 {
			m.loginMode = loginModePicker
			m.loginCursor = 0
			m.statusMsg = "pick provider with arrows, enter to select, esc to cancel"
			return m, nil
		}
		// Legacy single-shot path: store under OpenRouter.
		newKey := args[0]
		if err := store.SetAPIKey(string(provider.ProviderOpenRouter), newKey); err != nil {
			m.history.AppendLocal(text, "error: "+err.Error())
			m.saveSession()
			return m, nil
		}
		// Also update legacy config api_key for backward compat with
		// any code paths that still read it directly.
		cfg, _ := store.LoadConfig()
		cfg.APIKey = newKey
		_ = store.SaveConfig(cfg)
		m.apiKey = newKey
		safe := "/login " + store.MaskedAPIKey(newKey) + " (saved as openrouter)"
		m.history.AppendLocal(safe, "saved to "+store.AuthPath()+"\n\nuse /login (no args) for interactive provider picker")
		m.statusMsg = ""
		m.saveSession()
		return m, nil

	case "logout":
		// Without args: clear the legacy single api_key + every
		// provider in auth.json. With provider name: clear just that
		// one.
		if len(args) > 0 {
			pid := strings.ToLower(args[0])
			if err := store.ClearProvider(pid); err != nil {
				m.history.AppendLocal(text, "error: "+err.Error())
				m.saveSession()
				return m, nil
			}
			m.history.AppendLocal(text, "cleared "+pid+" from "+store.AuthPath())
			m.saveSession()
			return m, nil
		}
		// Clear everything.
		auth, _ := store.LoadAuth()
		for pid := range auth {
			_ = store.ClearProvider(pid)
		}
		cfg, _ := store.LoadConfig()
		cfg.APIKey = ""
		_ = store.SaveConfig(cfg)
		m.apiKey = ""
		m.history.AppendLocal(text, "cleared all credentials from "+store.AuthPath()+"\nuse /logout <provider> to clear just one")
		m.saveSession()
		return m, nil

	case "clear":
		// Reset the active branch to empty, starting a NEW branch from
		// before the root so prior turns stay in the tree (reachable via
		// /tree). Keeps the system prompt + name.
		if m.tree != nil {
			m.tree.SetLeaf("")
		}
		name := m.history.Name
		m.history = chat.History{System: m.history.System, Name: name}
		m.history.AppendLocal(text, "conversation cleared (system prompt + memory + skills intact; prior turns kept in /tree).")
		m.statusMsg = ""
		m.saveSession()
		return m, nil

	case "new":
		// Start a fresh session, abandoning the current one. The old
		// session file stays on disk; only the `current` pointer moves.
		newID := store.NewSessionID()
		cwd, _ := os.Getwd()
		m.sessionID = newID
		m.tree = chat.NewSessionTree(newID, cwd, m.history.System)
		m.history = chat.History{System: m.history.System}
		_ = store.SetCurrentSession(newID)
		m.history.AppendLocal(text, "started new session "+newID)
		m.saveSession()
		return m, nil

	case "name":
		// Set the session's display name. Recorded as a session_info
		// entry in the tree (via saveSession -> SyncFromHistory).
		if len(args) == 0 {
			cur := m.history.Name
			if cur == "" {
				cur = "(none - using session id " + m.sessionID + ")"
			}
			m.history.AppendLocal(text, "current name: "+cur+"\nusage: /name <new name>")
		} else {
			n := strings.Join(args, " ")
			m.history.Name = n
			if m.tree != nil {
				m.tree.AppendSessionInfo(n)
			}
			m.history.AppendLocal(text, "session name: "+n)
		}
		m.saveSession()
		return m, nil

	case "save":
		m.saveSession()
		m.history.AppendLocal(text, "saved to "+store.SessionTreePath(m.sessionID))
		m.saveSession()
		return m, nil

	case "session":
		// Show metadata for the current session.
		var b strings.Builder
		fmt.Fprintf(&b, "id:       %s\n", m.sessionID)
		fmt.Fprintf(&b, "name:     %s\n", or(m.history.Name, "(unnamed)"))
		fmt.Fprintf(&b, "model:    %s\n", m.model)
		fmt.Fprintf(&b, "turns:    %d (real: %d, local: %d)\n",
			len(m.history.Turns),
			countNonLocal(m.history.Turns),
			countLocal(m.history.Turns))
		if m.tree != nil {
			fmt.Fprintf(&b, "entries:  %d in tree (leaf %s)\n", len(m.tree.Tree()), or(m.tree.Leaf(), "(root)"))
		}
		fmt.Fprintf(&b, "path:     %s\n", store.SessionTreePath(m.sessionID))
		m.history.AppendLocal(text, b.String())
		m.saveSession()
		return m, nil

	case "resume":
		// Interactive session picker overlay (newest first).
		m.openResume()
		return m, nil

	case "sessions":
		// Text listing of sessions (newest first). /resume is the
		// interactive picker.
		ids, err := store.ListSessions()
		var body string
		if err != nil {
			body = "error listing: " + err.Error()
		} else if len(ids) == 0 {
			body = "no sessions yet"
		} else {
			var b strings.Builder
			b.WriteString(fmt.Sprintf("%d session(s), newest first:\n", len(ids)))
			for i, id := range ids {
				marker := "  "
				if id == m.sessionID {
					marker = "> "
				}
				b.WriteString(marker + id)
				if i >= 9 {
					b.WriteString(fmt.Sprintf("\n  ... and %d older", len(ids)-10))
					break
				}
				b.WriteByte('\n')
			}
			b.WriteString("\nuse /resume for an interactive picker")
			body = b.String()
		}
		m.history.AppendLocal(text, body)
		m.saveSession()
		return m, nil

	case "fork":
		// Overlay listing prior user messages; selecting one forks a new
		// branch from that point.
		if !m.openFork() {
			m.history.AppendLocal(text, "nothing to fork from (no prior user messages on this branch)")
			m.saveSession()
		}
		return m, nil

	case "clone":
		// Duplicate the active branch into a new session and switch.
		return m.cloneSession(text)

	case "tree":
		// Interactive overlay navigating the branch tree.
		if !m.openTree() {
			m.history.AppendLocal(text, "the session tree is empty")
			m.saveSession()
		}
		return m, nil

	case "import":
		// Load an external .jsonl session file into the store + switch.
		path := ""
		if len(args) > 0 {
			path = args[0]
		}
		return m.importSession(text, path)

	case "memory":
		mem, err := store.LoadMemory()
		var body string
		if err != nil {
			body = "error: " + err.Error()
		} else if strings.TrimSpace(mem) == "" {
			body = "(memory.md is empty - the model can add to it via the remember tool)"
		} else {
			body = strings.TrimSpace(mem)
		}
		m.history.AppendLocal(text, body)
		m.saveSession()
		return m, nil

	case "skill", "skills":
		if len(args) == 0 {
			skills, _ := store.ListSkills()
			if len(skills) == 0 {
				m.history.AppendLocal(text, "no skills installed (put markdown files in "+store.SkillsDir()+")")
			} else {
				var b strings.Builder
				b.WriteString(fmt.Sprintf("%d skill(s) installed:\n", len(skills)))
				for _, s := range skills {
					b.WriteString("  ")
					b.WriteString(s.Name)
					if s.Description != "" {
						b.WriteString(" - ")
						b.WriteString(s.Description)
					}
					b.WriteByte('\n')
				}
				m.history.AppendLocal(text, b.String())
			}
		} else {
			// Match pi's /skill:name -- build a model-visible skill block
			// and SUBMIT it as a real user turn so the model actually sees
			// the skill (AppendLocal is skipped by ToProviderMessages).
			name := args[0]
			body, err := store.ReadSkillBody(name)
			if err != nil {
				m.history.AppendLocal(text, "error: "+err.Error())
				m.saveSession()
				return m, nil
			}
			path := name
			if skills, _ := store.ListSkills(); skills != nil {
				for _, s := range skills {
					if s.Name == name {
						path = s.Path
						break
					}
				}
			}
			block := fmt.Sprintf("<skill name=%q location=%q>\n%s\n</skill>", name, path, body)
			if len(args) > 1 {
				block += "\n\n" + strings.Join(args[1:], " ")
			}
			m.history.AppendUser(block)
			m.saveSession()
			return m.beginStream()
		}
		m.saveSession()
		return m, nil

	case "config":
		// Show resolved config (api_key masked).
		var b strings.Builder
		cfg, _ := store.LoadConfig()
		fmt.Fprintf(&b, "config from %s:\n", store.ConfigPath())
		fmt.Fprintf(&b, "  api_key       = %s\n", store.MaskedAPIKey(cfg.APIKey))
		fmt.Fprintf(&b, "  model         = %s\n", or(cfg.Model, defaultModel+" (default)"))
		fmt.Fprintf(&b, "  api_url       = %s\n", or(cfg.APIURL, "(default)"))
		fmt.Fprintf(&b, "  ssl_cert_file = %s\n", or(cfg.SSLCertFile, "(none)"))
		m.history.AppendLocal(text, b.String())
		m.saveSession()
		return m, nil

	case "settings":
		// Task 5: interactive settings overlay (thinking level, auto-
		// compaction toggle, placeholder theme). Reuses the model/login
		// picker overlay pattern.
		m.settingsOpen = true
		m.settingsCursor = 0
		m.statusMsg = "settings: arrows to move, enter/space to change, esc to close"
		return m, nil

	case "trust":
		// Task 6: mark this project directory trusted so project-local
		// .pi/ resources (skills, prompts, SYSTEM.md, AGENTS.md) load.
		cwd, _ := os.Getwd()
		if err := store.SetTrust(cwd, store.TrustAlways); err != nil {
			m.history.AppendLocal(text, "error: "+err.Error())
			m.saveSession()
			return m, nil
		}
		m.trusted = true
		// Rebuild the system prompt now that project resources are
		// allowed to load.
		m.history.System = buildSystemPrompt()
		m.history.AppendLocal(text, "trusted "+cwd+"\nproject-local .pi/ resources now load. /reload already applied.")
		m.statusMsg = ""
		m.saveSession()
		return m, nil

	case "model":
		if len(args) == 0 {
			// Phase 12: open the interactive model picker. Status bar
			// shows current model + helper. Live OpenRouter fetch
			// happens async via a tea.Cmd so we don't block.
			m.modelPickerOpen = true
			m.modelCursor = 0
			m.modelQuery = m.modelQuery[:0]
			m.modelQueryCur = 0
			// Start with curated list immediately; live fetch will
			// merge in when modelsLoadedMsg arrives.
			m.modelList = provider.CuratedModels()
			m.statusMsg = "loading live model list..."
			return m, fetchModelsCmd(m.model)
		}
		old := m.model
		m.model = args[0]
		m.syncAPIKeyForModel()
		m.persistModel() // make the new model the default on next launch
		if m.tree != nil && m.model != old {
			m.tree.AppendModelChange(m.model)
		}
		m.history.AppendLocal(text, fmt.Sprintf("model: %s -> %s", old, m.model))
		m.saveSession()
		return m, nil

	case "scoped-models", "scoped":
		// Task 2.4: open the overlay to toggle which models Ctrl+P /
		// Shift+Ctrl+P cycle through. Reuses the model-picker overlay
		// pattern; a live OpenRouter fetch merges into modelList async so
		// the toggle list isn't limited to the curated set.
		m.scopedModelsOpen = true
		m.scopedCursor = 0
		m.scopedQuery = m.scopedQuery[:0]
		m.scopedQueryCur = 0
		if len(m.modelList) == 0 {
			m.modelList = provider.CuratedModels()
		}
		m.statusMsg = scopedStatus(m.enabledModels)
		return m, fetchModelsCmd(m.model)

	case "reload":
		// Re-read config + memory + skill index. Rebuilds system
		// prompt so the model picks up any changes the user just
		// made to /lib/pi9/memory.md or skills/.
		newCfg, err := store.LoadConfig()
		if err != nil {
			m.history.AppendLocal(text, "error: "+err.Error())
			m.saveSession()
			return m, nil
		}
		if newCfg.APIKey != "" {
			m.apiKey = newCfg.APIKey
		}
		if newCfg.Model != "" {
			m.model = newCfg.Model
		}
		m.syncAPIKeyForModel()
		m.history.System = buildSystemPrompt()
		m.history.AppendLocal(text, "reloaded config + memory + skill index")
		m.saveSession()
		return m, nil

	case "export":
		// Write the conversation as plain text to a file.
		var path string
		if len(args) > 0 {
			path = args[0]
		} else {
			path = "/tmp/pi9-" + m.sessionID + ".txt"
		}
		body := exportPlain(&m.history)
		if err := os.WriteFile(path, []byte(body), 0644); err != nil {
			m.history.AppendLocal(text, "error: "+err.Error())
		} else {
			m.history.AppendLocal(text, fmt.Sprintf("exported %d bytes to %s", len(body), path))
		}
		m.saveSession()
		return m, nil

	case "compact":
		// Real compaction (task 2): summarize the oldest turns into one
		// synthetic assistant turn via the current provider, keeping the
		// most-recent ~keepRecentTokens worth verbatim. Optional extra
		// instructions after the command focus the summary:
		//   /compact focus on the parser bug
		if m.streaming || m.compacting {
			m.history.AppendLocal(text, "busy - wait for the current request to finish")
			m.saveSession()
			return m, nil
		}
		if m.apiKey == "" {
			m.history.AppendLocal(text, "no API key set - run /login first")
			m.saveSession()
			return m, nil
		}
		cut := m.compactionCut()
		if cut <= 0 {
			// Force a cut for tiny histories so /compact always does
			// something useful: summarize all but the last turn.
			if len(m.history.Turns) >= 2 {
				cut = len(m.history.Turns) - 1
			} else {
				m.history.AppendLocal(text, fmt.Sprintf("only %d turn(s) - nothing to compact", len(m.history.Turns)))
				m.saveSession()
				return m, nil
			}
		}
		extra := strings.Join(args, " ")
		m.compacting = true
		m.statusMsg = "compacting..."
		turns := append([]chat.Turn(nil), m.history.Turns...)
		m.saveSession()
		return m, summarizeCmd(m.apiKey, m.model, m.thinkingLevel, turns, cut, extra, false)

	case "copy":
		// Pi's /copy puts the last assistant message on the
		// clipboard. Plan 9 has /dev/snarf as the clipboard
		// equivalent.
		var last string
		for i := len(m.history.Turns) - 1; i >= 0; i-- {
			t := m.history.Turns[i]
			if !t.Local && t.Assistant != "" {
				last = t.Assistant
				break
			}
		}
		if last == "" {
			m.history.AppendLocal(text, "no assistant message to copy")
			m.saveSession()
			return m, nil
		}
		var dst string
		if runtime.GOOS == "plan9" {
			dst = "/dev/snarf"
		} else {
			// On unix host, /tmp/pi9-snarf is a debugging fallback.
			dst = "/tmp/pi9-snarf"
		}
		if err := os.WriteFile(dst, []byte(last), 0644); err != nil {
			m.history.AppendLocal(text, "error: "+err.Error())
		} else {
			m.history.AppendLocal(text, fmt.Sprintf("copied %d bytes to %s", len(last), dst))
		}
		m.saveSession()
		return m, nil

	case "quit", "q", "exit":
		return m, tea.Quit

	default:
		// Task 4: prompt-template dispatch. When /word isn't a built-in,
		// try to resolve it as a prompt template (global $home/prompts/
		// *.md, plus project ./.pi/prompts/*.md when trusted). If found,
		// expand with the remaining args and submit as a normal user
		// message; otherwise fall through to "unknown command".
		cwd, _ := os.Getwd()
		if expanded, found, err := store.ResolvePromptTemplate(cmd, args, cwd, m.trusted); found && err == nil {
			expanded = strings.TrimSpace(expanded)
			if expanded == "" {
				m.history.AppendLocal(text, "template /"+cmd+" expanded to empty text")
				m.saveSession()
				return m, nil
			}
			m.history.AppendUser(expanded)
			m.saveSession()
			return m.beginStream()
		} else if err != nil {
			m.history.AppendLocal(text, "template /"+cmd+" error: "+err.Error())
			m.saveSession()
			return m, nil
		}
		m.history.AppendLocal(text, fmt.Sprintf("unknown command: /%s\nTry /help.", cmd))
		m.saveSession()
		return m, nil
	}
}

// countLocal returns the number of slash-command turns in turns.
func countLocal(turns []chat.Turn) int {
	n := 0
	for _, t := range turns {
		if t.Local {
			n++
		}
	}
	return n
}

// countNonLocal returns the number of LLM-exchange turns.
func countNonLocal(turns []chat.Turn) int {
	return len(turns) - countLocal(turns)
}

// exportPlain renders the history as a plain text transcript suitable
// for /export. Strips ANSI styling. Local turns are included.
func exportPlain(h *chat.History) string {
	var b strings.Builder
	fmt.Fprintf(&b, "# pi9 session\n")
	if h.Name != "" {
		fmt.Fprintf(&b, "# name: %s\n", h.Name)
	}
	fmt.Fprintf(&b, "# turns: %d\n\n", len(h.Turns))
	for i, t := range h.Turns {
		if t.Local {
			fmt.Fprintf(&b, "[%d] /command: %s\n", i+1, t.User)
			fmt.Fprintf(&b, "    %s\n\n", strings.ReplaceAll(t.Assistant, "\n", "\n    "))
			continue
		}
		fmt.Fprintf(&b, "[%d] you: %s\n", i+1, t.User)
		fmt.Fprintf(&b, "    pi9: %s\n", strings.ReplaceAll(t.Assistant, "\n", "\n         "))
		for _, c := range t.Calls {
			fmt.Fprintf(&b, "    -> %s(%s)\n", c.Name, c.Args)
			if c.Err != nil {
				fmt.Fprintf(&b, "       ERROR: %s\n", c.Err)
			} else if c.Output != "" {
				snippet := c.Output
				if len(snippet) > 200 {
					snippet = snippet[:200] + "..."
				}
				fmt.Fprintf(&b, "       %s\n", strings.ReplaceAll(snippet, "\n", "\n       "))
			}
		}
		b.WriteByte('\n')
	}
	return b.String()
}

// ---------- /login picker overlay (Phase 10) ----------

// handleLoginKey routes key events when we're in the /login picker
// overlay. Mode flow:
//
//	loginModePicker   ↑↓ to navigate, enter to pick, esc to cancel
//	loginModeKeyEntry typing into m.input, enter to save, esc back
//
// Saving stores the key in auth.json under the chosen provider id,
// rebuilds m.apiKey if the provider matches the current model's,
// then drops back to loginModeOff.
func (m pi9Model) handleLoginKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	providers := provider.AllProviders()

	switch m.loginMode {
	case loginModePicker:
		switch msg.String() {
		case "esc", "ctrl+c":
			m.loginMode = loginModeOff
			m.statusMsg = "login cancelled"
			return m, nil
		case "up", "ctrl+p", "k":
			if m.loginCursor > 0 {
				m.loginCursor--
			} else {
				m.loginCursor = len(providers) - 1 // wrap
			}
			return m, nil
		case "down", "ctrl+n", "j":
			if m.loginCursor < len(providers)-1 {
				m.loginCursor++
			} else {
				m.loginCursor = 0 // wrap
			}
			return m, nil
		case "enter", "ctrl+j", "ctrl+m":
			if m.loginCursor >= 0 && m.loginCursor < len(providers) {
				m.loginPicked = providers[m.loginCursor]
				m.input = m.input[:0]
				m.inputCursor = 0
				// Phase 10 S2: if this provider has OAuth available,
				// show the auth-method picker (subscription vs API key)
				// before going to key entry. Otherwise go directly to
				// key entry.
				// Phase 10 S3: if provider is OAuth-ONLY (Copilot),
				// skip the choice and go straight to OAuth.
				switch {
				case provider.RequiresOAuth(m.loginPicked):
					m.loginMode = loginModeOAuthRunning
					m.statusMsg = "starting OAuth flow..."
					return m, runOAuthLogin(m.loginPicked)
				case provider.GetOAuth(m.loginPicked) != nil:
					m.loginMode = loginModeAuthMethod
					m.loginCursor = 0
					m.statusMsg = "pick auth method for " + provider.DisplayName(m.loginPicked)
				default:
					m.loginMode = loginModeKeyEntry
					url := provider.KeyURL(m.loginPicked)
					if url != "" {
						m.statusMsg = "enter API key for " + provider.DisplayName(m.loginPicked) + " (get one at " + url + ")"
					} else {
						m.statusMsg = "enter API key for " + provider.DisplayName(m.loginPicked)
					}
				}
			}
			return m, nil
		}
		return m, nil

	case loginModeAuthMethod:
		// Two options: 0=Use subscription (OAuth), 1=Use API key.
		switch msg.String() {
		case "esc", "ctrl+c":
			// Back to provider list.
			m.loginMode = loginModePicker
			m.loginCursor = 0
			m.statusMsg = "pick provider with arrows, enter to select, esc to cancel"
			return m, nil
		case "up", "ctrl+p", "k":
			if m.loginCursor > 0 {
				m.loginCursor--
			} else {
				m.loginCursor = 1
			}
			return m, nil
		case "down", "ctrl+n", "j":
			if m.loginCursor < 1 {
				m.loginCursor++
			} else {
				m.loginCursor = 0
			}
			return m, nil
		case "enter", "ctrl+j", "ctrl+m":
			if m.loginCursor == 0 {
				// Subscription: kick off OAuth flow.
				m.loginMode = loginModeOAuthRunning
				m.statusMsg = "starting OAuth flow..."
				return m, runOAuthLogin(m.loginPicked)
			}
			// API key path: fall through to key entry.
			m.loginMode = loginModeKeyEntry
			m.input = m.input[:0]
			m.inputCursor = 0
			url := provider.KeyURL(m.loginPicked)
			if url != "" {
				m.statusMsg = "enter API key for " + provider.DisplayName(m.loginPicked) + " (get one at " + url + ")"
			} else {
				m.statusMsg = "enter API key for " + provider.DisplayName(m.loginPicked)
			}
			return m, nil
		}
		return m, nil

	case loginModeOAuthRunning:
		// User is mid-OAuth flow. They can:
		//   - esc / ctrl+c: cancel
		//   - enter: submit pasted text (code, code#state, URL) as
		//            manual fallback if the browser callback didn't
		//            land. The text goes into oauthManualCh which
		//            the OAuth goroutine drains.
		//   - any other key: collected into m.input like normal so
		//                    the user can compose / paste a long URL.
		// Completion arrives via oauthDoneMsg in the Update loop, not
		// here.
		switch msg.String() {
		case "esc", "ctrl+c":
			m.loginMode = loginModeOff
			m.statusMsg = "oauth cancelled"
			// Note: the background goroutine is still running and
			// will eventually deliver oauthDoneMsg. We'll just
			// ignore that message when it arrives because
			// loginMode is already off. Not perfectly clean but
			// avoids leaking the server socket — the goroutine's
			// defer closes it on completion.
			return m, nil
		case "enter", "ctrl+j", "ctrl+m":
			if len(m.input) == 0 {
				return m, nil
			}
			text := string(m.input)
			m.input = m.input[:0]
			m.inputCursor = 0
			// Pump to OAuth goroutine. Non-blocking — if no goroutine
			// is listening (race during cancel), drop silently.
			if oauthManualCh != nil {
				select {
				case oauthManualCh <- text:
					m.statusMsg = "pasted code submitted, exchanging..."
				default:
					m.statusMsg = "oauth not accepting input right now"
				}
			}
			return m, nil
		case "backspace", "ctrl+h":
			if len(m.input) > 0 && m.inputCursor > 0 {
				m.input = append(m.input[:m.inputCursor-1], m.input[m.inputCursor:]...)
				m.inputCursor--
			}
			return m, nil
		case "ctrl+u":
			m.input = m.input[:0]
			m.inputCursor = 0
			return m, nil
		}
		// Otherwise, accept rune input into the buffer like key entry.
		for _, r := range msg.Runes {
			m.input = append(m.input, 0)
			copy(m.input[m.inputCursor+1:], m.input[m.inputCursor:])
			m.input[m.inputCursor] = r
			m.inputCursor++
		}
		return m, nil

	case loginModeKeyEntry:
		switch msg.String() {
		case "esc", "ctrl+c":
			// Go back to picker, not all the way out — so user can
			// re-pick if they hit the wrong one.
			m.loginMode = loginModePicker
			m.input = m.input[:0]
			m.inputCursor = 0
			m.statusMsg = "pick provider with arrows, enter to select, esc to cancel"
			return m, nil
		case "enter", "ctrl+j", "ctrl+m":
			key := strings.TrimSpace(string(m.input))
			if key == "" {
				m.statusMsg = "key is empty - paste it, or esc to back out"
				return m, nil
			}
			pid := string(m.loginPicked)
			if err := store.SetAPIKey(pid, key); err != nil {
				m.statusMsg = "save failed: " + err.Error()
				return m, nil
			}
			// If this provider matches the current model's provider,
			// hot-update m.apiKey so the next message uses it
			// immediately (no relaunch needed).
			if provider.ProviderForModel(m.model) == m.loginPicked {
				m.apiKey = key
			}
			// Friendly local turn so it appears in scrollback with
			// the key masked.
			safe := "/login " + pid + " " + store.MaskedAPIKey(key)
			m.history.AppendLocal(safe, "saved to "+store.AuthPath())
			m.loginMode = loginModeOff
			m.input = m.input[:0]
			m.inputCursor = 0
			m.statusMsg = ""
			m.saveSession()
			return m, nil
		case "backspace", "ctrl+h":
			if m.inputCursor > 0 {
				m.input = append(m.input[:m.inputCursor-1], m.input[m.inputCursor:]...)
				m.inputCursor--
			}
			return m, nil
		case "ctrl+u":
			m.input = m.input[:0]
			m.inputCursor = 0
			return m, nil
		}
		// Normal character input goes into m.input for the key.
		for _, r := range msg.Runes {
			if r >= 0x20 && r != 0x7f {
				m.input = append(m.input[:m.inputCursor], append([]rune{r}, m.input[m.inputCursor:]...)...)
				m.inputCursor++
			}
		}
		return m, nil
	}
	return m, nil
}

// ---------- Phase 12: /model picker ----------

// handleModelPickerKey is the key dispatcher when the model picker
// overlay is open. Behaves like a mini-terminal-app:
//
//   - up/down (or ctrl+p/ctrl+n) navigate the visible (filtered) list
//   - enter selects the highlighted row → sets m.model + saves
//   - esc / ctrl+c cancels back to chat
//   - any printable char goes into the search box and re-filters
//   - backspace edits the search
func (m pi9Model) handleModelPickerKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	filtered := m.filteredModels()

	switch msg.String() {
	case "esc", "ctrl+c":
		m.modelPickerOpen = false
		m.statusMsg = ""
		return m, nil

	case "up", "ctrl+p":
		if m.modelCursor > 0 {
			m.modelCursor--
		}
		return m, nil

	case "down", "ctrl+n":
		if m.modelCursor < len(filtered)-1 {
			m.modelCursor++
		}
		return m, nil

	case "pgup":
		m.modelCursor -= 5
		if m.modelCursor < 0 {
			m.modelCursor = 0
		}
		return m, nil

	case "pgdown":
		m.modelCursor += 5
		if m.modelCursor >= len(filtered) {
			m.modelCursor = len(filtered) - 1
		}
		if m.modelCursor < 0 {
			m.modelCursor = 0
		}
		return m, nil

	case "home":
		m.modelCursor = 0
		return m, nil

	case "end":
		m.modelCursor = len(filtered) - 1
		if m.modelCursor < 0 {
			m.modelCursor = 0
		}
		return m, nil

	case "enter", "ctrl+j", "ctrl+m":
		if len(filtered) == 0 {
			return m, nil
		}
		if m.modelCursor < 0 || m.modelCursor >= len(filtered) {
			return m, nil
		}
		picked := filtered[m.modelCursor]
		old := m.model
		m.model = picked.ID
		m.syncAPIKeyForModel()
		m.persistModel() // make the picked model the default on next launch
		m.modelPickerOpen = false
		if m.tree != nil && m.model != old {
			m.tree.AppendModelChange(m.model)
		}
		m.history.AppendLocal("/model", fmt.Sprintf("model: %s -> %s (%s)", old, m.model, picked.Label))
		m.statusMsg = ""
		m.saveSession()
		return m, nil

	case "backspace", "ctrl+h":
		if m.modelQueryCur > 0 {
			m.modelQuery = append(m.modelQuery[:m.modelQueryCur-1], m.modelQuery[m.modelQueryCur:]...)
			m.modelQueryCur--
			m.modelCursor = 0 // reset selection after edit
		}
		return m, nil

	case "ctrl+u":
		m.modelQuery = m.modelQuery[:0]
		m.modelQueryCur = 0
		m.modelCursor = 0
		return m, nil

	case "ctrl+w":
		// delete word backwards
		for m.modelQueryCur > 0 && m.modelQuery[m.modelQueryCur-1] == ' ' {
			m.modelQuery = append(m.modelQuery[:m.modelQueryCur-1], m.modelQuery[m.modelQueryCur:]...)
			m.modelQueryCur--
		}
		for m.modelQueryCur > 0 && m.modelQuery[m.modelQueryCur-1] != ' ' {
			m.modelQuery = append(m.modelQuery[:m.modelQueryCur-1], m.modelQuery[m.modelQueryCur:]...)
			m.modelQueryCur--
		}
		m.modelCursor = 0
		return m, nil
	}

	// Printable chars → add to search query
	for _, r := range msg.Runes {
		if r >= 0x20 && r != 0x7f {
			m.modelQuery = append(m.modelQuery, 0)
			copy(m.modelQuery[m.modelQueryCur+1:], m.modelQuery[m.modelQueryCur:])
			m.modelQuery[m.modelQueryCur] = r
			m.modelQueryCur++
		}
	}
	m.modelCursor = 0 // reset selection on filter change
	return m, nil
}

// filteredModels returns the picker's current visible subset based on
// the search query. Empty query → entire list.
func (m pi9Model) filteredModels() []provider.ModelInfo {
	q := string(m.modelQuery)
	if q == "" {
		return m.modelList
	}
	out := make([]provider.ModelInfo, 0, len(m.modelList))
	for _, mi := range m.modelList {
		// Match against either ID or Label
		if provider.FuzzyMatch(q, mi.ID) || provider.FuzzyMatch(q, mi.Label) {
			out = append(out, mi)
		}
	}
	return out
}

// renderModelPicker draws the picker overlay. Layout:
//
//	+-----------------------------------------+
//	| /model: search... [42 / 200 matches]    |
//	+-----------------------------------------+
//	| > anthropic/claude-sonnet-4.5  200K    |
//	|   openai/gpt-5                          |
//	|   ... visible rows fitted to height     |
//	+-----------------------------------------+
//
// Highlighted row gets inverted color.
func (m pi9Model) renderModelPicker(cols, rows int) string {
	filtered := m.filteredModels()
	total := len(m.modelList)

	if rows < 5 {
		rows = 5
	}

	// Header with search query
	query := string(m.modelQuery)
	header := fmt.Sprintf(" /model: %s_  [%d/%d]", query, len(filtered), total)
	headerStyled := lipgloss.NewStyle().
		Background(lipgloss.Color("4")).
		Foreground(lipgloss.Color("15")).
		Render(fitRow(header, cols))

	// How many rows we can show
	listRows := rows - 3 // 1 header, 2 borders/footer
	if listRows < 1 {
		listRows = 1
	}

	// Window the list around the cursor
	start := 0
	if m.modelCursor >= listRows {
		start = m.modelCursor - listRows + 1
	}
	end := start + listRows
	if end > len(filtered) {
		end = len(filtered)
	}

	out := []string{headerStyled}

	for i := start; i < end; i++ {
		mi := filtered[i]
		// Provider tag, model ID, optional recent marker
		marker := " "
		if mi.Recent {
			marker = "*" // ASCII only; vtwin lacks U+2605
		}
		ctx := ""
		if mi.ContextWindow > 0 {
			ctx = fmt.Sprintf(" %dK", mi.ContextWindow/1000)
		}
		line := fmt.Sprintf("  %s %-22s %s%s",
			marker,
			"["+provider.DisplayName(mi.Provider)+"]",
			mi.ID,
			ctx)

		if i == m.modelCursor {
			line = lipgloss.NewStyle().
				Background(lipgloss.Color("14")).
				Foreground(lipgloss.Color("0")).
				Render(fitRow(line, cols))
		} else {
			line = fitRow(line, cols)
		}
		out = append(out, line)
	}

	// Pad to fill remaining rows
	for len(out) < rows-1 {
		out = append(out, fitRow("", cols))
	}

	// Footer
	footer := " ^v nav - enter select - type filter - esc cancel"
	out = append(out, lipgloss.NewStyle().
		Foreground(lipgloss.Color("8")).
		Render(fitRow(footer, cols)))

	return strings.Join(out, "\n")
}

// renderLoginPicker renders the picker overlay in place of the input
// box. Four modes:
//
//	loginModePicker:       provider list
//	loginModeAuthMethod:   subscription vs api key choice
//	loginModeKeyEntry:     masked key input box
//	loginModeOAuthRunning: progress message ("waiting for browser...")
//
// Both honor `cols` to fit the available width.
func (m pi9Model) renderLoginPicker(cols int) string {
	switch m.loginMode {
	case loginModeOAuthRunning:
		// Show an input box so the user can paste the authorization
		// code if the browser callback didn't work. statusMsg above
		// has the URL/progress; this is the paste row.
		raw := string(m.input)
		var shown string
		// Auth codes are long. Truncate-with-ellipsis if it
		// overflows; otherwise show literal so the user can verify
		// what they pasted.
		maxShow := cols - 20
		if maxShow < 8 {
			maxShow = 8
		}
		if len(raw) > maxShow {
			shown = raw[:6] + "..." + raw[len(raw)-6:]
		} else {
			shown = raw
		}
		prompt := fmt.Sprintf(" paste code (or wait for browser): %s_", shown)
		top := "+" + strings.Repeat("-", cols-2) + "+"
		mid := fitRow("|"+prompt, cols-1) + "|"
		return top + "\n" + mid + "\n" + top

	case loginModeAuthMethod:
		// Two-option list: subscription vs api key.
		header := " /login " + provider.DisplayName(m.loginPicked) + ":"
		opts := []string{
			"Use " + provider.DisplayName(m.loginPicked) + " subscription (browser OAuth)",
			"Use an API key (paste it)",
		}
		rows := []string{fitRow(header, cols)}
		for i, o := range opts {
			line := "   " + o
			if i == m.loginCursor {
				line = lipgloss.NewStyle().
					Foreground(lipgloss.Color("0")).
					Background(lipgloss.Color("14")).
					Render(" > " + o + " ")
			}
			rows = append(rows, fitRow(line, cols))
		}
		return strings.Join(rows, "\n")

	case loginModeKeyEntry:
		// Mask key as user types.
		raw := string(m.input)
		var shown string
		if len(raw) <= 6 {
			shown = strings.Repeat("*", len(raw))
		} else {
			shown = raw[:3] + strings.Repeat("*", len(raw)-6) + raw[len(raw)-3:]
		}
		prompt := fmt.Sprintf(" key for %s: %s_", provider.DisplayName(m.loginPicked), shown)
		top := "+" + strings.Repeat("-", cols-2) + "+"
		mid := fitRow("|"+prompt, cols-1) + "|"
		return top + "\n" + mid + "\n" + top
	}

	// loginModePicker (default)
	providers := provider.AllProviders()
	header := " /login - pick provider:"
	rows := []string{fitRow(header, cols)}
	visible := 5
	start := 0
	if m.loginCursor >= visible {
		start = m.loginCursor - visible + 1
	}
	end := start + visible
	if end > len(providers) {
		end = len(providers)
		start = end - visible
		if start < 0 {
			start = 0
		}
	}
	for i := start; i < end; i++ {
		p := providers[i]
		label := provider.DisplayName(p)
		// Phase 10 S2: append "(subscription)" badge if this
		// provider has OAuth support, so the user knows they can
		// use their Pro/Max plan instead of an API key.
		if provider.GetOAuth(p) != nil {
			label += " (subscription available)"
		}
		line := fmt.Sprintf("   %s", label)
		if i == m.loginCursor {
			line = lipgloss.NewStyle().
				Foreground(lipgloss.Color("0")).
				Background(lipgloss.Color("14")).
				Render(fmt.Sprintf(" > %s ", label))
		}
		rows = append(rows, fitRow(line, cols))
	}
	return strings.Join(rows, "\n")
}

// slashHelp returns the help text shown by /help. Multi-line for
// readability in the scrollback.
//
// Commands group: auth, session, content, settings, exit.
// Mirrors pi.dev's surface where reasonable; pi9-specific extras
// are flagged in the help text.
func slashHelp() string {
	return `pi9 slash commands:

  auth
    /login <key>       paste an API key (persists to config)
    /logout            clear the saved API key

  session
    /new               start a fresh session (old one stays on disk)
    /name <name>       set this session's display name
    /session           show current session metadata
    /sessions          list sessions, newest first  (pi9-specific)
    /resume            interactive session picker (newest first)
    /tree              navigate the branch tree, jump to any point
    /fork              fork a new branch from a prior user message
    /clone             duplicate the active branch into a new session
    /import <path>     load an external .jsonl session and switch to it
    /clear             clear conversation but keep this session
    /save              force-save current session
    /export [path]     write transcript to a file
    /compact [focus]   summarize older turns into one (frees context)

  content
    /memory            show $home/lib/pi9/memory.md     (pi9-specific)
    /skill [name]      list skills / inject one as a turn  (pi9-specific)
    /copy              copy last assistant message to /dev/snarf
    /reload            reload config + memory + skills

  settings
    /model [name]      show or switch model
    /scoped-models     toggle which models Ctrl+P cycles through
    /config            show resolved config (file values)
    /settings          interactive settings overlay (thinking, compaction)
    /trust             trust this project's .pi/ resources
    /hotkeys           list key bindings (aliased: /keys)

  exit
    /quit              (also: /q, /exit, ctrl-c, ctrl-d)

  keys
    shift+tab          cycle thinking level (off->minimal->...->xhigh->off)
    ctrl+t             toggle showing the model's thinking text
    ctrl+p             cycle active model forward through scoped set
    shift+ctrl+p       cycle active model backward
    pgup/pgdn          scroll - ctrl+end jump to latest

  editor
    /                  open slash-command + prompt-template completion
    @path              fuzzy-complete a file; on send, append its contents
    !cmd               run cmd, send its output to the model as context
    !!cmd              run cmd locally, do NOT send to the model
    tab / enter        accept the highlighted completion - esc dismiss

A /word that isn't a built-in is looked up as a prompt template
(global $home/prompts/*.md, plus project .pi/prompts/*.md when trusted),
expanded with its args, and sent to the LLM.

Anything not starting with / is sent to the LLM. Slash commands and
their responses are visible in the scrollback but excluded from what
the LLM sees.`
}

// or returns a if non-empty, else b. Tiny helper for /config.
func or(a, b string) string {
	if a != "" {
		return a
	}
	return b
}

func (m *pi9Model) beginStream() (tea.Model, tea.Cmd) {
	// Refuse politely if no API key set. Better than firing a request
	// with empty Authorization and surfacing a confusing 401 from
	// upstream.
	if m.apiKey == "" {
		m.history.FinishTurn(fmt.Errorf("no API key set. run /login <key> to set one"))
		m.streaming = false
		m.statusMsg = "no API key set - run /login <key>"
		m.saveSession()
		return m, nil
	}
	// Auto-compaction (task 3): if the estimated context exceeds the
	// model's window minus the reserve, summarize the oldest turns
	// first. The deferred user turn is sent from the summaryDoneMsg
	// handler once the synthetic summary turn is in place. Guard against
	// re-entry (m.compacting) so we don't fire two summarizations.
	if !m.compacting && m.shouldAutoCompact() {
		if cut := m.compactionCut(); cut > 0 {
			m.compacting = true
			m.streaming = true
			m.statusMsg = "context full - auto-compacting..."
			turns := append([]chat.Turn(nil), m.history.Turns...)
			return m, summarizeCmd(m.apiKey, m.model, m.thinkingLevel, turns, cut, "", true)
		}
	}

	m.streaming = true
	m.statusMsg = "thinking..."

	ctx, cancel := context.WithCancel(context.Background())
	m.streamCancel = cancel
	msgs := m.history.ToProviderMessages()
	return m, runStream(ctx, m.apiKey, m.model, m.thinkingLevel, m.tools, msgs)
}

// contextWindow returns the active model's context window in tokens, or
// 0 when unknown.
func (m pi9Model) contextWindow() int {
	return provider.ContextWindowFor(m.model)
}

// contextTokens returns the best estimate of current context usage:
// the provider-reported total when available, else a byte-based
// estimate of the whole history.
func (m pi9Model) contextTokens() int {
	if m.lastTotalTokens > 0 {
		return m.lastTotalTokens
	}
	return estimateHistoryTokens(&m.history)
}

// reserve returns the configured reserve-tokens, defaulting when unset.
func (m pi9Model) reserve() int {
	if m.reserveTokens > 0 {
		return m.reserveTokens
	}
	return defaultReserveTokens
}

// keepRecent returns the configured keep-recent-tokens, defaulting when unset.
func (m pi9Model) keepRecent() int {
	if m.keepRecentTokens > 0 {
		return m.keepRecentTokens
	}
	return defaultKeepRecentTokens
}

// shouldAutoCompact reports whether estimated context exceeds the
// window minus the reserve. Returns false when auto-compaction is off
// or the window is unknown.
func (m pi9Model) shouldAutoCompact() bool {
	if !m.autoCompact {
		return false
	}
	win := m.contextWindow()
	if win <= 0 {
		return false
	}
	return m.contextTokens() > win-m.reserve()
}

// compactionCut returns the turn index of the first turn to keep, using
// the configured keep-recent budget. 0 means "nothing to compact".
func (m pi9Model) compactionCut() int {
	return findCompactionCut(m.history.Turns, m.keepRecent())
}

// contextStatus renders the header's context-usage indicator. With a
// known window it shows "NN% ctx · 12.3k tok"; without one it shows just
// the token count. (est) marks a byte-based estimate (no provider usage
// reported yet).
func (m pi9Model) contextStatus() string {
	tok := m.contextTokens()
	if tok <= 0 {
		return ""
	}
	suffix := ""
	if m.lastTotalTokens == 0 {
		suffix = " est"
	}
	win := m.contextWindow()
	if win > 0 {
		pct := tok * 100 / win
		return fmt.Sprintf("%d%% ctx (%s/%s%s)", pct, humanTokens(tok), humanTokens(win), suffix)
	}
	return fmt.Sprintf("%s tok%s", humanTokens(tok), suffix)
}

// humanTokens formats a token count compactly: 850, 12.3k, 1.2M.
func humanTokens(n int) string {
	switch {
	case n >= 1_000_000:
		return fmt.Sprintf("%.1fM", float64(n)/1_000_000)
	case n >= 1000:
		return fmt.Sprintf("%.1fk", float64(n)/1000)
	default:
		return fmt.Sprintf("%d", n)
	}
}

// ---------- Update ----------

func (m pi9Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case startupMsg:
		// Task 3.3: flag-driven startup actions. -r/--resume opens the
		// picker; an initial @file/positional prompt is auto-submitted.
		// -r takes precedence: if both were given we open the picker and
		// stash the prompt in the editor for the user to send after they
		// pick a session.
		if m.startupResume {
			m.startupResume = false
			if m.startupPrompt != "" {
				m.input = []rune(m.startupPrompt)
				m.inputCursor = len(m.input)
				m.startupPrompt = ""
			}
			m.openResume()
			return m, nil
		}
		if m.startupPrompt != "" {
			prompt := m.startupPrompt
			m.startupPrompt = ""
			m.input = []rune(prompt)
			m.inputCursor = len(m.input)
			return m.submitInput()
		}
		return m, nil

	case tea.WindowSizeMsg:
		m.width, m.height = msg.Width, msg.Height
		// Clamp scroll offset to new size — if window grew, we may
		// have less to scroll than before.
		if m.scrollOffset > 0 {
			// Will get re-clamped in renderInput; just trigger render.
		}
		return m, nil

	case tea.MouseMsg:
		// Phase 10 polish: mouse-wheel scroll. vtwin sends SGR
		// scroll events (ESC[<64;X;YM for up, ESC[<65;X;YM for
		// down). Bubbletea parses these into MouseMsg with
		// Type=MouseWheelUp/Down. We scroll the scrollback overlay
		// by a few rows per tick.
		switch msg.Type {
		case tea.MouseWheelUp:
			m.scrollOffset += 3
			return m, nil
		case tea.MouseWheelDown:
			m.scrollOffset -= 3
			if m.scrollOffset < 0 {
				m.scrollOffset = 0
			}
			return m, nil
		}
		return m, nil

	case chunkMsg:
		m.history.AppendDelta(msg.delta)
		return m, nil

	case reasoningMsg:
		// Always accumulate reasoning so it can be revealed later by
		// toggling hideThinking; rendering decides whether to show it.
		m.history.AppendReasoning(msg.delta)
		return m, nil

	case oauthURLMsg:
		// OAuth URL ready. Update status bar so user can manually
		// copy/paste if the browser didn't open.
		m.statusMsg = "open in browser if needed: " + msg.url
		return m, nil

	case modelsLoadedMsg:
		// Live model list arrived. Replace the curated-only list
		// with the merged one and clear the loading status.
		if msg.err == nil && len(msg.merged) > 0 {
			m.modelList = msg.merged
		}
		if msg.err != nil {
			m.statusMsg = "live fetch failed; using curated list"
		} else {
			m.statusMsg = fmt.Sprintf("%d models. type to filter, enter to select, esc to cancel", len(m.modelList))
		}
		return m, nil

	case oauthDoneMsg:
		// OAuth login completed (success or error). Return to
		// normal chat input mode and surface result as local turn.
		m.loginMode = loginModeOff
		m.input = m.input[:0]
		m.inputCursor = 0
		if msg.err != nil {
			m.history.AppendLocal("/login (oauth)", "oauth failed: "+msg.err.Error())
			m.statusMsg = "oauth failed - see scrollback"
		} else {
			// Hot-update m.apiKey if the current model maps to this
			// provider, so next message uses the new token.
			if provider.ProviderForModel(m.model) == msg.provider {
				m.apiKey = store.LookupAPIKey(string(msg.provider))
			}
			m.history.AppendLocal(
				"/login "+string(msg.provider)+" (oauth)",
				"signed in to "+provider.DisplayName(msg.provider)+" - saved to "+store.AuthPath(),
			)
			m.statusMsg = ""
		}
		m.saveSession()
		return m, nil

	case streamDoneMsg:
		// Capture provider-reported token usage into model state for the
		// "NN% ctx" footer. The terminal chunk's TotalTokens is the most
		// accurate figure we get; keep the last non-zero value.
		if msg.usage != nil && msg.usage.TotalTokens > 0 {
			m.lastTotalTokens = msg.usage.TotalTokens
		}

		if msg.err == nil && len(msg.toolCalls) > 0 {
			var cmds []tea.Cmd
			for _, tc := range msg.toolCalls {
				idx := m.history.BeginCall(tc.ID, tc.Function.Name, tc.Function.Arguments)
				cmds = append(cmds, runTool(idx, tc.Function.Name, tc.Function.Arguments))
			}
			m.statusMsg = fmt.Sprintf("running %d tool%s...", len(msg.toolCalls), plural(len(msg.toolCalls)))
			m.saveSession()
			return m, tea.Batch(cmds...)
		}

		m.history.FinishTurn(msg.err)
		m.streaming = false
		m.streamCancel = nil
		if msg.err != nil {
			m.statusMsg = "error: " + msg.err.Error()
		} else {
			m.statusMsg = ""
		}
		m.saveSession()
		// The run fully finished: drain any queued steering (delivered
		// first) or follow-up messages as the next turn(s). On error we
		// still deliver so queued work isn't silently lost.
		if model, cmd, drained := m.drainFollowUp(); drained {
			return model, cmd
		}
		return m, nil

	case summaryDoneMsg:
		// A compaction summary finished streaming. Replace the
		// summarized turns with one synthetic assistant turn, then —
		// for auto-compaction — kick off the user turn we deferred.
		m.compacting = false
		if msg.err != nil {
			m.statusMsg = "compact failed: " + msg.err.Error()
			if !msg.auto {
				m.history.AppendLocal("/compact", "summarization failed: "+msg.err.Error())
			}
			m.saveSession()
			// Auto-compaction failed: still send the pending turn so the
			// user isn't stuck (the request may succeed or surface the
			// real provider error).
			if msg.auto {
				return m.beginStream()
			}
			return m, nil
		}
		// Recompute the cut against the current history in case it grew;
		// applyCompaction clamps defensively.
		cut := msg.cut
		if cut > len(m.history.Turns) {
			cut = len(m.history.Turns)
		}
		tokensBefore := m.contextTokens()
		// Record the compaction in the tree BEFORE mutating m.history:
		// sync the full pre-compaction history so every turn is an entry,
		// find the first-kept turn's entry id, then append a compaction
		// entry whose firstKeptEntryId points at it.
		m.recordCompaction(cut, msg.summary, tokensBefore)
		dropped := applyCompaction(&m.history, cut, msg.summary)
		// Provider usage no longer reflects the (now smaller) context;
		// reset so the footer falls back to the byte estimate until the
		// next real response reports fresh usage.
		m.lastTotalTokens = 0
		if msg.auto {
			m.statusMsg = fmt.Sprintf("auto-compacted %d turns", dropped)
			m.saveSession()
			return m.beginStream()
		}
		m.history.AppendLocal("/compact", fmt.Sprintf("summarized %d turns into 1; %d remain", dropped, len(m.history.Turns)))
		m.statusMsg = ""
		m.saveSession()
		return m, nil

	case toolResultMsg:
		m.history.FinishCall(msg.idx, msg.output, msg.err)
		if m.allCallsFinished() {
			m.saveSession()
			return m.continueLoop()
		}
		return m, nil

	case loopContinueMsg:
		return m.beginStream()

	case tea.KeyMsg:
		return m.handleKey(msg)
	}
	return m, nil
}

func (m pi9Model) allCallsFinished() bool {
	if len(m.history.Turns) == 0 {
		return true
	}
	t := m.history.Turns[len(m.history.Turns)-1]
	for _, c := range t.Calls {
		if c.Finished.IsZero() {
			return false
		}
	}
	return true
}

func (m pi9Model) continueLoop() (tea.Model, tea.Cmd) {
	if len(m.history.Turns) == 0 {
		return m, nil
	}
	t := m.history.Turns[len(m.history.Turns)-1]
	if len(t.Calls) >= maxTurnLoops*4 {
		m.history.FinishTurn(fmt.Errorf("hit max tool calls"))
		m.streaming = false
		m.streamCancel = nil
		m.statusMsg = "max tool loops"
		m.saveSession()
		// The run ended (capped): deliver any queued steering / follow-up
		// as a fresh turn rather than dropping it.
		if model, cmd, drained := m.drainFollowUp(); drained {
			return model, cmd
		}
		return m, nil
	}
	// SAFE POINT between tool rounds: if the user queued a steering message
	// while the tools ran, deliver it as the next user turn now (mid-run
	// course-correction) instead of looping back to the model unchanged.
	if model, cmd, drained := m.drainSteering(); drained {
		return model, cmd
	}
	return m, func() tea.Msg { return loopContinueMsg{} }
}

func plural(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}

// ---------- key handling ----------

func (m pi9Model) handleKey(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	// Phase 10: if we're in the /login picker overlay, route keys
	// there instead of normal chat input. esc cancels back to chat.
	if m.loginMode != loginModeOff {
		return m.handleLoginKey(msg)
	}
	// Phase 12: model picker overlay has its own key handling.
	if m.modelPickerOpen {
		return m.handleModelPickerKey(msg)
	}
	// Task 5: settings overlay has its own key handling.
	if m.settingsOpen {
		return m.handleSettingsKey(msg)
	}
	// Task 2.1: tree-session overlays each have their own key handling.
	if m.resumeOpen {
		return m.handleResumeKey(msg)
	}
	if m.forkOpen {
		return m.handleForkKey(msg)
	}
	if m.treeOpen {
		return m.handleTreeKey(msg)
	}
	// Task 2.4: /scoped-models overlay has its own key handling.
	if m.scopedModelsOpen {
		return m.handleScopedModelsKey(msg)
	}

	// Task 2.3: when the autocomplete dropdown is open, let it claim
	// navigation/accept/dismiss keys (tab/enter-on-nothing/up/down/esc)
	// before normal editor handling. acceptCompletion replaces the token;
	// plain typing falls through and re-filters via refreshCompletions.
	if m.completionsOpen() {
		switch msg.String() {
		case "up", "down", "tab", "esc":
			if m.handleCompletionKey(msg.String()) {
				return m, nil
			}
		}
	}

	switch msg.String() {
	case "ctrl+c":
		if m.streaming && m.streamCancel != nil {
			m.streamCancel()
			m.restoreQueuesToEditor()
			m.statusMsg = "cancelled"
			return m, nil
		}
		return m, tea.Quit
	case "ctrl+d":
		return m, tea.Quit
	case "esc":
		if m.streaming && m.streamCancel != nil {
			m.streamCancel()
			m.restoreQueuesToEditor()
			m.statusMsg = "cancelled"
			return m, nil
		}
		return m, tea.Quit
	case "enter", "ctrl+j", "ctrl+m":
		// Task 2.3: Enter accepts the highlighted completion when the
		// dropdown is open instead of submitting, so a /command or @file
		// can be picked with one key.
		if m.completionsOpen() {
			m.acceptCompletion()
			return m, nil
		}
		return m.submitInput()
	case "ctrl+p":
		// Task 2.4: cycle the active model forward through the scoped set.
		m.applyModelCycle(+1)
		return m, nil
	case "shift+ctrl+p", "ctrl+shift+p":
		// Cycle backward. Some terminals report the modifier order
		// differently, so accept both spellings.
		m.applyModelCycle(-1)
		return m, nil
	case "alt+enter":
		// Alt+Enter queues a FOLLOW-UP message (delivered only after the
		// whole run finishes). When idle it behaves like a normal submit.
		return m.queueFollowUp()
	case "backspace", "ctrl+h":
		// Plan 9's keyboard sends 0x08 for backspace which bubbletea
		// maps to "ctrl+h" (unix terminals send 0x7f → "backspace").
		// Accept both so backspace works regardless of host.
		if m.inputCursor > 0 {
			m.input = append(m.input[:m.inputCursor-1], m.input[m.inputCursor:]...)
			m.inputCursor--
		}
		m.refreshCompletions()
		return m, nil
	case "left":
		if m.inputCursor > 0 {
			m.inputCursor--
		}
		m.refreshCompletions()
		return m, nil
	case "right":
		if m.inputCursor < len(m.input) {
			m.inputCursor++
		}
		m.refreshCompletions()
		return m, nil
	case "home", "ctrl+a":
		m.inputCursor = 0
		m.refreshCompletions()
		return m, nil
	case "end", "ctrl+e":
		m.inputCursor = len(m.input)
		m.refreshCompletions()
		return m, nil
	case "ctrl+u":
		m.input = m.input[:0]
		m.inputCursor = 0
		m.clearCompletions()
		return m, nil

	// ----- Scrollback navigation -----
	// scrollOffset = rows scrolled UP from latest. View() does the
	// clamp + clipping; we just bump the offset here. New content
	// (chunkMsg/streamDoneMsg/AppendUser/AppendLocal) doesn't reset
	// the offset — the user is in control. ctrl+End or "end" returns
	// to live tail.
	case "pgup", "ctrl+b":
		// Page up = scroll up by (scrollH - 2) rows so we keep a bit
		// of overlap. scrollH approximately = m.height - 5.
		page := m.height - 7
		if page < 1 {
			page = 1
		}
		m.scrollOffset += page
		return m, nil
	case "pgdn", "ctrl+f":
		page := m.height - 7
		if page < 1 {
			page = 1
		}
		m.scrollOffset -= page
		if m.scrollOffset < 0 {
			m.scrollOffset = 0
		}
		return m, nil
	case "alt+up":
		// Alt+Up DEQUEUES the most recently queued (not-yet-sent) message
		// back into the editor for editing. When nothing is queued it falls
		// back to scrolling the scrollback one row (its prior behavior).
		if m.dequeueToEditor() {
			return m, nil
		}
		m.scrollOffset++
		return m, nil
	case "shift+up":
		m.scrollOffset++
		return m, nil
	case "shift+down", "alt+down":
		m.scrollOffset--
		if m.scrollOffset < 0 {
			m.scrollOffset = 0
		}
		return m, nil
	case "ctrl+end":
		// Jump to live tail.
		m.scrollOffset = 0
		return m, nil

	// ----- Thinking level -----
	// Shift+Tab cycles the requested thinking level; Ctrl+T toggles
	// whether streamed reasoning is shown. Neither sends a message —
	// the new level applies to the next request.
	case "shift+tab":
		old := m.thinkingLevel
		m.thinkingLevel = nextThinkingLevel(m.thinkingLevel)
		if m.tree != nil && m.thinkingLevel != old {
			m.tree.AppendThinkingChange(m.thinkingLevel)
			m.saveSession()
		}
		m.statusMsg = "thinking: " + m.thinkingLevel
		return m, nil
	case "ctrl+t":
		m.hideThinking = !m.hideThinking
		if m.hideThinking {
			m.statusMsg = "thinking hidden"
		} else {
			m.statusMsg = "thinking shown"
		}
		return m, nil
	}
	for _, r := range msg.Runes {
		if r < 0x20 {
			continue
		}
		m.input = append(m.input[:m.inputCursor], append([]rune{r}, m.input[m.inputCursor:]...)...)
		m.inputCursor++
	}
	// Task 2.3: re-evaluate the autocomplete dropdown after typing.
	m.refreshCompletions()
	return m, nil
}

// ---------- View rendering helpers ----------

// visibleWidth returns the number of terminal columns a string
// occupies, ignoring ANSI CSI/OSC escape sequences but counting
// every rune as one column (no East-Asian-Wide handling — pi9's
// content is ASCII-heavy and lipgloss-styled, so this is fine).
func visibleWidth(s string) int {
	w := 0
	for i := 0; i < len(s); i++ {
		if s[i] == 0x1b && i+1 < len(s) {
			// ANSI escape. Skip to the terminator.
			if s[i+1] == '[' {
				// CSI: skip until a final byte in 0x40..0x7e
				i += 2
				for ; i < len(s); i++ {
					if s[i] >= 0x40 && s[i] <= 0x7e {
						break
					}
				}
				continue
			}
			if s[i+1] == ']' {
				// OSC: skip until BEL or ST (\x1b\\)
				i += 2
				for ; i < len(s); i++ {
					if s[i] == 0x07 {
						break
					}
					if s[i] == 0x1b && i+1 < len(s) && s[i+1] == '\\' {
						i++
						break
					}
				}
				continue
			}
			// Other escapes (rare) — skip one byte after ESC.
			i++
			continue
		}
		// Skip continuation bytes of UTF-8 — count only leading bytes.
		if s[i] < 0x80 || (s[i]&0xc0) == 0xc0 {
			w++
		}
	}
	return w
}

// fitRow pads or truncates s so its visible width is exactly cols.
// Truncates by visible characters, preserving any ANSI styles up to
// the cut. Pads with spaces (with a final SGR reset so the styled
// region doesn't bleed into the pad).
func fitRow(s string, cols int) string {
	if cols <= 0 {
		return ""
	}
	cur := 0
	var b strings.Builder
	i := 0
	for i < len(s) {
		if s[i] == 0x1b && i+1 < len(s) {
			// Pass through ANSI escapes verbatim.
			start := i
			if s[i+1] == '[' {
				i += 2
				for ; i < len(s); i++ {
					if s[i] >= 0x40 && s[i] <= 0x7e {
						i++
						break
					}
				}
			} else if s[i+1] == ']' {
				i += 2
				for ; i < len(s); i++ {
					if s[i] == 0x07 {
						i++
						break
					}
					if s[i] == 0x1b && i+1 < len(s) && s[i+1] == '\\' {
						i += 2
						break
					}
				}
			} else {
				i += 2
			}
			b.WriteString(s[start:i])
			continue
		}
		if cur >= cols {
			// Drop the rest of this row but consume any remaining
			// ANSI escapes that might still affect output.
			break
		}
		// Walk one full UTF-8 rune.
		ch := s[i]
		size := 1
		switch {
		case ch&0x80 == 0:
			size = 1
		case ch&0xe0 == 0xc0:
			size = 2
		case ch&0xf0 == 0xe0:
			size = 3
		case ch&0xf8 == 0xf0:
			size = 4
		}
		if i+size > len(s) {
			size = len(s) - i
		}
		b.WriteString(s[i : i+size])
		i += size
		cur++
	}
	// Pad to width.
	if cur < cols {
		b.WriteString("\x1b[0m") // reset before padding
		b.WriteString(strings.Repeat(" ", cols-cur))
	}
	return b.String()
}

// fitBlock takes a multi-line block and enforces exactly `rows` rows
// of exactly `cols` columns each. Extra rows are dropped from the
// TOP (we want the bottom of long content visible). Extra cols are
// truncated. Short content is bottom-padded.
//
// Single newlines inside the block separate rows. Anything beyond
// `cols` on a single logical row gets truncated — we don't wrap.
// Pre-wrapped content (e.g. chat.Render output) already wrapped at
// `cols - 5` or similar, so this rarely truncates anything visible;
// it does prevent the terminal from re-wrapping.
func fitBlock(s string, rows, cols int) string {
	return fitBlockOffset(s, rows, cols, 0)
}

// fitBlockOffset is like fitBlock but takes the last `rows` lines
// starting at `offset` lines back from the END of s. offset=0 means
// "show the latest rows" (live tail). offset=5 means "show the last
// rows ending 5 lines before the end". Used for scrollback nav.
//
// If offset exceeds available history, clamps so the OLDEST rows
// fill the top — equivalent to "scroll all the way up".
func fitBlockOffset(s string, rows, cols, offset int) string {
	if rows <= 0 || cols <= 0 {
		return ""
	}
	lines := strings.Split(s, "\n")
	total := len(lines)

	// Window end = total - offset. If offset >= total - rows we're
	// past the top; clamp.
	end := total - offset
	if end < rows {
		end = rows
	}
	if end > total {
		end = total
	}
	start := end - rows
	if start < 0 {
		start = 0
		end = total
		if end > rows {
			end = rows
		}
	}
	lines = lines[start:end]

	// Pad to row count.
	for len(lines) < rows {
		lines = append(lines, "")
	}
	for i, l := range lines {
		lines[i] = fitRow(l, cols)
	}
	return strings.Join(lines, "\n")
}

// ---------- View rendering ----------

var (
	headerStyle = lipgloss.NewStyle().
			Bold(true).
			Foreground(lipgloss.Color("15")).
			Background(lipgloss.Color("4")). // accent bar
			Padding(0, 1)

	statusStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("8")).
			Italic(true)

	// Muted border so the rounded input box reads as a frame, not a
	// loud element, on the dark theme.
	inputBoxStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("8"))

	inputTextStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("15"))

	// Accent for the input prompt glyph + the streaming spinner.
	promptStyle = lipgloss.NewStyle().
			Bold(true).
			Foreground(lipgloss.Color("6")) // cyan

	hintStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("8"))
)

// renderInput draws a 3-row "bordered" input box at exactly `cols` wide.
//
// We use plain box-drawing characters and our own row-fitting instead
// of lipgloss.Border() — that path was the source of the stale-frame
// stacking in Phases 2-5, because lipgloss computes width from the
// raw string (including ANSI) and the box ended up wider than `cols`
// when the input was non-empty + styled, which caused vts/vtwin to
// wrap and "stack" a second row.
//
// Rows:
//
//	┌───────────────────────────────────────┐
//	│ > current input here, with cursor _   │
//	└───────────────────────────────────────┘
//
// All three rows are returned as one string with two embedded
// newlines. Each row is already fitted to exactly cols visible width.
func (m pi9Model) renderInput(cols int) string {
	if cols < 4 {
		// Tiny terminal — degrade gracefully.
		return strings.Repeat(" ", cols) + "\n" +
			strings.Repeat(" ", cols) + "\n" +
			strings.Repeat(" ", cols)
	}

	const (
		topLeft     = "╭"
		topRight    = "╮"
		botLeft     = "╰"
		botRight    = "╯"
		horiz       = "─"
		vert        = "│"
		leftMargin  = 2 // space after │ before content
		rightMargin = 2 // space before │ after content
	)

	// Inner content width = cols - vert(1) - leftMargin - rightMargin - vert(1)
	innerW := cols - 2 - leftMargin - rightMargin
	if innerW < 1 {
		innerW = 1
	}

	// Build content: "> typed_text_with_cursor"
	var content string
	if m.streaming {
		// Animated Braille spinner; the frame advances as the view
		// re-renders on each stream chunk (no separate tick needed).
		frames := []rune("⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏")
		sp := string(frames[(time.Now().UnixMilli()/100)%int64(len(frames))])
		content = promptStyle.Render(sp+" ") + hintStyle.Render("working... (ctrl-c to cancel)")
	} else {
		// Visible substring of input + cursor.
		text := string(m.input)
		cursorAt := m.inputCursor
		// Reserve 2 chars for "> ", 1 for cursor.
		visibleArea := innerW - 3
		if visibleArea < 1 {
			visibleArea = 1
		}
		// Window into the input string centered around the cursor.
		// If text fits, show it all. Otherwise slide the window so
		// the cursor is always visible.
		runes := []rune(text)
		start := 0
		end := len(runes)
		if end > visibleArea {
			// Try to keep cursor at position visibleArea-1 (rightmost
			// visible) so user sees what they just typed.
			start = cursorAt - (visibleArea - 1)
			if start < 0 {
				start = 0
			}
			end = start + visibleArea
			if end > len(runes) {
				end = len(runes)
				start = end - visibleArea
				if start < 0 {
					start = 0
				}
			}
		}
		// Render visible slice + cursor.
		var left, right string
		cur := cursorAt - start
		if cur < 0 {
			cur = 0
		}
		if cur > end-start {
			cur = end - start
		}
		left = string(runes[start : start+cur])
		right = string(runes[start+cur : end])
		content = promptStyle.Render("❯ ") + inputTextStyle.Render(left+"█"+right)
	}

	// Compute visible width of content. We need the content to be
	// EXACTLY innerW columns so the right vert lands at column cols-1.
	contentW := visibleWidth(content)
	if contentW < innerW {
		content = content + strings.Repeat(" ", innerW-contentW)
	} else if contentW > innerW {
		// Truncate ANSI-aware. fitRow does this.
		content = fitRow(content, innerW)
	}

	// Build the three rows. Each is exactly `cols` visible chars.
	top := inputBoxStyle.Render(topLeft + strings.Repeat(horiz, cols-2) + topRight)
	mid := inputBoxStyle.Render(vert) +
		strings.Repeat(" ", leftMargin-1) +
		content +
		strings.Repeat(" ", rightMargin-1) +
		inputBoxStyle.Render(vert)
	bot := inputBoxStyle.Render(botLeft + strings.Repeat(horiz, cols-2) + botRight)

	// fitRow handles any small width mismatch (e.g. ANSI escape counts
	// drifting) by truncating-or-padding to exactly cols.
	return fitRow(top, cols) + "\n" + fitRow(mid, cols) + "\n" + fitRow(bot, cols)
}

func (m pi9Model) View() string {
	if m.width == 0 || m.height == 0 {
		return "pi9 starting..."
	}

	// Phase 8 fix: vts auto-wraps when the cursor would land on
	// column m.width (after writing the last cell of the row). The
	// wrap triggers cellbuf_newline → if we're on the last row, the
	// buffer SCROLLS. Bubbletea's diff renderer assumes the cursor
	// stays put when it writes the last cell of the last row — and
	// when vts scrolls, everything shifts, then the next render
	// ends up at a wrong position. That's the "every keystroke
	// creates a new line" bug.
	//
	// Workaround: render to width-1 columns. Last column stays blank.
	// vts never auto-wraps. Slight visual loss; full functional win.
	usableW := m.width - 1
	if usableW < 1 {
		usableW = 1
	}

	// ----- Header -----
	headerText := fmt.Sprintf(" pi9 - %s ", m.model)
	header := headerStyle.Render(headerText)
	if m.inVts != "" {
		header += statusStyle.Render(fmt.Sprintf("  vts session %s  ", m.inVts))
	}
	// Show the session NAME if set, else the id. Names come from /name.
	label := m.sessionID
	if m.history.Name != "" {
		label = m.history.Name
	}
	if label != "" {
		header += statusStyle.Render(fmt.Sprintf("  %s  ", label))
	}
	// Task 1: context-usage indicator ("NN% ctx · 12.3k tok"). Always
	// visible in the header so the user can see how full the window is.
	if ctx := m.contextStatus(); ctx != "" {
		header += statusStyle.Render("  " + ctx + "  ")
	}
	header = fitRow(header, usableW)

	// ----- Scrollback -----
	const headerH, inputH, statusH = 1, 3, 1
	// Task 2.3: when the autocomplete dropdown is open it sits between the
	// scrollback and the input box, so reserve its rows from the
	// scrollback height (the total layout stays exactly m.height tall).
	dropdownH := 0
	if m.completionsOpen() {
		dropdownH = len(m.completeList)
		if dropdownH > 8 {
			dropdownH = 8
		}
	}
	scrollH := m.height - headerH - inputH - statusH - dropdownH
	if scrollH < 1 {
		scrollH = 1
	}
	scrollback := chat.Render(&m.history, usableW-2, m.hideThinking)
	scrollback = fitBlockOffset(scrollback, scrollH, usableW, m.scrollOffset)

	// Phase 12: model picker replaces the scrollback area entirely.
	// Header + status bar still visible. Input box hidden — picker
	// has its own search box at the top.
	if m.modelPickerOpen {
		pickerH := m.height - headerH - statusH
		if pickerH < 5 {
			pickerH = 5
		}
		picker := m.renderModelPicker(usableW, pickerH)

		var status string
		if m.statusMsg != "" {
			status = statusStyle.Render(" " + m.statusMsg)
		} else {
			status = statusStyle.Render(" /model - enter to select - esc to cancel")
		}
		status = fitRow(status, usableW)

		return header + "\n" + picker + "\n" + status
	}

	// Task 2.4: /scoped-models overlay replaces the scrollback area,
	// same as the model picker.
	if m.scopedModelsOpen {
		overlayH := m.height - headerH - statusH
		if overlayH < 5 {
			overlayH = 5
		}
		overlay := m.renderScopedModels(usableW, overlayH)
		var status string
		if m.statusMsg != "" {
			status = statusStyle.Render(" " + m.statusMsg)
		} else {
			status = statusStyle.Render(" /scoped-models - space to toggle - esc to close")
		}
		status = fitRow(status, usableW)
		return header + "\n" + overlay + "\n" + status
	}

	// Task 5: settings overlay replaces the scrollback area, same as
	// the model picker.
	if m.settingsOpen {
		overlayH := m.height - headerH - statusH
		if overlayH < 5 {
			overlayH = 5
		}
		overlay := m.renderSettings(usableW, overlayH)

		var status string
		if m.statusMsg != "" {
			status = statusStyle.Render(" " + m.statusMsg)
		} else {
			status = statusStyle.Render(" /settings - enter to change - esc to close")
		}
		status = fitRow(status, usableW)

		return header + "\n" + overlay + "\n" + status
	}

	// Task 2.1: tree-session overlays (/resume, /fork, /tree) replace the
	// scrollback area exactly like the model picker.
	if m.resumeOpen || m.forkOpen || m.treeOpen {
		overlayH := m.height - headerH - statusH
		if overlayH < 5 {
			overlayH = 5
		}
		var overlay, hint string
		switch {
		case m.resumeOpen:
			overlay = m.renderResume(usableW, overlayH)
			hint = " /resume - enter to load - esc to cancel"
		case m.forkOpen:
			overlay = m.renderFork(usableW, overlayH)
			hint = " /fork - enter to fork - esc to cancel"
		case m.treeOpen:
			overlay = m.renderTree(usableW, overlayH)
			hint = " /tree - enter to jump - esc to cancel"
		}
		var status string
		if m.statusMsg != "" {
			status = statusStyle.Render(" " + m.statusMsg)
		} else {
			status = statusStyle.Render(hint)
		}
		status = fitRow(status, usableW)
		return header + "\n" + overlay + "\n" + status
	}

	// ----- Input box -----
	// Input box: either the normal chat input, or the /login picker
	// overlay when we're in loginMode. Both are 3 rows tall so the
	// total layout doesn't shift.
	var input string
	if m.loginMode != loginModeOff {
		input = m.renderLoginPicker(usableW)
	} else {
		input = m.renderInput(usableW)
	}

	// ----- Status bar -----
	var status string
	if qs := m.queueStatus(); qs != "" && m.statusMsg == "" {
		status = statusStyle.Render(" " + qs)
	} else if m.statusMsg != "" {
		status = statusStyle.Render(" " + m.statusMsg)
	} else if m.scrollOffset > 0 {
		status = statusStyle.Render(fmt.Sprintf(" scrolled %d rows up - pgdn/ctrl+end to return", m.scrollOffset))
	} else {
		think := m.thinkingLevel
		if think == "" {
			think = "off"
		}
		hidden := ""
		if m.hideThinking {
			hidden = " (hidden)"
		}
		status = statusStyle.Render(fmt.Sprintf(" enter to send - /help - think:%s%s (shift+tab) - ctrl-c to quit", think, hidden))
	}
	status = fitRow(status, usableW)

	// Task 2.3: the autocomplete dropdown (when open) renders between the
	// scrollback and the input box. dropdownH rows were already reserved
	// from the scrollback height above.
	out := header + "\n" + scrollback + "\n"
	if dropdownH > 0 {
		out += m.renderCompletions(usableW, dropdownH) + "\n"
	}
	out += input + "\n" + status

	return out
}

// clipToBottom is no longer used (fitBlock handles trimming).
// Kept here as a comment for future-self in case we need fancier
// scrollback behavior (e.g. user-controlled scroll position).

// ---------- system prompt assembly ----------

// CLI system-prompt overrides, set once from --system-prompt /
// --append-system-prompt in main() (and mirrored into the headless
// dispatch). They are process-global because the override applies for the
// whole run, including system-prompt rebuilds triggered by /trust and
// /reload — matching pi, where the CLI flag replaces/extends the base
// prompt persistently rather than for a single message.
//
//	cliSystemPrompt       — when non-empty, REPLACES the base prompt
//	                        (and any SYSTEM.md override) entirely.
//	cliAppendSystemPrompt — when non-empty, appended after the base prompt
//	                        and the on-disk APPEND_SYSTEM.md text.
var (
	cliSystemPrompt       string
	cliAppendSystemPrompt string
)

// buildSystemPrompt assembles the system prompt, mirroring pi's
// resource-loader layering:
//
//  1. Base: --system-prompt (CLI override) when set, else
//     store.SystemOverride(cwd,trusted) (SYSTEM.md) when present,
//     else baseSystemPrompt (the always-on instructions).
//  2. + store.AppendSystem(cwd,trusted) (APPEND_SYSTEM.md text) and the
//     --append-system-prompt CLI text.
//  3. + store.LoadContextFiles(cwd,trusted) (AGENTS.md/CLAUDE.md, already
//     wrapped in <project_context> blocks).
//  4. + memory.md (declarative facts about user + environment).
//  5. + store.FormatSkillsXML(store.ListSkillsFor(cwd,trusted)) (the
//     model-facing <available_skills> index; full body via read_skill).
//  6. + the current date (ISO yyyy-mm-dd) and working directory.
//
// Project-local resources (SYSTEM.md, skills, AGENTS.md under cwd) are
// honored only when the project directory is trusted; global resources
// (under store.Home()) always apply. Missing/empty sources are omitted.
func buildSystemPrompt() string {
	cwd, err := os.Getwd()
	if err != nil {
		cwd = "."
	}
	trusted := store.IsTrusted(cwd)

	var b strings.Builder
	switch {
	case cliSystemPrompt != "":
		// CLI override wins over SYSTEM.md and the built-in base.
		b.WriteString(cliSystemPrompt)
	default:
		if override, ok := store.SystemOverride(cwd, trusted); ok {
			b.WriteString(override)
		} else {
			b.WriteString(baseSystemPrompt)
		}
	}

	if app := strings.TrimSpace(store.AppendSystem(cwd, trusted)); app != "" {
		b.WriteString("\n\n")
		b.WriteString(app)
	}

	if app := strings.TrimSpace(cliAppendSystemPrompt); app != "" {
		b.WriteString("\n\n")
		b.WriteString(app)
	}

	if ctx := strings.TrimSpace(store.LoadContextFiles(cwd, trusted)); ctx != "" {
		b.WriteString("\n\n")
		b.WriteString(ctx)
	}

	mem, _ := store.LoadMemory()
	mem = strings.TrimSpace(mem)
	if mem != "" {
		b.WriteString("\n\n══ Memory (long-term facts about user + environment) ══\n")
		b.WriteString(mem)
	}

	if skillsXML := store.FormatSkillsXML(store.ListSkillsFor(cwd, trusted)); skillsXML != "" {
		b.WriteString("\n\n══ Skills (call read_skill(name) to load) ══\n")
		b.WriteString(skillsXML)
	}

	b.WriteString("\n\n")
	b.WriteString(fmt.Sprintf("Today's date is %s.\n", time.Now().Format("2006-01-02")))
	b.WriteString(fmt.Sprintf("Current working directory: %s\n", cwd))

	return b.String()
}

// ---------- CLI flag helpers (Task 3.3) ----------

// applyModelProviderOverride resolves the effective model id (and an
// optional thinking level) from the --model and --provider flags.
// Precedence:
//
//   - modelFlag, when set, replaces the configured default model.
//   - A trailing ":LEVEL" on the (effective) model id, where LEVEL is a
//     recognized thinking level (off/minimal/low/medium/high/xhigh), is
//     parsed off and returned as level; the id keeps the part before it.
//     Any other suffix (e.g. OpenRouter's ":exacto") is left intact.
//   - providerFlag, when set, scopes the model to that provider. If the
//     (effective) model already carries a "provider/" prefix it is left
//     alone; otherwise the provider is prepended as "provider/model" so
//     provider.ProviderForModel routes it correctly. A bare --provider
//     with no model leaves the model untouched (the configured default's
//     own provider still applies).
//
// Pure: no I/O, easy to test. base is the already-resolved default model.
// level is "" when no valid thinking-level suffix was present.
func applyModelProviderOverride(base, modelFlag, providerFlag string) (model, level string) {
	model = base
	if m := strings.TrimSpace(modelFlag); m != "" {
		model = m
	}
	// Split on the LAST colon so OpenRouter provider/model qualifiers and
	// suffixes survive; only strip the suffix when it names a thinking
	// level (preserve e.g. "model:exacto").
	if i := strings.LastIndex(model, ":"); i >= 0 {
		if suffix := model[i+1:]; isThinkingLevel(suffix) {
			level = suffix
			model = model[:i]
		}
	}
	prov := strings.TrimSpace(providerFlag)
	if prov == "" {
		return model, level
	}
	// Already provider-qualified? Leave it. We treat any "/" as a
	// vendor/model qualifier (the OpenRouter convention pi shares).
	if strings.Contains(model, "/") {
		return model, level
	}
	if model == "" {
		return model, level
	}
	return prov + "/" + model, level
}

// mostRecentSession returns the newest session id for -c/--continue, or
// "" when there are no saved sessions. store.ListSessions already returns
// ids newest-first by mtime, so this is just the head — factored out as a
// named helper so the precedence is obvious and unit-testable via the
// injected lister.
func mostRecentSession(list func() ([]string, error)) string {
	ids, err := list()
	if err != nil || len(ids) == 0 {
		return ""
	}
	return ids[0]
}

// ---------- main ----------

func main() {
	// Force lipgloss to emit ANSI 256-color SGR escapes regardless of
	// $TERM. Vts doesn't set $TERM, so termenv's default detection
	// falls back to NoColor and strips everything — making pi9 look
	// like a plain monochrome shell. Pinning the profile fixes that.
	lipgloss.SetColorProfile(termenv.ANSI256)

	var (
		flagNew       = flag.Bool("new", false, "start a fresh session (do not resume)")
		flagSession   = flag.String("session", "", "load a specific session by id")
		flagThinking  = flag.String("thinking", "off", "thinking level: off, minimal, low, medium, high, xhigh")
		flagApprove   = flag.Bool("approve", false, "trust this project directory for this run (load project-local .pi/ resources)")
		flagNoApprove = flag.Bool("no-approve", false, "do not trust this project directory for this run (override a stored 'always')")
		flagModels    = flag.String("models", "", "comma-separated model patterns to seed the Ctrl+P cycle set (/scoped-models)")
		// Task 3.2 headless modes. -p/--print prints the answer and exits;
		// --mode json streams JSONL events; both skip the TUI entirely.
		flagPrint     = flag.Bool("p", false, "print mode: run the prompt non-interactively and exit (also -print)")
		flagPrintLong = flag.Bool("print", false, "alias for -p")
		flagMode      = flag.String("mode", "", "headless output mode: json (JSONL events) or rpc (not implemented)")
		flagNoSession = flag.Bool("no-session", false, "ephemeral run: do not persist the session")
		// Task 3.3 model/provider overrides.
		flagModel    = flag.String("model", "", "override the default model (pattern or provider/id)")
		flagProvider = flag.String("provider", "", "override the provider (anthropic, openai, openrouter, ...)")
		// Task 3.3 session selection. -c continues the most recent session;
		// -r opens the /resume picker at startup. Both have long aliases.
		flagContinue     = flag.Bool("c", false, "continue the most recent session (also -continue)")
		flagContinueLong = flag.Bool("continue", false, "alias for -c")
		flagResume       = flag.Bool("r", false, "open the /resume picker at startup (also -resume)")
		flagResumeLong   = flag.Bool("resume", false, "alias for -r")
		// Task 3.3 system-prompt overrides.
		flagSystemPrompt       = flag.String("system-prompt", "", "replace the base system prompt with this text")
		flagAppendSystemPrompt = flag.String("append-system-prompt", "", "append this text to the base system prompt")
	)
	flag.Parse()

	// Task 3.3: wire the system-prompt overrides into buildSystemPrompt
	// before ANY prompt is assembled (headless dispatch below and the TUI
	// path both call buildSystemPrompt). Process-global by design — the
	// override holds for the whole run, including /trust and /reload
	// rebuilds.
	cliSystemPrompt = *flagSystemPrompt
	cliAppendSystemPrompt = *flagAppendSystemPrompt

	// Long-form aliases fold into the canonical bool flags so the rest of
	// main() only checks one variable each.
	continueSession := *flagContinue || *flagContinueLong
	resumeAtStartup := *flagResume || *flagResumeLong

	// Task 3.2: headless dispatch. If a non-interactive mode is requested
	// (-p/--print or --mode), run it to completion and os.Exit BEFORE any
	// TUI / vts raw-mode setup. Config/auth/model resolution below is
	// shared, so we load just enough here first.
	if *flagPrint || *flagPrintLong || *flagMode != "" {
		if err := store.EnsureHome(); err != nil {
			fmt.Fprintf(os.Stderr, "pi9: ensure home: %v\n", err)
		}
		_, _ = store.WriteTemplate()
		hcfg, err := store.LoadConfig()
		if err != nil {
			fmt.Fprintf(os.Stderr, "pi9: load config: %v\n", err)
			os.Exit(1)
		}
		if hcfg.SSLCertFile != "" && os.Getenv("SSL_CERT_FILE") == "" {
			_ = os.Setenv("SSL_CERT_FILE", hcfg.SSLCertFile)
		}
		if hcfg.APIURL != "" && os.Getenv("OPENROUTER_API_URL") == "" {
			_ = os.Setenv("OPENROUTER_API_URL", hcfg.APIURL)
		}
		hmodel := hcfg.Model
		if hmodel == "" {
			hmodel = defaultModel
		}
		var hmodelLevel string
		hmodel, hmodelLevel = applyModelProviderOverride(hmodel, *flagModel, *flagProvider)
		hthinking := "off"
		for _, l := range thinkingLevels {
			if l == *flagThinking {
				hthinking = l
				break
			}
		}
		// A ":LEVEL" shorthand on --model wins over a default --thinking, but
		// an explicit --thinking flag still takes precedence.
		if hmodelLevel != "" && *flagThinking == "off" {
			hthinking = hmodelLevel
		}
		hcwd, _ := os.Getwd()
		hsession := *flagSession
		if hsession == "" {
			hsession = "ephemeral"
		}
		prompt := resolveHeadlessPrompt("", flag.Args(), os.Stdin, stdinIsPipe())
		os.Exit(dispatchHeadless(context.Background(), headlessOptions{
			print:     *flagPrint || *flagPrintLong,
			mode:      *flagMode,
			prompt:    prompt,
			model:     hmodel,
			apiKey:    hcfg.APIKey,
			thinking:  hthinking,
			sessionID: hsession,
			cwd:       hcwd,
			noSession: *flagNoSession,
		}))
	}

	if os.Getenv("vts") != "" {
		_ = os.MkdirAll("/n/vts", 0755)
		_ = exec.Command("/bin/mount", "/srv/vts", "/n/vts").Run()
		setVtsRaw(true)
		defer setVtsRaw(false)
	}

	if err := store.EnsureHome(); err != nil {
		fmt.Fprintf(os.Stderr, "pi9: ensure home: %v\n", err)
	}

	// Load config (file + env vars merged, env wins).
	// On first launch with no config file, write a template so the
	// user has something to edit. Exit cleanly with a pointer to it
	// so the error story is helpful, not cryptic.
	wrote, _ := store.WriteTemplate()
	cfg, err := store.LoadConfig()
	if err != nil {
		fmt.Fprintf(os.Stderr, "pi9: load config: %v\n", err)
		os.Exit(1)
	}
	// First launch with no config file: write template, exit so the
	// user can edit it before the TUI starts (no point launching a
	// chat UI before there's even a config path to /login into).
	if wrote && cfg.APIKey == "" {
		fmt.Fprintf(os.Stderr,
			"pi9: first launch - wrote template to %s\n"+
				"     run pi9 again to start the chat.\n"+
				"     then in pi9: /login sk-or-v1-...\n"+
				"     get an OpenRouter key at https://openrouter.ai/keys\n",
			store.ConfigPath())
		os.Exit(2)
	}
	// Subsequent launches: API key may be empty (e.g. after /logout
	// or fresh install). Launch into the TUI anyway so the user can
	// /login from inside. Streaming will fail with a clear error
	// until a key is set.
	//
	// noKeyYet is surfaced as the startup status message so the user
	// sees "API key not set - use /login" right away.
	noKeyYet := cfg.APIKey == ""

	// SSL_CERT_FILE is honored by our provider package via os.Getenv,
	// so propagate from config to env if the env var isn't already set
	// (env wins, but if env is unset and config has it, use that).
	if cfg.SSLCertFile != "" && os.Getenv("SSL_CERT_FILE") == "" {
		_ = os.Setenv("SSL_CERT_FILE", cfg.SSLCertFile)
	}
	// Same for OPENROUTER_API_URL (provider reads it via env).
	if cfg.APIURL != "" && os.Getenv("OPENROUTER_API_URL") == "" {
		_ = os.Setenv("OPENROUTER_API_URL", cfg.APIURL)
	}

	model := cfg.Model
	if model == "" {
		model = defaultModel
	}
	// Task 3.3: --model / --provider override the configured default.
	var modelLevel string
	model, modelLevel = applyModelProviderOverride(model, *flagModel, *flagProvider)
	apiKey := cfg.APIKey

	// Task 6: trust resolution. -approve / -no-approve persist a trust
	// decision for the cwd BEFORE the system prompt is assembled (the
	// prompt only pulls project-local .pi/ resources when trusted).
	// -no-approve wins if both are passed.
	cwd, _ := os.Getwd()
	switch {
	case *flagNoApprove:
		_ = store.SetTrust(cwd, store.TrustNever)
	case *flagApprove:
		_ = store.SetTrust(cwd, store.TrustAlways)
	}
	trusted := store.IsTrusted(cwd)

	// Decide which session to use.
	//
	// Precedence (highest first): --session <id> wins; then -c/--continue
	// selects the newest saved session; then -new forces a brand-new one
	// (handled below); otherwise we resume the last "current" session.
	// --no-session makes the run ephemeral: an in-memory session that is
	// never persisted (saveSession becomes a no-op for it).
	sessionID := *flagSession
	if sessionID == "" && continueSession {
		sessionID = mostRecentSession(store.ListSessions)
	}
	if sessionID == "" && !*flagNew {
		sessionID = store.CurrentSessionID()
	}

	// Build a fresh history with the assembled system prompt.
	systemPrompt := buildSystemPrompt()

	// Load (or create) the session TREE for this id. loadSessionTree
	// prefers the new .jsonl format, migrates a legacy .json on the fly,
	// and falls back to an empty tree when nothing is on disk.
	var tree *chat.SessionTree
	if sessionID != "" {
		tree = loadSessionTree(sessionID, cwd, systemPrompt)
	}

	// If we don't have a session id yet (either -new, no prior, or
	// load failed), allocate one and persist as current.
	if sessionID == "" || *flagNew {
		sessionID = store.NewSessionID()
		tree = nil
	}
	if tree == nil {
		tree = chat.NewSessionTree(sessionID, cwd, systemPrompt)
	}
	tree.ID = sessionID
	tree.System = systemPrompt

	// Materialize the active branch into the flat History the UI uses.
	history := *tree.Materialize()
	history.System = systemPrompt

	// --no-session: ephemeral run, so don't mark this id as the current
	// session on disk (nothing about this run should persist).
	if !*flagNoSession {
		_ = store.SetCurrentSession(sessionID)
	}

	// Validate the requested thinking level; fall back to "off" on a
	// bad value so a typo doesn't silently send something unexpected.
	thinking := "off"
	for _, l := range thinkingLevels {
		if l == *flagThinking {
			thinking = l
			break
		}
	}
	// A ":LEVEL" shorthand on --model (e.g. "sonnet:high") sets the initial
	// thinking level, unless an explicit --thinking flag overrides it.
	if modelLevel != "" && *flagThinking == "off" {
		thinking = modelLevel
	}

	m := pi9Model{
		inVts:         os.Getenv("vts"),
		apiKey:        apiKey,
		model:         model,
		tools:         tools.Schemas(),
		history:       history,
		tree:          tree,
		sessionID:     sessionID,
		thinkingLevel: thinking,
		trusted:       trusted,
		noSession:     *flagNoSession,
		startupResume: resumeAtStartup,
		// Positional args in interactive mode become the initial prompt
		// ("pi @file.md ... [prompt]"). submitInput() @file-expands it, so
		// we stash the RAW joined args (mentions intact) here.
		startupPrompt:    strings.TrimSpace(strings.Join(flag.Args(), " ")),
		autoCompact:      true, // on by default, matching pi
		reserveTokens:    defaultReserveTokens,
		keepRecentTokens: defaultKeepRecentTokens,
		// Task 2.4: seed the Ctrl+P cycle set from --models. The curated
		// catalog backs both seeding and the /scoped-models overlay; the
		// live OpenRouter list merges in when the overlay is opened.
		modelList:     provider.CuratedModels(),
		enabledModels: seedEnabledModels(*flagModels, provider.CuratedModels()),
	}
	if noKeyYet {
		m.statusMsg = "no API key set - run /login <key> to set one"
	} else if !trusted && store.HasProjectResources(cwd) {
		// Task 6: one-line hint when untrusted project-local .pi/
		// resources exist but aren't loaded.
		m.statusMsg = "untrusted project .pi/ resources found - run /trust to load them"
	}

	p := tea.NewProgram(
		m,
		tea.WithInput(os.Stdin),
		tea.WithOutput(os.Stdout),
		tea.WithAltScreen(),
		tea.WithoutSignalHandler(),
		// Phase 10 polish: enable mouse so vtwin's SGR scroll-wheel
		// sequences (ESC [ < 64/65 ; X ; Y M) reach us as MouseMsg
		// events. We handle MouseWheelUp/Down by scrolling the
		// scrollback overlay.
		tea.WithMouseCellMotion(),
	)

	teaSendFn = p.Send

	// DECAWM off: when the cursor would advance past the last column,
	// vts now sticks at the last column instead of wrapping. This
	// kills the "writing the last cell of the last row scrolls the
	// whole buffer" interaction that was breaking bubbletea's diff
	// renderer. Restored on exit.
	//
	// On a vts that doesn't support DECAWM (pre-Phase 9), this
	// escape is silently ignored — pi9 falls back to the width-1
	// workaround handled in View().
	_, _ = os.Stdout.WriteString("\x1b[?7l")
	defer os.Stdout.WriteString("\x1b[?7h")

	final, err := p.Run()
	if err != nil {
		fmt.Fprintf(os.Stderr, "pi9: %v\n", err)
		os.Exit(1)
	}

	// Final save on clean exit (the in-Update saves cover most cases, but
	// cancellation during a stream could otherwise lose state). Persist the
	// FINAL model — m.tree / m.sessionID / m.history may have changed during
	// the session (/resume, /new, /tree, /fork), so the startup locals are
	// stale; saving them would resurrect an old branch or move the leaf.
	if fm, ok := final.(pi9Model); ok {
		fm.saveSession()
	}
}

// setVtsRaw toggles raw mode + line-editor on the vts session this
// process runs inside. No-op outside vts.
func setVtsRaw(on bool) {
	s := os.Getenv("vts")
	if s == "" {
		return
	}
	ctl := "/n/vts/" + s + "/ctl"
	f, err := os.OpenFile(ctl, os.O_WRONLY, 0)
	if err != nil {
		return
	}
	defer f.Close()
	if on {
		_, _ = f.WriteString("rawon\n")
		_, _ = f.WriteString("edit off\n")
	} else {
		_, _ = f.WriteString("rawoff\n")
		_, _ = f.WriteString("edit on\n")
	}
}
