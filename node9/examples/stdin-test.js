// stdin-test.js — process.stdin really reads fd 0.
// Run it three ways on the box; each prints a line that must match:
//   echo -n hello | qjs stdin-test.js      -> STDIN[hello] chunks>=1
//   qjs stdin-test.js </dev/null           -> STDIN[] chunks=0
//   qjs stdin-test.js <somefile            -> STDIN[<contents>]
// The old stub emitted nothing at all, so any program that read piped input (every
// CLI with a --print or filter mode) silently saw an empty stream.
var chunks = 0, text = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", function (c) { chunks++; text += c; });
process.stdin.on("error", function (e) { console.log("STDIN ERROR " + e.message); });
process.stdin.on("end", function () {
	console.log("STDIN[" + text + "] chunks=" + chunks);
	if (globalThis.scriptArgs && scriptArgs.indexOf("--expect-empty") >= 0 && text !== "") throw new Error("expected empty stdin");
	if (globalThis.scriptArgs && scriptArgs.indexOf("--expect-hello") >= 0 && text.replace(/\n$/, "") !== "hello") throw new Error("expected 'hello', got " + JSON.stringify(text));
});
process.stdin.resume();
