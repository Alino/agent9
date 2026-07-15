const f=(n)=>n<2?n:f(n-1)+f(n-2); console.log(`fib(25) = ${f(25)}`);
try { throw new Error(`exceptions work`) } catch (e) { console.log(e.message) }
console.log([1,2,3].map(x=>x*x).join(`,`));
console.log(new Intl.NumberFormat(`de-DE`).format(1234567.89));
console.log(JSON.stringify({a:[1,{b:2n<<8n}].map(String)}));
console.log("äöü".toUpperCase(), "ΣΊΣΥΦΟΣ".toLowerCase());
console.log([..."x".repeat(3)].length, parseFloat("1e-7"), (0.1+0.2).toFixed(17));
console.log(new Date(0).toISOString());
let p = new Promise(r=>r(42)); p.then(v=>console.log(`async ${v}`));
class A { #x=7; get x(){return this.#x} } console.log(new A().x);
console.log(/(?<y>\d{4})/.exec("year 2026").groups.y);
