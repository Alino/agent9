// A CommonJS module, imported from ESM by esm-interop-test.mjs.
// QuickJS's loader only understands ES modules, so boot.js bridges this one.
const VERSION = "1.2.3";
function greet(who) { return "hello " + who; }
module.exports = { VERSION, greet };
module.exports.self = module.exports;
