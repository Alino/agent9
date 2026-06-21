package store

import (
	"path/filepath"
	"strings"
	"testing"
)

func TestSubstituteArgs(t *testing.T) {
	tests := []struct {
		name    string
		content string
		args    []string
		want    string
	}{
		{"positional", "a $1 b $2", []string{"X", "Y"}, "a X b Y"},
		{"missing positional", "v=$3", []string{"X"}, "v="},
		{"all args at", "all: $@", []string{"X", "Y"}, "all: X Y"},
		{"all args ARGUMENTS", "all: $ARGUMENTS", []string{"X", "Y"}, "all: X Y"},
		{"default present", "n=${1:-7}", []string{"3"}, "n=3"},
		{"default missing", "n=${1:-7}", nil, "n=7"},
		{"default empty arg", "n=${1:-7}", []string{""}, "n=7"},
		{"slice from", "rest=${@:2}", []string{"a", "b", "c"}, "rest=b c"},
		{"slice from len", "two=${@:1:2}", []string{"a", "b", "c"}, "two=a b"},
		{"slice past end", "x=${@:5}", []string{"a"}, "x="},
		{"slice len past end", "x=${@:2:5}", []string{"a", "b"}, "x=b"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := substituteArgs(tt.content, tt.args); got != tt.want {
				t.Fatalf("substituteArgs(%q,%v)=%q want %q", tt.content, tt.args, got, tt.want)
			}
		})
	}
}

func TestListAndResolvePromptTemplates(t *testing.T) {
	home := setHome(t)
	cwd := t.TempDir()

	writeFile(t, filepath.Join(home, "prompts", "review.md"),
		"---\ndescription: Review changes\nargument-hint: \"<path>\"\n---\nReview $1 now.")
	// Description fallback to first non-empty body line.
	writeFile(t, filepath.Join(home, "prompts", "plain.md"),
		"\n\nFirst real line\nSecond line")
	// Project-only template (trusted gating).
	writeFile(t, filepath.Join(cwd, ".pi", "prompts", "ship.md"),
		"---\ndescription: Ship it\n---\nShip ${1:-now}.")

	// Untrusted: no project template.
	got := ListPromptTemplates(cwd, false)
	if findTemplate(got, "ship") != nil {
		t.Fatal("untrusted: project template leaked")
	}
	rev := findTemplate(got, "review")
	if rev == nil || rev.Description != "Review changes" || rev.ArgumentHint != "<path>" {
		t.Fatalf("review template parse: %+v", rev)
	}
	pl := findTemplate(got, "plain")
	if pl == nil || pl.Description != "First real line" {
		t.Fatalf("plain description fallback: %+v", pl)
	}

	// Trusted: project template present.
	got = ListPromptTemplates(cwd, true)
	if findTemplate(got, "ship") == nil {
		t.Fatal("trusted: project template missing")
	}

	// Resolve with substitution, frontmatter stripped.
	text, found, err := ResolvePromptTemplate("review", []string{"main.go"}, cwd, true)
	if err != nil || !found {
		t.Fatalf("resolve review: found=%v err=%v", found, err)
	}
	if strings.Contains(text, "description:") {
		t.Fatalf("frontmatter not stripped: %q", text)
	}
	if strings.TrimSpace(text) != "Review main.go now." {
		t.Fatalf("resolve review text=%q", text)
	}

	// Project template resolution with default.
	text, found, _ = ResolvePromptTemplate("ship", nil, cwd, true)
	if !found || strings.TrimSpace(text) != "Ship now." {
		t.Fatalf("resolve ship text=%q found=%v", text, found)
	}

	// Unknown template.
	if _, found, _ := ResolvePromptTemplate("nope", nil, cwd, true); found {
		t.Fatal("unknown template should not be found")
	}

	// Untrusted cannot resolve project template.
	if _, found, _ := ResolvePromptTemplate("ship", nil, cwd, false); found {
		t.Fatal("untrusted should not resolve project template")
	}
}

func findTemplate(ts []PromptTemplate, name string) *PromptTemplate {
	for i := range ts {
		if ts[i].Name == name {
			return &ts[i]
		}
	}
	return nil
}
