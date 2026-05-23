//go:build plan9
// +build plan9

package tea

import (
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

// vtsListenForResize is the plan9 hook called from handleResize when
// p.ttyOutput is nil. Inside a vts session we still want resize
// updates — they come from polling /n/vts/<s>/ctl, not from SIGWINCH.
// Returns true if we started the watcher; false otherwise.
func vtsListenForResize(p *Program, done chan struct{}) bool {
	if vtsSessionName() == "" {
		return false
	}
	go p.listenForResize(done)
	return true
}
