// node9 parity proof: run the REAL Node.js lib/querystring.js on node9.
var NLIB = '/amd64/lib/node9/nlib/';
function readFile(f){ var fp=std.open(f,'rb'); if(!fp)return null; var s=fp.readAsString(); fp.close(); return s; }
(0,eval)(readFile(NLIB+'primordials.js'));
var cache={};
function nrequire(name){
  if(name==='primordials')return globalThis.primordials;
  if(cache[name])return cache[name].exports;
  var src=readFile(NLIB+name+'.js');
  var mod={exports:{}}; cache[name]=mod;
  if(src===null)return mod.exports;
  var fn=new Function('module','exports','require','primordials','process','internalBinding',src);
  fn(mod,mod.exports,nrequire,globalThis.primordials,globalThis.process,globalThis.internalBinding);
  return mod.exports;
}
var qs=nrequire('querystring');
var pass=0,fail=0;
function eq(a,b,l){ if(a===b)pass++; else{fail++; console.log('FAIL '+l+': got '+JSON.stringify(a)+' want '+JSON.stringify(b));} }

eq(qs.stringify({foo:'bar',baz:'qux'}), 'foo=bar&baz=qux', 'stringify');
eq(qs.stringify({a:[1,2,3]}), 'a=1&a=2&a=3', 'stringify-array');
eq(qs.stringify({w:'with space',s:'a/b'}), 'w=with%20space&s=a%2Fb', 'stringify-encode');
var p=qs.parse('foo=bar&baz=qux');
eq(p.foo,'bar','parse.foo'); eq(p.baz,'qux','parse.baz');
var p2=qs.parse('a=1&a=2&a=3');
eq(Array.isArray(p2.a),true,'parse-array'); eq(p2.a.join(','),'1,2,3','parse-array-vals');
eq(qs.parse('w=with%20space').w,'with space','parse-decode');
eq(qs.escape('a b/c'),'a%20b%2Fc','escape');
eq(qs.unescape('a%20b%2Fc'),'a b/c','unescape');
eq(qs.stringify({a:'x',b:'y'},';',':'), 'a:x;b:y', 'stringify-custom-sep');
eq(Object.keys(qs.parse('a:x;b:y',';',':')).join(','),'a,b','parse-custom-sep');

console.log('querystring parity: '+pass+' passed, '+fail+' failed');
console.log(fail===0 ? 'QUERYSTRING 1:1 OK (running REAL Node lib/querystring.js on node9)' : 'QS HAS FAILURES');
