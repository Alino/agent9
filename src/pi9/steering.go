package main

// Steering + follow-up message queues (pi parity).
//
// pi's interaction model lets the user keep typing while the agent runs:
//
//   - Enter while running   = a STEERING message. It is queued and
//     delivered as the next user turn at the safe point between tool
//     rounds (mid-run course-correction), without waiting for the whole
//     run to finish.
//   - Alt+Enter while running = a FOLLOW-UP message. It is queued and
//     delivered only after ALL current work finishes.
//   - Alt+Up = dequeue: pull the most recently queued (not-yet-sent)
//     message back into the editor for editing.
//   - Esc still aborts the current run (existing behavior); aborting also
//     restores any queued messages to the editor (pi parity).
//
// Both queues honor a drain MODE (matching pi's steeringMode /
// followUpMode settings):
//
//   - drainAll        — deliver everything queued at once (joined into one
//     turn) when the drain point is reached.
//   - drainOneAtATime — deliver only the oldest queued message; the rest
//     stay queued for the next drain point. This is pi's default.
//
// Queues are ephemeral: nothing extra is persisted. The logic here is a
// pure value type (msgQueue) with no UI/Bubble-Tea dependency so it can be
// unit-tested in isolation.

import "strings"

// drainMode selects how a queue is drained at a delivery point.
type drainMode int

const (
	// drainOneAtATime delivers a single queued message per drain point.
	// pi's default for both steering and follow-up.
	drainOneAtATime drainMode = iota
	// drainAll delivers every queued message at once (joined).
	drainAll
)

// String renders the mode the way pi names the setting values, so the
// /settings overlay and status hints read identically.
func (d drainMode) String() string {
	if d == drainAll {
		return "all"
	}
	return "one-at-a-time"
}

// toggleDrainMode flips between the two modes (there are only two values),
// for the /settings overlay's left/right/enter cycling.
func toggleDrainMode(d drainMode) drainMode {
	if d == drainAll {
		return drainOneAtATime
	}
	return drainAll
}

// msgQueue is a FIFO of pending message texts. The zero value is an empty,
// ready-to-use queue.
type msgQueue struct {
	items []string
}

// len reports how many messages are queued.
func (q *msgQueue) len() int { return len(q.items) }

// empty reports whether the queue has no messages.
func (q *msgQueue) empty() bool { return len(q.items) == 0 }

// push appends a message to the back of the queue. Blank messages (after
// trimming) are ignored so an accidental empty Enter doesn't queue noise.
func (q *msgQueue) push(text string) {
	if strings.TrimSpace(text) == "" {
		return
	}
	q.items = append(q.items, text)
}

// drain removes and returns the messages to deliver at one drain point,
// honoring the mode:
//
//   - drainAll        — every queued message, in order, joined by a blank
//     line into a single delivered text. Returns "" + false when empty.
//   - drainOneAtATime — only the oldest message. The rest stay queued.
//
// The second return is false when there was nothing to drain.
func (q *msgQueue) drain(mode drainMode) (string, bool) {
	if len(q.items) == 0 {
		return "", false
	}
	if mode == drainAll {
		text := strings.Join(q.items, "\n\n")
		q.items = nil
		return text, true
	}
	text := q.items[0]
	q.items = q.items[1:]
	return text, true
}

// popLast removes and returns the most recently queued message (the one
// Alt+Up restores to the editor first). Returns "" + false when empty.
func (q *msgQueue) popLast() (string, bool) {
	n := len(q.items)
	if n == 0 {
		return "", false
	}
	text := q.items[n-1]
	q.items = q.items[:n-1]
	return text, true
}

// drainAllItems removes and returns every queued message in order. Used
// when an abort restores all pending work to the editor at once.
func (q *msgQueue) drainAllItems() []string {
	if len(q.items) == 0 {
		return nil
	}
	out := q.items
	q.items = nil
	return out
}

// restoreQueuesToEditor drains BOTH queues (steering first, then follow-up)
// back into the editor, prepended to whatever the user already typed, so an
// aborted run doesn't silently discard pending messages (pi parity). No-op
// when both queues are empty.
func (m *pi9Model) restoreQueuesToEditor() {
	queued := append(m.steerQueue.drainAllItems(), m.followUpQueue.drainAllItems()...)
	if len(queued) == 0 {
		return
	}
	combined := strings.Join(queued, "\n\n")
	if cur := strings.TrimSpace(string(m.input)); cur != "" {
		combined = combined + "\n\n" + string(m.input)
	}
	m.input = []rune(combined)
	m.inputCursor = len(m.input)
}
