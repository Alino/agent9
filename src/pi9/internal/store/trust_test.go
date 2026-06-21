package store

import (
	"path/filepath"
	"testing"
)

func TestTrustRoundTrip(t *testing.T) {
	setHome(t)
	proj := t.TempDir()

	if IsTrusted(proj) {
		t.Fatal("fresh project should be untrusted")
	}

	if err := SetTrust(proj, TrustAlways); err != nil {
		t.Fatalf("set always: %v", err)
	}
	if !IsTrusted(proj) {
		t.Fatal("expected trusted after SetTrust always")
	}

	// Descendant inherits the ancestor decision.
	child := filepath.Join(proj, "sub", "dir")
	if !IsTrusted(child) {
		t.Fatal("descendant should inherit always")
	}

	// never overrides.
	if err := SetTrust(proj, TrustNever); err != nil {
		t.Fatalf("set never: %v", err)
	}
	if IsTrusted(proj) || IsTrusted(child) {
		t.Fatal("expected untrusted after never")
	}

	// A descendant with explicit always beats the ancestor never.
	if err := SetTrust(child, TrustAlways); err != nil {
		t.Fatalf("set child always: %v", err)
	}
	if !IsTrusted(child) {
		t.Fatal("child explicit always should win over ancestor never")
	}

	// Clear removes the entry; child falls back to ancestor never.
	if err := SetTrust(child, ""); err != nil {
		t.Fatalf("clear: %v", err)
	}
	if IsTrusted(child) {
		t.Fatal("after clear, child should inherit ancestor never")
	}

	// Invalid decision rejected.
	if err := SetTrust(proj, "maybe"); err == nil {
		t.Fatal("expected error for invalid decision")
	}
}
