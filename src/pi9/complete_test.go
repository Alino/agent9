package main

import "testing"

func TestDetectCompletion(t *testing.T) {
	cases := []struct {
		name      string
		text      string
		cursor    int
		wantKind  completionKind
		wantStart int
		wantQuery string
	}{
		{"empty", "", 0, completeNone, 0, ""},
		{"slash-only", "/", 1, completeSlash, 0, ""},
		{"slash-partial", "/mod", 4, completeSlash, 0, "mod"},
		{"slash-in-args", "/model gpt", 10, completeNone, 0, ""},
		{"slash-cursor-on-cmd", "/model gpt", 4, completeSlash, 0, "mod"},
		{"at-token", "look @ma", 8, completeAt, 5, "ma"},
		{"at-start", "@ma", 3, completeAt, 0, "ma"},
		{"at-just-typed", "see @", 5, completeAt, 4, ""},
		{"email-no-trigger", "foo@bar", 7, completeNone, 0, ""},
		{"plain-text", "hello world", 11, completeNone, 0, ""},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			k, start, q := detectCompletion(c.text, c.cursor)
			if k != c.wantKind || start != c.wantStart || q != c.wantQuery {
				t.Fatalf("detectCompletion(%q,%d) = (%d,%d,%q), want (%d,%d,%q)",
					c.text, c.cursor, k, start, q, c.wantKind, c.wantStart, c.wantQuery)
			}
		})
	}
}

func TestAcceptCompletionSlash(t *testing.T) {
	m := &pi9Model{
		input:          []rune("/mod"),
		inputCursor:    4,
		completeKind:   completeSlash,
		completeList:   []completion{{value: "model", display: "model — switch model"}},
		completeCursor: 0,
	}
	m.acceptCompletion()
	if got := string(m.input); got != "/model " {
		t.Fatalf("got %q, want %q", got, "/model ")
	}
	if m.inputCursor != len([]rune("/model ")) {
		t.Fatalf("cursor = %d, want %d", m.inputCursor, len([]rune("/model ")))
	}
}

func TestAcceptCompletionAtFile(t *testing.T) {
	m := &pi9Model{
		input:          []rune("see @ma"),
		inputCursor:    7,
		completeKind:   completeAt,
		completeStart:  4,
		completeList:   []completion{{value: "main.go", display: "main.go"}},
		completeCursor: 0,
	}
	m.acceptCompletion()
	if got := string(m.input); got != "see @main.go " {
		t.Fatalf("got %q, want %q", got, "see @main.go ")
	}
}

func TestAcceptCompletionAtDir(t *testing.T) {
	// Directory completion (trailing /) should NOT add a trailing space,
	// so the user can keep drilling in.
	m := &pi9Model{
		input:          []rune("@in"),
		inputCursor:    3,
		completeKind:   completeAt,
		completeStart:  0,
		completeList:   []completion{{value: "internal/", display: "internal/"}},
		completeCursor: 0,
	}
	m.acceptCompletion()
	if got := string(m.input); got != "@internal/" {
		t.Fatalf("got %q, want %q", got, "@internal/")
	}
}
