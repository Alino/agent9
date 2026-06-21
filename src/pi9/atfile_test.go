package main

import (
	"fmt"
	"strings"
	"testing"
)

func TestExtractAtTokens(t *testing.T) {
	cases := []struct {
		name string
		in   string
		want []string // paths, in order
	}{
		{"none", "just a plain message", nil},
		{"single", "look at @main.go please", []string{"main.go"}},
		{"start", "@main.go is the entry", []string{"main.go"}},
		{"multiple", "@a.go and @b/c.go", []string{"a.go", "b/c.go"}},
		{"dedup", "@x.go @x.go @y.go", []string{"x.go", "y.go"}},
		{"email-not-a-ref", "mail me at foo@bar.com", nil},
		{"bare-at", "@ alone", nil},
		{"newline-bound", "first\n@two.go", []string{"two.go"}},
		{"trailing", "see @path/to/file.txt", []string{"path/to/file.txt"}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			toks := extractAtTokens(c.in)
			var got []string
			for _, tk := range toks {
				got = append(got, tk.path)
			}
			if len(got) != len(c.want) {
				t.Fatalf("got %v, want %v", got, c.want)
			}
			for i := range got {
				if got[i] != c.want[i] {
					t.Fatalf("got %v, want %v", got, c.want)
				}
			}
		})
	}
}

func TestExpandAtFiles(t *testing.T) {
	read := func(p string) ([]byte, error) {
		switch p {
		case "a.go":
			return []byte("package a"), nil
		case "b.go":
			return []byte("package b"), nil
		}
		return nil, fmt.Errorf("no such file")
	}

	t.Run("no tokens unchanged", func(t *testing.T) {
		in := "plain message"
		if got := expandAtFiles(in, read); got != in {
			t.Fatalf("expected unchanged, got %q", got)
		}
	})

	t.Run("appends contents", func(t *testing.T) {
		got := expandAtFiles("review @a.go and @b.go", read)
		if !strings.Contains(got, "review @a.go and @b.go") {
			t.Fatalf("original message missing: %q", got)
		}
		if !strings.Contains(got, "===== @a.go =====") || !strings.Contains(got, "package a") {
			t.Fatalf("a.go block missing: %q", got)
		}
		if !strings.Contains(got, "===== @b.go =====") || !strings.Contains(got, "package b") {
			t.Fatalf("b.go block missing: %q", got)
		}
	})

	t.Run("unreadable file notes error", func(t *testing.T) {
		got := expandAtFiles("see @missing.go", read)
		if !strings.Contains(got, "could not read missing.go") {
			t.Fatalf("expected error note, got %q", got)
		}
	})
}

func TestParseInlineShell(t *testing.T) {
	cases := []struct {
		in       string
		wantKind shellKind
		wantCmd  string
	}{
		{"hello", shellNone, ""},
		{"!ls -la", shellSend, "ls -la"},
		{"!!git status", shellLocal, "git status"},
		{"!  spaced  ", shellSend, "spaced"},
		{"!", shellNone, ""},
		{"!!", shellNone, ""},
		{"!! ", shellNone, ""},
		{"!!echo hi", shellLocal, "echo hi"},
	}
	for _, c := range cases {
		k, cmd := parseInlineShell(c.in)
		if k != c.wantKind || cmd != c.wantCmd {
			t.Errorf("parseInlineShell(%q) = (%d, %q), want (%d, %q)", c.in, k, cmd, c.wantKind, c.wantCmd)
		}
	}
}
