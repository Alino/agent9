// runtime-test.js — console, Intl, fs.watch/watchFile, stdin plumbing, crypto extras.
// Everything here exists because a real npm package tripped over its absence.
// Run on the box:  qjs runtime-test.js
var pass = 0, fail = 0;
function ok(name, cond, extra) {
	if (cond) { pass++; console.log("ok   " + name); }
	else { fail++; console.log("FAIL " + name + (extra === undefined ? "" : " -- " + extra)); }
}
function eq(name, got, want) { ok(name, got === want, "got " + JSON.stringify(got) + " want " + JSON.stringify(want)); }

var fs = require("fs"), path = require("path"), os = require("os");

// ---- console: every method exists and routes to the right stream ----
// (a missing console.error is worse than useless: library catch blocks call it, so the
//  TypeError replaces the error they were trying to report)
function captureConsole(fn) {
	var out = [], err = [];
	var so = process.stdout.write, se = process.stderr.write;
	process.stdout.write = function (c) { out.push(String(c)); return true; };
	process.stderr.write = function (c) { err.push(String(c)); return true; };
	try { fn(); } finally { process.stdout.write = so; process.stderr.write = se; }
	return { out: out.join(""), err: err.join("") };
}
["log", "info", "debug", "error", "warn", "trace", "dir", "table", "group", "groupEnd", "time", "timeEnd", "count", "assert", "clear"].forEach(function (k) {
	ok("console." + k + " is a function", typeof console[k] === "function");
});
var cap = captureConsole(function () { console.log("to stdout"); console.error("to stderr"); });
eq("console.log -> stdout", cap.out, "to stdout\n");
eq("console.error -> stderr", cap.err, "to stderr\n");
cap = captureConsole(function () { console.log("%s has %d", "x", 3); });
eq("console.log formats", cap.out, "x has 3\n");
cap = captureConsole(function () { console.group("g"); console.log("inner"); console.groupEnd(); console.log("outer"); });
eq("console.group indents", cap.out, "g\n  inner\nouter\n");
cap = captureConsole(function () { console.assert(false, "boom"); console.assert(true, "quiet"); });
ok("console.assert only fires when falsy", cap.err.indexOf("boom") >= 0 && cap.err.indexOf("quiet") < 0);

// ---- Intl ----
ok("Intl.Segmenter exists", typeof Intl.Segmenter === "function");
function segs(s, gran) {
	var out = [];
	for (var seg of new Intl.Segmenter(undefined, { granularity: gran || "grapheme" }).segment(s)) out.push(seg.segment);
	return out;
}
eq("graphemes: plain ascii", segs("abc").length, 3);
eq("graphemes: combining mark joins", segs("éx").length, 2);
eq("graphemes: ZWJ emoji is one cluster", segs("\u{1F468}‍\u{1F4BB}").length, 1);
eq("graphemes: skin tone modifier joins", segs("\u{1F44D}\u{1F3FD}").length, 1);
eq("graphemes: regional pair is one flag", segs("\u{1F1E8}\u{1F1FF}").length, 1);
eq("graphemes: CRLF is one cluster", segs("a\r\nb").length, 3);
eq("graphemes: astral char stays whole", segs("\u{1F600}").length, 1);
ok("word granularity splits words", segs("hi there", "word").filter(function (s) { return s.trim(); }).length === 2);

// Collator: npm crashed when Intl existed but Collator did not
ok("Intl.Collator exists", typeof Intl.Collator === "function");
var coll = new Intl.Collator();
ok("collator sorts ascending", coll.compare("a", "b") < 0 && coll.compare("b", "a") > 0 && coll.compare("a", "a") === 0);
ok("collator compare is detachable", ["c", "a", "b"].sort(new Intl.Collator().compare).join("") === "abc");
var numeric = new Intl.Collator(undefined, { numeric: true });
ok("numeric collation orders by value", numeric.compare("file9", "file10") < 0);
eq("NumberFormat groups thousands", new Intl.NumberFormat().format(1234567), "1,234,567");
eq("NumberFormat honors fraction digits", new Intl.NumberFormat(undefined, { minimumFractionDigits: 2, maximumFractionDigits: 2 }).format(1.5), "1.50");
eq("DateTimeFormat formats a date", new Intl.DateTimeFormat(undefined, { dateStyle: "short" }).format(new Date(0)), "1970-01-01");

// ---- crypto extras ----
var crypto = require("crypto");
var ta = new Uint8Array(16);
crypto.getRandomValues(ta);
ok("crypto.getRandomValues fills", ta.some(function (b) { return b !== 0; }));
["createPrivateKey", "createPublicKey", "sign", "verify"].forEach(function (k) {
	var threw = false;
	try { crypto[k]("x"); } catch (e) { threw = /not supported on node9/.test(e.message); }
	ok("crypto." + k + " fails loudly", threw);
});
var threwHttps = false;
try { require("https").createServer(); } catch (e) { threwHttps = /not supported on node9/.test(e.message); }
ok("https.createServer fails loudly", threwHttps);

// ---- fs.watch / watchFile (polled) ----
var dir = fs.mkdtempSync(path.join(os.tmpdir(), "n9watch"));
var file = path.join(dir, "f.txt");
fs.writeFileSync(file, "one");

function later(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

async function watchTests() {
	var events = [];
	var w = fs.watch(file, { interval: 100 }, function (evt) { events.push(evt); });
	ok("fs.watch returns a closable watcher", typeof w.close === "function");
	await later(150);
	fs.writeFileSync(file, "one-two-three");
	await later(400);
	ok("fs.watch sees a write", events.length > 0, "events=" + JSON.stringify(events));
	w.close();
	var afterClose = events.length;
	fs.writeFileSync(file, "more");
	await later(300);
	eq("fs.watch stops after close", events.length, afterClose);

	var polled = [];
	fs.watchFile(file, { interval: 100 }, function (cur, prev) { polled.push([cur.size, prev.size]); });
	await later(150);
	fs.writeFileSync(file, "a much longer body than before");
	await later(400);
	ok("fs.watchFile sees the change", polled.length > 0);
	fs.unwatchFile(file);
	var afterUnwatch = polled.length;
	fs.writeFileSync(file, "x");
	await later(300);
	eq("fs.unwatchFile stops polling", polled.length, afterUnwatch);
	fs.rmSync(dir, { recursive: true, force: true });
}

// ---- process.stdin is a real Readable (piped input is tested by stdin-test.js) ----
ok("process.stdin is a Readable", typeof process.stdin.on === "function" && typeof process.stdin.resume === "function");
ok("process.stdin.setEncoding exists", typeof process.stdin.setEncoding === "function");
ok("process.stdin.setRawMode exists", typeof process.stdin.setRawMode === "function");
eq("process.stdin.fd", process.stdin.fd, 0);

watchTests().then(function () {
	console.log(pass + " passed, " + fail + " failed");
	if (fail) throw new Error(fail + " checks failed");
}).catch(function (e) { console.log("ERROR " + (e && e.stack || e)); throw e; });
