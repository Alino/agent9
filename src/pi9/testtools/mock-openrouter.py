#!/usr/bin/env python3
"""mock-openrouter.py — tiny OpenAI/OpenRouter SSE mock.

Listens on :8766 and pretends to be openrouter.ai/api/v1/chat/completions.

Behavior:
  - If the request's most recent message has role=tool, this is the
    second-stream round of an agent loop. Emit a short text reply
    and finish_reason=stop.
  - Otherwise, decide what to do based on the user message:
      * if message contains "ls" or "what's in"  -> emit one tool_call
        for run_rc("ls /tmp") then finish_reason=tool_calls
      * if message contains "read"               -> emit read_file(/etc/passwd)
        ... actually we'll only ask run_rc since /etc/passwd may not exist
      * otherwise -> emit a canned text response (Phase 2 behavior)

Usage:
    python3 mock-openrouter.py
    OPENROUTER_API_KEY=mockkey \
    OPENROUTER_API_URL=http://10.0.2.2:8766/chat \
    /tmp/pi9
"""
import http.server
import json
import socketserver
import sys
import time

PORT = 8766

CANNED_TEXT = [
    "Hello", " from", " the", " mock", " server!\n\n",
    "I'm", " not", " a", " real", " LLM", " —", " just", " a", " Python", " HTTP",
    " server", " streaming", " bytes", " to", " prove", " your", " SSE",
    " parser", " works.\n\n",
    "Phase", " 2", " streaming", " ✓",
]

POST_TOOL_TEXT = [
    "Looking", " at", " the", " output:\n\n",
    "Phase", " 3", " tool", " calling", " works", " ✓",
]


def emit_text(wfile, tokens):
    for tok in tokens:
        ev = {
            "id": "mock",
            "choices": [{"delta": {"content": tok}, "finish_reason": None}],
        }
        wfile.write(b"data: " + json.dumps(ev).encode() + b"\n\n")
        wfile.flush()
        time.sleep(0.04)


def emit_done(wfile, reason="stop"):
    ev = {"id": "mock", "choices": [{"delta": {}, "finish_reason": reason}]}
    wfile.write(b"data: " + json.dumps(ev).encode() + b"\n\n")
    wfile.write(b"data: [DONE]\n\n")
    wfile.flush()


def emit_tool_call(wfile, name, args, call_id="call_mock_1"):
    """Emit a tool call in three deltas (id+name; args; final)."""
    # Delta 1: id, type, name (no args yet)
    ev1 = {
        "id": "mock",
        "choices": [{
            "delta": {"tool_calls": [{
                "index": 0,
                "id": call_id,
                "type": "function",
                "function": {"name": name, "arguments": ""},
            }]},
            "finish_reason": None,
        }],
    }
    wfile.write(b"data: " + json.dumps(ev1).encode() + b"\n\n")
    wfile.flush()
    time.sleep(0.05)

    # Delta 2: just the arguments (could be multiple chunks; we keep
    # it as one for simplicity)
    args_json = json.dumps(args)
    ev2 = {
        "id": "mock",
        "choices": [{
            "delta": {"tool_calls": [{
                "index": 0,
                "function": {"arguments": args_json},
            }]},
            "finish_reason": None,
        }],
    }
    wfile.write(b"data: " + json.dumps(ev2).encode() + b"\n\n")
    wfile.flush()
    time.sleep(0.05)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        sys.stderr.write("mock: " + (format % args) + "\n")

    def do_POST(self):
        length = int(self.headers.get("content-length", 0))
        body = self.rfile.read(length)
        try:
            req = json.loads(body)
        except Exception:
            req = {}
        msgs = req.get("messages", [])
        last = msgs[-1] if msgs else {}
        last_role = last.get("role", "")
        last_text = last.get("content", "")
        sys.stderr.write(f"mock: POST model={req.get('model')} msgs={len(msgs)} last_role={last_role} stream={req.get('stream')}\n")

        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("cache-control", "no-cache")
        self.end_headers()

        # If the last message is a tool result, we're in the
        # post-tool round. Emit a brief text reply and stop.
        if last_role == "tool":
            emit_text(self.wfile, POST_TOOL_TEXT)
            emit_done(self.wfile, "stop")
            return

        # Decide based on user content.
        text = (last_text or "").lower()
        if "remember" in text or "save" in text:
            emit_tool_call(self.wfile, "remember", {"content": "Alex prefers concise replies. Plan 9 is the development target."})
            emit_done(self.wfile, "tool_calls")
            return

        if "skill" in text or "rc syntax" in text:
            emit_tool_call(self.wfile, "read_skill", {"name": "plan9-rc"})
            emit_done(self.wfile, "tool_calls")
            return

        if "namespace" in text or "ns " in text or "what's mounted" in text:
            emit_tool_call(self.wfile, "ns", {"filter": "/srv"})
            emit_done(self.wfile, "tool_calls")
            return

        if "walk" in text or "tree" in text or "explore" in text:
            emit_tool_call(self.wfile, "walk", {"path": "/sys/lib/tls", "depth": 2})
            emit_done(self.wfile, "tool_calls")
            return

        if "hget" in text or "fetch" in text or "download" in text or "url" in text:
            emit_tool_call(self.wfile, "hget", {"url": "http://10.0.2.2:8765/testtools/seed-skills/plan9-rc.md"})
            emit_done(self.wfile, "tool_calls")
            return

        if "plumb" in text:
            emit_tool_call(self.wfile, "plumb", {"port": "edit", "content": "/sys/sysname"})
            emit_done(self.wfile, "tool_calls")
            return

        if "ls" in text or "what's in" in text or "tmp" in text:
            emit_tool_call(self.wfile, "run_rc", {"command": "ls /tmp"})
            emit_done(self.wfile, "tool_calls")
            return

        if "read" in text and "file" in text:
            emit_tool_call(self.wfile, "read_file", {"path": "/sys/sysname"})
            emit_done(self.wfile, "tool_calls")
            return

        # Default: canned text response (Phase 2 behavior)
        emit_text(self.wfile, CANNED_TEXT)
        emit_done(self.wfile, "stop")


def main():
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("0.0.0.0", PORT), Handler) as srv:
        print(f"mock-openrouter listening on http://0.0.0.0:{PORT}/chat")
        srv.serve_forever()


if __name__ == "__main__":
    main()
