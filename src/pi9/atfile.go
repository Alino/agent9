package main

// Task 2.3 @file include + inline shell expansion (pi parity).
//
// When a submitted user message contains @path tokens, each is expanded
// by appending the referenced file's contents to the outgoing message,
// clearly delimited with a header line. Tab-completion of the paths is
// handled by the autocomplete dropdown (see complete.go); this file owns
// the pure extraction + expansion logic so it can be unit-tested without
// a TUI.
//
// Inline shell:
//   "!cmd"  runs cmd and SENDS its output to the model as context.
//   "!!cmd" runs cmd but does NOT send (local-only, rendered as a Local
//           turn).
// Both run via the same rc/sh path the run_rc tool uses.

import (
	"fmt"
	"os"
	"strings"
)

// atToken is one @path reference found in a message: the raw token as
// typed (including the leading '@') and the path it points at.
type atToken struct {
	raw  string // e.g. "@main.go"
	path string // e.g. "main.go"
}

// extractAtTokens scans msg for @path references and returns them in
// order of appearance, de-duplicated by path (first occurrence wins).
//
// A token starts with '@' at the start of the message or after
// whitespace, and runs until the next whitespace. A bare "@" (no path)
// or an email-like "x@y" (the '@' is not at a token boundary) is not a
// file reference and is ignored. The path may contain anything except
// whitespace, so quoting is not supported (matching pi's simple @ syntax).
func extractAtTokens(msg string) []atToken {
	var out []atToken
	seen := make(map[string]bool)

	runes := []rune(msg)
	n := len(runes)
	for i := 0; i < n; i++ {
		if runes[i] != '@' {
			continue
		}
		// '@' must be at a token boundary: start of string or after
		// whitespace. Otherwise it's part of another token (e.g. an
		// email address) and not a file reference.
		if i > 0 && !isSpace(runes[i-1]) {
			continue
		}
		// Collect the path up to the next whitespace.
		j := i + 1
		for j < n && !isSpace(runes[j]) {
			j++
		}
		path := string(runes[i+1 : j])
		if path == "" {
			i = j
			continue
		}
		if !seen[path] {
			seen[path] = true
			out = append(out, atToken{raw: "@" + path, path: path})
		}
		i = j
	}
	return out
}

// isSpace reports whether r is ASCII whitespace for token splitting.
func isSpace(r rune) bool {
	return r == ' ' || r == '\t' || r == '\n' || r == '\r' || r == '\f' || r == '\v'
}

// expandAtFiles takes a user message and returns it with the contents of
// every @path reference appended, each in a clearly delimited block:
//
//	<original message>
//
//	===== @path =====
//	<file contents>
//
// Unreadable paths get an error note instead of contents so the model
// still sees that the reference was attempted. The original message text
// (including the @path tokens) is left intact — pi keeps the mention in
// place and appends the bodies below. When msg has no @ tokens it is
// returned unchanged. readFile defaults to os.ReadFile; it is a parameter
// so tests can stub the filesystem.
func expandAtFiles(msg string, readFile func(string) ([]byte, error)) string {
	if readFile == nil {
		readFile = os.ReadFile
	}
	toks := extractAtTokens(msg)
	if len(toks) == 0 {
		return msg
	}
	var b strings.Builder
	b.WriteString(msg)
	for _, t := range toks {
		b.WriteString("\n\n===== ")
		b.WriteString(t.raw)
		b.WriteString(" =====\n")
		data, err := readFile(t.path)
		if err != nil {
			fmt.Fprintf(&b, "[could not read %s: %v]", t.path, err)
			continue
		}
		b.Write(data)
	}
	return b.String()
}

// shellKind classifies a message's inline-shell intent.
type shellKind int

const (
	shellNone  shellKind = iota // not an inline-shell message
	shellSend                   // "!cmd"  — run and send output to model
	shellLocal                  // "!!cmd" — run locally, do not send
)

// parseInlineShell classifies a (trimmed) message as an inline-shell
// invocation and returns the command to run. "!!cmd" is checked before
// "!cmd" so the double-bang local form wins. Returns shellNone and ""
// when the message is not an inline-shell command. A bare "!" or "!!"
// with no command is treated as shellNone (nothing to run).
func parseInlineShell(msg string) (shellKind, string) {
	if strings.HasPrefix(msg, "!!") {
		cmd := strings.TrimSpace(msg[2:])
		if cmd == "" {
			return shellNone, ""
		}
		return shellLocal, cmd
	}
	if strings.HasPrefix(msg, "!") {
		cmd := strings.TrimSpace(msg[1:])
		if cmd == "" {
			return shellNone, ""
		}
		return shellSend, cmd
	}
	return shellNone, ""
}
