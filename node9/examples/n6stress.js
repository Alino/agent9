// fib(32): ~7 million calls. Under the leaking shim (~150B each) this is >1GB -> OOM.
// Under the lazy-free shim, live alloca memory is bounded by stack depth (~32 frames).
function fib(n){ return n<2 ? n : fib(n-1)+fib(n-2); }
var t0 = Date.now();
var r = fib(32);
console.log('fib(32)=' + r + ' (' + (Date.now()-t0) + 'ms) -- completed, no OOM');
console.log('N6 STRESS OK: 7M alloca-path calls, bounded memory');
