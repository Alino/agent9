package store

import (
	"path/filepath"
	"strings"
	"testing"
)

func TestSkillFrontmatterAndXML(t *testing.T) {
	home := setHome(t)

	writeFile(t, filepath.Join(home, "skills", "alpha.md"),
		"---\nname: alpha\ndescription: Alpha skill\nlicense: MIT\ncompatibility: plan9\nallowed-tools: read bash edit\n---\n# Alpha\nbody")
	writeFile(t, filepath.Join(home, "skills", "beta.md"),
		"---\nname: beta\ndescription: Beta skill\ndisable-model-invocation: true\n---\n# Beta")

	skills, err := ListSkills()
	if err != nil {
		t.Fatalf("ListSkills: %v", err)
	}
	if len(skills) != 2 {
		t.Fatalf("want 2 skills, got %d", len(skills))
	}
	var alpha, beta *Skill
	for i := range skills {
		switch skills[i].Name {
		case "alpha":
			alpha = &skills[i]
		case "beta":
			beta = &skills[i]
		}
	}
	if alpha == nil || beta == nil {
		t.Fatalf("missing skills: %+v", skills)
	}
	if alpha.License != "MIT" || alpha.Compatibility != "plan9" {
		t.Fatalf("alpha frontmatter: %+v", alpha)
	}
	if len(alpha.AllowedTools) != 3 || alpha.AllowedTools[1] != "bash" {
		t.Fatalf("alpha allowed-tools: %v", alpha.AllowedTools)
	}
	if !beta.DisableModelInvocation {
		t.Fatal("beta should be disabled")
	}

	// XML omits the disabled skill.
	xml := FormatSkillsXML(skills)
	if !strings.Contains(xml, "<name>alpha</name>") {
		t.Fatalf("xml missing alpha:\n%s", xml)
	}
	if strings.Contains(xml, "beta") {
		t.Fatalf("xml should omit disabled beta:\n%s", xml)
	}
	if !strings.Contains(xml, "<available_skills>") || !strings.Contains(xml, "</available_skills>") {
		t.Fatalf("xml envelope missing:\n%s", xml)
	}

	// Disabled skill still loadable by name.
	body, err := ReadSkillBody("beta")
	if err != nil || !strings.Contains(body, "# Beta") {
		t.Fatalf("ReadSkillBody(beta)=%q err=%v", body, err)
	}
}

func TestFormatSkillsXMLEscaping(t *testing.T) {
	xml := FormatSkillsXML([]Skill{{Name: "x", Description: "a & b <c>", Path: "/p"}})
	if !strings.Contains(xml, "a &amp; b &lt;c&gt;") {
		t.Fatalf("escaping failed:\n%s", xml)
	}
}

func TestListSkillsForTrustGating(t *testing.T) {
	home := setHome(t)
	writeFile(t, filepath.Join(home, "skills", "g.md"),
		"---\nname: g\ndescription: Global\n---\nbody")

	root := t.TempDir()
	writeFile(t, filepath.Join(root, ".git", "HEAD"), "x")
	cwd := filepath.Join(root, "pkg")
	writeFile(t, filepath.Join(cwd, ".pi", "skills", "p.md"),
		"---\nname: p\ndescription: Project\n---\nbody")
	writeFile(t, filepath.Join(root, ".agents", "skills", "a.md"),
		"---\nname: a\ndescription: Ancestor agents\n---\nbody")

	// Untrusted: only global.
	skills := ListSkillsFor(cwd, false)
	if len(skills) != 1 || skills[0].Name != "g" {
		t.Fatalf("untrusted want [g], got %+v", skills)
	}

	// Trusted: global + project .pi + ancestor .agents.
	skills = ListSkillsFor(cwd, true)
	names := map[string]bool{}
	for _, s := range skills {
		names[s.Name] = true
	}
	for _, want := range []string{"g", "p", "a"} {
		if !names[want] {
			t.Fatalf("trusted missing %q: %+v", want, skills)
		}
	}
}
