// alloca stress: deep recursion + millions of calls that each hit the c_function/interp alloca path
function fib(n){ return n<2 ? n : fib(n-1)+fib(n-2); }
var t0 = Date.now();
var r = fib(28); // ~832040; ~832k calls, exercises JS_CallInternal alloca heavily
// also hammer c-function alloca via many builtin calls with varying argc
var acc = 0;
for (var i=0;i<200000;i++){ acc += Math.max(i, i-1, 1); acc += [i,1,2].indexOf(2); }
console.log('fib(28)=' + r + ' acc=' + acc + ' ms=' + (Date.now()-t0));
console.log('crypto sha512 still works: ' + require('crypto').createHash('sha512').update('abc').digest('hex').slice(0,16));
console.log('N6 SMOKE OK');
