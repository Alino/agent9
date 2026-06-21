package store

// Prompt templates: markdown snippets invoked as /name that expand into a
// full prompt, with bash-style argument substitution.
//
// Locations:
//   - Global:  Home()/prompts/*.md   (always)
//   - Project: ./.pi/prompts/*.md     (only when trusted)
//
// Discovery is non-recursive. The filename stem becomes the template name.
// Frontmatter `description` and `argument-hint` are honored; a missing
// description falls back to the first non-empty body line (truncated).
//
// Substitution mirrors pi's prompt-templates.ts:
//   - $1, $2, ...        positional args
//   - $@ / $ARGUMENTS    all args, space-joined
//   - ${N:-default}      arg N when present/non-empty, else default
//   - ${@:N}             args from the Nth (1-indexed) onward
//   - ${@:N:L}           L args starting at N

import (
	"errors"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// PromptTemplate is one loaded prompt template.
type PromptTemplate struct {
	Name         string // file stem (e.g. "review" for review.md)
	Description  string
	ArgumentHint string
	Path         string // absolute path to the .md file
}

// promptsSubRe matches the four substitution forms. Order in the alternation
// matters: the brace forms must precede the bare $name form.
var promptsSubRe = regexp.MustCompile(`\$\{(\d+):-([^}]*)\}|\$\{@:(\d+)(?::(\d+))?\}|\$(ARGUMENTS|@|\d+)`)

// GlobalPromptsDir returns the global prompts directory.
func GlobalPromptsDir() string {
	return filepath.Join(Home(), "prompts")
}

// ListPromptTemplates returns prompt templates from the global prompts dir,
// plus the project ./.pi/prompts dir when trusted. Results are sorted by
// name; on a name collision the first-loaded (global) wins.
func ListPromptTemplates(cwd string, trusted bool) []PromptTemplate {
	seen := make(map[string]bool)
	var out []PromptTemplate

	collect := func(dir string) {
		for _, t := range loadTemplatesFromDir(dir) {
			if seen[t.Name] {
				continue
			}
			seen[t.Name] = true
			out = append(out, t)
		}
	}

	collect(GlobalPromptsDir())
	if trusted {
		if abs, err := filepath.Abs(cwd); err == nil {
			collect(filepath.Join(abs, configDirName, "prompts"))
		}
	}

	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out
}

// loadTemplatesFromDir scans dir (non-recursive) for *.md files and parses
// each into a PromptTemplate. A missing directory yields no templates.
func loadTemplatesFromDir(dir string) []PromptTemplate {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []PromptTemplate
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".md") {
			continue
		}
		p := filepath.Join(dir, e.Name())
		t, ok := loadTemplateFromFile(p)
		if ok {
			out = append(out, t)
		}
	}
	return out
}

// loadTemplateFromFile parses a single prompt template file.
func loadTemplateFromFile(path string) (PromptTemplate, bool) {
	b, err := os.ReadFile(path)
	if err != nil {
		return PromptTemplate{}, false
	}
	front, body := splitFrontmatter(string(b))

	name := strings.TrimSuffix(filepath.Base(path), ".md")
	desc := front["description"]
	if desc == "" {
		// Fall back to the first non-empty body line, truncated to 60 chars.
		for _, line := range strings.Split(body, "\n") {
			if strings.TrimSpace(line) != "" {
				desc = line
				if len(line) > 60 {
					desc = line[:60] + "..."
				}
				break
			}
		}
	}
	return PromptTemplate{
		Name:         name,
		Description:  desc,
		ArgumentHint: front["argument-hint"],
		Path:         path,
	}, true
}

// ResolvePromptTemplate finds the named template, strips its frontmatter, and
// substitutes args. It returns (text, found, err). found is false when no
// template by that name exists; err is non-nil only on read failure.
func ResolvePromptTemplate(name string, args []string, cwd string, trusted bool) (string, bool, error) {
	var path string
	for _, t := range ListPromptTemplates(cwd, trusted) {
		if t.Name == name {
			path = t.Path
			break
		}
	}
	if path == "" {
		return "", false, nil
	}
	b, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return "", false, nil
		}
		return "", false, err
	}
	_, body := splitFrontmatter(string(b))
	return substituteArgs(body, args), true, nil
}

// substituteArgs replaces the supported placeholders in content using args.
// Defaults and arg values are NOT recursively substituted.
func substituteArgs(content string, args []string) string {
	all := strings.Join(args, " ")

	return promptsSubRe.ReplaceAllStringFunc(content, func(m string) string {
		sub := promptsSubRe.FindStringSubmatch(m)
		// sub[1]=defaultNum sub[2]=defaultValue sub[3]=sliceStart
		// sub[4]=sliceLength sub[5]=simple(ARGUMENTS|@|digits)
		switch {
		case sub[1] != "": // ${N:-default}
			idx, _ := strconv.Atoi(sub[1])
			idx--
			if idx >= 0 && idx < len(args) && args[idx] != "" {
				return args[idx]
			}
			return sub[2]
		case sub[3] != "": // ${@:N} or ${@:N:L}
			start, _ := strconv.Atoi(sub[3])
			start-- // 1-indexed -> 0-indexed
			if start < 0 {
				start = 0
			}
			if start > len(args) {
				start = len(args)
			}
			if sub[4] != "" {
				length, _ := strconv.Atoi(sub[4])
				end := start + length
				if length < 0 {
					end = start
				}
				if end > len(args) {
					end = len(args)
				}
				return strings.Join(args[start:end], " ")
			}
			return strings.Join(args[start:], " ")
		default: // $1.. / $@ / $ARGUMENTS
			s := sub[5]
			if s == "ARGUMENTS" || s == "@" {
				return all
			}
			idx, _ := strconv.Atoi(s)
			idx--
			if idx >= 0 && idx < len(args) {
				return args[idx]
			}
			return ""
		}
	})
}
