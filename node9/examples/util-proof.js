// node9 parity proof: run the REAL Node.js lib/util.js on node9 (over node9 inspect internals).
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
  std.err.puts("loading: "+name+"\n"); std.err.flush(); var fn=new Function('module','exports','require','primordials','process','internalBinding',src);
  fn(mod,mod.exports,nrequire,globalThis.primordials,globalThis.process,globalThis.internalBinding);
  return mod.exports;
}
nrequire("internal-bindings"); var util=nrequire("util");
var pass=0,fail=0;
function eq(a,b,l){ if(a===b)pass++; else{fail++; console.log('FAIL '+l+': got '+JSON.stringify(a)+' want '+JSON.stringify(b));} }
function ok(c,l){ if(c)pass++; else{fail++; console.log('FAIL '+l);} }

eq(util.format('%s:%d','x',5), 'x:5', 'format-sd');
eq(util.format('%s','hi','extra'), 'hi extra', 'format-extra');
eq(util.format('%j',{a:1}), '{"a":1}', 'format-j');
eq(util.format('100%% done'), '100% done', 'format-pct');
eq(util.format('no specifiers',1,2), 'no specifiers 1 2', 'format-trailing');
ok(typeof util.inspect({a:1})==='string', 'inspect-string');
ok(util.inspect([1,2,3]).indexOf('1')>=0, 'inspect-array');
eq(util.types.isDate(new Date()), true, 'types.isDate');
eq(util.types.isRegExp(/x/), true, 'types.isRegExp');
eq(util.types.isPromise(Promise.resolve()), true, 'types.isPromise');
ok(typeof util.promisify==='function', 'promisify-exists');
function Animal(){} function Dog(){}
util.inherits(Dog, Animal);
ok(new Dog() instanceof Animal, 'inherits');
eq(util.isDeepStrictEqual({a:[1,2]},{a:[1,2]}), true, 'isDeepStrictEqual-true');
eq(util.isDeepStrictEqual({a:1},{a:2}), false, 'isDeepStrictEqual-false');
ok(typeof util.deprecate(function(){},'msg')==='function', 'deprecate');

// promisify roundtrip
var pf = util.promisify(function(x, cb){ cb(null, x*2); });
pf(21).then(function(v){
  eq(v, 42, 'promisify-result');
  console.log('util parity: '+pass+' passed, '+fail+' failed');
  console.log(fail===0 ? 'UTIL OK (running REAL Node lib/util.js on node9)' : 'UTIL HAS FAILURES');
});
