//go:build plan9
// +build plan9

package tea

import (
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"

	"github.com/charmbracelet/x/term"
)

// vtsSessionName returns the session name we're running inside, or ""
// if not running under vts. vts(8) sets the "vts" env var in every rc
// child via putenv("vts", s->name) — see src/vts/session.c.
func vtsSessionName() string {
	return os.Getenv("vts")
}

// vtsSizePath returns the path to the size file for the current vts
// session, or "" if not in a vts session.
//
// Mount path: pi9 by default expects /srv/vts mounted at /n/vts. If
// the session was started from rc that hadn't mounted vts in its
// namespace, this returns the path anyway and reads will fail — that
// degrades to 80x24 defaults, not a crash.
func vtsSizePath() string {
	s := vtsSessionName()
	if s == "" {
		return ""
	}
	return "/n/vts/" + s + "/size"
}

// readVtsSize reads "rows cols" from the vts ctl file.
//
// The wire spec ([[vt-9p-namespace]]) claims size is a separate file
// "/n/vts/<s>/size", but the current vts implementation only exposes
// it via the ctl file's status output. Format from src/vts/srv.c is:
//
//	vts <version>; sessions=N
//	  1: 24x80 cur=R,C rc=PID alive=1
//	  2: ...
//
// We grep for the line starting with "  <session>:" and parse the
// RxC token.
func readVtsSize() (rows, cols int, err error) {
	s := vtsSessionName()
	if s == "" {
		return 0, 0, fmt.Errorf("not in a vts session")
	}
	b, err := os.ReadFile("/n/vts/ctl")
	if err != nil {
		return 0, 0, err
	}
	prefix := "  " + s + ":"
	for _, line := range strings.Split(string(b), "\n") {
		if !strings.HasPrefix(line, prefix) {
			continue
		}
		f := strings.Fields(strings.TrimPrefix(line, prefix))
		if len(f) == 0 {
			continue
		}
		// f[0] is "RxC"
		parts := strings.Split(f[0], "x")
		if len(parts) != 2 {
			continue
		}
		r, errR := strconv.Atoi(parts[0])
		c, errC := strconv.Atoi(parts[1])
		if errR != nil || errC != nil {
			continue
		}
		return r, c, nil
	}
	return 0, 0, fmt.Errorf("vts ctl: session %q not found", s)
}

// vtsFile wraps p.input (stdin) into a term.File so the rest of
// bubbletea's plumbing finds something to work with. When running
// under vts the pipes ARE the terminal.
type vtsFile struct {
	io.ReadWriteCloser
	fd uintptr
}

func (v *vtsFile) Fd() uintptr { return v.fd }

// initInput sets the terminal connected to p.input into raw mode and
// records the previous state so Restore can undo it on shutdown.
//
// Plan 9 path:
//   - Under vts, fd 0 is a pipe; treat it as the terminal directly.
//     vts already manages raw mode on the session via /n/vts/<s>/ctl
//     (the wire-protocol "rawon" command). We don't need MakeRaw.
//   - Outside vts, fall back to the upstream term.MakeRaw which
//     handles /dev/cons via /fd/Nctl.
func (p *Program) initInput() (err error) {
	if vtsSessionName() != "" {
		// We're in vts. fd 0/1 are pipes; vts handles raw mode for
		// us via /n/vts/<s>/ctl (the "rawon" wire command). Do NOT
		// set p.ttyInput / p.ttyOutput — bubbletea uses those for
		// term.GetSize/MakeRaw/Restore, all of which would hit
		// /dev/wctl (or worse) and fail. We send WindowSizeMsg
		// ourselves from listenForResize in signals_plan9.go.
		return nil
	}

	// Outside vts: behave like every other unix.
	if f, ok := p.input.(term.File); ok && term.IsTerminal(f.Fd()) {
		p.ttyInput = f
		p.previousTtyInputState, err = term.MakeRaw(p.ttyInput.Fd())
		if err != nil {
			return fmt.Errorf("error entering raw mode: %w", err)
		}
	}

	if f, ok := p.output.(term.File); ok && term.IsTerminal(f.Fd()) {
		p.ttyOutput = f
	}

	return nil
}

func openInputTTY() (*os.File, error) {
	// In vts: open the session's cons file. This isn't usually
	// reached — pi9 always uses WithInput(os.Stdin) — but cover the
	// case anyway for sanity.
	if s := vtsSessionName(); s != "" {
		p := "/n/vts/" + s + "/cons"
		f, err := os.OpenFile(p, os.O_RDWR, 0)
		if err == nil {
			return f, nil
		}
		// fall through to /dev/cons if vts file system isn't mounted
	}
	f, err := os.Open("/dev/cons")
	if err != nil {
		return nil, fmt.Errorf("could not open /dev/cons: %w", err)
	}
	return f, nil
}

// Plan 9 has no SIGTSTP / job control. Ctrl-Z is not a process-suspend
// signal; the user is expected to switch windows in rio or vtwin to
// push the program aside. So suspend is unsupported.
const suspendSupported = false

// suspendProcess is a no-op on plan9 since suspendSupported is false.
// Provided to satisfy the symbol referenced from tty.go.
func suspendProcess() {}
