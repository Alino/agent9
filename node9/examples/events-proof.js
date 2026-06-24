// node9 parity proof: run the REAL Node.js lib/events.js on node9.
var NLIB = '/amd64/lib/node9/nlib/';
function readFile(f) { var fp = std.open(f, 'rb'); if (!fp) return null; var s = fp.readAsString(); fp.close(); return s; }
(0, eval)(readFile(NLIB + 'primordials.js'));
var cache = {};
function nrequire(name) {
  if (name === 'primordials') return globalThis.primordials;
  if (cache[name]) return cache[name].exports;
  var src = readFile(NLIB + name + '.js');
  var mod = { exports: {} };
  cache[name] = mod;
  if (src === null) return mod.exports; // unused lazy dep -> empty stub
  var fn = new Function('module', 'exports', 'require', 'primordials', 'process', src);
  fn(mod, mod.exports, nrequire, globalThis.primordials, globalThis.process);
  return mod.exports;
}

var EventEmitter = nrequire('events');
var EE = EventEmitter.EventEmitter || EventEmitter;

var pass = 0, fail = 0, log = [];
function ok(cond, label) { if (cond) pass++; else { fail++; log.push('FAIL ' + label); } }
function eq(a, b, label) { ok(a === b, label + ' (got ' + JSON.stringify(a) + ' want ' + JSON.stringify(b) + ')'); }

var e = new EE();
var hits = [];
ok(e.on('x', function (v) { hits.push('a' + v); }) === e, 'on returns this');
e.on('x', function (v) { hits.push('b' + v); });
eq(e.emit('x', 1), true, 'emit returns true when listeners');
eq(hits.join(','), 'a1,b1', 'both listeners fired in order');
eq(e.emit('nope'), false, 'emit returns false no listeners');
eq(e.listenerCount('x'), 2, 'listenerCount method');
eq(EE.listenerCount(e, 'x'), 2, 'listenerCount static');

var onceN = 0;
e.once('y', function () { onceN++; });
e.emit('y'); e.emit('y'); e.emit('y');
eq(onceN, 1, 'once fires exactly once');

function h(v) { hits.push('rm'); }
e.on('z', h); e.removeListener('z', h); e.emit('z');
eq(e.listenerCount('z'), 0, 'removeListener works');

var pre = [];
e.on('p', function () { pre.push('on'); });
e.prependListener('p', function () { pre.push('prepend'); });
e.emit('p');
eq(pre.join(','), 'prepend,on', 'prependListener ordering');

eq(typeof EE.once, 'function', 'EE.once static exists');
eq(e.eventNames().includes('x'), true, 'eventNames');
e.setMaxListeners(42); eq(e.getMaxListeners(), 42, 'set/getMaxListeners');

// 'newListener' meta event
var meta = [];
var e2 = new EE();
e2.on('newListener', function (name) { meta.push(name); });
e2.on('hello', function () {});
eq(meta.join(','), 'hello', 'newListener meta event');

// error event throws when unhandled
var threw = false;
try { new EE().emit('error', new Error('boom')); } catch (err) { threw = true; }
eq(threw, true, 'unhandled error event throws');

// EE.once returns a Promise that resolves with emitted args
var p = EE.once(e, 'done');
ok(p instanceof Promise, 'EE.once returns Promise');
e.emit('done', 'RESULT');
p.then(function (args) {
  eq(args[0], 'RESULT', 'EE.once promise resolves with args');
  finish();
}, function () { fail++; log.push('FAIL EE.once rejected'); finish(); });

function finish() {
  log.forEach(function (l) { console.log(l); });
  console.log('events parity: ' + pass + ' passed, ' + fail + ' failed');
  console.log(fail === 0 ? 'EVENTS 1:1 OK (running REAL Node lib/events.js on node9)' : 'EVENTS HAS FAILURES');
}
