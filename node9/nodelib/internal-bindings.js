/* node9 native internalBinding — Plan 9 implementations of Node's native bindings.
   This is where "Plan 9 mindset" lives: Node's lib/*.js calls this contract; the
   implementation reads /dev, /net, etc. instead of POSIX syscalls. */
var std = globalThis.std;
function readDev(p, dflt) { try { var f = std.open(p, 'rb'); if (!f) return dflt; var s = f.readAsString().trim(); f.close(); return s || dflt; } catch (e) { return dflt; } }
var bindings = {
  os: {
    getOSInformation: function () { return ['Plan9', readDev('/dev/osversion', '4'), 'Plan 9 from Bell Labs']; },
    getHostname: function () { return readDev('/dev/sysname', 'plan9'); },
    getHomeDirectory: function () { return std.getenv('home') || '/usr/glenda'; },
    getInterfaceAddresses: function () { return []; },
    getUptime: function () { return 0; },
    getTotalMem: function () { return 0; },
    getFreeMem: function () { return 0; },
    getCPUs: function () { return []; },
    getUserInfo: function () { return { uid: -1, gid: -1, username: std.getenv('user') || 'glenda', homedir: std.getenv('home') || '/usr/glenda', shell: '/bin/rc' }; },
    getAvailableParallelism: function () { return 1; },
    getLoadAvg: function (a) { a[0] = a[1] = a[2] = 0; },
    getPriority: function () { return 0; },
    setPriority: function () { return 0; },
  },
  util: { getCallSites: function(){ return []; }, parseEnv: function(){ return {}; }, getConstructorName: function(o){ return o&&o.constructor?o.constructor.name:''; }, previewEntries: function(){ return [[],false]; } },
  credentials: { getTempDir: function () { return '/tmp'; } },
  constants: { os: { signals: {}, errno: {}, priority: {}, dlopen: {} } },
};
function internalBinding(name) { return bindings[name] || {}; }
if (typeof module !== 'undefined') module.exports = internalBinding;
globalThis.internalBinding = internalBinding;
