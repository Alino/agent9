// fetchtest.js — the Web fetch layer (fetch/Headers/Response/Request/Blob + body streams).
// Run on the box:  qjs fetchtest.js       (needs outbound TLS for the network checks)
var pass = 0, fail = 0;
function ok(name, cond, extra) {
	if (cond) { pass++; console.log("ok   " + name); }
	else { fail++; console.log("FAIL " + name + (extra === undefined ? "" : " -- " + extra)); }
}
function eq(name, got, want) { ok(name, got === want, "got " + JSON.stringify(got) + " want " + JSON.stringify(want)); }

// ---- Headers ----
var h = new Headers({ "Content-Type": "application/json", "X-Multi": ["a", "b"] });
eq("headers.get case-insensitive", h.get("content-TYPE"), "application/json");
eq("headers.get missing -> null", h.get("nope"), null);
eq("headers array init joins", h.get("x-multi"), "a, b");
h.set("X-One", "1");
ok("headers.has", h.has("x-one"));
h["delete"]("x-one");
ok("headers.delete", !h.has("x-one"));
var seen = {};
h.forEach(function (v, k) { seen[k] = v; });
eq("headers.forEach lowercases keys", seen["content-type"], "application/json");
var ents = [];
for (var e of h.entries()) ents.push(e[0]);
ok("headers.entries iterable", ents.indexOf("content-type") >= 0);
var copy = new Headers(h);
eq("headers copy-construct", copy.get("content-type"), "application/json");

// ---- Response bodies ----
async function bodies() {
	var r = new Response("hello", { status: 201, statusText: "Created", headers: { "x-a": "1" } });
	eq("response.status", r.status, 201);
	eq("response.statusText", r.statusText, "Created");
	ok("response.ok for 201", r.ok === true);
	eq("response header", r.headers.get("x-a"), "1");
	eq("response.text", await r.text(), "hello");

	var j = new Response(JSON.stringify({ a: [1, 2] }));
	var v = await j.json();
	eq("response.json", JSON.stringify(v), '{"a":[1,2]}');

	var b = new Response(new Uint8Array([104, 105]));
	var bytes = await b.bytes();
	ok("response.bytes", bytes.length === 2 && bytes[0] === 104);

	var ab = await new Response("xy").arrayBuffer();
	eq("response.arrayBuffer byteLength", ab.byteLength, 2);

	// reading twice must be refused, like Node
	var once = new Response("z");
	await once.text();
	var threw = false;
	try { await once.text(); } catch (err) { threw = true; }
	ok("body can only be read once", threw);

	// a buffered body still exposes a reader
	var s = new Response("abc").body.getReader();
	var chunk = await s.read();
	eq("body.getReader chunk", String.fromCharCode.apply(null, chunk.value), "abc");
	ok("body.getReader ends", (await s.read()).done === true);

	eq("Response.json static", (await Response.json({ k: 1 }).json()).k, 1);
	eq("Response.json sets content-type", Response.json({}).headers.get("content-type"), "application/json");

	var bl = new Blob(["ab", "cd"]);
	eq("blob.size", bl.size, 4);
	eq("blob.text", await bl.text(), "abcd");

	var rq = new Request("https://example.com/x", { method: "post", headers: { a: "1" } });
	eq("request.method uppercased", rq.method, "POST");
	eq("request.url", rq.url, "https://example.com/x");
	eq("request.headers", rq.headers.get("a"), "1");
}

// ---- TextDecoder streaming (SSE readers decode split chunks) ----
function decoder() {
	var dec = new TextDecoder();
	// "é" is 0xC3 0xA9 — split across two chunks
	var a = dec.decode(new Uint8Array([0xc3]), { stream: true });
	var b = dec.decode(new Uint8Array([0xa9]), { stream: true });
	eq("TextDecoder holds split utf8", a + b, "é");
	eq("TextDecoder one-shot", new TextDecoder().decode(new Uint8Array([104, 105])), "hi");
}

// ---- AbortSignal.any ----
function signals() {
	var c1 = new AbortController(), c2 = new AbortController();
	var any = AbortSignal.any([c1.signal, c2.signal]);
	ok("AbortSignal.any starts unaborted", any.aborted === false);
	c2.abort(new Error("boom"));
	ok("AbortSignal.any follows a member", any.aborted === true);
	ok("AbortSignal.any carries reason", any.reason && any.reason.message === "boom");
	ok("AbortSignal.any of an already-aborted signal", AbortSignal.any([AbortSignal.abort()]).aborted === true);
}

// ---- undici shim ----
function undiciShim() {
	var undici = require("undici");
	var pool = new undici.Pool("https://example.com", { connections: 4 });
	ok("undici.Pool constructs", pool.origin === "https://example.com");
	ok("undici dispatchers are EventEmitters", typeof pool.on === "function");
	undici.setGlobalDispatcher(pool);
	ok("undici.getGlobalDispatcher round-trips", undici.getGlobalDispatcher() === pool);
	undici.install();
	ok("undici.install leaves node9 fetch in place", globalThis.fetch === fetch);
	ok("undici.fetch delegates", typeof undici.fetch === "function");
}

// ---- live network (real TLS to the npm registry) ----
async function network() {
	var res = await fetch("https://registry.npmjs.org/left-pad");
	eq("fetch status", res.status, 200);
	ok("fetch ok flag", res.ok === true);
	ok("fetch content-type", (res.headers.get("content-type") || "").indexOf("json") >= 0);
	var body = await res.json();
	eq("fetch json body", body.name, "left-pad");

	// stream the same document through a reader, the way SSE consumers do
	var res2 = await fetch("https://registry.npmjs.org/left-pad");
	var reader = res2.body.getReader(), dec = new TextDecoder(), text = "", n = 0;
	for (;;) {
		var r = await reader.read();
		if (r.done) break;
		n++;
		text += dec.decode(r.value, { stream: true });
	}
	text += dec.decode();
	ok("streamed body chunks", n >= 1, "chunks=" + n);
	eq("streamed body parses", JSON.parse(text).name, "left-pad");

	// for-await over the body stream
	var res3 = await fetch("https://registry.npmjs.org/left-pad");
	var total = 0;
	for await (var chunk of res3.body) total += chunk.length;
	ok("for-await over body", total > 100, "total=" + total);

	// POST with a body reaches the server (httpbin-free: the registry 405s a POST to a doc)
	var res4 = await fetch("https://registry.npmjs.org/left-pad", { method: "POST", body: JSON.stringify({ a: 1 }), headers: { "content-type": "application/json" } });
	ok("POST completes and returns a status", res4.status >= 400 && res4.status < 500, "status=" + res4.status);

	// abort before the request starts
	var threw = false;
	try { await fetch("https://registry.npmjs.org/left-pad", { signal: AbortSignal.abort() }); } catch (err) { threw = true; }
	ok("pre-aborted signal rejects", threw);

	// abort mid-flight
	var ctl = new AbortController();
	var p = fetch("https://registry.npmjs.org/lodash", { signal: ctl.signal });
	ctl.abort();
	var threw2 = false;
	try { await p; } catch (err) { threw2 = true; }
	ok("mid-flight abort rejects", threw2);
}

async function main() {
	await bodies();
	decoder();
	signals();
	undiciShim();
	if (globalThis.scriptArgs && scriptArgs.indexOf("--offline") >= 0) console.log("(skipping network checks)");
	else await network();
	console.log(pass + " passed, " + fail + " failed");
	if (fail) throw new Error(fail + " checks failed");
}

main().catch(function (e) { console.log("ERROR " + (e && e.stack || e)); throw e; });
