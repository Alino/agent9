package main

import (
	"reflect"
	"testing"
)

func TestMsgQueuePushSkipsBlank(t *testing.T) {
	var q msgQueue
	q.push("  ")
	q.push("")
	q.push("\n\t")
	if !q.empty() {
		t.Fatalf("blank pushes should be ignored, got len %d", q.len())
	}
	q.push("hello")
	if q.len() != 1 {
		t.Fatalf("len = %d, want 1", q.len())
	}
}

func TestMsgQueueDrainOneAtATime(t *testing.T) {
	var q msgQueue
	q.push("a")
	q.push("b")
	q.push("c")

	got, ok := q.drain(drainOneAtATime)
	if !ok || got != "a" {
		t.Fatalf("first drain = %q,%v; want a,true", got, ok)
	}
	if q.len() != 2 {
		t.Fatalf("after one drain len = %d, want 2", q.len())
	}
	got, _ = q.drain(drainOneAtATime)
	if got != "b" {
		t.Fatalf("second drain = %q; want b", got)
	}
	got, _ = q.drain(drainOneAtATime)
	if got != "c" {
		t.Fatalf("third drain = %q; want c", got)
	}
	if _, ok := q.drain(drainOneAtATime); ok {
		t.Fatalf("drain on empty queue should report false")
	}
}

func TestMsgQueueDrainAll(t *testing.T) {
	var q msgQueue
	q.push("a")
	q.push("b")
	q.push("c")

	got, ok := q.drain(drainAll)
	if !ok {
		t.Fatalf("drainAll on non-empty should report true")
	}
	if got != "a\n\nb\n\nc" {
		t.Fatalf("drainAll = %q; want joined a/b/c", got)
	}
	if !q.empty() {
		t.Fatalf("drainAll should empty the queue, len = %d", q.len())
	}
	if _, ok := q.drain(drainAll); ok {
		t.Fatalf("drainAll on empty should report false")
	}
}

func TestMsgQueuePopLast(t *testing.T) {
	var q msgQueue
	q.push("a")
	q.push("b")
	q.push("c")

	got, ok := q.popLast()
	if !ok || got != "c" {
		t.Fatalf("popLast = %q,%v; want c,true", got, ok)
	}
	got, _ = q.popLast()
	if got != "b" {
		t.Fatalf("second popLast = %q; want b", got)
	}
	// "a" remains; drain should still get it.
	if q.len() != 1 {
		t.Fatalf("len = %d, want 1", q.len())
	}
	if _, _ = q.popLast(); q.len() != 0 {
		t.Fatalf("queue should be empty after popping all")
	}
	if _, ok := q.popLast(); ok {
		t.Fatalf("popLast on empty should report false")
	}
}

func TestMsgQueueDrainAllItems(t *testing.T) {
	var q msgQueue
	if items := q.drainAllItems(); items != nil {
		t.Fatalf("empty drainAllItems = %v; want nil", items)
	}
	q.push("a")
	q.push("b")
	items := q.drainAllItems()
	if !reflect.DeepEqual(items, []string{"a", "b"}) {
		t.Fatalf("drainAllItems = %v; want [a b]", items)
	}
	if !q.empty() {
		t.Fatalf("drainAllItems should empty the queue")
	}
}

func TestDrainModeStringAndDefault(t *testing.T) {
	if drainOneAtATime.String() != "one-at-a-time" {
		t.Fatalf("drainOneAtATime string = %q", drainOneAtATime.String())
	}
	if drainAll.String() != "all" {
		t.Fatalf("drainAll string = %q", drainAll.String())
	}
	// Default (zero value) must be one-at-a-time, matching pi.
	var zero drainMode
	if zero != drainOneAtATime {
		t.Fatalf("zero drainMode = %v; want drainOneAtATime", zero)
	}
}

func TestToggleDrainMode(t *testing.T) {
	if toggleDrainMode(drainOneAtATime) != drainAll {
		t.Fatalf("toggle one-at-a-time should give all")
	}
	if toggleDrainMode(drainAll) != drainOneAtATime {
		t.Fatalf("toggle all should give one-at-a-time")
	}
}

func TestFirstLine(t *testing.T) {
	if got := firstLine("  hi there  "); got != "hi there" {
		t.Fatalf("firstLine trim = %q", got)
	}
	if got := firstLine("line one\nline two"); got != "line one" {
		t.Fatalf("firstLine multiline = %q", got)
	}
	long := "this is a very long single line that should be clipped at forty eight chars"
	got := firstLine(long)
	if len([]byte(got)) > 48+len("…") {
		t.Fatalf("firstLine clip too long: %q (%d bytes)", got, len(got))
	}
}

func TestQueueStatus(t *testing.T) {
	var m pi9Model
	if m.queueStatus() != "" {
		t.Fatalf("empty queues should yield empty status")
	}
	m.steerQueue.push("a")
	m.steerQueue.push("b")
	if got := m.queueStatus(); got != "2 steering queued (alt+up to edit)" {
		t.Fatalf("steering-only status = %q", got)
	}
	m.followUpQueue.push("c")
	if got := m.queueStatus(); got != "2 steering, 1 follow-up queued (alt+up to edit)" {
		t.Fatalf("combined status = %q", got)
	}
}

func TestDequeueToEditorOrder(t *testing.T) {
	var m pi9Model
	m.steerQueue.push("s1")
	m.followUpQueue.push("f1")

	// Follow-up dequeues first.
	if !m.dequeueToEditor() {
		t.Fatalf("dequeue should succeed")
	}
	if string(m.input) != "f1" {
		t.Fatalf("first dequeue input = %q; want f1", string(m.input))
	}
	// Reset editor; next dequeue pulls the steering message.
	m.input = nil
	m.inputCursor = 0
	if !m.dequeueToEditor() {
		t.Fatalf("second dequeue should succeed")
	}
	if string(m.input) != "s1" {
		t.Fatalf("second dequeue input = %q; want s1", string(m.input))
	}
	// Nothing left.
	m.input = nil
	if m.dequeueToEditor() {
		t.Fatalf("dequeue on empty queues should report false")
	}
}

func TestDequeuePreservesEditorText(t *testing.T) {
	var m pi9Model
	m.steerQueue.push("queued")
	m.input = []rune("typed so far")
	m.inputCursor = len(m.input)
	if !m.dequeueToEditor() {
		t.Fatalf("dequeue should succeed")
	}
	if string(m.input) != "queued\n\ntyped so far" {
		t.Fatalf("dequeue combined = %q", string(m.input))
	}
}

func TestRestoreQueuesToEditor(t *testing.T) {
	var m pi9Model
	m.steerQueue.push("s1")
	m.steerQueue.push("s2")
	m.followUpQueue.push("f1")
	m.input = []rune("draft")
	m.restoreQueuesToEditor()
	want := "s1\n\ns2\n\nf1\n\ndraft"
	if string(m.input) != want {
		t.Fatalf("restore = %q; want %q", string(m.input), want)
	}
	if !m.steerQueue.empty() || !m.followUpQueue.empty() {
		t.Fatalf("restore should empty both queues")
	}
	// No-op when empty.
	var m2 pi9Model
	m2.input = []rune("unchanged")
	m2.restoreQueuesToEditor()
	if string(m2.input) != "unchanged" {
		t.Fatalf("restore on empty queues should not change editor")
	}
}
