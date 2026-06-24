function f(a, b){ function a(){ return 7; } return a(); }
console.log('arg-hoist f=', f(1, 2));
function g(x){ function x(){ return 'X'; } var x; return typeof x; }
console.log('g=', g(0));
console.log('NESTED OK');
