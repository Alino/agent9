package tools

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// mustJSON marshals v to a JSON string, failing the test on error.
func mustJSON(t *testing.T, v any) string {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	return string(b)
}

func TestEdit(t *testing.T) {
	tests := []struct {
		name     string
		content  string
		edits    []map[string]string
		wantErr  string   // substring expected in error (empty = no error)
		wantFile string   // expected file content on success
		wantIn   []string // substrings expected in the result summary
	}{
		{
			name:    "missing match",
			content: "hello world\n",
			edits:   []map[string]string{{"oldText": "nope", "newText": "x"}},
			wantErr: "not found",
		},
		{
			name:    "non-unique",
			content: "ab ab ab\n",
			edits:   []map[string]string{{"oldText": "ab", "newText": "x"}},
			wantErr: "occurs 3 times",
		},
		{
			name:    "multi-edit",
			content: "one\ntwo\nthree\n",
			edits: []map[string]string{
				{"oldText": "one", "newText": "1"},
				{"oldText": "three", "newText": "3"},
			},
			wantFile: "1\ntwo\n3\n",
			wantIn:   []string{"2 replacement", "- one", "+ 1", "- three", "+ 3"},
		},
		{
			name:     "success single",
			content:  "alpha\nbeta\n",
			edits:    []map[string]string{{"oldText": "beta", "newText": "gamma"}},
			wantFile: "alpha\ngamma\n",
			wantIn:   []string{"1 replacement"},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "f.txt")
			if err := os.WriteFile(path, []byte(tc.content), 0644); err != nil {
				t.Fatal(err)
			}
			args := map[string]any{"path": path, "edits": tc.edits}
			res, err := Run("edit", mustJSON(t, args))
			if tc.wantErr != "" {
				if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
					t.Fatalf("want err containing %q, got %v", tc.wantErr, err)
				}
				// File must be unchanged on error.
				b, _ := os.ReadFile(path)
				if string(b) != tc.content {
					t.Fatalf("file mutated on error: %q", b)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected err: %v", err)
			}
			b, _ := os.ReadFile(path)
			if string(b) != tc.wantFile {
				t.Fatalf("file = %q, want %q", b, tc.wantFile)
			}
			for _, sub := range tc.wantIn {
				if !strings.Contains(res, sub) {
					t.Fatalf("result missing %q\ngot:\n%s", sub, res)
				}
			}
		})
	}
}

func TestReadOffsetLimit(t *testing.T) {
	path := filepath.Join(t.TempDir(), "lines.txt")
	content := "l1\nl2\nl3\nl4\nl5"
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
	tests := []struct {
		name    string
		args    map[string]any
		want    string
		wantErr string
	}{
		{"whole file", map[string]any{"path": path}, content, ""},
		{"offset", map[string]any{"path": path, "offset": 3}, "l3\nl4\nl5", ""},
		{"limit", map[string]any{"path": path, "limit": 2}, "l1\nl2", ""},
		{"offset+limit", map[string]any{"path": path, "offset": 2, "limit": 2}, "l2\nl3", ""},
		{"offset beyond", map[string]any{"path": path, "offset": 99}, "", "beyond end"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			res, err := Run("read", mustJSON(t, tc.args))
			if tc.wantErr != "" {
				if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
					t.Fatalf("want err %q, got %v", tc.wantErr, err)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected err: %v", err)
			}
			if res != tc.want {
				t.Fatalf("got %q, want %q", res, tc.want)
			}
		})
	}
}

func TestGrep(t *testing.T) {
	dir := t.TempDir()
	files := map[string]string{
		"a.go":      "package main\nfunc Foo() {}\nfunc Bar() {}\n",
		"b.txt":     "foo bar\nFOO BAR\nbaz\n",
		"sub/c.go":  "func Foo() {}\n",
		"skip/d.go": "func Foo() {}\n",
	}
	for rel, content := range files {
		full := filepath.Join(dir, rel)
		if err := os.MkdirAll(filepath.Dir(full), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(content), 0644); err != nil {
			t.Fatal(err)
		}
	}

	t.Run("regex", func(t *testing.T) {
		res, err := Run("grep", mustJSON(t, map[string]any{"pattern": "func \\w+", "path": dir}))
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(res, "a.go:2:") || !strings.Contains(res, "Bar()") {
			t.Fatalf("regex result:\n%s", res)
		}
	})

	t.Run("literal", func(t *testing.T) {
		// As a literal, "func \w+" should match nothing (no such text).
		res, err := Run("grep", mustJSON(t, map[string]any{"pattern": "func \\w+", "path": dir, "literal": true}))
		if err != nil {
			t.Fatal(err)
		}
		if res != "No matches found" {
			t.Fatalf("literal want no matches, got:\n%s", res)
		}
	})

	t.Run("ignoreCase", func(t *testing.T) {
		res, err := Run("grep", mustJSON(t, map[string]any{"pattern": "foo bar", "path": dir, "ignoreCase": true, "glob": "*.txt"}))
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(res, "b.txt:1:") || !strings.Contains(res, "b.txt:2:") {
			t.Fatalf("ignoreCase result:\n%s", res)
		}
	})

	t.Run("glob", func(t *testing.T) {
		res, err := Run("grep", mustJSON(t, map[string]any{"pattern": "Foo", "path": dir, "glob": "*.go"}))
		if err != nil {
			t.Fatal(err)
		}
		if strings.Contains(res, "b.txt") {
			t.Fatalf("glob should exclude .txt:\n%s", res)
		}
		if !strings.Contains(res, "a.go") || !strings.Contains(res, filepath.ToSlash("sub/c.go")) {
			t.Fatalf("glob missing .go matches:\n%s", res)
		}
	})

	t.Run("limit", func(t *testing.T) {
		res, err := Run("grep", mustJSON(t, map[string]any{"pattern": "Foo", "path": dir, "limit": 1}))
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(res, "limit reached") {
			t.Fatalf("expected limit notice:\n%s", res)
		}
		matchLines := 0
		for _, l := range strings.Split(res, "\n") {
			if strings.Contains(l, ".go:") {
				matchLines++
			}
		}
		if matchLines != 1 {
			t.Fatalf("want 1 match line, got %d:\n%s", matchLines, res)
		}
	})

	t.Run("context", func(t *testing.T) {
		res, err := Run("grep", mustJSON(t, map[string]any{"pattern": "Bar", "path": dir, "glob": "a.go", "context": 1}))
		if err != nil {
			t.Fatal(err)
		}
		// Context line before the match uses a '-' separator.
		if !strings.Contains(res, "a.go-2-") || !strings.Contains(res, "a.go:3:") {
			t.Fatalf("context result:\n%s", res)
		}
	})
}

func TestFind(t *testing.T) {
	dir := t.TempDir()
	for _, rel := range []string{"a.go", "b.txt", "sub/c.go", "sub/deep/d.go", "node_modules/e.go"} {
		full := filepath.Join(dir, rel)
		if err := os.MkdirAll(filepath.Dir(full), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte("x"), 0644); err != nil {
			t.Fatal(err)
		}
	}

	t.Run("glob basename", func(t *testing.T) {
		res, err := Run("find", mustJSON(t, map[string]any{"pattern": "*.go", "path": dir}))
		if err != nil {
			t.Fatal(err)
		}
		got := strings.Split(res, "\n")
		// node_modules pruned; basename glob matches at any depth.
		want := map[string]bool{"a.go": true, "sub/c.go": true, "sub/deep/d.go": true}
		for _, g := range got {
			if !want[g] {
				t.Fatalf("unexpected result %q in:\n%s", g, res)
			}
		}
		if len(got) != 3 {
			t.Fatalf("want 3 results, got %d:\n%s", len(got), res)
		}
	})

	t.Run("doublestar", func(t *testing.T) {
		res, err := Run("find", mustJSON(t, map[string]any{"pattern": "sub/**/*.go", "path": dir}))
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(res, "sub/c.go") || !strings.Contains(res, "sub/deep/d.go") {
			t.Fatalf("doublestar result:\n%s", res)
		}
		if strings.Contains(res, "a.go\n") || res == "a.go" {
			t.Fatalf("doublestar should not match top-level a.go:\n%s", res)
		}
	})

	t.Run("no match", func(t *testing.T) {
		res, err := Run("find", mustJSON(t, map[string]any{"pattern": "*.rs", "path": dir}))
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(res, "No files found") {
			t.Fatalf("want no-match notice, got:\n%s", res)
		}
	})
}

func TestLs(t *testing.T) {
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "subdir"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "afile.txt"), []byte("x"), 0644); err != nil {
		t.Fatal(err)
	}
	res, err := Run("ls", mustJSON(t, map[string]any{"path": dir}))
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(res, "\n")
	if len(lines) != 2 || lines[0] != "afile.txt" || lines[1] != "subdir/" {
		t.Fatalf("ls result:\n%s", res)
	}
}

func TestAliasResolution(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "x.txt")

	// write_file alias should dispatch to the write tool.
	if _, err := Run("write_file", mustJSON(t, map[string]any{"path": path, "content": "hi"})); err != nil {
		t.Fatalf("write_file alias: %v", err)
	}
	// read_file alias should dispatch to the read tool.
	res, err := Run("read_file", mustJSON(t, map[string]any{"path": path}))
	if err != nil {
		t.Fatalf("read_file alias: %v", err)
	}
	if res != "hi" {
		t.Fatalf("read_file alias got %q", res)
	}

	// Canonical names also work.
	if _, err := Run("write", mustJSON(t, map[string]any{"path": path, "content": "bye"})); err != nil {
		t.Fatalf("write: %v", err)
	}
	res, err = Run("read", mustJSON(t, map[string]any{"path": path}))
	if err != nil || res != "bye" {
		t.Fatalf("read canonical got %q err %v", res, err)
	}

	// Unknown tool errors.
	if _, err := Run("does_not_exist", "{}"); err == nil {
		t.Fatal("expected error for unknown tool")
	}
}

func TestSchemasAdvertiseCanonicalOnly(t *testing.T) {
	names := map[string]bool{}
	for _, s := range Schemas() {
		names[s.Name] = true
	}
	for _, want := range []string{"read", "write", "edit", "grep", "find", "ls"} {
		if !names[want] {
			t.Errorf("Schemas missing canonical %q", want)
		}
	}
	for _, alias := range []string{"read_file", "write_file"} {
		if names[alias] {
			t.Errorf("Schemas should not advertise alias %q", alias)
		}
	}
}

// TestEditApplyOrdering verifies that multiple edits are matched against the
// ORIGINAL content and applied without one edit's newText colliding with a
// later edit's oldText (pi parity: match-against-original semantics).
func TestEditApplyOrdering(t *testing.T) {
	path := filepath.Join(t.TempDir(), "f.txt")
	// "A" -> "B"; separately "B" -> "C". Naive sequential replace on the
	// accumulating string would turn the freshly-written "B" into "C".
	if err := os.WriteFile(path, []byte("A\nB\n"), 0644); err != nil {
		t.Fatal(err)
	}
	args := map[string]any{
		"path": path,
		"edits": []map[string]string{
			{"oldText": "A", "newText": "B"},
			{"oldText": "B", "newText": "C"},
		},
	}
	if _, err := Run("edit", mustJSON(t, args)); err != nil {
		t.Fatalf("edit: %v", err)
	}
	got, _ := os.ReadFile(path)
	if string(got) != "B\nC\n" {
		t.Fatalf("apply ordering: got %q want %q", got, "B\nC\n")
	}
}

// TestEditOverlapRejected verifies overlapping edits are rejected atomically.
func TestEditOverlapRejected(t *testing.T) {
	path := filepath.Join(t.TempDir(), "f.txt")
	if err := os.WriteFile(path, []byte("hello world"), 0644); err != nil {
		t.Fatal(err)
	}
	args := map[string]any{
		"path": path,
		"edits": []map[string]string{
			{"oldText": "hello world", "newText": "x"},
			{"oldText": "lo wo", "newText": "y"},
		},
	}
	if _, err := Run("edit", mustJSON(t, args)); err == nil {
		t.Fatal("expected overlap error")
	}
	// File must be untouched on rejection.
	got, _ := os.ReadFile(path)
	if string(got) != "hello world" {
		t.Fatalf("file mutated on rejected overlap: %q", got)
	}
}

// TestFilePathAlias verifies read/write/edit accept the file_path alias that
// upstream pi tolerates, in addition to path.
func TestFilePathAlias(t *testing.T) {
	path := filepath.Join(t.TempDir(), "f.txt")
	if _, err := Run("write", mustJSON(t, map[string]any{"file_path": path, "content": "hi"})); err != nil {
		t.Fatalf("write via file_path: %v", err)
	}
	res, err := Run("read", mustJSON(t, map[string]any{"file_path": path}))
	if err != nil || !strings.Contains(res, "hi") {
		t.Fatalf("read via file_path: res=%q err=%v", res, err)
	}
	if _, err := Run("edit", mustJSON(t, map[string]any{
		"file_path": path,
		"edits":     []map[string]string{{"oldText": "hi", "newText": "bye"}},
	})); err != nil {
		t.Fatalf("edit via file_path: %v", err)
	}
	got, _ := os.ReadFile(path)
	if string(got) != "bye" {
		t.Fatalf("edit via file_path: got %q", got)
	}
}
