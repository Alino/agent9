// esm-interop-test.mjs — ES-module resolution: CommonJS bridging, wildcard "exports"
// subpaths, JSON import attributes, and node: builtins reached by bare specifier.
// Every check here is a bug that stopped a real package from loading.
// Run on the box (from this directory):  qjs esm-interop-test.mjs
import cjsDefault, { VERSION, greet } from "./fixtures/cjs-dep.js";
import wildDefault, { thing } from "wild/sub/thing";
import { root } from "wild";
import data from "./fixtures/data-attr.json" with { type: "json" };
import { homedir, platform } from "node:os";
import bareOs from "os";
import { join } from "path";

let pass = 0, fail = 0;
function ok(name, cond, extra) {
	if (cond) { pass++; console.log("ok   " + name); }
	else { fail++; console.log("FAIL " + name + (extra === undefined ? "" : " -- " + extra)); }
}
function eq(name, got, want) { ok(name, got === want, "got " + JSON.stringify(got) + " want " + JSON.stringify(want)); }

// CommonJS imported from ESM: default is module.exports, named keys are re-exported
eq("cjs default export is module.exports", cjsDefault.VERSION, "1.2.3");
eq("cjs named export", VERSION, "1.2.3");
eq("cjs named function export", greet("plan9"), "hello plan9");
ok("cjs default identity is preserved", cjsDefault.self === cjsDefault);

// package "exports" with a wildcard subpath
eq("wildcard subpath named export", thing, "wildcard-subpath-ok");
eq("wildcard subpath default export", wildDefault, "wild-default");
eq("package root export", root, "root-export");

// JSON module (import attributes)
eq("json import name", data.name, "attr-json");
eq("json import array", data.values.join(","), "1,2,3");

// node: builtins, and the bare specifier "os" meaning node:os rather than the engine's
eq("node:os platform", platform(), "plan9");
ok("node:os homedir", typeof homedir() === "string" && homedir().length > 0);
ok("bare 'os' is node:os, not the quickjs module", typeof bareOs.homedir === "function");
eq("bare 'os' agrees with node:os", bareOs.platform(), platform());
eq("path join still works", join("/a", "b"), "/a/b");

// dynamic import of a CommonJS file inside an ESM package
const dyn = await import("wild/sub/cjs-inside");
ok("dynamic import of .cjs inside an ESM package", (dyn.default || dyn).fromCjsInsideEsmPackage === true);

console.log(pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " checks failed");
