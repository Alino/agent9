// node9 parity proof: run the REAL Node.js lib/os.js on node9, over a Plan 9-native
// internalBinding('os') (hostname<-/dev/sysname, etc.).
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
  if (src === null) return mod.exports;
  var fn = new Function('module', 'exports', 'require', 'primordials', 'process', 'internalBinding', src);
  fn(mod, mod.exports, nrequire, globalThis.primordials, globalThis.process, globalThis.internalBinding);
  return mod.exports;
}
nrequire('internal-bindings'); // sets globalThis.internalBinding (Plan 9 native)
var os = nrequire('os');

var pass = 0, fail = 0;
function eq(a, b, label) { if (a === b) pass++; else { fail++; console.log('FAIL ' + label + ': got ' + JSON.stringify(a) + ' want ' + JSON.stringify(b)); } }
function ok(c, label) { if (c) pass++; else { fail++; console.log('FAIL ' + label); } }

eq(os.type(), 'Plan9', 'type');
eq(os.platform(), 'plan9', 'platform');
eq(os.arch(), 'x64', 'arch');
eq(os.tmpdir(), '/tmp', 'tmpdir');
eq(os.homedir(), '/usr/glenda', 'homedir');
eq(os.EOL, '\n', 'EOL');
eq(os.hostname(), 'cirno', 'hostname (from /dev/sysname)');
ok(typeof os.release() === 'string', 'release is string');
ok(Array.isArray(os.cpus()), 'cpus is array');
eq(os.userInfo().username, 'glenda', 'userInfo.username');
eq(os.userInfo().shell, '/bin/rc', 'userInfo.shell');
ok(typeof os.totalmem() === 'number', 'totalmem number');
ok(typeof os.freemem() === 'number', 'freemem number');
ok(typeof os.uptime() === 'number', 'uptime number');
ok(os.constants && typeof os.constants === 'object', 'constants object');
ok(Array.isArray(os.loadavg()), 'loadavg array');
ok(typeof os.networkInterfaces() === 'object', 'networkInterfaces object');

console.log('os parity: ' + pass + ' passed, ' + fail + ' failed');
console.log(fail === 0 ? 'OS 1:1 OK (REAL Node lib/os.js over Plan 9-native binding)' : 'OS HAS FAILURES');
