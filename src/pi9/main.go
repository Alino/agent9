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
  read_file(path)         read a file
  write_file(path,content) write a file
  run_rc(command)         run a shell command (rc on plan9, sh elsewhere)
  remember(content)       save a durable fact to long-term memory
  read_skill(name)        load detailed instructions for a named skill

Tools (Plan 9-native — use these when relevant, no other agent can):
  plumb(port,content)     route text via plumber (port: edit/web/image/…)
  hget(url)               fetch URL via plan9's native HTTP client
  walk(path,depth)        recursive directory listing
  ns(filter)              dump current namespace (per-process!)
  bind(src,dst,flag)      bind path into your own namespace
  mount(srv,mountpoint)   mount /srv/X file into your namespace

When the user asks you to do something concrete, just do it: call
tools, observe results, report back briefly. When the user just
chats, chat back.

On Plan 9 the shell is rc — use rc syntax, not bash. e.g. ` + "`" + `for(f in *)` + "`" + ` not ` + "`" + `for f in *` + "`" + `, no $() or backticks for command substitution.`

	defaultModel = "moonshotai/kimi-k2.5"
	maxTurnLoops = 10
)

// ---------- Bubble Tea model ----------

type pi9Model struct {
	width, height int
	inVts         string

	history chat.History

	input        []rune
	inputCursor  int
	streaming    bool
	streamCancel context.CancelFunc

	statusMsg string

	apiKey    string
	model     string
	tools     []provider.Tool
	sessionID string

	scrollOffset int

	// Phase 10 login picker state. inputMode flips between normal
	// chat input and provider/key picker overlays for /login.
	loginMode    loginMode
	loginCursor  int             // which provider is highlighted
	loginPicked  provider.ProviderID // chosen provider; we're now in key-entry

	// Model picker state (Phase 12). Activated by /model with no
	// args. modelList is the merged curated+live list; modelQuery
	// is the fuzzy search input; modelCursor is which row is
	// highlighted (within the filtered subset).
	modelPickerOpen bool
	modelList       []provider.ModelInfo
	modelQuery      []rune
	modelQueryCur   int
	modelCursor     int
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

func (m pi9Model) Init() tea.Cmd { return nil }

// ---------- streaming plumbing ----------

type chunkMsg struct{ delta string }

// Phase 10 S2 OAuth messages. Sent from the OAuth login goroutine
// back to the bubbletea Update loop so the picker UI can react.
//
//   oauthURLMsg     - URL is built + callback server is up; show URL in status
//   oauthDoneMsg    - login completed (success or error). UI returns to chat
//                     and shows result as a local turn.
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

func runStream(ctx context.Context, apiKey, model string, toolList []provider.Tool, msgs []provider.Message) tea.Cmd {
	return func() tea.Msg {
		// Phase 10: pick the provider for this model. If the user
		// has a key for it in auth.json, use that — otherwise fall
		// back to the apiKey passed in (legacy config.api_key, set
		// at startup from the OpenRouter slot).
		providerID := provider.ProviderForModel(model)

		// Phase 10 S2: if this provider has an OAuth entry and the
		// token is expired (or near-expired), refresh BEFORE making
		// the request. The refresh hits the token endpoint
		// synchronously — adds 1-2s to the first message after a
		// long pause but is invisible to the user otherwise.
		key := refreshIfNeeded(ctx, providerID)
		if key == "" {
			key = store.LookupAPIKey(string(providerID))
		}
		if key == "" {
			key = apiKey // legacy fallback
		}
		if key == "" {
			return streamDoneMsg{err: fmt.Errorf("no credentials for %q. run /login to add", providerID)}
		}

		// Phase 10 S4: when ProviderOpenAI is in use AND the auth
		// entry is OAuth (ChatGPT Plus/Pro), dispatch to the Codex
		// Responses API provider instead of the standard OpenAI
		// Chat Completions provider. The Responses API needs the
		// chatgpt-account-id header — we smuggle it through the
		// Config.APIURL field with a "codex:" prefix.
		var impl provider.Provider
		cfg := provider.Config{
			APIKey:    key,
			Model:     model,
			MaxTokens: 4096,
			Tools:     toolList,
		}
		if providerID == provider.ProviderOpenAI {
			if entry, ok := store.LookupAuthEntry(string(provider.ProviderOpenAI)); ok && entry.Type == "oauth" {
				// OAuth-authed OpenAI = Codex Responses API.
				impl = provider.GetCodexResponses()
				cfg.APIURL = "codex:" + entry.AccountID
			}
		}
		if impl == nil {
			impl = provider.Get(providerID)
		}
		if impl == nil {
			return streamDoneMsg{err: fmt.Errorf("provider %q is not implemented in this build", providerID)}
		}

		chunks, errs := impl.Stream(ctx, cfg, msgs)
		var toolCalls []provider.ToolCall
		for {
			select {
			case <-ctx.Done():
				return streamDoneMsg{err: ctx.Err()}
			case c, ok := <-chunks:
				if !ok {
					select {
					case e := <-errs:
						return streamDoneMsg{err: e, toolCalls: toolCalls}
					default:
						return streamDoneMsg{toolCalls: toolCalls}
					}
				}
				if c.Delta != "" {
					teaSendFn(chunkMsg{delta: c.Delta})
				}
				if c.Done {
					toolCalls = c.ToolCalls
				}
			case e := <-errs:
				if e != nil {
					return streamDoneMsg{err: e, toolCalls: toolCalls}
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

// saveSession writes the current history to disk. Best-effort: errors
// are swallowed because we don't want autosave failures crashing the
// agent loop. The user sees them via the status bar instead.
func (m *pi9Model) saveSession() {
	if m.sessionID == "" {
		return
	}
	if err := store.SaveAny(m.sessionID, m.history); err != nil {
		m.statusMsg = "save failed: " + err.Error()
		return
	}
}

// ---------- input handling ----------

func (m *pi9Model) submitInput() (tea.Model, tea.Cmd) {
	text := strings.TrimSpace(string(m.input))
	if text == "" {
		return m, nil
	}
	if m.streaming {
		return m, nil
	}
	m.input = m.input[:0]
	m.inputCursor = 0

	// Slash command? Handle locally without bothering the LLM.
	if strings.HasPrefix(text, "/") {
		return m.handleSlash(text)
	}

	m.history.AppendUser(text)
	m.saveSession()
	return m.beginStream()
}

// handleSlash dispatches a /command. Returns a tea.Cmd; the command
// itself is rendered as a Local turn in the history.
//
// Convention: every slash command appends a Local turn so the user
// has a record of what they ran and what came back. Local turns are
// excluded from ToProviderMessages.
func (m *pi9Model) handleSlash(text string) (tea.Model, tea.Cmd) {
	parts := strings.Fields(text)
	cmd := strings.TrimPrefix(parts[0], "/")
	args := parts[1:]

	switch cmd {
	case "help", "?", "hotkeys":
		m.history.AppendLocal(text, slashHelp())
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
		// Drop all turns but keep the system prompt.
		m.history.Turns = nil
		m.history.AppendLocal(text, "history cleared (system prompt + memory + skills intact).")
		m.statusMsg = ""
		m.saveSession()
		return m, nil

	case "new":
		// Start a fresh session, abandoning the current one. The old
		// session JSON stays on disk; only `current` pointer moves.
		newID := store.NewSessionID()
		m.history.Turns = nil
		m.history.Name = ""
		m.sessionID = newID
		_ = store.SetCurrentSession(newID)
		m.history.AppendLocal(text, "started new session "+newID)
		m.saveSession()
		return m, nil

	case "name":
		// Set the session's display name. Persists to JSON.
		if len(args) == 0 {
			cur := m.history.Name
			if cur == "" {
				cur = "(none — using session id "+m.sessionID+")"
			}
			m.history.AppendLocal(text, "current name: "+cur+"\nusage: /name <new name>")
		} else {
			n := strings.Join(args, " ")
			m.history.Name = n
			m.history.AppendLocal(text, "session name: "+n)
		}
		m.saveSession()
		return m, nil

	case "save":
		m.saveSession()
		m.history.AppendLocal(text, "saved to "+store.SessionPath(m.sessionID))
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
		fmt.Fprintf(&b, "path:     %s\n", store.SessionPath(m.sessionID))
		m.history.AppendLocal(text, b.String())
		m.saveSession()
		return m, nil

	case "sessions", "resume":
		// List sessions; "/resume" is pi-compatible alias.
		// True /resume picks a session interactively — we don't have
		// a picker yet, so we list and tell the user to relaunch
		// with -session <id>.
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
			if cmd == "resume" {
				b.WriteString("\nrelaunch pi9 with -session <id> to resume one")
			}
			body = b.String()
		}
		m.history.AppendLocal(text, body)
		m.saveSession()
		return m, nil

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
			body, err := store.ReadSkillBody(args[0])
			if err != nil {
				m.history.AppendLocal(text, "error: "+err.Error())
			} else {
				m.history.AppendLocal(text, body)
			}
		}
		m.saveSession()
		return m, nil

	case "config", "settings":
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
		m.history.AppendLocal(text, fmt.Sprintf("model: %s -> %s", old, m.model))
		m.saveSession()
		return m, nil

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
		// Manually drop the oldest half of turns. Pi's /compact
		// actually asks the model to summarize them; we'd need an
		// extra round-trip with the provider. For now we just trim,
		// which gets the user out of "running out of context" without
		// needing a summarization call.
		//
		// Future: send turns[:N] to provider with "summarize" prompt,
		// replace with synthetic assistant turn containing the summary.
		n := len(m.history.Turns)
		if n < 4 {
			m.history.AppendLocal(text, fmt.Sprintf("only %d turns - too few to compact", n))
		} else {
			drop := n / 2
			m.history.Turns = m.history.Turns[drop:]
			m.history.AppendLocal(text, fmt.Sprintf("dropped oldest %d turns; %d remain", drop, len(m.history.Turns)))
		}
		m.saveSession()
		return m, nil

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
//   loginModePicker   ↑↓ to navigate, enter to pick, esc to cancel
//   loginModeKeyEntry typing into m.input, enter to save, esc back
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
		m.modelPickerOpen = false
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
//   +-----------------------------------------+
//   | /model: search... [42 / 200 matches]    |
//   +-----------------------------------------+
//   | > anthropic/claude-sonnet-4.5  200K    |
//   |   openai/gpt-5                          |
//   |   ... visible rows fitted to height     |
//   +-----------------------------------------+
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
			marker = "★"
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
	footer := " ↑↓ nav · enter select · type filter · esc cancel"
	out = append(out, lipgloss.NewStyle().
		Foreground(lipgloss.Color("8")).
		Render(fitRow(footer, cols)))

	return strings.Join(out, "\n")
}

// renderLoginPicker renders the picker overlay in place of the input
// box. Four modes:
//
//   loginModePicker:       provider list
//   loginModeAuthMethod:   subscription vs api key choice
//   loginModeKeyEntry:     masked key input box
//   loginModeOAuthRunning: progress message ("waiting for browser...")
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
    /resume            list sessions + how to load one
    /clear             clear conversation but keep this session
    /save              force-save current session
    /export [path]     write transcript to a file
    /compact           trim oldest half of turns

  content
    /memory            show $home/lib/pi9/memory.md     (pi9-specific)
    /skill [name]      list / show a named skill         (pi9-specific)
    /copy              copy last assistant message to /dev/snarf
    /reload            reload config + memory + skills

  settings
    /model [name]      show or switch model
    /config            show resolved config (aliased: /settings)
    /hotkeys           same as /help

  exit
    /quit              (also: /q, /exit, ctrl-c, ctrl-d)

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
	m.streaming = true
	m.statusMsg = "thinking..."

	ctx, cancel := context.WithCancel(context.Background())
	m.streamCancel = cancel
	msgs := m.history.ToProviderMessages()
	return m, runStream(ctx, m.apiKey, m.model, m.tools, msgs)
}

// ---------- Update ----------

func (m pi9Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
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
		return m, nil
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

	switch msg.String() {
	case "ctrl+c":
		if m.streaming && m.streamCancel != nil {
			m.streamCancel()
			m.statusMsg = "cancelled"
			return m, nil
		}
		return m, tea.Quit
	case "ctrl+d":
		return m, tea.Quit
	case "esc":
		if m.streaming && m.streamCancel != nil {
			m.streamCancel()
			m.statusMsg = "cancelled"
			return m, nil
		}
		return m, tea.Quit
	case "enter", "ctrl+j", "ctrl+m":
		return m.submitInput()
	case "backspace", "ctrl+h":
		// Plan 9's keyboard sends 0x08 for backspace which bubbletea
		// maps to "ctrl+h" (unix terminals send 0x7f → "backspace").
		// Accept both so backspace works regardless of host.
		if m.inputCursor > 0 {
			m.input = append(m.input[:m.inputCursor-1], m.input[m.inputCursor:]...)
			m.inputCursor--
		}
		return m, nil
	case "left":
		if m.inputCursor > 0 {
			m.inputCursor--
		}
		return m, nil
	case "right":
		if m.inputCursor < len(m.input) {
			m.inputCursor++
		}
		return m, nil
	case "home", "ctrl+a":
		m.inputCursor = 0
		return m, nil
	case "end", "ctrl+e":
		m.inputCursor = len(m.input)
		return m, nil
	case "ctrl+u":
		m.input = m.input[:0]
		m.inputCursor = 0
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
	case "shift+up", "alt+up":
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
	}
	for _, r := range msg.Runes {
		if r < 0x20 {
			continue
		}
		m.input = append(m.input[:m.inputCursor], append([]rune{r}, m.input[m.inputCursor:]...)...)
		m.inputCursor++
	}
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
			Background(lipgloss.Color("4")). // Luna blue
			Padding(0, 1)

	statusStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("8")).
			Italic(true)

	inputBoxStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("4")) // Luna blue border color

	inputTextStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("15"))

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
		topLeft     = "+"
		topRight    = "+"
		botLeft     = "+"
		botRight    = "+"
		horiz       = "-"
		vert        = "|"
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
		content = "> " + hintStyle.Render("working... (ctrl-c to cancel)")
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
		content = "> " + inputTextStyle.Render(left+"_"+right)
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
	header = fitRow(header, usableW)

	// ----- Scrollback -----
	const headerH, inputH, statusH = 1, 3, 1
	scrollH := m.height - headerH - inputH - statusH
	if scrollH < 1 {
		scrollH = 1
	}
	scrollback := chat.Render(&m.history, usableW-2)
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
			status = statusStyle.Render(" /model · enter to select · esc to cancel")
		}
		status = fitRow(status, usableW)

		return header + "\n" + picker + "\n" + status
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
	if m.statusMsg != "" {
		status = statusStyle.Render(" " + m.statusMsg)
	} else if m.scrollOffset > 0 {
		status = statusStyle.Render(fmt.Sprintf(" scrolled %d rows up - pgdn/ctrl+end to return", m.scrollOffset))
	} else {
		status = statusStyle.Render(" enter to send · /help · ctrl-c to quit")
	}
	status = fitRow(status, usableW)

	out := header + "\n" + scrollback + "\n" + input + "\n" + status

	return out
}

// clipToBottom is no longer used (fitBlock handles trimming).
// Kept here as a comment for future-self in case we need fancier
// scrollback behavior (e.g. user-controlled scroll position).

// ---------- system prompt assembly ----------

// buildSystemPrompt assembles the system prompt from three sources:
//
//	1. baseSystemPrompt (the always-on instructions)
//	2. memory.md (declarative facts about user + environment)
//	3. an index of skills (name + description per line; full body is
//	   loaded by the model via read_skill)
//
// Sources 2 and 3 are optional; missing or empty just gets omitted.
func buildSystemPrompt() string {
	var b strings.Builder
	b.WriteString(baseSystemPrompt)

	mem, _ := store.LoadMemory()
	mem = strings.TrimSpace(mem)
	if mem != "" {
		b.WriteString("\n\n══ Memory (long-term facts about user + environment) ══\n")
		b.WriteString(mem)
	}

	skills, _ := store.ListSkills()
	if len(skills) > 0 {
		b.WriteString("\n\n══ Skills (call read_skill(name) to load) ══\n")
		for _, s := range skills {
			b.WriteString("- ")
			b.WriteString(s.Name)
			if s.Description != "" {
				b.WriteString(" — ")
				b.WriteString(s.Description)
			}
			b.WriteString("\n")
		}
	}

	return b.String()
}

// ---------- main ----------

func main() {
	// Force lipgloss to emit ANSI 256-color SGR escapes regardless of
	// $TERM. Vts doesn't set $TERM, so termenv's default detection
	// falls back to NoColor and strips everything — making pi9 look
	// like a plain monochrome shell. Pinning the profile fixes that.
	lipgloss.SetColorProfile(termenv.ANSI256)

	var (
		flagNew     = flag.Bool("new", false, "start a fresh session (do not resume)")
		flagSession = flag.String("session", "", "load a specific session by id")
	)
	flag.Parse()

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
			"pi9: first launch — wrote template to %s\n"+
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
	apiKey := cfg.APIKey

	// Decide which session to use.
	sessionID := *flagSession
	if sessionID == "" && !*flagNew {
		sessionID = store.CurrentSessionID()
	}

	// Build a fresh history with the assembled system prompt.
	systemPrompt := buildSystemPrompt()
	history := chat.History{System: systemPrompt}

	// Attempt to load if we have an id.
	if sessionID != "" {
		if data, err := store.LoadSession(sessionID); err == nil {
			var loaded chat.History
			if err := json.Unmarshal(data, &loaded); err == nil {
				loaded.RestoreErrs()
				// Always update system prompt from current sources;
				// memory/skills may have changed since last save.
				loaded.System = systemPrompt
				history = loaded
			}
		} else if !os.IsNotExist(err) {
			// Non-not-exist errors are unexpected but not fatal.
			fmt.Fprintf(os.Stderr, "pi9: load session %s: %v\n", sessionID, err)
		}
	}

	// If we don't have a session id yet (either -new, no prior, or
	// load failed), allocate one and persist as current.
	if sessionID == "" || *flagNew {
		sessionID = store.NewSessionID()
	}
	_ = store.SetCurrentSession(sessionID)

	m := pi9Model{
		inVts:     os.Getenv("vts"),
		apiKey:    apiKey,
		model:     model,
		tools:     tools.Schemas(),
		history:   history,
		sessionID: sessionID,
	}
	if noKeyYet {
		m.statusMsg = "no API key set - run /login <key> to set one"
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

	if _, err := p.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "pi9: %v\n", err)
		os.Exit(1)
	}

	// Final save on clean exit (the in-Update saves cover most cases,
	// but cancellation during a stream could otherwise lose state).
	_ = store.SaveAny(sessionID, history)
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
