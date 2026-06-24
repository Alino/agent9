// node9 feature smoke test — exercises engine, libregexp, libunicode.
const add = a => b => a + b;
function fib(n){ return n < 2 ? n : fib(n-1) + fib(n-2); }
let out = {
  closure:   add(3)(4),                                       // 7
  recursion: fib(15),                                         // 610
  arrayhof:  [1,2,3,4,5].filter(x=>x%2).map(x=>x*x).reduce((a,b)=>a+b,0), // 35
  regex:     "hello world 123".match(/(\w+)\s+(\w+)\s+(\d+)/).slice(1), // [hello,world,123]
  split:     "a,b,c,d".split(",").length,                     // 4
  replace:   "foo123bar456".replace(/\d+/g, "#"),             // foo#bar#
  unicode:   [..."café"].length,                              // 4
  upper:     "straße".toUpperCase(),                          // STRASSE
  trycatch:  (() => { try { null.x; } catch(e){ return e.constructor.name; } })(), // TypeError
  json:      JSON.parse('{"nested":{"arr":[true,null,3.5]}}').nested.arr,
  map:       (() => { let m = new Map([["k",99]]); return m.get("k"); })(),
  set:       new Set([1,1,2,2,3]).size,                       // 3
  spread:    Math.max(...[4,8,15,16,23,42]),                  // 42
  bigint:    (2n ** 64n).toString(),                          // 18446744073709551616
};
JSON.stringify(out, null, 1);
