'use strict';
// minimal node9 test harness shim for Node's test/common
var assert = require('assert');
var util = require('util');
var pending = [];
exports.mustCall = function (fn, exact) {
  if (typeof fn === 'number') { exact = fn; fn = undefined; }
  fn = fn || function () {}; exact = exact === undefined ? 1 : exact;
  var rec = { expected: exact, actual: 0, name: fn.name || '<anonymous>' }; pending.push(rec);
  return function () { rec.actual++; return fn.apply(this, arguments); };
};
exports.mustCallAtLeast = function (fn, minimum) {
  if (typeof fn === 'number') { minimum = fn; fn = undefined; }
  fn = fn || function () {}; minimum = minimum === undefined ? 1 : minimum;
  var rec = { min: minimum, actual: 0, name: fn.name || '<anonymous>' }; pending.push(rec);
  return function () { rec.actual++; return fn.apply(this, arguments); };
};
exports.mustNotCall = function (msg) { return function () { throw new Error('mustNotCall' + (msg ? ': ' + msg : '') + ' (' + Array.prototype.join.call(arguments, ',') + ')'); }; };
exports.mustSucceed = function (fn) { return exports.mustCall(function (err) { assert.ifError(err); if (fn) return fn.apply(this, Array.prototype.slice.call(arguments, 1)); }); };
exports.__verify = function () { for (var i = 0; i < pending.length; i++) { var r = pending[i]; if (r.expected !== undefined && r.actual !== r.expected) return 'mustCall ' + r.name + ': expected ' + r.expected + ' got ' + r.actual; if (r.min !== undefined && r.actual < r.min) return 'mustCallAtLeast ' + r.name + ': expected >=' + r.min + ' got ' + r.actual; } return null; };
exports.__reset = function () { pending = []; };
exports.isWindows = false; exports.isLinux = false; exports.isMacOS = false; exports.isOSX = false; exports.isSunOS = false; exports.isAIX = false; exports.isFreeBSD = false; exports.isOpenBSD = false; exports.isIBMi = false; exports.isMainThread = true; exports.isDumbTerminal = true; exports.isWorker = false;
exports.hasCrypto = true; exports.hasIntl = false; exports.hasOpenSSL3 = true; exports.hasFipsCrypto = false; exports.hasIPv6 = false; exports.hasMultiLocalhost = function () { return false; };
exports.platformTimeout = function (ms) { return ms; };
exports.skip = function (msg) { print('SKIP: ' + msg); std.exit(0); };
exports.printSkipMessage = function (msg) { print('SKIP: ' + msg); };
exports.skipIfInspectorDisabled = function () {}; exports.skipIf32Bits = function () {}; exports.skipIfWorker = function () {};
exports.expectsError = function () {}; exports.expectWarning = function () {}; exports.allowGlobals = function () {};
exports.mustNotMutateObjectDeep = function (o) { return o; };
exports.invalidArgTypeHelper = function (input) { if (input == null) return ' Received ' + input; if (typeof input === 'function') return ' Received function ' + (input.name || ''); if (typeof input === 'object') return ' Received an instance of ' + (input.constructor ? input.constructor.name : 'Object'); return ' Received type ' + (typeof input) + ' (' + util.inspect(input) + ')'; };
exports.getArrayBufferViews = function (buf) { var ab = buf.buffer || buf; return [new Uint8Array(ab), new Int8Array(ab), new Uint16Array(ab, 0, ab.byteLength >> 1), new Int16Array(ab, 0, ab.byteLength >> 1), new Uint32Array(ab, 0, ab.byteLength >> 2), new Int32Array(ab, 0, ab.byteLength >> 2), new Float32Array(ab, 0, ab.byteLength >> 2), new Float64Array(ab, 0, ab.byteLength >> 3), new DataView(ab)]; };
exports.getBufferSources = function (buf) { return exports.getArrayBufferViews(buf).concat([buf]); };
exports.canCreateSymLink = function () { return false; };
exports.localhostIPv4 = '127.0.0.1'; exports.PIPE = '/tmp/n9t.sock';
exports.runWithInvalidFD = function () {};
exports.buildType = 'Release';
var fs = require('fs');
var tmp = { path: '/tmp/n9test-tmp', refresh: function () { try { fs.rmSync(this.path, { recursive: true, force: true }); } catch (e) {} fs.mkdirSync(this.path, { recursive: true }); }, resolve: function (p) { return this.path + (p ? '/' + p : ''); } };
exports.tmpdir = tmp;
Object.defineProperty(exports, 'inspect', { get: function () { return util.inspect; } });
exports.noop = function () {};
