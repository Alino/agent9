// node9 parity proof: run the REAL Node.js lib/path.js (v24.18.0) on node9,
// backed by the node9 primordials shim. Verify against Node's documented behavior.
var NLIB = '/amd64/lib/node9/nlib/';
function readFile(f) { var fp = std.open(f, 'rb'); if (!fp) throw new Error('cannot open ' + f); var s = fp.readAsString(); fp.close(); return s; }
(0, eval)(readFile(NLIB + 'primordials.js')); // sets globalThis.primordials

var cache = {};
function nrequire(name) {
  if (name === 'primordials') return globalThis.primordials;
  if (cache[name]) return cache[name].exports;
  var src = readFile(NLIB + name + '.js');
  var mod = { exports: {} };
  cache[name] = mod;
  var fn = new Function('module', 'exports', 'require', 'primordials', 'process', src);
  fn(mod, mod.exports, nrequire, globalThis.primordials, globalThis.process);
  return mod.exports;
}

var path = nrequire('path');
var posix = path.posix || path;

var pass = 0, fail = 0;
function eq(actual, expected, label) {
  if (actual === expected) pass++;
  else { fail++; console.log('FAIL ' + label + ': got ' + JSON.stringify(actual) + ' want ' + JSON.stringify(expected)); }
}
eq(posix.join('/foo', 'bar', 'baz/..'), '/foo/bar', 'join1');
eq(posix.join('/foo', 'bar', 'baz/asdf', 'quux', '..'), '/foo/bar/baz/asdf', 'join2');
eq(posix.normalize('/foo/bar//baz/asdf/quux/..'), '/foo/bar/baz/asdf', 'normalize');
eq(posix.resolve('/foo/bar', './baz'), '/foo/bar/baz', 'resolve1');
eq(posix.resolve('/foo/bar', '/tmp/file/'), '/tmp/file', 'resolve2');
eq(posix.dirname('/foo/bar/baz/asdf/quux'), '/foo/bar/baz/asdf', 'dirname');
eq(posix.basename('/foo/bar/baz/asdf/quux.html'), 'quux.html', 'basename1');
eq(posix.basename('/foo/bar/baz/asdf/quux.html', '.html'), 'quux', 'basename2');
eq(posix.extname('index.html'), '.html', 'extname1');
eq(posix.extname('index.coffee.md'), '.md', 'extname2');
eq(posix.extname('index.'), '.', 'extname3');
eq(posix.extname('index'), '', 'extname4');
eq(posix.isAbsolute('/foo/bar'), true, 'isAbs1');
eq(posix.isAbsolute('qux/'), false, 'isAbs2');
var pp = posix.parse('/home/user/dir/file.txt');
eq(pp.root, '/', 'parse.root'); eq(pp.dir, '/home/user/dir', 'parse.dir');
eq(pp.base, 'file.txt', 'parse.base'); eq(pp.ext, '.txt', 'parse.ext'); eq(pp.name, 'file', 'parse.name');
eq(posix.format({ dir: '/home/user/dir', base: 'file.txt' }), '/home/user/dir/file.txt', 'format');
eq(posix.sep, '/', 'sep');
eq(posix.relative('/data/orandea/test/aaa', '/data/orandea/impl/bbb'), '../../impl/bbb', 'relative');
var threw = false, code = '';
try { posix.join(1); } catch (e) { threw = true; code = e.code; }
eq(threw, true, 'join-throws'); eq(code, 'ERR_INVALID_ARG_TYPE', 'join-error-code');

console.log('path parity: ' + pass + ' passed, ' + fail + ' failed');
console.log(fail === 0 ? 'PATH 1:1 OK (running REAL Node lib/path.js on node9)' : 'PATH HAS FAILURES');
