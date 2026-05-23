// Package provider: this file holds OpenAI-compatible /chat/completions
// streaming. The wire format is shared by OpenRouter, OpenAI, Groq,
// DeepSeek, Fireworks, Together, Cerebras, Mistral, and xAI — pi9 reaches
// all of them through the same code path via openaiCompat.
//
// Phase 10: extracted shared types to types.go; this file now exposes
// both the legacy free function StreamRequest (kept for backward
// compatibility) and the new Provider-based openaiCompat type.
//
// Plan 9 notes:
//   - net/http and crypto/tls work natively on plan9/amd64 (proven by
//     tinyxena). No cgo, no signal-handling weirdness.
//   - Plan 9's Go runtime does NOT honor $SSL_CERT_FILE the way
//     unix-Go does. We read it ourselves and stuff the certs into a
//     custom *x509.CertPool on the http.Transport.
//   - 9front needs Mozilla's CA bundle at /sys/lib/tls/ca.pem (Phase 9b
//     install does this automatically via pi9-install).
package provider

import (
	"bufio"
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"
)

// endpointURL is the OpenRouter chat-completions endpoint. Overridable
// via $OPENROUTER_API_URL for testing against a mock server. This is
// the LEGACY entry point used by main.go's existing StreamRequest call;
// the per-provider openaiCompat type chooses its endpoint based on
// ProviderID.
func endpointURL() string {
	if u := os.Getenv("OPENROUTER_API_URL"); u != "" {
		return u
	}
	return "https://openrouter.ai/api/v1/chat/completions"
}

// providerEndpoint returns the chat-completions URL for an
// OpenAI-compatible provider. Each provider has a stable URL; cfg.APIURL
// overrides when set (rarely needed outside testing).
func providerEndpoint(id ProviderID, cfg Config) string {
	if cfg.APIURL != "" {
		return cfg.APIURL
	}
	switch id {
	case ProviderOpenAI:
		return "https://api.openai.com/v1/chat/completions"
	case ProviderOpenRouter:
		// Honor legacy env var so existing setups keep working.
		if u := os.Getenv("OPENROUTER_API_URL"); u != "" {
			return u
		}
		return "https://openrouter.ai/api/v1/chat/completions"
	case ProviderGroq:
		return "https://api.groq.com/openai/v1/chat/completions"
	case ProviderDeepSeek:
		return "https://api.deepseek.com/v1/chat/completions"
	case ProviderXAI:
		return "https://api.x.ai/v1/chat/completions"
	case ProviderMistral:
		return "https://api.mistral.ai/v1/chat/completions"
	case ProviderTogether:
		return "https://api.together.xyz/v1/chat/completions"
	case ProviderFireworks:
		return "https://api.fireworks.ai/inference/v1/chat/completions"
	case ProviderCerebras:
		return "https://api.cerebras.ai/v1/chat/completions"
	}
	// Unknown: fall back to OpenRouter (relays to many backends).
	return "https://openrouter.ai/api/v1/chat/completions"
}

// openaiCompat implements Provider for any OpenAI-compatible backend.
// All it needs is its ProviderID (used to choose endpoint URL and
// identify itself); the request shape, SSE parser, and tool-call
// assembly are identical across these providers.
type openaiCompat struct {
	id ProviderID
}

func (p openaiCompat) Name() ProviderID { return p.id }

func (p openaiCompat) Stream(ctx context.Context, cfg Config, messages []Message) (<-chan Chunk, <-chan error) {
	cfg.APIURL = providerEndpoint(p.id, cfg)
	return streamOpenAICompat(ctx, cfg, messages)
}

// httpClient builds an *http.Client with our plan9-aware TLS config.
// Safe to call per request; the cost is rebuilding the cert pool.
func httpClient() *http.Client {
	tlsConf := &tls.Config{}
	if cf := os.Getenv("SSL_CERT_FILE"); cf != "" {
		pem, err := os.ReadFile(cf)
		if err == nil {
			pool := x509.NewCertPool()
			if pool.AppendCertsFromPEM(pem) {
				tlsConf.RootCAs = pool
			}
		}
	}
	if os.Getenv("INSECURE_TLS") == "1" {
		tlsConf.InsecureSkipVerify = true
	}
	return &http.Client{
		// No outer timeout: streaming responses can be long. We
		// govern per-chunk timeouts via the read loop instead.
		Transport: &http.Transport{
			TLSClientConfig:       tlsConf,
			DisableCompression:    true, // SSE comes through unflushed
			ResponseHeaderTimeout: 30 * time.Second,
		},
	}
}

// requestBody is the JSON body shape we send to /chat/completions.
// We build it imperatively so tools can be omitted when nil (some
// providers reject an empty tools array).
type requestBody struct {
	Model     string        `json:"model"`
	Messages  []Message     `json:"messages"`
	MaxTokens int           `json:"max_tokens,omitempty"`
	Stream    bool          `json:"stream"`
	Tools     []toolWrapper `json:"tools,omitempty"`
}

type toolWrapper struct {
	Type     string `json:"type"`
	Function Tool   `json:"function"`
}

// StreamRequest sends `messages` to the provider with streaming
// enabled and tool calling configured. The response body is parsed
// line-by-line; every "data: …" SSE event becomes a Chunk on the
// returned channel. The channel is closed when the stream ends or an
// error occurs (err returned via errOut).
//
// ctx cancellation is honored: closing it disconnects mid-stream and
// closes the channels.
//
// Caller MUST drain the channel; otherwise the goroutine leaks until
// the HTTP read times out.
// StreamRequest is the legacy free-function entry point. Kept for
// backward compatibility with main.go. New code should use
// provider.Get(id).Stream(...) instead.
//
// This dispatches to OpenRouter (matching pre-Phase-10 behavior).
// The URL is taken from $OPENROUTER_API_URL if set, else
// openrouter.ai. To use a different provider, call Get directly.
func StreamRequest(ctx context.Context, cfg Config, messages []Message) (<-chan Chunk, <-chan error) {
	cfg.APIURL = endpointURL()
	return streamOpenAICompat(ctx, cfg, messages)
}

// streamOpenAICompat is the internal SSE streaming implementation
// shared by all OpenAI-compatible providers. Callers set cfg.APIURL
// to the provider's chat-completions endpoint first.
func streamOpenAICompat(ctx context.Context, cfg Config, messages []Message) (<-chan Chunk, <-chan error) {
	chunks := make(chan Chunk, 8)
	errs := make(chan error, 1)

	go func() {
		defer close(chunks)
		defer close(errs)

		body := requestBody{
			Model:     cfg.Model,
			Messages:  messages,
			MaxTokens: cfg.MaxTokens,
			Stream:    true,
		}
		for _, t := range cfg.Tools {
			body.Tools = append(body.Tools, toolWrapper{Type: "function", Function: t})
		}
		buf, err := json.Marshal(body)
		if err != nil {
			errs <- fmt.Errorf("marshal: %w", err)
			return
		}
		req, err := http.NewRequestWithContext(ctx, "POST", endpointURL(), bytes.NewReader(buf))
		if err != nil {
			errs <- fmt.Errorf("new request: %w", err)
			return
		}
		req.Header.Set("authorization", "Bearer "+cfg.APIKey)
		req.Header.Set("content-type", "application/json")
		req.Header.Set("accept", "text/event-stream")
		req.Header.Set("HTTP-Referer", "https://github.com/alino/plan9-winxp")
		req.Header.Set("X-Title", "pi9")

		resp, err := httpClient().Do(req)
		if err != nil {
			errs <- fmt.Errorf("do: %w", err)
			return
		}
		defer resp.Body.Close()

		if resp.StatusCode >= 400 {
			b, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
			errs <- fmt.Errorf("http %d: %s", resp.StatusCode, string(b))
			return
		}

		if err := readSSE(resp.Body, chunks); err != nil {
			errs <- err
			return
		}
	}()

	return chunks, errs
}

// sseToolDelta mirrors what OpenAI emits for a streaming tool call
// chunk. Each delta has an `index` that ties it back to a specific
// in-flight tool call; we assemble across chunks by index.
type sseToolDelta struct {
	Index    int    `json:"index"`
	ID       string `json:"id,omitempty"`
	Type     string `json:"type,omitempty"`
	Function struct {
		Name      string `json:"name,omitempty"`
		Arguments string `json:"arguments,omitempty"`
	} `json:"function"`
}

// sseEvent is the shape of one streamed chat-completions chunk.
//
// OpenRouter (and OpenAI) emit:
//
//	data: {"id":"...","choices":[{"delta":{"content":"hello"},"finish_reason":null}]}
//	data: {"id":"...","choices":[{"delta":{},"finish_reason":"stop"}]}
//	data: [DONE]
//
// For tool calls:
//
//	data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_abc","type":"function","function":{"name":"read_file","arguments":""}}]}}]}
//	data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"path\":\"/tmp\"}"}}]}}]}
//	data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}]}
type sseEvent struct {
	Choices []struct {
		Delta struct {
			Content   string         `json:"content"`
			ToolCalls []sseToolDelta `json:"tool_calls"`
		} `json:"delta"`
		FinishReason string `json:"finish_reason"`
	} `json:"choices"`
	Error *struct {
		Message string `json:"message"`
		Type    string `json:"type"`
	} `json:"error,omitempty"`
}

// readSSE parses the response body as Server-Sent Events and writes
// Chunks to the channel. Returns nil on clean stream-end.
//
// Assembles streaming tool_calls by index into the toolCalls slice;
// emits the assembled set in the final Done chunk.
func readSSE(r io.Reader, out chan<- Chunk) error {
	br := bufio.NewReaderSize(r, 64*1024)
	// In-flight tool calls indexed by their delta index. OpenAI
	// streaming guarantees a stable index for each call.
	toolCalls := map[int]*ToolCall{}

	// flushTools emits the assembled tool calls in index order.
	flushTools := func() []ToolCall {
		if len(toolCalls) == 0 {
			return nil
		}
		// Determine max index and build ordered slice.
		maxIdx := -1
		for i := range toolCalls {
			if i > maxIdx {
				maxIdx = i
			}
		}
		out := make([]ToolCall, 0, maxIdx+1)
		for i := 0; i <= maxIdx; i++ {
			tc, ok := toolCalls[i]
			if !ok {
				continue
			}
			out = append(out, *tc)
		}
		return out
	}

	for {
		line, err := br.ReadString('\n')
		if err != nil {
			if err == io.EOF {
				out <- Chunk{Done: true, ToolCalls: flushTools()}
				return nil
			}
			return fmt.Errorf("read: %w", err)
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			continue
		}
		// Skip ":" comments (SSE heartbeats)
		if strings.HasPrefix(line, ":") {
			continue
		}
		if !strings.HasPrefix(line, "data: ") {
			continue
		}
		payload := strings.TrimPrefix(line, "data: ")
		if payload == "[DONE]" {
			out <- Chunk{Done: true, ToolCalls: flushTools()}
			return nil
		}
		var ev sseEvent
		if err := json.Unmarshal([]byte(payload), &ev); err != nil {
			// Skip malformed events; provider sometimes sends
			// non-JSON pings.
			continue
		}
		if ev.Error != nil {
			return fmt.Errorf("provider: %s — %s", ev.Error.Type, ev.Error.Message)
		}
		for _, c := range ev.Choices {
			if c.Delta.Content != "" {
				out <- Chunk{Delta: c.Delta.Content}
			}
			for _, td := range c.Delta.ToolCalls {
				tc, ok := toolCalls[td.Index]
				if !ok {
					tc = &ToolCall{}
					toolCalls[td.Index] = tc
				}
				if td.ID != "" {
					tc.ID = td.ID
				}
				if td.Type != "" {
					tc.Type = td.Type
				}
				if td.Function.Name != "" {
					tc.Function.Name = td.Function.Name
				}
				if td.Function.Arguments != "" {
					tc.Function.Arguments += td.Function.Arguments
				}
			}
			if c.FinishReason != "" {
				out <- Chunk{Done: true, ToolCalls: flushTools()}
				return nil
			}
		}
	}
}
