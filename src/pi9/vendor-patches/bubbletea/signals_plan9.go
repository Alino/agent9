//go:build plan9
// +build plan9

package tea

import (
	"os"
	"strconv"
	"strings"
	"time"
)

// Plan 9 has no SIGWINCH. Two ways the terminal can change size:
//
//  1. The outer window manager (rio/vtwin) resizes the vts window,
//     vts writes new "rows cols" to its size file, and POSTS a note
//     `sys: window size change` to rc (and rc's children).
//  2. The user runs `echo size R C > /n/vts/<s>/ctl`.
//
// Go's signal.Notify on plan9 maps notes onto syscall.Note-style
// pseudo-signals but the API is fiddly; for v0 we poll the vts size
// file every 500ms and send WindowSizeMsg directly.
//
// Standalone (non-vts) usage falls through to a single initial size
// check (a no-op since p.ttyOutput stays nil), then idles.
func (p *Program) listenForResize(done chan struct{}) {
	defer close(done)

	// Always do one check at startup so the program knows its
	// dimensions before the first render.
	if vtsSessionName() == "" {
		// Not under vts. A terminal hosting us on pipes (alacritty9)
		// maintains LINES/COLS as live files in the shared /env group
		// (no RFENVG at spawn) — poll them like the vts size file.
		// os.Getenv won't do: Go snapshots the environment at startup.
		if r, c, ok := readEnvSize(); ok {
			p.Send(WindowSizeMsg{Width: c, Height: r})
			lastR, lastC := r, c
			t := time.NewTicker(500 * time.Millisecond)
			defer t.Stop()
			for {
				select {
				case <-p.ctx.Done():
					return
				case <-t.C:
					r, c, ok := readEnvSize()
					if ok && (r != lastR || c != lastC) {
						lastR, lastC = r, c
						p.Send(WindowSizeMsg{Width: c, Height: r})
					}
				}
			}
		}
		// No size source at all: classic default beats rendering nothing.
		p.Send(WindowSizeMsg{Width: 80, Height: 24})
		p.checkResize()
		<-p.ctx.Done()
		return
	}

	// In vts: send the initial size, then poll for changes.
	send := func() (int, int, bool) {
		r, c, err := readVtsSize()
		if err != nil {
			return 0, 0, false
		}
		p.Send(WindowSizeMsg{Width: c, Height: r})
		return r, c, true
	}

	lastR, lastC, _ := send()
	t := time.NewTicker(500 * time.Millisecond)
	defer t.Stop()

	for {
		select {
		case <-p.ctx.Done():
			return
		case <-t.C:
			r, c, err := readVtsSize()
			if err != nil {
				continue
			}
			if r != lastR || c != lastC {
				lastR, lastC = r, c
				p.Send(WindowSizeMsg{Width: c, Height: r})
			}
		}
	}
}

// readEnvSize reads the live /env/LINES and /env/COLS files (Plan 9 env
// vars are files; re-reading sees updates from the hosting terminal).
func readEnvSize() (rows, cols int, ok bool) {
	parse := func(name string) (int, bool) {
		b, err := os.ReadFile("/env/" + name)
		if err != nil {
			return 0, false
		}
		n, err := strconv.Atoi(strings.TrimSpace(strings.TrimRight(string(b), "\x00")))
		if err != nil || n <= 0 {
			return 0, false
		}
		return n, true
	}
	r, okr := parse("LINES")
	c, okc := parse("COLS")
	return r, c, okr && okc
}

// vtsListenForResize is the plan9 hook called from handleResize when
// p.ttyOutput is nil. Always starts the plan9 watcher: listenForResize
// itself picks the size source — vts ctl polling inside a vts session,
// /env/LINES+COLS polling under a pipe-hosting terminal (alacritty9),
// or a one-shot 80x24 default so the UI renders at all.
func vtsListenForResize(p *Program, done chan struct{}) bool {
	go p.listenForResize(done)
	return true
}
