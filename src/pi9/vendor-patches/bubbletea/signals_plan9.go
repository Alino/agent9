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
		// publishes the live size; readTermSize picks the channel that
		// actually updates. Poll it like the vts size file — os.Getenv
		// won't do, since Go snapshots the environment at startup.
		if r, c, ok := readTermSize(); ok {
			p.Send(WindowSizeMsg{Width: c, Height: r})
			lastR, lastC := r, c
			t := time.NewTicker(500 * time.Millisecond)
			defer t.Stop()
			for {
				select {
				case <-p.ctx.Done():
					return
				case <-t.C:
					r, c, ok := readTermSize()
					if ok && (r != lastR || c != lastC) {
						lastR, lastC = r, c
						p.Send(WindowSizeMsg{Width: c, Height: r})
					}
				}
			}
		}
		// No size source at all: classic default beats rendering nothing.
		// (No checkResize here: p.ttyOutput is always nil on plan9 —
		// see tty_plan9.go — and x/term's GetSize needs /dev/wctl.)
		p.Send(WindowSizeMsg{Width: 80, Height: 24})
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

// readTermSize reports the terminal's CURRENT size.
//
// $A9_SIZE_FILE first, and it matters: /env is NOT a live channel to a child
// on this platform. fork here is rfork(RFPROC|RFFDG|RFENVG), so the child gets
// a COPY of the environment group at exec and never sees the terminal's later
// writes — /env/COLS stays frozen at whatever it was when pi9 started.
// Measured under alacritty9: after resizing 1024x768 -> 620x660, /env/COLS
// still read 113 while the size file read "68 38". Laying out 113 columns wide
// in a 68-column window is exactly the bug this fixes: the window manager
// clips each line's tail, and the cursor-up arithmetic in the streaming redraw
// (rows counted for the wrong wrap width) lands on the wrong rows, so rows
// repeat with a few more words on each.
//
// The file holds "COLS LINES" — columns first, the order alacritty9 writes.
func readTermSize() (rows, cols int, ok bool) {
	if r, c, ok := readSizeFile(); ok {
		return r, c, true
	}
	return readEnvSize()
}

// readSizeFile reads the live size file whose path the hosting terminal put in
// $A9_SIZE_FILE. The path is fixed for the session, so a startup snapshot of
// the variable is fine; the FILE is what changes.
func readSizeFile() (rows, cols int, ok bool) {
	path := os.Getenv("A9_SIZE_FILE")
	if path == "" {
		return 0, 0, false
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return 0, 0, false
	}
	f := strings.Fields(strings.TrimRight(string(b), "\x00"))
	if len(f) < 2 {
		return 0, 0, false
	}
	c, errC := strconv.Atoi(f[0])
	r, errR := strconv.Atoi(f[1])
	if errC != nil || errR != nil || c <= 0 || r <= 0 {
		return 0, 0, false
	}
	return r, c, true
}

// readEnvSize reads /env/LINES and /env/COLS. Kept as the fallback for
// terminals that publish no size file (a plain console session), but see
// readTermSize: under a resize these go stale.
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
