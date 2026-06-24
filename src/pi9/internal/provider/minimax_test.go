package provider

import "testing"

// TestMiniMaxProvider locks in the MiniMax port: it rides the Anthropic
// Messages wire format at MiniMax's base URL, is selectable, and its
// models resolve back to the minimax provider. Mirrors pi.dev's
// minimax.ts / minimax.models.ts.
func TestMiniMaxProvider(t *testing.T) {
	p := Get(ProviderMiniMax)
	if p == nil {
		t.Fatal("Get(ProviderMiniMax) returned nil")
	}
	if p.Name() != ProviderMiniMax {
		t.Errorf("Name() = %q, want %q", p.Name(), ProviderMiniMax)
	}

	// Backed by the anthropic wire format with MiniMax's base URL baked in.
	a, ok := p.(anthropic)
	if !ok {
		t.Fatalf("Get(ProviderMiniMax) is %T, want anthropic", p)
	}
	if want := "https://api.minimax.io/anthropic/v1/messages"; a.baseURL != want {
		t.Errorf("baseURL = %q, want %q", a.baseURL, want)
	}

	// The native anthropic provider must be unaffected (zero value).
	if got := Get(ProviderAnthropic).Name(); got != ProviderAnthropic {
		t.Errorf("anthropic Name() = %q, want %q", got, ProviderAnthropic)
	}

	if DisplayName(ProviderMiniMax) != "MiniMax" {
		t.Errorf("DisplayName = %q, want MiniMax", DisplayName(ProviderMiniMax))
	}

	// Model-name inference routes MiniMax-* to the minimax provider.
	for _, m := range []string{"MiniMax-M3", "MiniMax-M2.7", "MiniMax-M2.7-highspeed"} {
		if got := ProviderForModel(m); got != ProviderMiniMax {
			t.Errorf("ProviderForModel(%q) = %q, want %q", m, got, ProviderMiniMax)
		}
	}

	// MiniMax-M3 ("minimax 3") is curated with its 512K context window.
	if got := ContextWindowFor("MiniMax-M3"); got != 512000 {
		t.Errorf("ContextWindowFor(MiniMax-M3) = %d, want 512000", got)
	}
}
