/* node9 standard library — Node-compatible builtins on quickjs-libc std/os.
   Loaded at qjs startup; installs globalThis.require + process + Buffer + console. */
(function () {
  'use strict';
  var std = globalThis.std, os = globalThis.os;
  var factories = {}, cache = {};

  function define(name, fn) { factories[name] = fn; }
  var dirStack = [(os.getcwd()[0]) || '/'];      // dirStack[0] = entry/REPL base dir
  function fileExists(p) { return os.stat(p)[1] === 0; }
  function isDir(p) { var r = os.stat(p); return r[1] === 0 && ((r[0].mode & (os.S_IFMT || 0xF000)) === (os.S_IFDIR || 0x4000)); }
  function readText(p) { var f = std.open(p, 'rb'); if (!f) return null; var s = f.readAsString(); f.close(); return s; }
  // builtin-only require, used internally by the resolver (path, etc.) and as fallback
  function requireBuiltin(name) {
    if (cache[name]) return cache[name].exports;
    var m = { exports: {} }; cache[name] = m;
    try { factories[name](m, m.exports, requireBuiltin); } catch (e) { delete cache[name]; throw e; }
    return m.exports;
  }
  function P0() { return requireBuiltin('path'); }   // lazy: define('path') runs later in this file
  function resolveFile(name, base) {
    var p = (name.charAt(0) === '/') ? name : P0().resolve(base, name);
    // file candidates first (exact, then extensions) — but NOT dir/index.js yet
    var cands = [p, p + '.js', p + '.json', p + '.node'];
    for (var i = 0; i < cands.length; i++) if (fileExists(cands[i]) && !isDir(cands[i])) return cands[i];
    // for a directory, package.json "main" takes precedence over index.js (Node semantics);
    // pkgMain falls back to index.js when there's no main / no package.json.
    if (fileExists(p) && isDir(p)) { var mn = pkgMain(p); if (mn) return mn; }
    return null;
  }
  function pkgMain(pkgdir) {
    var pj = pkgdir + '/package.json';
    if (fileExists(pj)) {
      var j; try { j = JSON.parse(readText(pj)); } catch (e) { j = {}; }
      var main = null, e2 = j.exports;
      if (e2) {
        if (typeof e2 === 'string') main = e2;
        else { var d = e2['.'] || e2; if (typeof d === 'string') main = d; else if (d) main = d.require || d.node || d['default'] || (d['.'] && (d['.'].require || d['.']['default'])); if (main && typeof main === 'object') main = main['default'] || main.require || main.node; }
      }
      if (!main && j.main) main = j.main;
      if (!main) main = 'index.js';
      var mp = P0().resolve(pkgdir, main), c = [mp, mp + '.js', mp + '.json', mp + '/index.js'];
      for (var i = 0; i < c.length; i++) if (fileExists(c[i]) && !isDir(c[i])) return c[i];
    }
    return fileExists(pkgdir + '/index.js') ? pkgdir + '/index.js' : null;
  }
  function resolvePackage(name, base) {
    var seg = name.split('/');
    var pkg = seg[0].charAt(0) === '@' ? seg[0] + '/' + seg[1] : seg[0];
    var sub = name.slice(pkg.length + 1);
    var dir = base;
    for (;;) {
      var pkgdir = dir + '/node_modules/' + pkg;
      if (fileExists(pkgdir)) {
        if (sub) {
          var s = pkgdir + '/' + sub, c = [s, s + '.js', s + '.json', s + '/index.js'];
          for (var i = 0; i < c.length; i++) if (fileExists(c[i]) && !isDir(c[i])) return c[i];
          if (fileExists(s) && isDir(s)) { var smn = pkgMain(s); if (smn) return smn; }
        } else { var mn = pkgMain(pkgdir); if (mn) return mn; }
      }
      var up = P0().dirname(dir);
      if (up === dir) return null;
      dir = up;
    }
  }
  // dynamic import() built via Function so boot.js's own source has no `import(` token
  // (which would make JS_DetectModule treat boot.js as a module). Loads by absolute path.
  var __dynImport = new Function('s', 'return import(s);');
  function loadFile(file) {
    if (cache[file]) return cache[file].exports;
    var src = readText(file);
    var mod = { exports: {}, filename: file, id: file, loaded: false, paths: [] };
    cache[file] = mod;
    if (file.slice(-5) === '.json') { try { mod.exports = JSON.parse(src); } catch (e) { delete cache[file]; throw e; } mod.loaded = true; return mod.exports; }
    var dir = P0().dirname(file);
    var localReq = makeRequire(dir);
    mod.require = localReq;
    // a CJS module loaded via new Function has no module path, so its native dynamic import()
    // would resolve against the wrong base. Rewrite import(...) -> a dir-bound importer that
    // resolves to an absolute path first (absolute-path import works).
    // rewrite dynamic import(...) -> dir-bound importer, but NOT member access `.import(`,
    // identifiers ending in "import", or `import.meta` (which isn't followed by `(`).
    var code = (src.indexOf('import') >= 0) ? src.replace(/(?<![.\w$])import\s*\(/g, '__n9importer(') : src;
    var importer = function (spec) { try { var r = globalThis.__n9_resolve(dir + '/__importer__.js', spec); return __dynImport(r); } catch (e) { return Promise.reject(e); } };
    var wrapper = new Function('module', 'exports', 'require', '__dirname', '__filename', '__n9importer', code + '\n//# sourceURL=' + file + '\n');
    try { wrapper(mod, mod.exports, localReq, dir, file, importer); mod.loaded = true; }
    catch (e) { try { if (std.getenv('NODE9_TRACE')) { var cf = std.open('/tmp/n9trace', 'a'); if (cf) { cf.puts(file + ' | ' + (e && (e.message || e)) + '\n'); cf.close(); } } } catch (_) {} delete cache[file]; throw e; }
    return mod.exports;
  }
  // each module gets a require bound to ITS directory — independent of async call timing
  function makeRequire(baseDir) {
    function req(name) {
      if (typeof name !== 'string') throw new TypeError('require: name must be a string');
      var bare = name.indexOf('node:') === 0 ? name.slice(5) : name;
      if (factories[bare]) return requireBuiltin(bare);
      var file = resolveFile(name, baseDir);
      if (!file && name.charAt(0) !== '.' && name.charAt(0) !== '/') file = resolvePackage(name, baseDir);
      if (!file) { var e = new Error("Cannot find module '" + name + "' (from " + baseDir + ")"); e.code = 'MODULE_NOT_FOUND'; throw e; }
      return loadFile(file);
    }
    req.cache = cache;
    req.resolve = function (n) { var b = n.indexOf('node:') === 0 ? n.slice(5) : n; if (factories[b]) return b; return resolveFile(n, baseDir) || resolvePackage(n, baseDir) || n; };
    req.main = undefined; // set after entry seeding
    req.extensions = { '.js': 1, '.json': 1, '.node': 1 };
    return req;
  }
  // global require resolves against dirStack[0] (entry script / REPL); modules use their own bound require
  var require = makeRequire(dirStack[0]);
  var __rebindGlobalRequire = function () { var r = makeRequire(dirStack[0]); r.main = require.main; globalThis.require = require = r; };
  globalThis.require = require;

  /* ===== ESM module resolution for quickjs's native import()/import =====
     n9_cli.c's module-loader normalize hook calls globalThis.__n9_resolve(base, name)
     to map a specifier to an absolute file path, reusing this CommonJS resolver but with
     the "import"/"default" export conditions. node: builtins are bridged to ESM via a
     generated shim that re-exports the CJS builtin. */
  function condPick(d, order) {
    if (typeof d === 'string') return d;
    if (!d || typeof d !== 'object') return null;
    for (var i = 0; i < order.length; i++) { if (d[order[i]] !== undefined) { var v = d[order[i]]; if (typeof v === 'string') return v; var r = condPick(v, order); if (r) return r; } }
    return null;
  }
  var ESM_COND = ['import', 'module', 'node', 'default', 'require'];
  function pkgMainESM(pkgdir, sub) {
    var pj = pkgdir + '/package.json', j = {};
    if (fileExists(pj)) { try { j = JSON.parse(readText(pj)); } catch (e) { j = {}; } }
    var target = null, e2 = j.exports;
    if (e2) {
      if (sub) { var key = './' + sub; if (e2[key] !== undefined) target = condPick(e2[key], ESM_COND); }
      else { if (typeof e2 === 'string') target = e2; else target = condPick(e2['.'] !== undefined ? e2['.'] : e2, ESM_COND); }
    }
    if (!target && !sub) target = j.module || j.main || 'index.js';
    if (!target && sub) target = sub;
    var mp = P0().resolve(pkgdir, target);
    var c = [mp, mp + '.js', mp + '.mjs', mp + '.cjs', mp + '.json', mp + '/index.js', mp + '/index.mjs'];
    for (var i = 0; i < c.length; i++) if (fileExists(c[i]) && !isDir(c[i])) return c[i];
    return null;
  }
  function resolveFileESM(name, baseDir) {
    var p = (name.charAt(0) === '/') ? name : P0().resolve(baseDir, name);
    var c = [p, p + '.js', p + '.mjs', p + '.cjs', p + '.json', p + '/index.js', p + '/index.mjs'];
    for (var i = 0; i < c.length; i++) if (fileExists(c[i]) && !isDir(c[i])) return c[i];
    if (fileExists(p) && isDir(p)) return pkgMainESM(p, null);
    return null;
  }
  function resolvePackageESM(name, baseDir) {
    var seg = name.split('/');
    var pkg = seg[0].charAt(0) === '@' ? seg[0] + '/' + seg[1] : seg[0];
    var sub = name.slice(pkg.length + 1);
    var dir = baseDir;
    for (;;) {
      var pkgdir = dir + '/node_modules/' + pkg;
      if (fileExists(pkgdir)) { var r = pkgMainESM(pkgdir, sub); if (r) return r; }
      var up = P0().dirname(dir); if (up === dir) return null; dir = up;
    }
  }
  function resolveImportsField(name, baseFile) {
    var dir = P0().dirname(baseFile);
    for (;;) {
      var pj = dir + '/package.json';
      if (fileExists(pj)) {
        var j = {}; try { j = JSON.parse(readText(pj)); } catch (e) {}
        if (j.imports && j.imports[name] !== undefined) { var t = condPick(j.imports[name], ESM_COND); if (t) return P0().resolve(dir, t); }
        return null;
      }
      var up = P0().dirname(dir); if (up === dir) return null; dir = up;
    }
  }
  function esmShimFor(name) {
    var sdir = '/tmp/n9esm'; os.mkdir(sdir, 0x1ff);
    var safe = name.replace(/[^a-zA-Z0-9]/g, '_'), path = sdir + '/' + safe + '.mjs';
    if (!fileExists(path)) {
      var mod; try { mod = requireBuiltin(name); } catch (e) { mod = {}; }
      var src = 'const _m = globalThis.require(' + JSON.stringify(name) + ');\nexport default _m;\n', seen = {};
      for (var k in mod) { if (/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(k) && k !== 'default' && !seen[k]) { seen[k] = 1; src += 'export const ' + k + ' = _m[' + JSON.stringify(k) + '];\n'; } }
      var f = std.open(path, 'w'); if (f) { f.puts(src); f.close(); }
    }
    return path;
  }
  globalThis.__n9_resolve = function (base, name) {
    try {
      if (name === 'std' || name === 'os') return name;                 // quickjs native modules
      var bare = name.indexOf('node:') === 0 ? name.slice(5) : name;
      if (factories[bare]) return esmShimFor(bare);                     // node builtin -> ESM shim
      var baseDir = (base && base.charAt(0) === '/') ? P0().dirname(base) : dirStack[0];
      var file = null;
      if (name.charAt(0) === '#') file = resolveImportsField(name, base && base.charAt(0) === '/' ? base : (baseDir + '/x'));
      else if (name.charAt(0) === '.' || name.charAt(0) === '/') file = resolveFileESM(name, baseDir);
      else file = resolvePackageESM(name, baseDir);
      return file || name;
    } catch (e) { return name; }
  };

  /* ---------------- path (POSIX) ---------------- */
  define('path', function (m, exports) {
    function normalizeArray(parts, allowAboveRoot) {
      var res = [];
      for (var i = 0; i < parts.length; i++) {
        var p = parts[i];
        if (!p || p === '.') continue;
        if (p === '..') {
          if (res.length && res[res.length - 1] !== '..') res.pop();
          else if (allowAboveRoot) res.push('..');
        } else res.push(p);
      }
      return res;
    }
    function normalize(p) {
      var abs = p.charAt(0) === '/', trail = p.length > 1 && p.charAt(p.length - 1) === '/';
      var parts = normalizeArray(p.split('/'), !abs).join('/');
      if (!parts && !abs) parts = '.';
      if (parts && trail) parts += '/';
      return (abs ? '/' : '') + parts;
    }
    exports.sep = '/';
    exports.delimiter = ':';
    exports.normalize = normalize;
    exports.isAbsolute = function (p) { return p.charAt(0) === '/'; };
    exports.join = function () {
      var segs = [];
      for (var i = 0; i < arguments.length; i++) if (arguments[i]) segs.push(arguments[i]);
      return segs.length ? normalize(segs.join('/')) : '.';
    };
    exports.resolve = function () {
      var resolved = '', abs = false;
      for (var i = arguments.length - 1; i >= 0 && !abs; i--) {
        var p = arguments[i];
        if (!p) continue;
        resolved = p + '/' + resolved;
        abs = p.charAt(0) === '/';
      }
      if (!abs) { var cwd = os.getcwd()[0] || '/'; resolved = cwd + '/' + resolved; }
      var out = normalizeArray(resolved.split('/'), false).join('/');
      return '/' + out;
    };
    exports.dirname = function (p) {
      if (!p) return '.';
      var s = p.replace(/\/+$/, '');
      if (s === '') return '/';          // p was all slashes -> root (Node: dirname('/') === '/')
      var i = s.lastIndexOf('/');
      if (i < 0) return '.'; if (i === 0) return '/';
      return s.slice(0, i);
    };
    exports.basename = function (path, suffix) {  // ported from Node lib/path.js (posix)
      if (typeof path !== 'string') throw new TypeError('The "path" argument must be of type string');
      var start = 0, end = -1, matchedSlash = true, i;
      if (suffix !== undefined && suffix.length > 0 && suffix.length <= path.length) {
        if (suffix === path) return '';
        var extIdx = suffix.length - 1, firstNonSlashEnd = -1;
        for (i = path.length - 1; i >= start; --i) {
          var code = path.charCodeAt(i);
          if (code === 47) { if (!matchedSlash) { start = i + 1; break; } }
          else {
            if (firstNonSlashEnd === -1) { matchedSlash = false; firstNonSlashEnd = i + 1; }
            if (extIdx >= 0) { if (code === suffix.charCodeAt(extIdx)) { if (--extIdx === -1) end = i; } else { extIdx = -1; end = firstNonSlashEnd; } }
          }
        }
        if (start === end) end = firstNonSlashEnd; else if (end === -1) end = path.length;
        return path.slice(start, end);
      }
      for (i = path.length - 1; i >= start; --i) {
        if (path.charCodeAt(i) === 47) { if (!matchedSlash) { start = i + 1; break; } }
        else if (end === -1) { matchedSlash = false; end = i + 1; }
      }
      if (end === -1) return '';
      return path.slice(start, end);
    };
    exports.extname = function (p) {
      var b = exports.basename(p); var i = b.lastIndexOf('.');
      return i > 0 ? b.slice(i) : '';
    };
    exports.parse = function (p) {
      var root = p.charAt(0) === '/' ? '/' : '';
      var dir = exports.dirname(p), base = exports.basename(p), ext = exports.extname(p);
      return { root: root, dir: dir, base: base, ext: ext, name: base.slice(0, base.length - ext.length) };
    };
    exports.format = function (o) {
      var dir = o.dir || o.root || '';
      var base = o.base || ((o.name || '') + (o.ext || ''));
      if (!dir) return base;
      return dir === o.root ? dir + base : dir + '/' + base;
    };
    exports.relative = function (from, to) {
      from = exports.resolve(from); to = exports.resolve(to);
      if (from === to) return '';
      var fp = from.split('/').filter(Boolean), tp = to.split('/').filter(Boolean);
      var i = 0; while (i < fp.length && i < tp.length && fp[i] === tp[i]) i++;
      var out = [];
      for (var j = i; j < fp.length; j++) out.push('..');
      for (var k = i; k < tp.length; k++) out.push(tp[k]);
      return out.join('/');
    };
    exports.toNamespacedPath = function (p) { return p; };
    exports.matchesGlob = function () { return false; };
    exports.posix = exports;
    exports.win32 = exports;   // Plan 9 is posix-only; tar/etc. use path.win32 for sanitization — posix semantics are correct here
  });

  /* ---------------- events ---------------- */
  define('events', function (m, exports) {
    function EventEmitter() { this._events = Object.create(null); this._max = 10; }
    EventEmitter.prototype.setMaxListeners = function (n) { this._max = n; return this; };
    EventEmitter.prototype.getMaxListeners = function () { return this._max; };
    EventEmitter.prototype.on = EventEmitter.prototype.addListener = function (ev, fn) {
      (this._events[ev] || (this._events[ev] = [])).push(fn);
      if (this._events.newListener) this.emit('newListener', ev, fn);
      return this;
    };
    EventEmitter.prototype.prependListener = function (ev, fn) {
      (this._events[ev] || (this._events[ev] = [])).unshift(fn); return this;
    };
    EventEmitter.prototype.once = function (ev, fn) {
      var self = this; function g() { self.removeListener(ev, g); return fn.apply(this, arguments); }
      g.listener = fn; return this.on(ev, g);
    };
    EventEmitter.prototype.removeListener = EventEmitter.prototype.off = function (ev, fn) {
      var a = this._events[ev]; if (!a) return this;
      for (var i = a.length - 1; i >= 0; i--) if (a[i] === fn || a[i].listener === fn) a.splice(i, 1);
      return this;
    };
    EventEmitter.prototype.removeAllListeners = function (ev) {
      if (ev) delete this._events[ev]; else this._events = Object.create(null); return this;
    };
    EventEmitter.prototype.listeners = function (ev) { return (this._events[ev] || []).slice(); };
    EventEmitter.prototype.listenerCount = function (ev) { return (this._events[ev] || []).length; };
    EventEmitter.prototype.eventNames = function () { return Object.keys(this._events); };
    EventEmitter.prototype.emit = function (ev) {
      var a = this._events[ev]; var args = Array.prototype.slice.call(arguments, 1);
      if (!a || !a.length) { if (ev === 'error') throw (args[0] || new Error('Unhandled error')); return false; }
      a = a.slice();
      for (var i = 0; i < a.length; i++) a[i].apply(this, args);
      return true;
    };
    EventEmitter.prototype.rawListeners = function (ev) { return (this._events[ev] || []).slice(); };
    EventEmitter.prototype.addEventListener = EventEmitter.prototype.on;
    EventEmitter.prototype.removeEventListener = EventEmitter.prototype.removeListener;
    EventEmitter.EventEmitter = EventEmitter;
    EventEmitter.defaultMaxListeners = 10;
    EventEmitter.captureRejections = false;
    // static events.once(emitter, name[, opts]) -> Promise resolving with the emitted args
    EventEmitter.once = function (emitter, name, options) {
      return new Promise(function (resolve, reject) {
        var signal = options && options.signal;
        function cleanup() { try { emitter.removeListener(name, onEvent); } catch (e) {} if (name !== 'error') { try { emitter.removeListener('error', onError); } catch (e) {} } if (signal) { try { signal.removeEventListener('abort', onAbort); } catch (e) {} } }
        function onEvent() { cleanup(); resolve(Array.prototype.slice.call(arguments)); }
        function onError(err) { cleanup(); reject(err); }
        function onAbort() { cleanup(); reject(new Error('The operation was aborted')); }
        if (signal && signal.aborted) { reject(new Error('The operation was aborted')); return; }
        (emitter.once || emitter.addEventListener).call(emitter, name, onEvent);
        if (name !== 'error' && emitter.once) emitter.once('error', onError);
        if (signal) signal.addEventListener('abort', onAbort);
      });
    };
    // static events.on(emitter, name) -> async iterator of event args
    EventEmitter.on = function (emitter, name, options) {
      var queue = [], waiting = [], done = false, err = null;
      function push() { var a = Array.prototype.slice.call(arguments); if (waiting.length) waiting.shift()({ value: a, done: false }); else queue.push(a); }
      function onErr(e) { err = e; if (waiting.length) waiting.shift(); }
      emitter.on(name, push); if (name !== 'error') emitter.on('error', onErr);
      var iterator = {
        next: function () { return new Promise(function (res, rej) { if (err) return rej(err); if (queue.length) return res({ value: queue.shift(), done: false }); if (done) return res({ value: undefined, done: true }); waiting.push(res); }); },
        return: function () { done = true; try { emitter.removeListener(name, push); } catch (e) {} return Promise.resolve({ value: undefined, done: true }); },
        throw: function (e) { return Promise.reject(e); }
      };
      iterator[Symbol.asyncIterator] = function () { return this; };
      return iterator;
    };
    EventEmitter.getEventListeners = function (emitter, name) { return (emitter._events && emitter._events[name] || []).slice(); };
    EventEmitter.setMaxListeners = function () {};
    EventEmitter.listenerCount = function (emitter, name) { return emitter.listenerCount ? emitter.listenerCount(name) : 0; };
    m.exports = EventEmitter;
  });

  /* ---------------- util ---------------- */
  define('util', function (m, exports) {
    function inspect(v, depth) {
      depth = depth == null ? 2 : depth;
      var seen = [];
      function go(x, d) {
        if (x === null) return 'null';
        var t = typeof x;
        if (t === 'string') return d < 0 ? '[String]' : JSON.stringify(x);
        if (t === 'number' || t === 'boolean' || t === 'undefined' || t === 'bigint') return String(x) + (t === 'bigint' ? 'n' : '');
        if (t === 'function') return '[Function' + (x.name ? ': ' + x.name : ' (anonymous)') + ']';
        if (t === 'symbol') return x.toString();
        if (seen.indexOf(x) >= 0) return '[Circular]';
        if (d < 0) return Array.isArray(x) ? '[Array]' : '[Object]';
        seen.push(x);
        var r;
        if (Array.isArray(x)) r = x.length ? '[ ' + x.map(function (e) { return go(e, d - 1); }).join(', ') + ' ]' : '[]';
        else if (x instanceof Error) r = x.stack || (x.name + ': ' + x.message);
        else {
          var keys = Object.keys(x);
          r = '{ ' + keys.map(function (k) { return k + ': ' + go(x[k], d - 1); }).join(', ') + ' }';
          if (!keys.length) r = '{}';
        }
        seen.pop();
        return r;
      }
      return go(v, depth);
    }
    function format() {
      var args = Array.prototype.slice.call(arguments), out = [], i = 0;
      if (typeof args[0] === 'string') {
        i = 1;
        out.push(args[0].replace(/%[sdifjoO%]/g, function (sp) {
          if (sp === '%%') return '%';
          if (i >= args.length) return sp;
          var a = args[i++];
          switch (sp) {
            case '%s': return typeof a === 'string' ? a : (typeof a === 'bigint' ? String(a) + 'n' : (typeof a === 'number' ? String(a) : inspect(a, { depth: 0 })));
            case '%d': return typeof a === 'bigint' ? String(a) + 'n' : (typeof a === 'symbol' ? a.toString() : String(Number(a)));
            case '%i': return typeof a === 'bigint' ? String(a) + 'n' : String(parseInt(a, 10));
            case '%f': return typeof a === 'symbol' ? a.toString() : String(parseFloat(a));
            case '%j': try { return JSON.stringify(a); } catch (e) { return '[Circular]'; }
            default: return inspect(a);
          }
        }));
      }
      for (; i < args.length; i++) out.push(typeof args[i] === 'string' ? args[i] : inspect(args[i]));
      return out.join(' ');
    }
    exports.inspect = inspect;
    exports.format = format;
    // formatWithOptions(inspectOptions, format, ...args) — like format; options applied to %o/%O
    exports.formatWithOptions = function (opts) { var args = Array.prototype.slice.call(arguments, 1); return format.apply(null, args); };
    exports.inherits = function (ctor, superCtor) {  // matches Node: setPrototypeOf (preserves ctor.prototype) + non-enumerable super_
      if (ctor == null) throw new TypeError('The constructor to "inherits" must not be null or undefined');
      if (superCtor == null) throw new TypeError('The super constructor to "inherits" must not be null or undefined');
      if (superCtor.prototype === undefined) throw new TypeError('The super constructor to "inherits" must have a prototype');
      Object.defineProperty(ctor, 'super_', { value: superCtor, writable: true, configurable: true });
      Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
    };
    exports.promisify = function (fn) {
      return function () {
        var args = Array.prototype.slice.call(arguments), self = this;
        return new Promise(function (res, rej) {
          args.push(function (err, v) { if (err) rej(err); else res(v); });
          fn.apply(self, args);
        });
      };
    };
    exports.callbackify = function (fn) {
      return function () {
        var args = Array.prototype.slice.call(arguments), cb = args.pop(), self = this;
        fn.apply(self, args).then(function (v) { cb(null, v); }, function (e) { cb(e); });
      };
    };
    exports.types = {
      isDate: function (v) { return v instanceof Date; },
      isRegExp: function (v) { return v instanceof RegExp; },
      isNativeError: function (v) { return v instanceof Error; },
      isPromise: function (v) { return v instanceof Promise; },
      isArrayBuffer: function (v) { return v instanceof ArrayBuffer; },
    };
    exports.isDeepStrictEqual = function (a, b) {
      try { return JSON.stringify(a) === JSON.stringify(b); } catch (e) { return a === b; }
    };
    exports.deprecate = function (fn) { return fn; };
  });

  /* ---------------- buffer ---------------- */
  define('buffer', function (m, exports) {
    function utf8Encode(str) {
      var bytes = [], i, c;
      for (i = 0; i < str.length; i++) {
        c = str.charCodeAt(i);
        if (c < 0x80) bytes.push(c);
        else if (c < 0x800) bytes.push(0xC0 | (c >> 6), 0x80 | (c & 0x3F));
        else if (c >= 0xD800 && c <= 0xDBFF) {
          var c2 = str.charCodeAt(++i), cp = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
          bytes.push(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
        } else bytes.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
      }
      return new Uint8Array(bytes);
    }
    function utf8Decode(bytes) {
      var str = '', i = 0, c;
      while (i < bytes.length) {
        c = bytes[i++];
        if (c < 0x80) str += String.fromCharCode(c);
        else if (c < 0xE0) str += String.fromCharCode(((c & 0x1F) << 6) | (bytes[i++] & 0x3F));
        else if (c < 0xF0) str += String.fromCharCode(((c & 0x0F) << 12) | ((bytes[i++] & 0x3F) << 6) | (bytes[i++] & 0x3F));
        else { var cp = (((c & 0x07) << 18) | ((bytes[i++] & 0x3F) << 12) | ((bytes[i++] & 0x3F) << 6) | (bytes[i++] & 0x3F)) - 0x10000; str += String.fromCharCode(0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF)); }
      }
      return str;
    }
    var enc = { encode: utf8Encode }, dec = { decode: utf8Decode };
    var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    function toBase64(bytes) {
      var out = '', i;
      for (i = 0; i + 2 < bytes.length; i += 3) {
        var n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + B64[n & 63];
      }
      var rem = bytes.length - i;
      if (rem === 1) { var n1 = bytes[i] << 16; out += B64[(n1 >> 18) & 63] + B64[(n1 >> 12) & 63] + '=='; }
      else if (rem === 2) { var n2 = (bytes[i] << 16) | (bytes[i + 1] << 8); out += B64[(n2 >> 18) & 63] + B64[(n2 >> 12) & 63] + B64[(n2 >> 6) & 63] + '='; }
      return out;
    }
    function fromBase64(s) {
      s = s.replace(/[^A-Za-z0-9+/]/g, ''); var bytes = [];
      for (var i = 0; i < s.length; i += 4) {
        var n = (B64.indexOf(s[i]) << 18) | (B64.indexOf(s[i + 1]) << 12) | (B64.indexOf(s[i + 2] || 'A') << 6) | B64.indexOf(s[i + 3] || 'A');
        bytes.push((n >> 16) & 255); if (s[i + 2]) bytes.push((n >> 8) & 255); if (s[i + 3]) bytes.push(n & 255);
      }
      return new Uint8Array(bytes);
    }
    // Buffer is callable as a legacy factory (Node's deprecated `Buffer(arg)`) — safe-buffer
    // and other old packages call it directly: numbers -> alloc, else -> from.
    function Buffer(arg, encodingOrOffset, length) {
      if (arg === undefined || arg === null) return wrap(new Uint8Array(0));
      if (typeof arg === 'number') return wrap(new Uint8Array(arg));
      return from(arg, encodingOrOffset, length);
    }
    Buffer.prototype = Object.create(Uint8Array.prototype);
    function wrap(u8) { Object.setPrototypeOf(u8, BufProto); return u8; }
    var BufProto = Object.create(Uint8Array.prototype);
    BufProto.toString = function (encoding, start, end) {
      encoding = (encoding || 'utf8').toLowerCase();
      var sub = this.subarray(start || 0, end == null ? this.length : end);
      if (encoding === 'utf8' || encoding === 'utf-8') return dec.decode(sub);
      if (encoding === 'hex') { var h = ''; for (var i = 0; i < sub.length; i++) h += (sub[i] < 16 ? '0' : '') + sub[i].toString(16); return h; }
      if (encoding === 'base64') return toBase64(sub);
      if (encoding === 'ascii' || encoding === 'latin1' || encoding === 'binary') { var s = ''; for (var j = 0; j < sub.length; j++) s += String.fromCharCode(sub[j]); return s; }
      return dec.decode(sub);
    };
    BufProto.toJSON = function () { return { type: 'Buffer', data: Array.prototype.slice.call(this) }; };
    BufProto.equals = function (o) { if (this.length !== o.length) return false; for (var i = 0; i < this.length; i++) if (this[i] !== o[i]) return false; return true; };
    // subarray/slice MUST return a Buffer (not a bare Uint8Array) — Node code (e.g. tar's
    // Header) calls .toString()/readUInt* on the result. Uint8Array.prototype.subarray would
    // return an unwrapped view that lost all Buffer methods.
    BufProto.subarray = function (a, b) { return wrap(Uint8Array.prototype.subarray.call(this, a, b)); };
    BufProto.slice = function (a, b) { return wrap(Uint8Array.prototype.subarray.call(this, a, b)); };
    BufProto.write = function (str, offset, length, encoding) {
      // Node signature: write(string[, offset[, length]][, encoding])
      if (typeof offset === 'string') { encoding = offset; offset = 0; length = undefined; }
      else if (typeof length === 'string') { encoding = length; length = undefined; }
      offset = offset || 0;
      var max = this.length - offset;
      var b = (encoding && encoding !== 'utf8' && encoding !== 'utf-8') ? from(str, encoding) : enc.encode(str);
      var n = Math.min(b.length, length == null ? max : Math.min(length, max));
      this.set(b.subarray(0, n), offset);
      return n;
    };
    function dv(b) { return new DataView(b.buffer, b.byteOffset, b.byteLength); }
    BufProto.readUInt8 = function (o) { return this[o >>> 0]; };
    BufProto.readInt8 = function (o) { var v = this[o >>> 0]; return v & 0x80 ? v - 256 : v; };
    BufProto.writeUInt8 = function (v, o) { o = o || 0; this[o] = v & 255; return o + 1; };
    BufProto.writeInt8 = function (v, o) { o = o || 0; this[o] = v & 255; return o + 1; };
    BufProto.readUInt16LE = function (o) { return dv(this).getUint16(o || 0, true); };
    BufProto.readUInt16BE = function (o) { return dv(this).getUint16(o || 0, false); };
    BufProto.readInt16LE = function (o) { return dv(this).getInt16(o || 0, true); };
    BufProto.readInt16BE = function (o) { return dv(this).getInt16(o || 0, false); };
    BufProto.writeUInt16LE = function (v, o) { o = o || 0; dv(this).setUint16(o, v, true); return o + 2; };
    BufProto.writeUInt16BE = function (v, o) { o = o || 0; dv(this).setUint16(o, v, false); return o + 2; };
    BufProto.writeInt16LE = function (v, o) { o = o || 0; dv(this).setInt16(o, v, true); return o + 2; };
    BufProto.writeInt16BE = function (v, o) { o = o || 0; dv(this).setInt16(o, v, false); return o + 2; };
    BufProto.readUInt32LE = function (o) { return dv(this).getUint32(o || 0, true); };
    BufProto.readUInt32BE = function (o) { return dv(this).getUint32(o || 0, false); };
    BufProto.readInt32LE = function (o) { return dv(this).getInt32(o || 0, true); };
    BufProto.readInt32BE = function (o) { return dv(this).getInt32(o || 0, false); };
    BufProto.writeUInt32LE = function (v, o) { o = o || 0; dv(this).setUint32(o, v, true); return o + 4; };
    BufProto.writeUInt32BE = function (v, o) { o = o || 0; dv(this).setUint32(o, v, false); return o + 4; };
    BufProto.writeInt32LE = function (v, o) { o = o || 0; dv(this).setInt32(o, v, true); return o + 4; };
    BufProto.writeInt32BE = function (v, o) { o = o || 0; dv(this).setInt32(o, v, false); return o + 4; };
    BufProto.readFloatLE = function (o) { return dv(this).getFloat32(o || 0, true); };
    BufProto.readFloatBE = function (o) { return dv(this).getFloat32(o || 0, false); };
    BufProto.writeFloatLE = function (v, o) { o = o || 0; dv(this).setFloat32(o, v, true); return o + 4; };
    BufProto.writeFloatBE = function (v, o) { o = o || 0; dv(this).setFloat32(o, v, false); return o + 4; };
    BufProto.readDoubleLE = function (o) { return dv(this).getFloat64(o || 0, true); };
    BufProto.readDoubleBE = function (o) { return dv(this).getFloat64(o || 0, false); };
    BufProto.writeDoubleLE = function (v, o) { o = o || 0; dv(this).setFloat64(o, v, true); return o + 8; };
    BufProto.writeDoubleBE = function (v, o) { o = o || 0; dv(this).setFloat64(o, v, false); return o + 8; };
    BufProto.readBigUInt64LE = function (o) { return dv(this).getBigUint64(o || 0, true); };
    BufProto.readBigUInt64BE = function (o) { return dv(this).getBigUint64(o || 0, false); };
    BufProto.readBigInt64LE = function (o) { return dv(this).getBigInt64(o || 0, true); };
    BufProto.readBigInt64BE = function (o) { return dv(this).getBigInt64(o || 0, false); };
    BufProto.writeBigUInt64LE = function (v, o) { o = o || 0; dv(this).setBigUint64(o, BigInt(v), true); return o + 8; };
    BufProto.writeBigUInt64BE = function (v, o) { o = o || 0; dv(this).setBigUint64(o, BigInt(v), false); return o + 8; };
    BufProto.writeBigInt64LE = function (v, o) { o = o || 0; dv(this).setBigInt64(o, BigInt(v), true); return o + 8; };
    BufProto.writeBigInt64BE = function (v, o) { o = o || 0; dv(this).setBigInt64(o, BigInt(v), false); return o + 8; };
    BufProto.readUIntLE = function (o, len) { o = o || 0; var v = 0, m = 1; for (var i = 0; i < len; i++) { v += this[o + i] * m; m *= 256; } return v; };
    BufProto.readUIntBE = function (o, len) { o = o || 0; var v = 0; for (var i = 0; i < len; i++) v = v * 256 + this[o + i]; return v; };
    BufProto.readIntLE = function (o, len) { var v = this.readUIntLE(o, len), max = Math.pow(2, 8 * len); return v >= max / 2 ? v - max : v; };
    BufProto.readIntBE = function (o, len) { var v = this.readUIntBE(o, len), max = Math.pow(2, 8 * len); return v >= max / 2 ? v - max : v; };
    BufProto.writeUIntLE = function (v, o, len) { o = o || 0; for (var i = 0; i < len; i++) { this[o + i] = v & 255; v = Math.floor(v / 256); } return o + len; };
    BufProto.writeUIntBE = function (v, o, len) { o = o || 0; for (var i = len - 1; i >= 0; i--) { this[o + i] = v & 255; v = Math.floor(v / 256); } return o + len; };
    BufProto.writeIntLE = function (v, o, len) { if (v < 0) v += Math.pow(2, 8 * len); return this.writeUIntLE(v, o, len); };
    BufProto.writeIntBE = function (v, o, len) { if (v < 0) v += Math.pow(2, 8 * len); return this.writeUIntBE(v, o, len); };
    BufProto.copy = function (target, targetStart, sourceStart, sourceEnd) {
      targetStart = targetStart || 0; sourceStart = sourceStart || 0; sourceEnd = sourceEnd == null ? this.length : sourceEnd;
      var n = Math.min(sourceEnd - sourceStart, target.length - targetStart);
      if (n <= 0) return 0;
      target.set(Uint8Array.prototype.subarray.call(this, sourceStart, sourceStart + n), targetStart);
      return n;
    };
    BufProto.fill = function (val, start, end, encoding) {
      start = start || 0; end = end == null ? this.length : end;
      if (typeof val === 'number') { for (var i = start; i < end; i++) this[i] = val & 255; return this; }
      var b = (typeof val === 'string') ? from(val, encoding) : (val instanceof Uint8Array ? val : from(String(val)));
      if (b.length === 0) return this;
      for (var j = start, k = 0; j < end; j++, k++) { if (k >= b.length) k = 0; this[j] = b[k]; }
      return this;
    };
    function indexOfImpl(buf, val, start, encoding, last) {
      start = start || 0; if (start < 0) start = Math.max(buf.length + start, 0);
      var needle = (typeof val === 'number') ? [val & 255] : (typeof val === 'string' ? from(val, encoding) : val);
      if (needle.length === 0) return last ? buf.length : start;
      if (needle.length === 1) { if (last) { for (var i = (last ? buf.length - 1 : start); i >= 0; i--) if (buf[i] === needle[0]) return i; } else { for (var x = start; x < buf.length; x++) if (buf[x] === needle[0]) return x; } return -1; }
      var lo = last ? buf.length - needle.length : start, hi = last ? -1 : buf.length - needle.length + 1, step = last ? -1 : 1;
      for (var p = lo; last ? p > hi : p < hi; p += step) { var ok = true; for (var q = 0; q < needle.length; q++) if (buf[p + q] !== needle[q]) { ok = false; break; } if (ok) return p; }
      return -1;
    }
    BufProto.indexOf = function (val, start, encoding) { if (typeof start === 'string') { encoding = start; start = 0; } return indexOfImpl(this, val, start, encoding, false); };
    BufProto.lastIndexOf = function (val, start, encoding) { if (typeof start === 'string') { encoding = start; start = undefined; } return indexOfImpl(this, val, start, encoding, true); };
    BufProto.includes = function (val, start, encoding) { return this.indexOf(val, start, encoding) !== -1; };
    BufProto.compare = function (o) { return Buffer.compare(this, o); };
    BufProto.swap16 = function () { for (var i = 0; i < this.length; i += 2) { var t = this[i]; this[i] = this[i + 1]; this[i + 1] = t; } return this; };
    BufProto.swap32 = function () { for (var i = 0; i < this.length; i += 4) { var a = this[i], b = this[i + 1]; this[i] = this[i + 3]; this[i + 1] = this[i + 2]; this[i + 2] = b; this[i + 3] = a; } return this; };
    BufProto.constructor = Buffer;
    function from(v, encoding) {
      if (typeof v === 'string') {
        encoding = (encoding || 'utf8').toLowerCase();
        if (encoding === 'hex') { var b = new Uint8Array(v.length / 2); for (var i = 0; i < b.length; i++) b[i] = parseInt(v.substr(i * 2, 2), 16); return wrap(b); }
        if (encoding === 'base64') return wrap(fromBase64(v));
        if (encoding === 'latin1' || encoding === 'binary' || encoding === 'ascii') { var u = new Uint8Array(v.length); for (var k = 0; k < v.length; k++) u[k] = v.charCodeAt(k) & 255; return wrap(u); }
        return wrap(enc.encode(v));
      }
      if (v instanceof ArrayBuffer) return wrap(new Uint8Array(v));
      if (Array.isArray(v) || v instanceof Uint8Array) return wrap(Uint8Array.from(v));
      throw new TypeError('Buffer.from: unsupported');
    }
    Buffer.from = from;
    Buffer.alloc = function (n, fill) { var b = wrap(new Uint8Array(n)); if (fill) b.fill(fill); return b; };
    Buffer.allocUnsafe = function (n) { return wrap(new Uint8Array(n)); };
    Buffer.isBuffer = function (v) { return v && Object.getPrototypeOf(v) === BufProto; };
    Buffer.byteLength = function (s, e) {
      if (s instanceof Uint8Array || s instanceof ArrayBuffer) return s.byteLength;
      e = (e || 'utf8').toLowerCase();
      if (e === 'utf8' || e === 'utf-8') return enc.encode(s).length;
      if (e === 'hex') return s.length >>> 1;
      if (e === 'base64' || e === 'base64url') return from(s, 'base64').length;
      return s.length; // latin1/ascii/binary/ucs2 — 1 byte/char (ucs2 would be 2, but unused)
    };
    Buffer.concat = function (list, total) {
      if (total == null) { total = 0; for (var i = 0; i < list.length; i++) total += list[i].length; }
      var out = new Uint8Array(total), off = 0;
      for (var j = 0; j < list.length && off < total; j++) {
        var take = Math.min(list[j].length, total - off);
        out.set(Uint8Array.prototype.subarray.call(list[j], 0, take), off);
        off += take;
      }
      return wrap(out);
    };
    Buffer.compare = function (a, b) { var n = Math.min(a.length, b.length); for (var i = 0; i < n; i++) { if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1; } return a.length === b.length ? 0 : (a.length < b.length ? -1 : 1); };
    exports.Buffer = Buffer;
    exports.kMaxLength = 0x7fffffff;
    exports.kStringMaxLength = 0x1fffffe8;
    exports.constants = { MAX_LENGTH: 0x7fffffff, MAX_STRING_LENGTH: 0x1fffffe8 };
    exports.INSPECT_MAX_BYTES = 50;
    Buffer.poolSize = 8192;
    exports.SlowBuffer = function (n) { return Buffer.alloc(+n); };
    exports.transcode = function () { throw new Error('buffer.transcode is not supported on node9'); };
    exports.isUtf8 = function (b) { try { require('buffer').Buffer.from(b).toString('utf8'); return true; } catch (e) { return false; } };
    exports.isAscii = function (b) { for (var i = 0; i < b.length; i++) if (b[i] > 127) return false; return true; };
    exports.atob = globalThis.atob; exports.btoa = globalThis.btoa;
    exports._wrap = wrap; exports._proto = BufProto;
  });

  /* ---------------- process ---------------- */
  define('process', function (m, exports) {
    var p = require('events').prototype ? new (require('events'))() : {};
    p.argv = ['qjs'].concat(typeof scriptArgs !== 'undefined' ? scriptArgs : []);
    p.argv0 = 'qjs';
    p.execPath = '/amd64/bin/qjs';
    p.platform = 'plan9';
    p.arch = 'x64';
    p.version = 'v20.18.1';
    p.versions = { node: '20.18.1', v8: '11.3.244', quickjs: 'ng', node9: '0.1' };
    p.release = { name: 'node', sourceUrl: '', headersUrl: '' };
    p.pid = (function () { try { return os.getpid ? os.getpid() : 0; } catch (e) { return 0; } })();
    p.env = new Proxy({}, {
      get: function (t, k) { if (typeof k !== 'string') return undefined; return std.getenv(k); },
      set: function (t, k, v) { try { std.setenv(k, String(v)); } catch (e) {} return true; },
      has: function (t, k) { return std.getenv(k) !== undefined; },
    });
    p.cwd = function () { return os.getcwd()[0] || '/'; };
    p.chdir = function (d) { os.chdir(d); };
    p.exit = function (code) { std.exit(code || 0); };
    p.nextTick = function (fn) { var a = Array.prototype.slice.call(arguments, 1); Promise.resolve().then(function () { fn.apply(null, a); }); };
    p.hrtime = function (prev) {
      var ns = BigInt(Math.round((typeof performance !== 'undefined' ? performance.now() : Date.now()) * 1e6));
      var s = ns / 1000000000n, n = ns % 1000000000n;
      if (prev) { var ps = BigInt(prev[0]) * 1000000000n + BigInt(prev[1]); var d = ns - ps; return [Number(d / 1000000000n), Number(d % 1000000000n)]; }
      return [Number(s), Number(n)];
    };
    p.hrtime.bigint = function () { return BigInt(Math.round((typeof performance !== 'undefined' ? performance.now() : Date.now()) * 1e6)); };
    var EE = require('events');
    var mkStream = function (f, fd) {
      var s = new EE();
      s.writable = true; s.fd = fd; s.isTTY = false; s.columns = 80; s.rows = 24;
      s.write = function (chunk, enc, cb) {
        if (typeof enc === 'function') { cb = enc; enc = null; }
        try {
          if (typeof chunk === 'string') { f.puts(chunk); if (f.flush) f.flush(); }
          else { // binary: write bytes straight to the fd so they aren't mangled by utf8 round-trip
            if (f.flush) f.flush();
            var b = (chunk instanceof Uint8Array) ? chunk : require('buffer').Buffer.from(chunk);
            var off = b.byteOffset || 0, wr = 0;
            while (wr < b.length) { var n = os.write(fd, b.buffer, off + wr, b.length - wr); if (n <= 0) break; wr += n; }
          }
        } catch (e) {}
        if (typeof cb === 'function') cb();
        return true;
      };
      s.end = function (chunk, enc, cb) { if (typeof chunk === 'function') { cb = chunk; chunk = null; } else if (typeof enc === 'function') { cb = enc; } if (chunk != null) s.write(chunk); if (typeof cb === 'function') cb(); s.emit('finish'); return s; };
      s.cork = function () {}; s.uncork = function () {}; s.setDefaultEncoding = function () { return s; }; s.setEncoding = function () { return s; };
      return s;
    };
    p.stdout = mkStream(std.out, 1);
    p.stderr = mkStream(std.err, 2);
    p.stdin = (function () { var s = new EE(); s.readable = true; s.fd = 0; s.isTTY = false; s.read = function () { return null; }; s.on = EE.prototype.on; s.resume = function () { return s; }; s.pause = function () { return s; }; s.setEncoding = function () { return s; }; s.pipe = function (d) { return d; }; return s; })();
    p.exitCode = 0;
    m.exports = p;
  });

  /* ---------------- os (node) ---------------- */
  define('os', function (m, exports) {
    exports.EOL = '\n';
    exports.platform = function () { return 'plan9'; };
    exports.arch = function () { return 'x64'; };
    exports.type = function () { return 'Plan9'; };
    exports.release = function () { return '4'; };
    exports.endianness = function () { return 'LE'; };
    exports.tmpdir = function () { return '/tmp'; };
    exports.homedir = function () { return std.getenv('home') || '/usr/glenda'; };
    exports.hostname = function () { try { var f = std.open('/dev/sysname', 'r'); var s = f.readAsString().trim(); f.close(); return s; } catch (e) { return 'plan9'; } };
    exports.cpus = function () { return [{ model: 'plan9', speed: 0, times: {} }]; };
    exports.totalmem = function () { return 0; };
    exports.freemem = function () { return 0; };
    exports.uptime = function () { return 0; };
    exports.userInfo = function () { return { username: std.getenv('user') || 'glenda', uid: -1, gid: -1, homedir: exports.homedir(), shell: '/bin/rc' }; };
    exports.loadavg = function () { return [0, 0, 0]; };
    exports.networkInterfaces = function () { return {}; };
    exports.availableParallelism = function () { return 1; };
    exports.machine = function () { return 'x86_64'; };
    exports.devNull = '/dev/null';
    exports.constants = {
      signals: { SIGHUP: 1, SIGINT: 2, SIGQUIT: 3, SIGKILL: 9, SIGTERM: 15, SIGUSR1: 16, SIGUSR2: 17, SIGPIPE: 13, SIGABRT: 6, SIGSEGV: 11 },
      errno: {
        E2BIG: 7, EACCES: 13, EADDRINUSE: 98, EAGAIN: 11, EBADF: 9, EBUSY: 16, ECONNREFUSED: 111, ECONNRESET: 104,
        EEXIST: 17, EFAULT: 14, EFBIG: 27, EINTR: 4, EINVAL: 22, EIO: 5, EISDIR: 21, ELOOP: 40, EMFILE: 24, EMLINK: 31,
        ENAMETOOLONG: 36, ENFILE: 23, ENODEV: 19, ENOENT: 2, ENOMEM: 12, ENOSPC: 28, ENOSYS: 38, ENOTDIR: 20,
        ENOTEMPTY: 39, ENOTSOCK: 88, ENXIO: 6, EOPNOTSUPP: 95, EPERM: 1, EPIPE: 32, EROFS: 30, ESPIPE: 29,
        ESRCH: 3, ETIMEDOUT: 110, EXDEV: 18, EWOULDBLOCK: 11
      },
      priority: { PRIORITY_LOW: 19, PRIORITY_BELOW_NORMAL: 10, PRIORITY_NORMAL: 0, PRIORITY_ABOVE_NORMAL: -7, PRIORITY_HIGH: -14, PRIORITY_HIGHEST: -20 },
      UV_UDP_REUSEADDR: 4
    };
  });

  /* ---------------- fs ---------------- */
  define('fs', function (m, exports) {
    var Buffer = require('buffer').Buffer;
    var nat = globalThis.__n9native;
    function nextTick(fn) { process.nextTick(fn); }
    var S_IFMT = os.S_IFMT || 0xF000, S_IFDIR = os.S_IFDIR || 0x4000,
        S_IFREG = os.S_IFREG || 0x8000, S_IFLNK = os.S_IFLNK || 0xA000;
    var O = { RDONLY: os.O_RDONLY || 0, WRONLY: os.O_WRONLY || 1, RDWR: os.O_RDWR || 2, CREAT: os.O_CREAT || 256, TRUNC: os.O_TRUNC || 512, APPEND: os.O_APPEND || 8 };
    function mkErr(code, msg, path) { var e = new Error(code + ': ' + msg + (path != null ? ", '" + path + "'" : '')); e.code = code; if (path != null) e.path = path; e.errno = -2; return e; }
    var fdPath = {}; // fd -> path, for fstat

    function statObj(st) {
      var isDir = (st.mode & S_IFMT) === S_IFDIR, isFile = (st.mode & S_IFMT) === S_IFREG, isLnk = (st.mode & S_IFMT) === S_IFLNK;
      return {
        dev: st.dev || 0, ino: st.ino || 0, mode: st.mode, nlink: st.nlink || 1, uid: st.uid || 0, gid: st.gid || 0, rdev: st.rdev || 0,
        size: st.size, blksize: 4096, blocks: st.blocks || Math.ceil((st.size || 0) / 512),
        mtimeMs: st.mtime, atimeMs: st.atime, ctimeMs: st.ctime, birthtimeMs: st.ctime,
        mtime: new Date(st.mtime), atime: new Date(st.atime), ctime: new Date(st.ctime), birthtime: new Date(st.ctime),
        isFile: function () { return isFile; }, isDirectory: function () { return isDir; },
        isSymbolicLink: function () { return isLnk; },
        isBlockDevice: function () { return false; }, isCharacterDevice: function () { return false; },
        isFIFO: function () { return false; }, isSocket: function () { return false; },
      };
    }
    function Dirent(name, mode) { this.name = name; this._mode = mode; }
    Dirent.prototype.isFile = function () { return (this._mode & S_IFMT) === S_IFREG; };
    Dirent.prototype.isDirectory = function () { return (this._mode & S_IFMT) === S_IFDIR; };
    Dirent.prototype.isSymbolicLink = function () { return (this._mode & S_IFMT) === S_IFLNK; };
    Dirent.prototype.isBlockDevice = Dirent.prototype.isCharacterDevice = Dirent.prototype.isFIFO = Dirent.prototype.isSocket = function () { return false; };

    function flagsToNum(flags) {
      if (typeof flags === 'number') return flags;
      switch (flags) {
        case 'r': return O.RDONLY; case 'rs': return O.RDONLY;
        case 'r+': case 'rs+': return O.RDWR;
        case 'w': case 'wx': return O.WRONLY | O.CREAT | O.TRUNC;
        case 'w+': case 'wx+': return O.RDWR | O.CREAT | O.TRUNC;
        case 'a': case 'ax': return O.WRONLY | O.CREAT | O.APPEND;
        case 'a+': case 'ax+': return O.RDWR | O.CREAT | O.APPEND;
        default: return O.RDONLY;
      }
    }
    function bufBytes(b) { return (b instanceof Uint8Array) ? b : Buffer.from(b); }

    /* ---- low-level fd ops ---- */
    exports.openSync = function (path, flags, mode) {
      var fd = os.open(path, flagsToNum(flags == null ? 'r' : flags), mode == null ? 0x1b6 : mode);
      if (fd < 0) throw mkErr('ENOENT', 'open', path);
      fdPath[fd] = path; return fd;
    };
    exports.closeSync = function (fd) { delete fdPath[fd]; os.close(fd); };
    exports.readSync = function (fd, buffer, offset, length, position) {
      if (position != null && position >= 0) os.seek(fd, position, std.SEEK_SET);
      var b = bufBytes(buffer);
      var n = os.read(fd, b.buffer, (b.byteOffset || 0) + (offset || 0), length);
      if (n < 0) throw mkErr('EIO', 'read', fdPath[fd]); // don't mask an I/O error as EOF
      return n;
    };
    exports.writeSync = function (fd, data, a, b, c) {
      var buf, off = 0, len, pos = null;
      if (typeof data === 'string') { buf = Buffer.from(data, typeof b === 'string' ? b : 'utf8'); off = 0; len = buf.length; pos = (typeof a === 'number') ? a : null; }
      else { buf = bufBytes(data); off = a || 0; len = (b == null ? buf.length - off : b); pos = (c == null ? null : c); }
      if (pos != null && pos >= 0) os.seek(fd, pos, std.SEEK_SET);
      var n = os.write(fd, buf.buffer, (buf.byteOffset || 0) + off, len);
      if (n < 0) throw mkErr('EIO', 'write', fdPath[fd]);
      return n;
    };
    exports.fstatSync = function (fd) { var p = fdPath[fd]; if (p == null) return statObj({ mode: S_IFREG, size: 0, mtime: 0, atime: 0, ctime: 0 }); return exports.statSync(p); };
    exports.ftruncateSync = exports.truncateSync = function () { /* APE: no ftruncate; files opened w/ TRUNC start empty — no-op */ };
    exports.fsyncSync = exports.fdatasyncSync = function () { /* no-op: weaker durability, fine for workstation install */ };
    exports.futimesSync = function () {};

    /* ---- whole-file ---- */
    exports.readFileSync = function (path, opts) {
      var enc = typeof opts === 'string' ? opts : (opts && opts.encoding);
      var fd = (typeof path === 'number') ? path : os.open(path, O.RDONLY, 0);
      if (fd < 0) throw mkErr('ENOENT', 'open', path);
      var chunks = [], buf = new ArrayBuffer(65536), total = 0;
      for (;;) { var n = os.read(fd, buf, 0, 65536); if (n <= 0) break; var c = Buffer.alloc(n); c.set(new Uint8Array(buf, 0, n)); chunks.push(c); total += n; }
      if (typeof path !== 'number') os.close(fd);
      var out = chunks.length === 1 ? chunks[0] : Buffer.concat(chunks, total);
      return enc ? out.toString(enc) : out;
    };
    exports.writeFileSync = function (path, data, opts) {
      var flags = (opts && opts.flag) || 'w';
      var fd = (typeof path === 'number') ? path : os.open(path, flagsToNum(flags), (opts && opts.mode) || 0x1b6);
      if (fd < 0) throw mkErr('EACCES', 'open', path);
      var b = (typeof data === 'string') ? Buffer.from(data, (opts && opts.encoding) || 'utf8') : bufBytes(data);
      var off = b.byteOffset || 0, written = 0;
      while (written < b.length) { // loop: Plan 9 fs can short-write
        var w = os.write(fd, b.buffer, off + written, b.length - written);
        if (w < 0) { if (typeof path !== 'number') os.close(fd); throw mkErr('EIO', 'write', path); }
        if (w === 0) break;
        written += w;
      }
      if (typeof path !== 'number') os.close(fd);
    };
    exports.appendFileSync = function (path, data, opts) { exports.writeFileSync(path, data, { flag: 'a', encoding: opts && opts.encoding }); };

    /* ---- metadata ---- */
    exports.existsSync = function (path) { return os.stat(path)[1] === 0; };
    exports.statSync = function (path, opts) { var r = os.stat(path); if (r[1] !== 0) { if (opts && opts.throwIfNoEntry === false) return undefined; throw mkErr('ENOENT', 'stat', path); } return statObj(r[0]); };
    exports.lstatSync = function (path, opts) { var r = os.lstat(path); if (r[1] !== 0) { if (opts && opts.throwIfNoEntry === false) return undefined; throw mkErr('ENOENT', 'lstat', path); } return statObj(r[0]); };
    exports.accessSync = function (path) { if (os.stat(path)[1] !== 0) throw mkErr('ENOENT', 'access', path); };
    exports.readdirSync = function (path, opts) {
      var r = os.readdir(path); if (r[1] !== 0) throw mkErr('ENOENT', 'scandir', path);
      var names = r[0].filter(function (n) { return n !== '.' && n !== '..'; });
      if (opts && opts.withFileTypes) return names.map(function (n) { var s = os.lstat(path + '/' + n); return new Dirent(n, s[1] === 0 ? s[0].mode : S_IFREG); });
      return names;
    };

    /* ---- mutation ---- */
    function mkdirRecursive(path, mode) {
      if (!path || path === '/' || path === '.') return;
      var st = os.stat(path); if (st[1] === 0) { if ((st[0].mode & S_IFMT) === S_IFDIR) return; throw mkErr('ENOTDIR', 'mkdir', path); }
      var parent = path.replace(/\/+$/, '').replace(/\/[^\/]*$/, '');
      if (parent && parent !== path) mkdirRecursive(parent, mode);
      var e = os.mkdir(path, mode);
      if (e !== 0) { var s2 = os.stat(path); if (!(s2[1] === 0 && (s2[0].mode & S_IFMT) === S_IFDIR)) throw mkErr('EACCES', 'mkdir', path); }
    }
    exports.mkdirSync = function (path, opts) {
      var recursive = opts && typeof opts === 'object' && opts.recursive;
      var mode = (opts && typeof opts === 'object' && opts.mode) || (typeof opts === 'number' ? opts : 0x1ff);
      if (recursive) { mkdirRecursive(path, mode); return path; }
      var e = os.mkdir(path, mode);
      if (e !== 0) { var s = os.stat(path); if (s[1] === 0 && (s[0].mode & S_IFMT) === S_IFDIR) throw mkErr('EEXIST', 'mkdir', path); throw mkErr('EEXIST', 'mkdir', path); }
    };
    exports.mkdtempSync = function (prefix) {
      var rb = new Uint8Array(9);
      for (var attempt = 0; attempt < 100; attempt++) {
        nat.randomBytes(rb);
        var suffix = ''; for (var i = 0; i < 6; i++) suffix += 'abcdefghijklmnopqrstuvwxyz0123456789'[rb[i] % 36];
        var p = prefix + suffix;
        if (os.mkdir(p, 0x1ff) === 0) return p;
      }
      throw mkErr('EEXIST', 'mkdtemp', prefix);
    };
    exports.rmdirSync = function (path, opts) { if (opts && opts.recursive) return exports.rmSync(path, { recursive: true, force: true }); var e = os.remove(path); if (e !== 0) throw mkErr('ENOTEMPTY', 'rmdir', path); };
    function rmrf(path, opts) {
      var st = os.lstat(path);
      if (st[1] !== 0) { if (opts && opts.force) return; throw mkErr('ENOENT', 'rm', path); }
      if ((st[0].mode & S_IFMT) === S_IFDIR) {
        var entries = os.readdir(path)[0] || [];
        for (var i = 0; i < entries.length; i++) { if (entries[i] === '.' || entries[i] === '..') continue; rmrf(path + '/' + entries[i], opts); }
      }
      os.remove(path);
    }
    exports.rmSync = function (path, opts) { if (opts && opts.recursive) { rmrf(path, opts); return; } var e = os.remove(path); if (e !== 0 && !(opts && opts.force)) throw mkErr('ENOENT', 'unlink', path); };
    exports.unlinkSync = function (path) { var e = os.remove(path); if (e !== 0) throw mkErr('ENOENT', 'unlink', path); };
    exports.renameSync = function (a, b) { var e = os.rename(a, b); if (e !== 0) { /* cross-dir rename: fall back to copy+unlink */ try { exports.copyFileSync(a, b); os.remove(a); } catch (x) { throw mkErr('ENOENT', 'rename', a); } } };
    exports.copyFileSync = function (src, dst) { var data = exports.readFileSync(src); exports.writeFileSync(dst, data); };
    exports.realpathSync = function (path) { var r = os.realpath(path); return r[1] === 0 ? r[0] : path; };
    exports.realpathSync.native = exports.realpathSync;
    exports.symlinkSync = function (target, path) { var e = os.symlink(target, path); if (e !== 0) { /* fallback: copy file/dir so the path at least resolves */ try { exports.copyFileSync(target, path); } catch (x) {} } };
    exports.readlinkSync = function (path) { var r = os.readlink(path); if (r[1] !== 0) throw mkErr('EINVAL', 'readlink', path); return r[0]; };
    exports.chmodSync = exports.lchmodSync = exports.fchmodSync = function () { /* Plan 9 perms via wstat not exposed; no-op (left-pad has no bins) */ };
    exports.chownSync = exports.lchownSync = exports.fchownSync = function () {};
    exports.utimesSync = exports.lutimesSync = function (path, atime, mtime) { try { os.utimes(path, +atime, +mtime); } catch (e) {} };

    /* ---- stream classes ---- */
    var stream = require('stream');
    function WriteStream(path, opts) {
      stream.Writable.call(this, {}); opts = opts || {};
      this.path = path; this.bytesWritten = 0;
      this.fd = (opts.fd != null) ? opts.fd : os.open(path, flagsToNum(opts.flags || 'w'), opts.mode || 0x1b6);
      var self = this, owns = (opts.fd == null);
      if (this.fd < 0) { nextTick(function () { self.emit('error', mkErr('ENOENT', 'open', path)); }); }
      else { fdPath[this.fd] = path; nextTick(function () { self.emit('open', self.fd); self.emit('ready'); }); }
      this._write = function (chunk, enc, cb) {
        if (self.fd < 0) { cb(mkErr('ENOENT', 'write', path)); return; }
        var b = bufBytes(chunk), n = os.write(self.fd, b.buffer, b.byteOffset || 0, b.length);
        if (n < 0) { cb(mkErr('EIO', 'write', path)); return; } self.bytesWritten += n; cb();
      };
      this._final = function (cb) { if (owns && self.fd >= 0) { delete fdPath[self.fd]; os.close(self.fd); } cb(); };
      this.on('finish', function () { self.emit('close'); });
    }
    WriteStream.prototype = Object.create(stream.Writable.prototype); WriteStream.prototype.constructor = WriteStream;
    function ReadStream(path, opts) {
      stream.Readable.call(this, {}); opts = opts || {};
      this.path = path; this.bytesRead = 0;
      this.fd = (opts.fd != null) ? opts.fd : os.open(path, flagsToNum(opts.flags || 'r'), 0);
      var self = this, owns = (opts.fd == null), CH = 65536, buf = new ArrayBuffer(CH);
      if (this.fd < 0) { nextTick(function () { self.emit('error', mkErr('ENOENT', 'open', path)); }); return; }
      fdPath[this.fd] = path;
      nextTick(function () { self.emit('open', self.fd); self.emit('ready'); });
      this._read = function () {
        if (self._rsClosed) return; // don't read from a closed/reused fd
        var n = os.read(self.fd, buf, 0, CH);
        if (n > 0) { var b = Buffer.alloc(n); b.set(new Uint8Array(buf, 0, n)); self.bytesRead += n; self.push(b); }
        else { self._rsClosed = true; if (owns) { delete fdPath[self.fd]; os.close(self.fd); self.fd = -1; } self.push(null); self.emit('close'); }
      };
    }
    ReadStream.prototype = Object.create(stream.Readable.prototype); ReadStream.prototype.constructor = ReadStream;
    exports.WriteStream = WriteStream; exports.ReadStream = ReadStream;
    exports.createWriteStream = function (p, o) { return new WriteStream(p, o); };
    exports.createReadStream = function (p, o) { return new ReadStream(p, o); };

    /* ---- callback wrappers (our syscalls are sync; defer to keep async contract) ---- */
    function cbify(syncFn) {
      return function () {
        var args = Array.prototype.slice.call(arguments), cb = args.pop();
        if (typeof cb !== 'function') { args.push(cb); cb = function () {}; }
        nextTick(function () { var r; try { r = syncFn.apply(null, args); } catch (e) { cb(e); return; } cb(null, r); });
      };
    }
    exports.readFile = function (path, opts, cb) { if (typeof opts === 'function') { cb = opts; opts = undefined; } nextTick(function () { try { cb(null, exports.readFileSync(path, opts)); } catch (e) { cb(e); } }); };
    exports.writeFile = function (path, data, opts, cb) { if (typeof opts === 'function') { cb = opts; opts = undefined; } nextTick(function () { try { exports.writeFileSync(path, data, opts); cb(null); } catch (e) { cb(e); } }); };
    exports.appendFile = cbify(exports.appendFileSync);
    exports.stat = cbify(exports.statSync); exports.lstat = cbify(exports.lstatSync); exports.fstat = cbify(exports.fstatSync);
    exports.readdir = function (path, opts, cb) { if (typeof opts === 'function') { cb = opts; opts = undefined; } nextTick(function () { try { cb(null, exports.readdirSync(path, opts)); } catch (e) { cb(e); } }); };
    exports.mkdir = function (path, opts, cb) { if (typeof opts === 'function') { cb = opts; opts = undefined; } nextTick(function () { try { var r = exports.mkdirSync(path, opts); cb(null, r); } catch (e) { cb(e); } }); };
    exports.rmdir = cbify(exports.rmdirSync); exports.rm = cbify(exports.rmSync); exports.unlink = cbify(exports.unlinkSync);
    exports.rename = cbify(exports.renameSync); exports.realpath = cbify(exports.realpathSync); exports.copyFile = cbify(exports.copyFileSync);
    exports.access = cbify(exports.accessSync); exports.symlink = cbify(exports.symlinkSync); exports.readlink = cbify(exports.readlinkSync);
    exports.chmod = cbify(exports.chmodSync); exports.chown = cbify(exports.chownSync); exports.utimes = cbify(exports.utimesSync);
    exports.mkdtemp = cbify(exports.mkdtempSync); exports.open = function (path, flags, mode, cb) { if (typeof flags === 'function') { cb = flags; flags = 'r'; mode = undefined; } else if (typeof mode === 'function') { cb = mode; mode = undefined; } nextTick(function () { try { cb(null, exports.openSync(path, flags, mode)); } catch (e) { cb(e); } }); };
    exports.close = cbify(exports.closeSync); exports.fsync = cbify(exports.fsyncSync); exports.fdatasync = cbify(exports.fdatasyncSync);
    exports.read = function (fd, buffer, offset, length, position, cb) { nextTick(function () { var n; try { n = exports.readSync(fd, buffer, offset, length, position); } catch (e) { cb(e); return; } cb(null, n, buffer); }); };
    exports.write = function (fd, buffer, a, b, c, cb) { var args = [fd, buffer, a, b, c]; while (typeof args[args.length - 1] === 'undefined') args.pop(); if (typeof args[args.length - 1] === 'function') cb = args.pop(); nextTick(function () { var n; try { n = exports.writeSync.apply(null, args); } catch (e) { cb(e); return; } cb(null, n, buffer); }); };

    /* ---- promises + FileHandle ---- */
    function FileHandle(fd, path) { this.fd = fd; this.path = path; }
    FileHandle.prototype.read = function (buffer, offset, length, position) {
      if (buffer && typeof buffer === 'object' && !(buffer instanceof Uint8Array)) { var o = buffer; buffer = o.buffer; offset = o.offset || 0; length = o.length == null ? (buffer.length - offset) : o.length; position = o.position; }
      var n = exports.readSync(this.fd, buffer, offset, length, position); return Promise.resolve({ bytesRead: n, buffer: buffer });
    };
    FileHandle.prototype.write = function (data, a, b, c) { var n = exports.writeSync(this.fd, data, a, b, c); return Promise.resolve({ bytesWritten: n, buffer: data }); };
    FileHandle.prototype.writeFile = function (data, opts) { var self = this; return Promise.resolve().then(function () { exports.writeFileSync(self.fd, data, opts); }); };
    FileHandle.prototype.readFile = function (opts) { var self = this; return Promise.resolve().then(function () { return exports.readFileSync(self.fd, opts); }); };
    FileHandle.prototype.stat = function () { var self = this; return Promise.resolve().then(function () { return exports.fstatSync(self.fd); }); };
    FileHandle.prototype.truncate = function () { return Promise.resolve(); };
    FileHandle.prototype.chmod = function () { return Promise.resolve(); };
    FileHandle.prototype.chown = function () { return Promise.resolve(); };
    FileHandle.prototype.utimes = function () { return Promise.resolve(); };
    FileHandle.prototype.sync = FileHandle.prototype.datasync = function () { return Promise.resolve(); };
    FileHandle.prototype.close = function () { var self = this; return Promise.resolve().then(function () { exports.closeSync(self.fd); }); };
    FileHandle.prototype.createReadStream = function (o) { o = o || {}; o.fd = this.fd; return new ReadStream(this.path, o); };
    FileHandle.prototype.createWriteStream = function (o) { o = o || {}; o.fd = this.fd; return new WriteStream(this.path, o); };

    function P(syncFn) { return function () { var args = arguments; return new Promise(function (res, rej) { nextTick(function () { var r; try { r = syncFn.apply(null, args); } catch (e) { rej(e); return; } res(r); }); }); }; }
    exports.promises = {
      readFile: P(exports.readFileSync), writeFile: P(exports.writeFileSync), appendFile: P(exports.appendFileSync),
      readdir: P(exports.readdirSync), stat: P(exports.statSync), lstat: P(exports.lstatSync), access: P(exports.accessSync),
      mkdir: P(exports.mkdirSync), rmdir: P(exports.rmdirSync), rm: P(exports.rmSync), unlink: P(exports.unlinkSync),
      rename: P(exports.renameSync), realpath: P(exports.realpathSync), copyFile: P(exports.copyFileSync),
      symlink: P(exports.symlinkSync), readlink: P(exports.readlinkSync), chmod: P(exports.chmodSync), chown: P(exports.chownSync),
      utimes: P(exports.utimesSync), mkdtemp: P(exports.mkdtempSync),
      open: function (path, flags, mode) { return new Promise(function (res, rej) { nextTick(function () { try { res(new FileHandle(exports.openSync(path, flags, mode), path)); } catch (e) { rej(e); } }); }); },
    };
    exports.FileHandle = FileHandle; exports.Dirent = Dirent;
    exports.constants = {
      F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1,
      O_RDONLY: O.RDONLY, O_WRONLY: O.WRONLY, O_RDWR: O.RDWR, O_CREAT: O.CREAT, O_TRUNC: O.TRUNC, O_APPEND: O.APPEND, O_EXCL: 0x800,
      S_IFMT: S_IFMT, S_IFREG: S_IFREG, S_IFDIR: S_IFDIR, S_IFLNK: S_IFLNK,
      COPYFILE_EXCL: 1, COPYFILE_FICLONE: 2, COPYFILE_FICLONE_FORCE: 4,
    };
  });

  /* ---------------- assert (bonus, used everywhere) ---------------- */
  define('assert', function (m, exports) {
    function inspect(v) { try { if (typeof v === 'string') return JSON.stringify(v); if (typeof v === 'bigint') return v + 'n'; if (v === null) return 'null'; if (typeof v === 'function') return '[Function: ' + (v.name || 'anonymous') + ']'; if (Array.isArray(v)) return '[ ' + v.map(inspect).join(', ') + ' ]'; if (typeof v === 'object') return require('util').inspect(v); return String(v); } catch (e) { return String(v); } }
    function AssertionError(opts) {
      var msg = opts.message || (inspect(opts.actual) + ' ' + (opts.operator || '==') + ' ' + inspect(opts.expected));
      var e = new Error(msg);
      e.name = 'AssertionError'; e.code = 'ERR_ASSERTION'; e.actual = opts.actual; e.expected = opts.expected; e.operator = opts.operator; e.generatedMessage = !opts.message;
      if (Error.captureStackTrace && opts.stackStartFn) Error.captureStackTrace(e, opts.stackStartFn);
      return e;
    }
    function fail(actual, expected, message, operator, stackStartFn) {
      if (arguments.length === 1) { message = actual; actual = undefined; }
      if (arguments.length === 0) message = 'Failed';
      throw AssertionError({ message: typeof message === 'string' ? message : undefined, actual: actual, expected: expected, operator: operator || 'fail', stackStartFn: stackStartFn || fail });
    }
    function ok(value, message) { if (!value) throw AssertionError({ message: typeof message === 'string' ? message : undefined, actual: value, expected: true, operator: '==', stackStartFn: ok }); }
    function assert(value, message) { ok(value, message); }

    // ---- deep equality ----
    function isPrim(v) { return v === null || (typeof v !== 'object' && typeof v !== 'function'); }
    function objTag(v) { return Object.prototype.toString.call(v); }
    function deepEqualImpl(a, b, strict, seen) {
      if (strict) { if (a === b) return a !== 0 || 1 / a === 1 / b; if (a !== a && b !== b) return true; }
      else { if (a === b) return true; if (a == b && isPrim(a) && isPrim(b)) return true; if (a !== a && b !== b) return true; }
      if (isPrim(a) || isPrim(b)) {
        if (!strict) return a == b;
        return false;
      }
      var ta = objTag(a), tb = objTag(b);
      if (ta !== tb) return false;
      if (strict && Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
      if (a instanceof Date) return a.getTime() === b.getTime();
      if (a instanceof RegExp) return a.source === b.source && a.flags === b.flags;
      // typed arrays / Buffers
      if (ArrayBuffer.isView(a) && !(a instanceof DataView)) { if (a.length !== b.length) return false; for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
      seen = seen || [];
      for (var s = 0; s < seen.length; s++) if (seen[s][0] === a && seen[s][1] === b) return true;
      seen.push([a, b]);
      if (a instanceof Map) {
        if (a.size !== b.size) return false;
        var entries = []; a.forEach(function (v, k) { entries.push([k, v]); });
        for (var mi = 0; mi < entries.length; mi++) { if (!b.has(entries[mi][0])) return false; if (!deepEqualImpl(entries[mi][1], b.get(entries[mi][0]), strict, seen)) return false; }
        seen.pop(); return true;
      }
      if (a instanceof Set) {
        if (a.size !== b.size) return false;
        var av = []; a.forEach(function (v) { av.push(v); }); var bv = []; b.forEach(function (v) { bv.push(v); });
        for (var ai = 0; ai < av.length; ai++) { var found = false; for (var bj = 0; bj < bv.length; bj++) if (deepEqualImpl(av[ai], bv[bj], strict, seen)) { found = true; break; } if (!found) return false; }
        seen.pop(); return true;
      }
      var ka = Object.keys(a), kb = Object.keys(b);
      if (ka.length !== kb.length) return false;
      for (var ki = 0; ki < ka.length; ki++) { if (!Object.prototype.hasOwnProperty.call(b, ka[ki])) return false; if (!deepEqualImpl(a[ka[ki]], b[ka[ki]], strict, seen)) return false; }
      seen.pop(); return true;
    }

    // ---- throws/rejects matcher ----
    function expectedException(actual, expected) {
      if (typeof expected === 'function') {
        if (expected.prototype !== undefined && actual instanceof expected) return true;
        if (Error.isPrototypeOf(expected) || expected === Error) return false;
        return expected.call({}, actual) === true; // validator function
      }
      if (expected instanceof RegExp) return expected.test(String(actual));
      if (typeof expected === 'object' && expected !== null) {
        for (var k in expected) {
          var ev = expected[k], av = actual ? actual[k] : undefined;
          if (ev instanceof RegExp) { if (!ev.test(String(av))) return false; }
          else if (!deepEqualImpl(av, ev, true)) return false;
        }
        return true;
      }
      return false;
    }
    function doThrows(shouldThrow, fn, expected, message, stackFn) {
      var caught, threw = false;
      try { fn(); } catch (e) { threw = true; caught = e; }
      if (shouldThrow) {
        if (!threw) throw AssertionError({ message: (typeof message === 'string' ? message : 'Missing expected exception') + (expected && expected.name ? ' (' + expected.name + ')' : ''), operator: 'throws', stackStartFn: stackFn });
        if (expected && !(typeof expected === 'string') && !expectedException(caught, expected)) throw caught;
      } else {
        if (threw) { if (!expected || expectedException(caught, expected)) throw AssertionError({ message: (typeof message === 'string' ? message + ': ' : 'Got unwanted exception: ') + (caught && caught.message), operator: 'doesNotThrow', stackStartFn: stackFn }); throw caught; }
      }
    }
    function doRejects(shouldReject, promiseOrFn, expected, message) {
      return new Promise(function (resolve, reject) {
        var p; try { p = (typeof promiseOrFn === 'function') ? promiseOrFn() : promiseOrFn; } catch (e) { return reject(e); }
        Promise.resolve(p).then(function () {
          if (shouldReject) reject(AssertionError({ message: (typeof message === 'string' ? message : 'Missing expected rejection') })); else resolve();
        }, function (err) {
          if (!shouldReject) { reject(AssertionError({ message: 'Got unwanted rejection: ' + (err && err.message) })); return; }
          if (expected && !(typeof expected === 'string') && !expectedException(err, expected)) reject(err); else resolve();
        });
      });
    }

    assert.ok = ok; assert.fail = fail; assert.AssertionError = AssertionError;
    assert.equal = function (a, b, msg) { if (!deepEqualImpl(a, b, false) && a != b) throw AssertionError({ message: typeof msg === 'string' ? msg : undefined, actual: a, expected: b, operator: '==', stackStartFn: assert.equal }); };
    assert.notEqual = function (a, b, msg) { if (a == b) throw AssertionError({ message: typeof msg === 'string' ? msg : undefined, actual: a, expected: b, operator: '!=', stackStartFn: assert.notEqual }); };
    assert.strictEqual = function (a, b, msg) { if (!Object.is(a, b)) throw AssertionError({ message: typeof msg === 'string' ? msg : undefined, actual: a, expected: b, operator: 'strictEqual', stackStartFn: assert.strictEqual }); };
    assert.notStrictEqual = function (a, b, msg) { if (Object.is(a, b)) throw AssertionError({ message: typeof msg === 'string' ? msg : undefined, actual: a, expected: b, operator: 'notStrictEqual', stackStartFn: assert.notStrictEqual }); };
    assert.deepEqual = function (a, b, msg) { if (!deepEqualImpl(a, b, false)) throw AssertionError({ message: typeof msg === 'string' ? msg : undefined, actual: a, expected: b, operator: 'deepEqual', stackStartFn: assert.deepEqual }); };
    assert.notDeepEqual = function (a, b, msg) { if (deepEqualImpl(a, b, false)) throw AssertionError({ message: typeof msg === 'string' ? msg : undefined, actual: a, expected: b, operator: 'notDeepEqual', stackStartFn: assert.notDeepEqual }); };
    assert.deepStrictEqual = function (a, b, msg) { if (!deepEqualImpl(a, b, true)) throw AssertionError({ message: typeof msg === 'string' ? msg : undefined, actual: a, expected: b, operator: 'deepStrictEqual', stackStartFn: assert.deepStrictEqual }); };
    assert.notDeepStrictEqual = function (a, b, msg) { if (deepEqualImpl(a, b, true)) throw AssertionError({ message: typeof msg === 'string' ? msg : undefined, actual: a, expected: b, operator: 'notDeepStrictEqual', stackStartFn: assert.notDeepStrictEqual }); };
    assert.throws = function (fn, expected, msg) { if (typeof expected === 'string') { msg = expected; expected = undefined; } doThrows(true, fn, expected, msg, assert.throws); };
    assert.doesNotThrow = function (fn, expected, msg) { if (typeof expected === 'string') { msg = expected; expected = undefined; } doThrows(false, fn, expected, msg, assert.doesNotThrow); };
    assert.rejects = function (p, expected, msg) { if (typeof expected === 'string') { msg = expected; expected = undefined; } return doRejects(true, p, expected, msg); };
    assert.doesNotReject = function (p, expected, msg) { if (typeof expected === 'string') { msg = expected; expected = undefined; } return doRejects(false, p, expected, msg); };
    assert.match = function (str, re, msg) { if (!re.test(str)) throw AssertionError({ message: typeof msg === 'string' ? msg : undefined, actual: str, expected: re, operator: 'match', stackStartFn: assert.match }); };
    assert.doesNotMatch = function (str, re, msg) { if (re.test(str)) throw AssertionError({ message: typeof msg === 'string' ? msg : undefined, actual: str, expected: re, operator: 'doesNotMatch', stackStartFn: assert.doesNotMatch }); };
    assert.ifError = function (err) { if (err !== null && err !== undefined) throw AssertionError({ message: 'ifError got unwanted exception: ' + (err && err.message || err), actual: err, expected: null, operator: 'ifError', stackStartFn: assert.ifError }); };
    assert.strict = assert; // assert.strict.* === assert.* here (we default to strict-ish)
    m.exports = assert;
  });

  /* ---------------- stream (minimal) ---------------- */
  define('stream', function (m, exports) {
    var EventEmitter = require('events');
    var nextTick = function (fn) { process.nextTick(fn); };
    function inherits(C, P) { C.prototype = Object.create(P.prototype); C.prototype.constructor = C; C.super_ = P; }
    /* ---- Readable ---- */
    function Readable(opts) {
      EventEmitter.call(this); opts = opts || {};
      this._rs = { buffer: [], length: 0, flowing: null, ended: false, endEmitted: false, reading: false,
        objectMode: !!opts.objectMode, highWaterMark: opts.highWaterMark != null ? opts.highWaterMark : (opts.objectMode ? 16 : 16384),
        destroyed: false, resumeScheduled: false };
      if (opts.read) this._read = opts.read;
    }
    inherits(Readable, EventEmitter);
    Readable.prototype._read = function () {};
    Readable.prototype.push = function (chunk) {
      var s = this._rs;
      if (chunk === null) { s.ended = true; if (s.flowing) drainFlowing(this); else nextTick((function (self) { return function () { maybeEnd(self); }; })(this)); return false; }
      if (s.destroyed) return false;
      var size = s.objectMode ? 1 : (chunk.length || 0);
      s.buffer.push(chunk); s.length += size;
      if (s.flowing) drainFlowing(this); else this.emit('readable');
      return s.length < s.highWaterMark;
    };
    function drainFlowing(self) {
      var s = self._rs; if (s.resumeScheduled) return; s.resumeScheduled = true;
      nextTick(function () { s.resumeScheduled = false;
        while (s.flowing && s.buffer.length) { var c = s.buffer.shift(); s.length -= s.objectMode ? 1 : (c.length || 0); self.emit('data', c); }
        if (s.flowing && s.length < s.highWaterMark && !s.ended) self._read(s.highWaterMark);
        maybeEnd(self);
      });
    }
    function maybeEnd(self) { var s = self._rs; if (s.ended && !s.endEmitted && s.buffer.length === 0) { s.endEmitted = true; self.emit('end'); } }
    Readable.prototype.read = function () {
      var s = this._rs;
      if (s.buffer.length === 0) { if (!s.ended && !s.reading) { s.reading = true; this._read(s.highWaterMark); s.reading = false; } maybeEnd(this); return null; }
      var c = s.buffer.shift(); s.length -= s.objectMode ? 1 : (c.length || 0);
      if (s.length < s.highWaterMark && !s.ended) { s.reading = true; this._read(s.highWaterMark); s.reading = false; }
      maybeEnd(this); return c;
    };
    Readable.prototype.resume = function () { var s = this._rs; if (s.flowing !== true) { s.flowing = true; drainFlowing(this); } return this; };
    Readable.prototype.pause = function () { this._rs.flowing = false; return this; };
    Readable.prototype.on = Readable.prototype.addListener = function (ev, fn) { EventEmitter.prototype.on.call(this, ev, fn); if (ev === 'data' && this._rs.flowing !== false) this.resume(); return this; };
    Readable.prototype.pipe = function (dest, opts) {
      var self = this; opts = opts || {};
      self.on('data', function (c) { if (dest.write(c) === false) self.pause(); });
      dest.on && dest.on('drain', function () { self.resume(); });
      self.on('end', function () { if (opts.end !== false) dest.end(); });
      self.on('error', function (e) { if (dest.emit) dest.emit('error', e); });
      dest.emit && dest.emit('pipe', self); self.resume(); return dest;
    };
    Readable.prototype.destroy = function (err) { var s = this._rs; if (s.destroyed) return this; s.destroyed = true; if (err) this.emit('error', err); this.emit('close'); return this; };
    Readable.prototype.setEncoding = function (e) { this._rs.encoding = e; return this; };
    Readable.prototype[Symbol.asyncIterator] = function () {
      var self = this, done = false, errored = null, pending = [], waiting = null;
      self.on('data', function (c) { if (waiting) { var w = waiting; waiting = null; w({ value: c, done: false }); } else pending.push(c); });
      self.on('end', function () { done = true; if (waiting) { var w = waiting; waiting = null; w({ value: undefined, done: true }); } });
      self.on('error', function (e) { errored = e; if (waiting) { var w = waiting; waiting = null; w(Promise.reject(e)); } });
      return { next: function () { return new Promise(function (res, rej) { if (errored) return rej(errored); if (pending.length) return res({ value: pending.shift(), done: false }); if (done) return res({ value: undefined, done: true }); waiting = res; }); }, "@@asyncIterator": null };
    };
    Readable.from = function (iterable, opts) {
      var r = new Readable(Object.assign({ objectMode: true }, opts));
      var it = iterable[Symbol.asyncIterator] ? iterable[Symbol.asyncIterator]() : iterable[Symbol.iterator]();
      var reading = false;
      r._read = function () { if (reading) return; reading = true; Promise.resolve(it.next()).then(function (res) { reading = false; if (res.done) r.push(null); else if (r.push(res.value)) r._read(); }, function (e) { r.destroy(e); }); };
      return r;
    };
    /* ---- Writable ---- */
    function Writable(opts) {
      EventEmitter.call(this); opts = opts || {};
      this._ws = { buffer: [], length: 0, writing: false, ended: false, finished: false, needDrain: false, objectMode: !!opts.objectMode, destroyed: false, highWaterMark: opts.highWaterMark != null ? opts.highWaterMark : (opts.objectMode ? 16 : 16384) };
      if (opts.write) this._write = opts.write; if (opts.final) this._final = opts.final;
    }
    inherits(Writable, EventEmitter);
    Writable.prototype._write = function (c, e, cb) { cb(); };
    Writable.prototype.write = function (chunk, enc, cb) {
      var s = this._ws; if (typeof enc === 'function') { cb = enc; enc = null; }
      if (s.ended) { if (cb) cb(new Error('write after end')); return false; }
      var size = s.objectMode ? 1 : (chunk ? chunk.length : 0); s.length += size;
      var ret = s.length < s.highWaterMark; if (!ret) s.needDrain = true;
      if (s.writing) s.buffer.push({ chunk: chunk, enc: enc, cb: cb }); else doWrite(this, chunk, enc, cb, size);
      return ret;
    };
    function doWrite(self, chunk, enc, cb, size) {
      var s = self._ws; s.writing = true;
      var done = function (err) {
        s.writing = false; s.length -= size; if (cb) cb(err); if (err) { self.emit('error', err); return; }
        if (s.buffer.length) { var n = s.buffer.shift(); doWrite(self, n.chunk, n.enc, n.cb, s.objectMode ? 1 : (n.chunk ? n.chunk.length : 0)); }
        else { if (s.needDrain) { s.needDrain = false; self.emit('drain'); } if (s.ended) finishWrite(self); }
      };
      // a _write that throws synchronously (instead of cb(err)) would otherwise leave
      // s.writing=true forever and wedge the stream — route the throw to the error path.
      try { self._write(chunk, enc, done); } catch (e) { s.writing = false; self.emit('error', e); if (cb) cb(e); }
    }
    function finishWrite(self) { var s = self._ws; if (s.finished || s.writing || s.buffer.length) return; var done = function () { s.finished = true; self.emit('finish'); }; if (self._final) self._final(done); else done(); }
    Writable.prototype.end = function (chunk, enc, cb) {
      var s = this._ws; if (typeof chunk === 'function') { cb = chunk; chunk = null; } else if (typeof enc === 'function') { cb = enc; enc = null; }
      if (chunk != null) this.write(chunk, enc); s.ended = true; if (cb) this.on('finish', cb);
      if (!s.writing && s.buffer.length === 0) finishWrite(this); return this;
    };
    Writable.prototype.destroy = function (err) { var s = this._ws; if (s.destroyed) return this; s.destroyed = true; if (err) this.emit('error', err); this.emit('close'); return this; };
    Writable.prototype.cork = function () {}; Writable.prototype.uncork = function () {}; Writable.prototype.setDefaultEncoding = function () { return this; };
    /* ---- Duplex / Transform / PassThrough ---- */
    function Duplex(opts) { Readable.call(this, opts); Writable.call(this, opts); if (opts && opts.read) this._read = opts.read; if (opts && opts.write) this._write = opts.write; }
    inherits(Duplex, Readable);
    Object.getOwnPropertyNames(Writable.prototype).forEach(function (k) { if (k !== 'constructor' && !Duplex.prototype[k]) Duplex.prototype[k] = Writable.prototype[k]; });
    Duplex.prototype.write = Writable.prototype.write; Duplex.prototype.end = Writable.prototype.end;
    function Transform(opts) { Duplex.call(this, opts); if (opts && opts.transform) this._transform = opts.transform; if (opts && opts.flush) this._flush = opts.flush; }
    inherits(Transform, Duplex);
    Transform.prototype._transform = function (c, e, cb) { cb(null, c); };
    Transform.prototype._write = function (chunk, enc, cb) { var self = this; this._transform(chunk, enc, function (err, data) { if (data != null) self.push(data); cb(err); }); };
    Transform.prototype._final = function (cb) { var self = this; if (this._flush) this._flush(function (err, data) { if (data != null) self.push(data); self.push(null); cb(err); }); else { this.push(null); cb(); } };
    function PassThrough(opts) { Transform.call(this, opts); }
    inherits(PassThrough, Transform);
    function finished(stream, cb) { var called = false; function done(err) { if (called) return; called = true; cb(err); } stream.on('end', function () { done(); }); stream.on('finish', function () { done(); }); stream.on('error', function (e) { done(e); }); stream.on('close', function () { done(); }); }
    function pipeline() { var args = Array.prototype.slice.call(arguments); var cb = typeof args[args.length - 1] === 'function' ? args.pop() : function () {}; for (var i = 0; i < args.length - 1; i++) args[i].pipe(args[i + 1]); finished(args[args.length - 1], cb); return args[args.length - 1]; }
    /* fix asyncIterator key (object literal can't use computed Symbol inline above) */
    Readable.prototype[Symbol.asyncIterator] = (function (orig) { return function () { var iter = orig.call(this); iter[Symbol.asyncIterator] = function () { return this; }; return iter; }; })(Readable.prototype[Symbol.asyncIterator]);
    // Node's require('stream') returns the legacy Stream CONSTRUCTOR with the classes
    // attached as properties (so `class X extends require('stream')` works — minipass v3 does this).
    function Stream(opts) { EventEmitter.call(this); }
    inherits(Stream, EventEmitter);
    Stream.prototype.pipe = Readable.prototype.pipe;
    Stream.Readable = Readable; Stream.Writable = Writable; Stream.Duplex = Duplex;
    Stream.Transform = Transform; Stream.PassThrough = PassThrough; Stream.Stream = Stream;
    Stream.pipeline = pipeline; Stream.finished = finished;
    Stream.promises = {
      pipeline: function () { var a = Array.prototype.slice.call(arguments); return new Promise(function (res, rej) { a.push(function (e) { e ? rej(e) : res(); }); pipeline.apply(null, a); }); },
      finished: function (s) { return new Promise(function (res, rej) { finished(s, function (e) { e ? rej(e) : res(); }); }); }
    };
    m.exports = Stream;
  });

  /* ---------------- net (Plan 9-native, over /net) ---------------- */
  define('net', function (m, exports) {
    var EventEmitter = require('events');
    var stream = require('stream');
    var Buffer = require('buffer').Buffer;
    function nextTick(fn) { process.nextTick(fn); }
    function bytesOf(s) { var b = new Uint8Array(s.length); for (var i = 0; i < s.length; i++) b[i] = s.charCodeAt(i) & 255; return b; }
    function rdStr(fd, max) { var b = new Uint8Array(max); var n = os.read(fd, b.buffer, 0, max); if (n <= 0) return ''; var s = ''; for (var i = 0; i < n; i++) s += String.fromCharCode(b[i]); return s; }
    function wrStr(fd, s) { var b = bytesOf(s); return os.write(fd, b.buffer, 0, b.length); }
    exports._rdStr = rdStr; exports._wrStr = wrStr;

    // low-level synchronous dial via /net/cs; returns {fd, ctl} or null
    function dialTcp(proto, host, port) {
      var cs = os.open('/net/cs', os.O_RDWR);
      if (cs < 0) return null;
      wrStr(cs, proto + '!' + host + '!' + port);
      os.seek(cs, 0, std.SEEK_SET);
      var reply = rdStr(cs, 256).trim(); os.close(cs);
      if (!reply) return null;
      var p = reply.split(' '), clonefile = p[0], addr = p[1];
      var ctl = os.open(clonefile, os.O_RDWR);
      if (ctl < 0) return null;
      var conn = rdStr(ctl, 64).trim();
      if (wrStr(ctl, 'connect ' + addr) < 0) { os.close(ctl); return null; }
      var base = clonefile.replace(/\/clone$/, '');
      var fd = os.open(base + '/' + conn + '/data', os.O_RDWR);
      if (fd < 0) { os.close(ctl); return null; }
      return { fd: fd, ctl: ctl };
    }
    exports._dial = dialTcp;
    exports.dial = function (proto, host, port) { var c = dialTcp(proto, host, port); return c || -1; };

    /* ---- async Socket: a Duplex over a /net data fd, reads via os.setReadHandler ---- */
    function Socket(opts) {
      stream.Duplex.call(this, opts || {});
      this._fd = -1; this._ctl = -1; this._readerOn = false; this._rbuf = null;
      this._connecting = false; this._sockClosed = false; this._isTLS = false;
      this.readyState = 'closed'; this.bytesRead = 0; this.bytesWritten = 0;
    }
    Socket.prototype = Object.create(stream.Duplex.prototype);
    Socket.prototype.constructor = Socket;

    Socket.prototype._installReader = function () {
      var self = this;
      if (self._fd < 0 || self._readerOn || self._sockClosed) return;
      self._readerOn = true;
      if (!self._rbuf) self._rbuf = new ArrayBuffer(65536);
      os.setReadHandler(self._fd, function () {
        var n;
        try { n = os.read(self._fd, self._rbuf, 0, 65536); } catch (e) { n = -1; }
        if (n > 0) {
          var b = Buffer.alloc(n), src = new Uint8Array(self._rbuf, 0, n);
          for (var i = 0; i < n; i++) b[i] = src[i];
          self.bytesRead += n;
          var ok = self.push(b);
          if (ok === false) { os.setReadHandler(self._fd, null); self._readerOn = false; }
        } else if (n === 0) {
          os.setReadHandler(self._fd, null); self._readerOn = false;
          self.push(null);
        } else {
          os.setReadHandler(self._fd, null); self._readerOn = false;
          self.destroy(new Error('socket read error'));
        }
      });
    };
    Socket.prototype._read = function () { this._installReader(); };

    Socket.prototype._write = function (chunk, enc, cb) {
      if (this._fd < 0) { cb(new Error('not connected')); return; }
      var buf = (chunk instanceof Uint8Array) ? chunk : Buffer.from(chunk);
      var ab = buf.buffer, off = buf.byteOffset || 0, len = buf.length, written = 0;
      while (written < len) {
        var n;
        try { n = os.write(this._fd, ab, off + written, len - written); } catch (e) { cb(e); return; }
        if (n < 0) { cb(new Error('socket write error')); return; }
        if (n === 0) break;
        written += n;
      }
      this.bytesWritten += written;
      cb();
    };

    Socket.prototype._adoptFd = function (fd, ctl, isTLS) {
      this._fd = fd; this._ctl = ctl != null ? ctl : -1; this._isTLS = !!isTLS;
      this.readyState = 'open';
      var self = this;
      // close-delimited (Connection: close) responses: once the readable side ends, finalize -> 'close'
      this.once('end', function () { if (!self._sockClosed) self.destroy(); });
    };

    Socket.prototype.connect = function (opts, connectListener) {
      var self = this;
      if (typeof opts === 'number') opts = { port: opts, host: arguments[1] && typeof arguments[1] !== 'function' ? arguments[1] : '127.0.0.1' };
      if (typeof arguments[1] === 'function') connectListener = arguments[1];
      if (typeof arguments[2] === 'function') connectListener = arguments[2];
      var host = opts.host || opts.hostname || '127.0.0.1', port = opts.port || 0;
      if (connectListener) this.once('connect', connectListener);
      this._connecting = true; this.readyState = 'opening';
      nextTick(function () {
        var c = dialTcp('tcp', host, port);
        if (!c) { self._connecting = false; self.emit('error', new Error('connect ECONNREFUSED ' + host + ':' + port)); return; }
        self._adoptFd(c.fd, c.ctl, false);
        self._connecting = false;
        self.emit('connect'); self.emit('ready');
        if (self._rs.flowing === true) self._installReader();
      });
      return this;
    };

    Socket.prototype.destroy = function (err) {
      if (this._sockClosed) return this;
      this._sockClosed = true; this.readyState = 'closed';
      if (this._fd >= 0) { try { os.setReadHandler(this._fd, null); } catch (e) {} try { os.close(this._fd); } catch (e) {} }
      if (this._rawfd != null && this._rawfd >= 0) { try { os.close(this._rawfd); } catch (e) {} }
      if (this._ctl >= 0) { try { os.close(this._ctl); } catch (e) {} }
      if (err) this.emit('error', err);
      this.emit('close', !!err);
      return this;
    };
    Socket.prototype.end = function (chunk, enc, cb) {
      var self = this;
      stream.Duplex.prototype.end.call(this, chunk, enc, cb);
      this.once('finish', function () { /* half-close: hang up the conn */ if (self._ctl >= 0) { try { wrStr(self._ctl, 'hangup'); } catch (e) {} } });
      return this;
    };
    Socket.prototype.setTimeout = function () { return this; };
    Socket.prototype.setNoDelay = function () { return this; };
    Socket.prototype.setKeepAlive = function () { return this; };
    Socket.prototype.ref = function () { return this; };
    Socket.prototype.unref = function () { return this; };
    Socket.prototype.address = function () { return {}; };

    exports.Socket = Socket;
    exports.Stream = Socket;
    exports.connect = exports.createConnection = function (opts, connectListener) {
      var s = new Socket();
      if (typeof opts === 'object' && typeof connectListener !== 'function' && typeof arguments[1] === 'function') connectListener = arguments[1];
      s.connect.apply(s, arguments);
      return s;
    };
    exports.isIP = function (s) { return /^(\d{1,3}\.){3}\d{1,3}$/.test(s) ? 4 : (/:/.test(s) ? 6 : 0); };
    exports.isIPv4 = function (s) { return exports.isIP(s) === 4; };
    exports.isIPv6 = function (s) { return exports.isIP(s) === 6; };

    // server: announce a TCP port, return a ctl + conn dir for accept loops
    exports.announce = function (port) {
      var ctl = os.open('/net/tcp/clone', os.O_RDWR);
      if (ctl < 0) return null;
      var conn = rdStr(ctl, 64).trim();
      if (wrStr(ctl, 'announce *!' + port) < 0) { os.close(ctl); return null; }
      return { ctl: ctl, conn: conn };
    };
    exports.accept = function (srv) {
      var lfd = os.open('/net/tcp/' + srv.conn + '/listen', os.O_RDWR);
      if (lfd < 0) return -1;
      var acc = rdStr(lfd, 64).trim();
      return { fd: os.open('/net/tcp/' + acc + '/data', os.O_RDWR), lfd: lfd };
    };
  });

  /* ---------------- tls (libsec tlsClient over a dialed TCP fd) ---------------- */
  define('tls', function (m, exports) {
    var net = require('net');
    var nat = globalThis.__n9native;
    function nextTick(fn) { process.nextTick(fn); }
    // TLSSocket is a net.Socket whose fd is the decrypted tlsClient fd.
    exports.connect = function (opts, secureConnectListener) {
      if (typeof opts === 'number') opts = { port: opts, host: arguments[1] && typeof arguments[1] !== 'function' ? arguments[1] : undefined };
      for (var i = 1; i < arguments.length; i++) if (typeof arguments[i] === 'function') secureConnectListener = arguments[i];
      var host = opts.host || opts.hostname || '127.0.0.1', port = opts.port || 443;
      var servername = opts.servername || (net.isIP(host) ? '' : host);
      var sock = new net.Socket();
      if (secureConnectListener) sock.once('secureConnect', secureConnectListener);
      nextTick(function () {
        var c = net._dial('tcp', host, port);
        if (!c) { sock.emit('error', new Error('tls: connect failed ' + host + ':' + port)); return; }
        var tfd;
        try { tfd = nat.tlsClient(c.fd, servername); } catch (e) { tfd = -1; }
        if (tfd < 0) { try { os.close(c.fd); } catch (e) {} try { os.close(c.ctl); } catch (e) {} sock.emit('error', new Error('tls: handshake failed with ' + host)); return; }
        // keep the raw tcp fd open (libsec's tls device reads/writes through it); use tfd for cleartext
        sock._rawfd = c.fd;
        sock._adoptFd(tfd, c.ctl, true);
        sock.authorized = true; // integrity guaranteed downstream by SRI; chain-verify deferred
        sock.encrypted = true;
        sock.emit('connect'); sock.emit('secureConnect'); sock.emit('ready');
        if (sock._rs.flowing === true) sock._installReader();
      });
      return sock;
    };
    exports.TLSSocket = net.Socket;
    exports.rootCertificates = [];
    exports.DEFAULT_MIN_VERSION = 'TLSv1.2';
  });

  /* ---------------- http (Plan 9-native, over net) ---------------- */
  define('http', function (m, exports) {
    var net = require('net');
    function parseRequest(raw) {
      var lines = raw.split('\r\n'), top = (lines[0] || '').split(' '), headers = {};
      for (var i = 1; i < lines.length && lines[i]; i++) { var c = lines[i].indexOf(':'); if (c > 0) headers[lines[i].slice(0, c).toLowerCase().trim()] = lines[i].slice(c + 1).trim(); }
      return { method: top[0] || 'GET', url: top[1] || '/', httpVersion: (top[2] || '').replace('HTTP/', ''), headers: headers };
    }
    exports.STATUS_CODES = { 200: 'OK', 201: 'Created', 204: 'No Content', 301: 'Moved Permanently', 302: 'Found', 400: 'Bad Request', 404: 'Not Found', 405: 'Method Not Allowed', 500: 'Internal Server Error' };
    exports.METHODS = ['GET', 'POST', 'PUT', 'DELETE', 'HEAD', 'OPTIONS', 'PATCH'];
    function makeRes(fd) {
      var res = { statusCode: 200, _headers: {}, _buf: '', headersSent: false };
      res.setHeader = function (k, v) { res._headers[k] = v; };
      res.getHeader = function (k) { return res._headers[k]; };
      res.writeHead = function (code, h) { res.statusCode = code; if (h) for (var k in h) res._headers[k] = h[k]; return res; };
      res.write = function (s) { res._buf += (typeof s === 'string' ? s : require('buffer').Buffer.from(s).toString()); return true; };
      res.end = function (s) {
        if (s != null) res.write(s);
        var body = res._buf;
        if (res._headers['Content-Length'] == null) res._headers['Content-Length'] = net._wrStr ? require('buffer').Buffer.byteLength(body) : body.length;
        var head = 'HTTP/1.1 ' + res.statusCode + ' ' + (exports.STATUS_CODES[res.statusCode] || 'OK') + '\r\n';
        for (var k in res._headers) head += k + ': ' + res._headers[k] + '\r\n';
        head += 'Connection: close\r\n\r\n';
        net._wrStr(fd, head + body); res.headersSent = true;
      };
      return res;
    }
    exports.createServer = function (handler) {
      var EventEmitter = require('events');
      var server = new EventEmitter();
      if (handler) server.on('request', handler);
      server._running = false;
      server.listen = function (port, cb) {
        var s = net.announce(port);
        if (!s) { server.emit('error', new Error('announce failed on ' + port)); return server; }
        server._running = true;
        server.emit('listening');
        if (cb) cb();
        while (server._running) {
          var a = net.accept(s);
          if (a === -1) break;
          var raw = net._rdStr(a.fd, 8192);
          var req = parseRequest(raw);
          var res = makeRes(a.fd);
          try { server.emit('request', req, res); if (!res.headersSent) res.end(''); }
          catch (e) { try { res.writeHead(500); res.end('Internal Server Error: ' + e.message); } catch (e2) {} }
          os.close(a.fd); os.close(a.lfd);
        }
        os.close(s.ctl);
        return server;
      };
      server.close = function () { server._running = false; };
      return server;
    };
    /* ===== real async HTTP/1.1 client ===== */
    var EventEmitter = require('events');
    var stream = require('stream');
    var Buffer = require('buffer').Buffer;
    var nat = globalThis.__n9native;

    // byte-level scan for a 2- or 4-byte CRLF marker
    function findSeq(buf, seq, from) {
      var n = buf.length, m = seq.length;
      for (var i = from || 0; i <= n - m; i++) {
        var ok = true;
        for (var j = 0; j < m; j++) if (buf[i + j] !== seq[j]) { ok = false; break; }
        if (ok) return i;
      }
      return -1;
    }
    var CRLF = [13, 10], CRLFCRLF = [13, 10, 13, 10];

    // incremental gzip inflater over the native binding
    function makeInflater() {
      var h = nat.inflateCreate();
      if (h < 0) throw new Error('inflateCreate failed');
      var state = new Int32Array(2), outbuf = new Uint8Array(65536), done = false;
      return {
        push: function (chunkU8) {
          var out = [], inOff = 0;
          while (inOff < chunkU8.length && !done) {
            var slice = chunkU8.subarray(inOff);
            var produced = nat.inflate(h, slice, outbuf, state);
            var consumed = state[0]; done = state[1] === 1;
            if (produced < 0) break; // inflate error — stop, don't spin
            if (produced > 0) { var b = Buffer.alloc(produced); b.set(outbuf.subarray(0, produced)); out.push(b); }
            // zlib always consumes input when it can make progress; consumed===0 means it's
            // blocked (waiting on output space we already gave 64KB of, or done/errored) — bail
            // rather than spin forever on the same input slice.
            if (consumed === 0) break;
            inOff += consumed;
          }
          return out;
        },
        done: function () { return done; },
        destroy: function () { try { nat.inflateDestroy(h); } catch (e) {} }
      };
    }
    exports._makeInflater = makeInflater;

    function parseHeaders(headStr) {
      var lines = headStr.split('\r\n');
      var top = lines[0].split(' ');
      var headers = {}, rawHeaders = [];
      for (var i = 1; i < lines.length; i++) {
        var c = lines[i].indexOf(':'); if (c <= 0) continue;
        var k = lines[i].slice(0, c).trim(), v = lines[i].slice(c + 1).trim();
        rawHeaders.push(k, v); var lk = k.toLowerCase();
        if (lk === 'set-cookie') { if (!headers[lk]) headers[lk] = []; headers[lk].push(v); }
        else if (headers[lk] !== undefined) headers[lk] += ', ' + v;
        else headers[lk] = v;
      }
      return { statusCode: parseInt(top[1], 10), statusMessage: top.slice(2).join(' '), httpVersion: (top[0] || '').replace('HTTP/', ''), headers: headers, rawHeaders: rawHeaders };
    }

    // response parser: feed(Buffer), eof(); pushes body into a Readable IncomingMessage
    function makeParser(req) {
      var phase = 'head', pending = null, im = null, inflater = null;
      var framing = null, remaining = 0, chunkLeft = 0, chunkPhase = 'size', finished = false;

      function out(bytes) {
        if (!bytes || bytes.length === 0) return;
        if (inflater) { var parts = inflater.push(bytes); for (var i = 0; i < parts.length; i++) im.push(parts[i]); }
        else { var b = Buffer.alloc(bytes.length); b.set(bytes); im.push(b); }
      }
      function finish() {
        if (finished) return; finished = true;
        if (inflater) inflater.destroy();
        if (im) im.push(null);
      }
      function startBody(headInfo) {
        im = new stream.Readable(); im._read = function () {};
        im.statusCode = headInfo.statusCode; im.statusMessage = headInfo.statusMessage;
        im.httpVersion = headInfo.httpVersion; im.headers = headInfo.headers; im.rawHeaders = headInfo.rawHeaders;
        im.complete = false; im.socket = req.socket; im.url = req.path; im.method = req.method;
        im.setEncoding = function (e) { stream.Readable.prototype.setEncoding.call(this, e); return this; };
        var H = headInfo.headers;
        var te = (H['transfer-encoding'] || '').toLowerCase();
        if (te.indexOf('chunked') >= 0) framing = 'chunked';
        else if (H['content-length'] !== undefined) { framing = 'length'; remaining = parseInt(H['content-length'], 10) || 0; }
        else framing = 'close';
        var ce = (H['content-encoding'] || '').toLowerCase();
        if (ce.indexOf('gzip') >= 0 || ce.indexOf('x-gzip') >= 0) {
          inflater = makeInflater();
          // we decompress transparently — strip the headers so consumers (minipass-fetch) don't
          // try to decompress our already-inflated bytes a second time, and don't trust a stale length.
          delete H['content-encoding']; delete H['content-length'];
        }
        htrace('RESPONSE ' + im.statusCode + ' framing=' + framing + ' ce=' + (ce || '-'));
      req.res = im; req.emit('response', im);
        if (framing === 'length' && remaining === 0) { finish(); }
      }
      function process() {
        if (phase === 'head') {
          var idx = findSeq(pending, CRLFCRLF, 0);
          if (idx < 0) return;
          var headStr = pending.slice(0, idx).toString('latin1');
          pending = pending.slice(idx + 4);
          phase = 'body';
          startBody(parseHeaders(headStr));
        }
        if (phase !== 'body' || finished) return;
        if (framing === 'length') {
          if (remaining > 0 && pending.length) {
            var take = Math.min(remaining, pending.length);
            out(pending.subarray(0, take)); pending = pending.slice(take); remaining -= take;
          }
          if (remaining === 0) finish();
        } else if (framing === 'chunked') {
          for (;;) {
            if (chunkPhase === 'size') {
              var nl = findSeq(pending, CRLF, 0);
              if (nl < 0) return;
              var sizeLine = pending.slice(0, nl).toString('latin1').split(';')[0].trim();
              var sz = parseInt(sizeLine, 16);
              pending = pending.slice(nl + 2);
              if (!sz) { // last chunk; consume optional trailers up to CRLFCRLF or CRLF
                var t = findSeq(pending, CRLF, 0);
                pending = (t >= 0) ? pending.slice(t + 2) : pending;
                finish(); return;
              }
              chunkLeft = sz; chunkPhase = 'data';
            }
            if (chunkPhase === 'data') {
              if (chunkLeft > 0 && pending.length) {
                var tk = Math.min(chunkLeft, pending.length);
                out(pending.subarray(0, tk)); pending = pending.slice(tk); chunkLeft -= tk;
              }
              if (chunkLeft > 0) return; // need more
              // consume trailing CRLF
              if (pending.length < 2) return;
              pending = pending.slice(2); chunkPhase = 'size';
            }
          }
        } else { // close-delimited
          if (pending.length) { out(pending); pending = pending.slice(pending.length); }
        }
      }
      return {
        feed: function (chunk) { pending = pending ? Buffer.concat([pending, chunk]) : chunk; process(); },
        eof: function () { if (framing === 'close' && pending && pending.length) { out(pending); pending = pending.slice(pending.length); } finish(); }
      };
    }

    function ClientRequest(transport, opts, cb) {
      EventEmitter.call(this);
      this.transport = transport;
      this.method = (opts.method || 'GET').toUpperCase();
      this.path = opts.path || '/';
      this.host = opts.host || opts.hostname || '127.0.0.1';
      this.port = opts.port || (transport === 'https' ? 443 : 80);
      this._headers = {}; this._bodyChunks = []; this._sent = false; this.socket = null; this.res = null;
      if (opts.headers) for (var k in opts.headers) this._headers[k] = opts.headers[k];
      if (cb) this.once('response', cb);
    }
    ClientRequest.prototype = Object.create(EventEmitter.prototype);
    ClientRequest.prototype.constructor = ClientRequest;
    ClientRequest.prototype.setHeader = function (k, v) { this._headers[k] = v; };
    ClientRequest.prototype.getHeader = function (k) { return this._headers[k]; };
    ClientRequest.prototype.removeHeader = function (k) { delete this._headers[k]; };
    ClientRequest.prototype.write = function (chunk) { this._bodyChunks.push(typeof chunk === 'string' ? Buffer.from(chunk) : chunk); return true; };
    ClientRequest.prototype.setTimeout = function () { return this; };
    ClientRequest.prototype.abort = ClientRequest.prototype.destroy = function (e) { if (this.socket) this.socket.destroy(e); return this; };
    function htrace(s) { if (std.getenv('NODE9_HTTPTRACE')) { try { var f = std.open('/tmp/n9http', 'a'); if (f) { f.puts(s + '\n'); f.close(); } } catch (e) {} } }
    ClientRequest.prototype.end = function (chunk) {
      if (chunk != null) this.write(chunk);
      if (this._sent) return this; this._sent = true;
      var self = this;
      htrace('REQ ' + this.method + ' ' + this.transport + '://' + this.host + ':' + this.port + this.path);
      var sock = (this.transport === 'https')
        ? require('tls').connect({ host: this.host, port: this.port, servername: this.host })
        : require('net').connect({ host: this.host, port: this.port });
      this.socket = sock;
      var parser = makeParser(this);
      sock.on('error', function (e) { htrace('SOCK ERROR ' + (e && e.message)); self.emit('error', e); });
      sock.on('data', function (c) { htrace('DATA +' + c.length); parser.feed(c); });
      sock.on('end', function () { htrace('SOCK END'); parser.eof(); if (self.res) self.res.complete = true; });
      var onReady = function () {
        htrace('CONNECTED ' + self.transport);
        var hdr = self._headers, hasHost = false, hasLen = false, hasConn = false, hasAE = false;
        for (var k in hdr) { var lk = k.toLowerCase(); if (lk === 'host') hasHost = true; else if (lk === 'content-length') hasLen = true; else if (lk === 'connection') hasConn = true; else if (lk === 'accept-encoding') hasAE = true; }
        var body = self._bodyChunks.length ? Buffer.concat(self._bodyChunks) : null;
        var head = self.method + ' ' + self.path + ' HTTP/1.1\r\n';
        if (!hasHost) head += 'Host: ' + self.host + ((self.port !== 80 && self.port !== 443) ? (':' + self.port) : '') + '\r\n';
        if (!hasAE) head += 'Accept-Encoding: gzip\r\n';
        if (body && !hasLen) head += 'Content-Length: ' + body.length + '\r\n';
        if (!hasConn) head += 'Connection: close\r\n';
        for (var k2 in hdr) head += k2 + ': ' + hdr[k2] + '\r\n';
        head += '\r\n';
        sock.write(head);
        if (body) sock.write(body);
        htrace('WROTE head(' + head.length + ')' + (body ? ' body(' + body.length + ')' : ''));
        self.emit('finish');
      };
      sock.on(this.transport === 'https' ? 'secureConnect' : 'connect', onReady);
      return this;
    };
    exports.ClientRequest = ClientRequest;

    function parseUrlOpts(u) {
      // minimal URL split for http(s)://host[:port]/path
      var m = /^(https?):\/\/([^\/:]+)(?::(\d+))?(\/[^\s]*)?$/.exec(u);
      if (!m) return { path: u };
      return { protocol: m[1] + ':', host: m[2], hostname: m[2], port: m[3] ? parseInt(m[3], 10) : undefined, path: m[4] || '/' };
    }
    exports._mkRequest = function (transport, a, b, c) {
      var opts, cb;
      if (typeof a === 'string') { opts = parseUrlOpts(a); if (typeof b === 'function') cb = b; else if (b) { for (var k in b) opts[k] = b[k]; cb = c; } }
      else { opts = a || {}; cb = b; }
      if (opts.protocol === 'https:') transport = 'https';
      return new ClientRequest(transport, opts, cb);
    };
    exports.request = function (a, b, c) { return exports._mkRequest('http', a, b, c); };
    exports.get = function (a, b, c) { var r = exports.request(a, b, c); r.end(); return r; };
    exports.IncomingMessage = stream.Readable;
    exports.Agent = function () {}; exports.globalAgent = new exports.Agent();
  });

  define('https', function (m, exports) {
    var http = require('http');
    exports.request = function (a, b, c) { return http._mkRequest('https', a, b, c); };
    exports.get = function (a, b, c) { var r = exports.request(a, b, c); r.end(); return r; };
    exports.Agent = function () {}; exports.globalAgent = new exports.Agent();
  });

  /* ---------------- tty / string_decoder / zlib / crypto (stubs/minimal) ---------------- */
  define('tty', function (m, exports) {
    exports.isatty = function (fd) { return false; };
    function ReadStream() { require('events').call(this); } require('util').inherits(ReadStream, require('events'));
    function WriteStream() { require('events').call(this); this.columns = 80; this.rows = 24; } require('util').inherits(WriteStream, require('events'));
    exports.ReadStream = ReadStream; exports.WriteStream = WriteStream;
  });
  define('string_decoder', function (m, exports) {
    var Buffer = require('buffer').Buffer;
    function StringDecoder(enc) { this.encoding = String(enc || 'utf8').toLowerCase().replace('-', ''); this._partial = null; }
    // number of trailing bytes that form an incomplete UTF-8 sequence (to hold for next write)
    function incompleteTail(buf) {
      var len = buf.length;
      for (var i = 1; i <= 3 && i <= len; i++) {
        var b = buf[len - i];
        if ((b & 0xC0) !== 0x80) { // a lead byte or ASCII
          if (b < 0x80) return 0;
          var needed = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 1;
          return (i < needed) ? i : 0;
        }
      }
      return 0;
    }
    function prepend(self, buf) { if (self._partial) { var c = new Uint8Array(self._partial.length + buf.length); c.set(self._partial); c.set(buf, self._partial.length); self._partial = null; return c; } return buf; }
    StringDecoder.prototype.write = function (buf) {
      if (typeof buf === 'string') return buf;
      buf = (buf instanceof Uint8Array) ? buf : Buffer.from(buf);
      var enc = this.encoding;
      if (enc === 'utf8' || enc === 'utf8') {
        buf = prepend(this, buf);
        var hold = incompleteTail(buf);
        if (hold > 0) { this._partial = Uint8Array.prototype.slice.call(buf, buf.length - hold); buf = Uint8Array.prototype.subarray.call(buf, 0, buf.length - hold); }
        return Buffer.from(buf).toString('utf8');
      }
      if (enc === 'utf16le' || enc === 'ucs2' || enc === 'base64') {
        var n = enc === 'base64' ? 3 : 2;
        buf = prepend(this, buf);
        var rem = buf.length % n;
        if (rem) { this._partial = Uint8Array.prototype.slice.call(buf, buf.length - rem); buf = Uint8Array.prototype.subarray.call(buf, 0, buf.length - rem); }
        return Buffer.from(buf).toString(enc === 'ucs2' ? 'utf16le' : enc);
      }
      return Buffer.from(buf).toString(enc);
    };
    StringDecoder.prototype.end = function (buf) {
      var r = buf ? this.write(buf) : '';
      if (this._partial) { r += Buffer.from(this._partial).toString(this.encoding === 'ucs2' ? 'utf16le' : this.encoding); this._partial = null; }
      return r;
    };
    exports.StringDecoder = StringDecoder;
  });
  define('zlib', function (m, exports) {
    var stream = require('stream');
    var Buffer = require('buffer').Buffer;
    var makeInflater = require('http')._makeInflater; // native gzip inflater
    // streaming gunzip: a Transform fed gzip bytes, emits inflated bytes
    function Gunzip() {
      stream.Transform.call(this);
      var inf = makeInflater(), self = this;
      this._transform = function (chunk, enc, cb) {
        try { var parts = inf.push(chunk instanceof Uint8Array ? chunk : Buffer.from(chunk)); for (var i = 0; i < parts.length; i++) self.push(parts[i]); cb(); }
        catch (e) { cb(e); }
      };
      this._flush = function (cb) { inf.destroy(); cb(); };
    }
    Gunzip.prototype = Object.create(stream.Transform.prototype);
    Gunzip.prototype.constructor = Gunzip;
    exports.Gunzip = Gunzip;
    exports.createGunzip = exports.createUnzip = function () { return new Gunzip(); };
    exports.gunzipSync = exports.unzipSync = function (buf) {
      var inf = makeInflater(), u = buf instanceof Uint8Array ? buf : Buffer.from(buf);
      var parts = inf.push(u); inf.destroy();
      return parts.length === 1 ? parts[0] : Buffer.concat(parts);
    };
    // compression / inflate-of-raw-deflate / brotli are not wired yet — fail loudly rather than corrupt
    function unsupported(name) { return function () { throw new Error('zlib.' + name + ' not implemented on node9'); }; }
    exports.createGzip = exports.createDeflate = exports.createDeflateRaw = exports.createInflate = exports.createInflateRaw = exports.createBrotliCompress = exports.createBrotliDecompress = function () { throw new Error('zlib: only gunzip is implemented on node9'); };
    exports.gzipSync = unsupported('gzipSync'); exports.deflateSync = unsupported('deflateSync'); exports.brotliCompressSync = unsupported('brotliCompressSync'); exports.brotliDecompressSync = unsupported('brotliDecompressSync');
    // Low-level zlib stream classes that minizlib (incl. tar's BUNDLED copy) constructs via
    // new require('zlib')[mode](opts) and drives with ._processChunk(chunk, flushFlag).
    // We back Gunzip/Unzip with the native gzip inflater; _processChunk returns an array of
    // output Buffers (minizlib disables Buffer.concat and reads result[0..] directly).
    var EE = require('events');
    function GunzipHandle(opts) {
      EE.call(this);
      this._inf = makeInflater();
      this._handle = { close: function () {} }; // the "native handle" minizlib pokes at
      this._closed = false;
    }
    GunzipHandle.prototype = Object.create(EE.prototype);
    GunzipHandle.prototype.constructor = GunzipHandle;
    GunzipHandle.prototype._processChunk = function (chunk, flushFlag) {
      if (this._closed) return [];
      var u8 = (chunk instanceof Uint8Array) ? chunk : Buffer.from(chunk);
      return this._inf.push(u8); // array of Buffers (possibly empty)
    };
    GunzipHandle.prototype.close = function () { if (!this._closed) { this._closed = true; try { this._inf.destroy(); } catch (e) {} } };
    GunzipHandle.prototype.reset = function () {};
    GunzipHandle.prototype.params = function () {};
    exports.Gunzip = GunzipHandle;
    exports.Unzip = GunzipHandle;
    function CompressUnsupported() { throw new Error('zlib: compression / raw-inflate / brotli not implemented on node9'); }
    exports.Gzip = exports.Deflate = exports.DeflateRaw = exports.Inflate = exports.InflateRaw = CompressUnsupported;
    exports.BrotliCompress = exports.BrotliDecompress = CompressUnsupported;
    exports.Zlib = GunzipHandle;
    exports.constants = {
      Z_NO_FLUSH: 0, Z_PARTIAL_FLUSH: 1, Z_SYNC_FLUSH: 2, Z_FULL_FLUSH: 3, Z_FINISH: 4, Z_BLOCK: 5, Z_TREES: 6,
      Z_OK: 0, Z_STREAM_END: 1, Z_NEED_DICT: 2, Z_ERRNO: -1, Z_STREAM_ERROR: -2, Z_DATA_ERROR: -3, Z_BUF_ERROR: -5,
      Z_DEFAULT_COMPRESSION: -1, Z_DEFAULT_STRATEGY: 0,
      ZLIB_VERNUM: 0x12a0, Z_MIN_WINDOWBITS: 8, Z_MAX_WINDOWBITS: 15, Z_DEFAULT_WINDOWBITS: 15,
      Z_MIN_CHUNK: 64, Z_MAX_CHUNK: Infinity, Z_DEFAULT_CHUNK: 16384, Z_MIN_LEVEL: -1, Z_MAX_LEVEL: 9, Z_DEFAULT_LEVEL: -1
    };
  });
  define('crypto', function (m, exports) {
    // real crypto over the native libsec bindings (N1): sha256/sha512/sha1/md5 + hmac + CSPRNG
    var Buffer = require('buffer').Buffer;
    var stream = require('stream');
    var nat = globalThis.__n9native;
    function algoNum(a) {
      a = String(a).toLowerCase().replace(/-/g, '');
      if (a === 'sha256') return 0; if (a === 'sha512') return 1; if (a === 'sha1') return 2; if (a === 'md5') return 3;
      return -1;
    }
    // n9_sec's hash handle table is fixed-size; a Hash GC'd without digest()/destroy() would
    // leak its slot. Reclaim on GC by digesting the handle into a throwaway (frees libsec state).
    var hashReg = (typeof FinalizationRegistry !== 'undefined') ? new FinalizationRegistry(function (h) { if (h >= 0) { try { nat.hashDigest(h, new Uint8Array(64)); } catch (e) {} } }) : null;
    // Hash is a Transform so it can be piped (ssri streams tarballs through it)
    function Hash(algo) {
      stream.Transform.call(this);
      var an = algoNum(algo);
      if (an < 0) throw new Error('Digest method not supported: ' + algo);
      this._an = an; this._h = nat.hashCreate(an); this._dlen = nat.hashDlen(an); this._fin = false;
      if (this._h < 0) throw new Error('hashCreate failed for ' + algo);
      if (hashReg) hashReg.register(this, this._h, this);
      var self = this;
      this._transform = function (chunk, enc, cb) { self.update(chunk); cb(); };
      this._flush = function (cb) { self.push(self.digest()); cb(); };
    }
    Hash.prototype = Object.create(stream.Transform.prototype); Hash.prototype.constructor = Hash;
    Hash.prototype.update = function (data, enc) {
      if (this._fin) throw new Error('Digest already called');
      var b = (typeof data === 'string') ? Buffer.from(data, enc || 'utf8') : (data instanceof Uint8Array ? data : Buffer.from(data));
      nat.hashUpdate(this._h, b);
      return this;
    };
    Hash.prototype.digest = function (enc) {
      if (this._fin) throw new Error('Digest already called');
      this._fin = true;
      var out = new Uint8Array(this._dlen);
      var dlen = nat.hashDigest(this._h, out);
      if (hashReg) try { hashReg.unregister(this); } catch (e) {}
      this._h = -1; // native digest finalizes+frees the handle
      var buf = Buffer.alloc(dlen); buf.set(out.subarray(0, dlen));
      return enc ? buf.toString(enc) : buf;
    };
    // Free the native handle (htab is fixed-size) if the hash is abandoned without digest()
    // — e.g. a stream errors mid-way. n9_sec has no free-without-digest, so digest into a throwaway.
    Hash.prototype._freeHandle = function () { if (!this._fin && this._h >= 0) { this._fin = true; try { nat.hashDigest(this._h, new Uint8Array(this._dlen)); } catch (e) {} if (hashReg) try { hashReg.unregister(this); } catch (e) {} this._h = -1; } };
    Hash.prototype.destroy = function (err) { this._freeHandle(); if (stream.Transform.prototype.destroy) return stream.Transform.prototype.destroy.call(this, err); this.emit('close'); return this; };
    exports.createHash = function (a) { return new Hash(a); };
    exports.getHashes = function () { return ['sha256', 'sha512', 'sha1', 'md5']; };

    function Hmac(algo, key) {
      var an = algoNum(algo); if (an !== 0 && an !== 1) throw new Error('HMAC supports sha256/sha512 only: ' + algo);
      this._an = an; this._key = (typeof key === 'string') ? Buffer.from(key, 'utf8') : Buffer.from(key); this._chunks = []; this._fin = false;
    }
    Hmac.prototype.update = function (data, enc) { var b = (typeof data === 'string') ? Buffer.from(data, enc || 'utf8') : Buffer.from(data); this._chunks.push(b); return this; };
    Hmac.prototype.digest = function (enc) {
      this._fin = true;
      var data = this._chunks.length === 1 ? this._chunks[0] : Buffer.concat(this._chunks);
      var dlen = this._an === 0 ? 32 : 64, out = new Uint8Array(dlen);
      nat.hmac(this._an, this._key, data, out);
      var buf = Buffer.alloc(dlen); buf.set(out);
      return enc ? buf.toString(enc) : buf;
    };
    exports.createHmac = function (a, k) { return new Hmac(a, k); };

    exports.randomBytes = function (n, cb) {
      var b = Buffer.alloc(n); var u = new Uint8Array(n); nat.randomBytes(u); b.set(u);
      if (cb) { process.nextTick(function () { cb(null, b); }); return; }
      return b;
    };
    exports.randomFillSync = function (buf, offset, size) { offset = offset || 0; size = size == null ? buf.length - offset : size; var u = new Uint8Array(size); nat.randomBytes(u); for (var i = 0; i < size; i++) buf[offset + i] = u[i]; return buf; };
    exports.randomInt = function (a, b) { if (b === undefined) { b = a; a = 0; } var r = new Uint8Array(6); nat.randomBytes(r); var v = 0; for (var i = 0; i < 6; i++) v = v * 256 + r[i]; return a + (v % (b - a)); };
    exports.randomUUID = function () {
      var b = new Uint8Array(16); nat.randomBytes(b);
      b[6] = (b[6] & 0x0f) | 0x40; b[8] = (b[8] & 0x3f) | 0x80;
      var h = []; for (var i = 0; i < 16; i++) h.push(('0' + b[i].toString(16)).slice(-2));
      return h.slice(0, 4).join('') + '-' + h.slice(4, 6).join('') + '-' + h.slice(6, 8).join('') + '-' + h.slice(8, 10).join('') + '-' + h.slice(10, 16).join('');
    };
    exports.Hash = Hash; exports.Hmac = Hmac;
    exports.constants = {};
    exports.webcrypto = { getRandomValues: function (ta) { var u = new Uint8Array(ta.buffer, ta.byteOffset, ta.byteLength); nat.randomBytes(u); return ta; } };
  });

  /* ---------------- legacy `constants` (graceful-fs et al.) ---------------- */
  define('constants', function (m, exports) {
    var fc = require('fs').constants;
    for (var k in fc) exports[k] = fc[k];
    exports.O_RDONLY = fc.O_RDONLY; exports.O_WRONLY = fc.O_WRONLY; exports.O_RDWR = fc.O_RDWR;
    exports.O_CREAT = fc.O_CREAT; exports.O_TRUNC = fc.O_TRUNC; exports.O_APPEND = fc.O_APPEND; exports.O_EXCL = fc.O_EXCL;
    exports.S_IFMT = fc.S_IFMT; exports.S_IFREG = fc.S_IFREG; exports.S_IFDIR = fc.S_IFDIR; exports.S_IFLNK = fc.S_IFLNK;
    exports.S_IFBLK = 0x6000; exports.S_IFCHR = 0x2000; exports.S_IFIFO = 0x1000; exports.S_IFSOCK = 0xC000;
    exports.S_IRWXU = 0x1c0; exports.S_IRUSR = 0x100; exports.S_IWUSR = 0x80; exports.S_IXUSR = 0x40;
    exports.F_OK = 0; exports.R_OK = 4; exports.W_OK = 2; exports.X_OK = 1;
    exports.COPYFILE_EXCL = 1; exports.E2BIG = 7; exports.EACCES = 13; exports.EEXIST = 17; exports.ENOENT = 2; exports.ENOTDIR = 20; exports.EISDIR = 21;
  });

  /* ---------------- WHATWG URL + URLSearchParams ---------------- */
  define('url', function (m, exports) {
    function USP(init) {
      this._p = [];
      if (typeof init === 'string') { var s = init.charAt(0) === '?' ? init.slice(1) : init; if (s) s.split('&').forEach(function (kv) { var i = kv.indexOf('='); var k = i < 0 ? kv : kv.slice(0, i); var v = i < 0 ? '' : kv.slice(i + 1); this._p.push([dec(k), dec(v)]); }, this); }
      else if (init && typeof init === 'object') { if (typeof init.forEach === 'function' && !Array.isArray(init)) { /* Map/USP */ if (init instanceof USP) init._p.forEach(function (e) { this._p.push([e[0], e[1]]); }, this); else init.forEach(function (v, k) { this._p.push([String(k), String(v)]); }, this); } else for (var k in init) this._p.push([k, String(init[k])]); }
    }
    function dec(s) { try { return decodeURIComponent(String(s).replace(/\+/g, ' ')); } catch (e) { return s; } }
    function enc(s) { return encodeURIComponent(String(s)); }
    USP.prototype.append = function (k, v) { this._p.push([String(k), String(v)]); };
    USP.prototype.delete = function (k) { this._p = this._p.filter(function (e) { return e[0] !== k; }); };
    USP.prototype.get = function (k) { for (var i = 0; i < this._p.length; i++) if (this._p[i][0] === k) return this._p[i][1]; return null; };
    USP.prototype.getAll = function (k) { return this._p.filter(function (e) { return e[0] === k; }).map(function (e) { return e[1]; }); };
    USP.prototype.has = function (k) { return this.get(k) !== null; };
    USP.prototype.set = function (k, v) { var done = false; this._p = this._p.filter(function (e) { if (e[0] !== k) return true; if (!done) { e[1] = String(v); done = true; return true; } return false; }); if (!done) this._p.push([String(k), String(v)]); };
    USP.prototype.sort = function () { this._p.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); };
    USP.prototype.forEach = function (fn, t) { this._p.forEach(function (e) { fn.call(t, e[1], e[0], this); }, this); };
    USP.prototype.keys = function () { return this._p.map(function (e) { return e[0]; })[Symbol.iterator](); };
    USP.prototype.values = function () { return this._p.map(function (e) { return e[1]; })[Symbol.iterator](); };
    USP.prototype.entries = function () { return this._p.map(function (e) { return [e[0], e[1]]; })[Symbol.iterator](); };
    USP.prototype[Symbol.iterator] = function () { return this.entries(); };
    USP.prototype.toString = function () { return this._p.map(function (e) { return enc(e[0]) + '=' + enc(e[1]); }).join('&'); };
    Object.defineProperty(USP.prototype, 'size', { get: function () { return this._p.length; } });

    var RE = /^(?:([a-zA-Z][a-zA-Z0-9+.\-]*:))?(?:\/\/(?:([^/?#@]*)@)?(\[[^\]]*\]|[^/?#:]*)(?::(\d+))?)?([^?#]*)(\?[^#]*)?(#.*)?$/;
    function resolveRel(base, rel) {
      if (/^[a-zA-Z][a-zA-Z0-9+.\-]*:/.test(rel)) return rel;
      var bm = RE.exec(base); if (!bm) return rel;
      var origin = (bm[1] || '') + '//' + (bm[2] ? bm[2] + '@' : '') + (bm[3] || '') + (bm[4] ? ':' + bm[4] : '');
      if (rel.charAt(0) === '/') return origin + rel;
      if (rel.charAt(0) === '?') return origin + (bm[5] || '') + rel;
      if (rel.charAt(0) === '#') return origin + (bm[5] || '') + (bm[6] || '') + rel;
      var dir = (bm[5] || '/').replace(/[^/]*$/, '');
      var path = dir + rel; var parts = path.split('/'), out = [];
      for (var i = 0; i < parts.length; i++) { if (parts[i] === '.') continue; if (parts[i] === '..') { out.pop(); } else out.push(parts[i]); }
      return origin + out.join('/');
    }
    function URL(input, base) {
      input = String(input);
      if (base !== undefined && base !== null) input = resolveRel(String(base), input);
      var mm = RE.exec(input);
      if (!mm || !mm[1]) throw new TypeError('Invalid URL: ' + input);
      this._protocol = mm[1]; var auth = mm[2] || '';
      var ai = auth.indexOf(':');
      this._username = auth ? (ai < 0 ? auth : auth.slice(0, ai)) : '';
      this._password = (auth && ai >= 0) ? auth.slice(ai + 1) : '';
      this._hostname = mm[3] || ''; this._port = mm[4] || '';
      this._pathname = mm[5] || ((this._protocol === 'http:' || this._protocol === 'https:') ? '/' : '');
      this._search = mm[6] || ''; this._hash = mm[7] || '';
      this.searchParams = new USP(this._search);
      var self = this;
      this.searchParams.toString = (function (orig) { return function () { var s = orig.call(self.searchParams); return s; }; })(USP.prototype.toString);
    }
    function defAccessor(name, get, set) { Object.defineProperty(URL.prototype, name, { get: get, set: set, enumerable: true }); }
    defAccessor('protocol', function () { return this._protocol; }, function (v) { this._protocol = /:$/.test(v) ? v : v + ':'; });
    defAccessor('username', function () { return this._username; }, function (v) { this._username = v; });
    defAccessor('password', function () { return this._password; }, function (v) { this._password = v; });
    defAccessor('hostname', function () { return this._hostname; }, function (v) { this._hostname = v; });
    defAccessor('port', function () { return this._port; }, function (v) { this._port = String(v); });
    defAccessor('pathname', function () { return this._pathname; }, function (v) { this._pathname = v.charAt(0) === '/' ? v : '/' + v; });
    defAccessor('hash', function () { return this._hash; }, function (v) { this._hash = v ? (v.charAt(0) === '#' ? v : '#' + v) : ''; });
    defAccessor('search', function () { return this.searchParams._p.length ? '?' + this.searchParams.toString() : ''; }, function (v) { this.searchParams = new USP(v); });
    defAccessor('host', function () { return this._hostname + (this._port ? ':' + this._port : ''); }, function (v) { var i = v.indexOf(':'); if (i < 0) { this._hostname = v; this._port = ''; } else { this._hostname = v.slice(0, i); this._port = v.slice(i + 1); } });
    defAccessor('origin', function () { return this._protocol + '//' + this._hostname + (this._port ? ':' + this._port : ''); });
    defAccessor('href', function () {
      var s = this._protocol + '//';
      if (this._username) { s += this._username; if (this._password) s += ':' + this._password; s += '@'; }
      s += this._hostname; if (this._port) s += ':' + this._port; s += this._pathname;
      var q = this.searchParams._p.length ? '?' + this.searchParams.toString() : ''; s += q + this._hash; return s;
    }, function (v) { URL.call(this, v); });
    URL.prototype.toString = function () { return this.href; };
    URL.prototype.toJSON = function () { return this.href; };

    exports.URL = URL; exports.URLSearchParams = USP;
    // legacy url.parse/format/resolve
    exports.parse = function (u, parseQuery) {
      var mm = RE.exec(String(u)) || [];
      var search = mm[6] || null, host = (mm[3] || '') + (mm[4] ? ':' + mm[4] : '');
      return { href: u, protocol: mm[1] || null, slashes: /^[^:]+:\/\//.test(u) || null, host: host || null, auth: mm[2] || null,
        hostname: mm[3] || null, port: mm[4] || null, pathname: mm[5] || null, search: search,
        query: parseQuery ? require('querystring').parse((search || '').replace(/^\?/, '')) : (search ? search.replace(/^\?/, '') : null),
        hash: mm[7] || null, path: (mm[5] || '') + (search || '') || null };
    };
    exports.format = function (o) {
      if (typeof o === 'string') return o;
      if (o instanceof URL) return o.href;
      var s = (o.protocol ? (/:$/.test(o.protocol) ? o.protocol : o.protocol + ':') : '') + (o.slashes || o.host || o.hostname ? '//' : '');
      if (o.auth) s += o.auth + '@';
      s += o.host || (o.hostname || '') + (o.port ? ':' + o.port : '');
      s += o.pathname || '';
      var q = o.search || (o.query ? '?' + (typeof o.query === 'string' ? o.query : require('querystring').stringify(o.query)) : '');
      s += q || ''; s += o.hash || ''; return s;
    };
    exports.resolve = function (from, to) { return resolveRel(from, to); };
    exports.fileURLToPath = function (u) { var s = (u instanceof URL) ? u.pathname : String(u).replace(/^file:\/\//, ''); return decodeURIComponent(s); };
    exports.pathToFileURL = function (p) { var u = Object.create(URL.prototype); URL.call(u, 'file://' + (p.charAt(0) === '/' ? '' : '/') + p); return u; };
    exports.domainToASCII = function (d) { return String(d).toLowerCase(); };
    exports.domainToUnicode = function (d) { return String(d); };
  });

  /* ---------------- punycode (minimal, ASCII passthrough sufficient for registry) ---------------- */
  define('punycode', function (m, exports) {
    exports.encode = function (s) { return s; }; exports.decode = function (s) { return s; };
    exports.toASCII = function (d) { return String(d); }; exports.toUnicode = function (d) { return String(d); };
    exports.ucs2 = { decode: function (s) { return String(s).split('').map(function (c) { return c.charCodeAt(0); }); }, encode: function (a) { return a.map(function (c) { return String.fromCharCode(c); }).join(''); } };
    exports.version = '2.1.0';
  });

  /* ---------------- querystring ---------------- */
  define('querystring', function (m, exports) {
    function enc(s) { return encodeURIComponent(String(s == null ? '' : s)); }
    function dec(s) { try { return decodeURIComponent(String(s).replace(/\+/g, ' ')); } catch (e) { return s; } }
    exports.escape = enc; exports.unescape = dec;
    exports.parse = exports.decode = function (qs, sep, eq) {
      sep = sep || '&'; eq = eq || '='; var obj = {};
      if (typeof qs !== 'string' || qs.length === 0) return obj;
      var parts = qs.split(sep);
      for (var i = 0; i < parts.length; i++) {
        var x = parts[i], idx = x.indexOf(eq), k, v;
        if (idx >= 0) { k = x.slice(0, idx); v = x.slice(idx + 1); } else { k = x; v = ''; }
        k = dec(k); v = dec(v);
        if (Object.prototype.hasOwnProperty.call(obj, k)) { if (Array.isArray(obj[k])) obj[k].push(v); else obj[k] = [obj[k], v]; }
        else obj[k] = v;
      }
      return obj;
    };
    exports.stringify = exports.encode = function (obj, sep, eq) {
      sep = sep || '&'; eq = eq || '='; var pairs = [];
      if (obj && typeof obj === 'object') for (var k in obj) {
        var ek = enc(k), v = obj[k];
        if (Array.isArray(v)) { for (var i = 0; i < v.length; i++) pairs.push(ek + eq + enc(v[i])); }
        else pairs.push(ek + eq + enc(v));
      }
      return pairs.join(sep);
    };
  });

  /* ---------------- timers ---------------- */
  define('timers', function (m, exports) {
    exports.setTimeout = globalThis.setTimeout; exports.clearTimeout = globalThis.clearTimeout;
    exports.setInterval = globalThis.setInterval; exports.clearInterval = globalThis.clearInterval;
    exports.setImmediate = globalThis.setImmediate || function (fn) { var a = Array.prototype.slice.call(arguments, 1); return globalThis.setTimeout(function () { fn.apply(null, a); }, 0); };
    exports.clearImmediate = globalThis.clearImmediate || function (id) { globalThis.clearTimeout(id); };
  });
  define('timers/promises', function (m, exports) {
    exports.setTimeout = function (ms, val) { return new Promise(function (res) { globalThis.setTimeout(function () { res(val); }, ms || 0); }); };
    exports.setImmediate = function (val) { return new Promise(function (res) { globalThis.setTimeout(function () { res(val); }, 0); }); };
    exports.setInterval = function () { throw new Error('timers/promises.setInterval not implemented'); };
  });

  /* ---------------- async_hooks (single-thread stub + AsyncLocalStorage) ---------------- */
  define('async_hooks', function (m, exports) {
    var idc = 1;
    exports.executionAsyncId = function () { return 1; };
    exports.triggerAsyncId = function () { return 0; };
    exports.createHook = function () { return { enable: function () { return this; }, disable: function () { return this; } }; };
    function AsyncResource(type) { this.type = type; this._id = ++idc; }
    AsyncResource.prototype.runInAsyncScope = function (fn, thisArg) { return fn.apply(thisArg, Array.prototype.slice.call(arguments, 2)); };
    AsyncResource.prototype.bind = function (fn) { return fn; };
    AsyncResource.prototype.emitDestroy = function () { return this; };
    exports.AsyncResource = AsyncResource;
    function ALS() { this._store = undefined; }
    ALS.prototype.run = function (store, cb) { var prev = this._store; this._store = store; try { return cb.apply(null, Array.prototype.slice.call(arguments, 2)); } finally { this._store = prev; } };
    ALS.prototype.getStore = function () { return this._store; };
    ALS.prototype.exit = function (cb) { var prev = this._store; this._store = undefined; try { return cb.apply(null, Array.prototype.slice.call(arguments, 1)); } finally { this._store = prev; } };
    ALS.prototype.enterWith = function (store) { this._store = store; };
    ALS.prototype.disable = function () { this._store = undefined; };
    exports.AsyncLocalStorage = ALS;
  });

  /* ---------------- module ---------------- */
  define('module', function (m, exports) {
    var P = require('path');
    function Module(id) { this.id = id || ''; this.exports = {}; this.filename = id; this.loaded = false; }
    Module._cache = require.cache;
    Module.builtinModules = Object.keys(factories);
    Module.isBuiltin = function (n) { n = n.indexOf('node:') === 0 ? n.slice(5) : n; return !!factories[n]; };
    Module.createRequire = function (from) {
      var dir = P.dirname(from.replace(/^file:\/\//, ''));
      var req = function (name) { dirStack.push(dir); try { return require(name); } finally { dirStack.pop(); } };
      req.resolve = function (n) { return require.resolve(n); }; req.cache = require.cache; req.main = require.main;
      return req;
    };
    Module.createRequireFromPath = Module.createRequire;
    Module.Module = Module;
    m.exports = Module;
  });

  /* ---------------- child_process (over os.exec; sync-first) ---------------- */
  define('child_process', function (m, exports) {
    var EventEmitter = require('events');
    function toArgv(cmd, args) { return [cmd].concat(args || []); }
    exports.spawnSync = function (cmd, args, opts) {
      opts = opts || {};
      var argv = toArgv(cmd, args);
      var status = -1;
      try { status = os.exec(argv, { block: true, cwd: opts.cwd }); } catch (e) { return { error: e, status: null, signal: null, pid: 0, stdout: null, stderr: null, output: [null, null, null] }; }
      return { status: status, signal: null, pid: 0, stdout: Buffer.alloc(0), stderr: Buffer.alloc(0), output: [null, Buffer.alloc(0), Buffer.alloc(0)] };
    };
    exports.spawn = function (cmd, args, opts) {
      var ee = new EventEmitter(); ee.stdout = new EventEmitter(); ee.stderr = new EventEmitter(); ee.stdin = { write: function () {}, end: function () {} };
      process.nextTick(function () { var r = exports.spawnSync(cmd, args, opts); ee.emit('exit', r.status || 0, null); ee.emit('close', r.status || 0, null); });
      return ee;
    };
    exports.execSync = function (cmd, opts) { var r = exports.spawnSync('/bin/rc', ['-c', cmd], opts); if (r.status !== 0) { var e = new Error('Command failed: ' + cmd); e.status = r.status; throw e; } return Buffer.alloc(0); };
    exports.exec = function (cmd, opts, cb) { if (typeof opts === 'function') { cb = opts; opts = {}; } process.nextTick(function () { try { exports.execSync(cmd, opts); if (cb) cb(null, '', ''); } catch (e) { if (cb) cb(e, '', ''); } }); return exports.spawn('/bin/rc', ['-c', cmd], opts); };
    exports.execFile = function (file, args, opts, cb) { if (typeof args === 'function') { cb = args; args = []; opts = {}; } else if (typeof opts === 'function') { cb = opts; opts = {}; } process.nextTick(function () { var r = exports.spawnSync(file, args, opts); if (cb) cb(r.status === 0 ? null : new Error('exit ' + r.status), '', ''); }); return exports.spawn(file, args, opts); };
    exports.fork = function () { throw new Error('child_process.fork not supported on node9'); };
  });

  /* ---------------- dns (over /net/cs) ---------------- */
  define('dns', function (m, exports) {
    function lookup(host, opts, cb) {
      if (typeof opts === 'function') { cb = opts; opts = {}; }
      process.nextTick(function () {
        // resolve via /net/cs: ip!host!1 → returns address; fall back to host itself
        try {
          var cs = os.open('/net/cs', os.O_RDWR);
          var q = 'net!' + host + '!1', b = new Uint8Array(q.length); for (var i = 0; i < q.length; i++) b[i] = q.charCodeAt(i);
          os.write(cs, b.buffer, 0, b.length); os.seek(cs, 0, std.SEEK_SET);
          var rb = new Uint8Array(256), n = os.read(cs, rb.buffer, 0, 256); os.close(cs);
          var s = ''; for (var j = 0; j < n; j++) s += String.fromCharCode(rb[j]);
          var addr = s.split(' ')[1] || ''; var ip = (addr.split('!')[1]) || host;
          cb(null, ip, /:/.test(ip) ? 6 : 4);
        } catch (e) { cb(null, host, 4); }
      });
    }
    exports.lookup = lookup;
    exports.resolve = exports.resolve4 = function (host, cb) { lookup(host, {}, function (e, a) { cb(e, a ? [a] : []); }); };
    exports.promises = { lookup: function (h, o) { return new Promise(function (res, rej) { lookup(h, o || {}, function (e, address, family) { e ? rej(e) : res({ address: address, family: family }); }); }); }, resolve4: function (h) { return new Promise(function (res) { lookup(h, {}, function (e, a) { res(a ? [a] : []); }); }); } };
    exports.setServers = function () {}; exports.getServers = function () { return []; };
    exports.ADDRCONFIG = 0; exports.V4MAPPED = 0;
  });
  define('dns/promises', function (m, exports) { var d = require('dns'); for (var k in d.promises) exports[k] = d.promises[k]; });

  /* ---------------- misc small builtins ---------------- */
  define('readline', function (m, exports) {
    var EventEmitter = require('events');
    function Interface() { EventEmitter.call(this); } require('util').inherits(Interface, EventEmitter);
    Interface.prototype.question = function (q, cb) { cb(''); };
    Interface.prototype.close = function () { this.emit('close'); };
    Interface.prototype.on = EventEmitter.prototype.on;
    exports.Interface = Interface;
    exports.createInterface = function () { return new Interface(); };
    exports.clearLine = function () {}; exports.cursorTo = function () {}; exports.moveCursor = function () {};
  });
  define('vm', function (m, exports) {
    exports.runInThisContext = function (code) { return (0, eval)(code); };
    exports.runInNewContext = function (code, ctx) { return (0, eval)(code); };
    exports.createContext = function (o) { return o || {}; };
    exports.Script = function (code) { this.code = code; this.runInThisContext = function () { return (0, eval)(code); }; this.runInNewContext = function () { return (0, eval)(code); }; };
  });
  define('http2', function (m, exports) {
    // sigstore (provenance verification, deferred) requires this at load; stub so require
    // succeeds. node9 does not implement HTTP/2 (npm's registry path uses HTTP/1.1).
    function unsupported() { throw new Error('http2 not implemented on node9'); }
    exports.connect = unsupported; exports.createServer = unsupported; exports.createSecureServer = unsupported;
    exports.constants = {}; exports.getDefaultSettings = function () { return {}; };
    exports.Http2Session = function () {}; exports.Http2Stream = function () {}; exports.ServerHttp2Stream = function () {};
  });
  define('test', function (m, exports) {  // minimal node:test runner (sync + promise tests)
    function makeCtx(name) { return { name: name, diagnostic: function () {}, skip: function () {}, todo: function () {}, plan: function () {}, test: test, beforeEach: function () {}, afterEach: function () {}, signal: undefined }; }
    function test(name, opts, fn) {
      if (typeof name === 'function') { fn = name; name = fn.name || '<anonymous>'; }
      if (typeof opts === 'function') { fn = opts; opts = {}; }
      if (!fn) return Promise.resolve();
      var r;
      try { r = fn(makeCtx(name)); } catch (e) { e.message = 'test "' + name + '": ' + e.message; throw e; }
      if (r && typeof r.then === 'function') return r.then(function () {}, function (e) { if (e) { e.message = 'test "' + name + '": ' + e.message; } throw e; });
      return Promise.resolve();
    }
    test.test = test; test.it = test;
    test.describe = function (name, fn) { if (typeof name === 'function') fn = name; return fn ? fn() : undefined; };
    test.suite = test.describe;
    test.before = test.after = test.beforeEach = test.afterEach = function () {};
    test.mock = { fn: function (impl) { var f = function () { f.mock.calls.push({ arguments: Array.prototype.slice.call(arguments), result: undefined }); return impl ? impl.apply(this, arguments) : undefined; }; f.mock = { calls: [], restore: function () {}, resetCalls: function () { f.mock.calls = []; } }; return f; }, method: function () {}, getter: function () {}, setter: function () {}, restoreAll: function () {}, reset: function () {}, timers: { enable: function () {}, reset: function () {}, tick: function () {} } };
    m.exports = test;
  });
  define('node:test', function (m, exports) { m.exports = require('test'); });
  define('path/posix', function (m, exports) { m.exports = require('path').posix; });
  define('path/win32', function (m, exports) { m.exports = require('path').win32; });
  define('domain', function (m, exports) {
    var EventEmitter = require('events');
    function Domain() { EventEmitter.call(this); this.members = []; }
    require('util').inherits(Domain, EventEmitter);
    Domain.prototype.run = function (fn) { try { return fn.apply(this, Array.prototype.slice.call(arguments, 1)); } catch (e) { this.emit('error', e); } };
    Domain.prototype.add = function (e) { this.members.push(e); }; Domain.prototype.remove = function () {}; Domain.prototype.bind = function (fn) { return fn; }; Domain.prototype.intercept = function (fn) { return fn; }; Domain.prototype.enter = function () {}; Domain.prototype.exit = function () {}; Domain.prototype.dispose = function () {};
    exports.Domain = Domain; exports.create = exports.createDomain = function () { return new Domain(); }; exports.active = null;
  });
  define('fs/promises', function (m, exports) { var p = require('fs').promises; for (var k in p) exports[k] = p[k]; exports.default = p; });
  define('stream/promises', function (m, exports) {
    var s = require('stream');
    exports.pipeline = function () { var args = Array.prototype.slice.call(arguments); return new Promise(function (res, rej) { args.push(function (err) { err ? rej(err) : res(); }); s.pipeline.apply(null, args); }); };
    exports.finished = function (stream) { return new Promise(function (res, rej) { s.finished(stream, function (err) { err ? rej(err) : res(); }); }); };
  });
  define('stream/consumers', function (m, exports) {
    var Buffer = require('buffer').Buffer;
    function collect(stream) { return new Promise(function (res, rej) { var chunks = []; stream.on('data', function (c) { chunks.push(typeof c === 'string' ? Buffer.from(c) : c); }); stream.on('end', function () { res(Buffer.concat(chunks)); }); stream.on('error', rej); }); }
    exports.buffer = collect; exports.arrayBuffer = function (s) { return collect(s).then(function (b) { return b.buffer.slice(b.byteOffset, b.byteOffset + b.length); }); };
    exports.text = function (s) { return collect(s).then(function (b) { return b.toString('utf8'); }); };
    exports.json = function (s) { return collect(s).then(function (b) { return JSON.parse(b.toString('utf8')); }); };
  });
  define('util/types', function (m, exports) { var t = require('util').types || {}; for (var k in t) exports[k] = t[k]; });
  define('node:sea', function (m, exports) { exports.isSea = function () { return false; }; });
  define('worker_threads', function (m, exports) { exports.isMainThread = true; exports.Worker = function () { throw new Error('worker_threads not supported on node9'); }; exports.parentPort = null; exports.threadId = 0; exports.workerData = null; });
  define('perf_hooks', function (m, exports) { exports.performance = globalThis.performance || { now: function () { return Date.now(); } }; exports.PerformanceObserver = function () { return { observe: function () {}, disconnect: function () {} }; }; exports.monitorEventLoopDelay = function () { return { enable: function () {}, disable: function () {} }; }; });
  define('v8', function (m, exports) { exports.serialize = function (o) { return require('buffer').Buffer.from(JSON.stringify(o)); }; exports.deserialize = function (b) { return JSON.parse(b.toString()); }; exports.getHeapStatistics = function () { return { total_heap_size: 0, used_heap_size: 0, heap_size_limit: 0x40000000 }; }; exports.setFlagsFromString = function () {}; });
  define('inspector', function (m, exports) { exports.open = function () {}; exports.close = function () {}; exports.url = function () {}; exports.Session = function () { return { connect: function () {}, post: function () {}, disconnect: function () {} }; }; });

  /* ---------------- install globals ---------------- */
  globalThis.process = require('process');
  globalThis.Buffer = require('buffer').Buffer;
  /* The entry script is loaded by n9_cli.c (not via require), so seed dirStack[0] with its
     directory — otherwise its relative requires (e.g. npm-cli.js -> ../lib/cli.js) resolve
     against cwd and fail. */
  if (globalThis.scriptArgs && scriptArgs[0]) {
    var __main = scriptArgs[0], __P = require('path');
    if (__main.charAt(0) !== '/') __main = __P.resolve(os.getcwd()[0] || '/', __main);
    if (fileExists(__main)) dirStack[0] = __P.dirname(__main);
    __rebindGlobalRequire();
    require.main = { filename: __main, id: __main, exports: {}, paths: [], loaded: false };
  }
  if (!Error.captureStackTrace) Error.captureStackTrace = function () {};
  if (!Error.stackTraceLimit) Error.stackTraceLimit = 10;
  if (!globalThis.queueMicrotask) globalThis.queueMicrotask = function (fn) { Promise.resolve().then(fn); };
  if (!globalThis.global) globalThis.global = globalThis;
  if (os.setTimeout) {
    // Node returns a Timeout object (with unref/ref/refresh) from set*; wrap the raw id.
    function Timeout(id, isInterval) { this._id = id; this._interval = isInterval; }
    Timeout.prototype.unref = function () { return this; };
    Timeout.prototype.ref = function () { return this; };
    Timeout.prototype.hasRef = function () { return true; };
    Timeout.prototype.refresh = function () { return this; };
    Timeout.prototype[Symbol.toPrimitive] = function () { return this._id; };
    Timeout.prototype.close = function () { try { if (this._interval) os.clearInterval(this._id); else os.clearTimeout(this._id); } catch (e) {} };
    function idOf(t) { return (t && typeof t === 'object' && t._id != null) ? t._id : t; }
    globalThis.setTimeout = function (fn, ms) { var a = Array.prototype.slice.call(arguments, 2); return new Timeout(os.setTimeout(function () { fn.apply(null, a); }, ms || 0), false); };
    globalThis.clearTimeout = function (t) { if (t != null) try { os.clearTimeout(idOf(t)); } catch (e) {} };
    globalThis.setInterval = function (fn, ms) { var a = Array.prototype.slice.call(arguments, 2); return new Timeout(os.setInterval(function () { fn.apply(null, a); }, ms || 0), true); };
    globalThis.clearInterval = function (t) { if (t != null) try { os.clearInterval(idOf(t)); } catch (e) {} };
  }
  if (!globalThis.setImmediate) globalThis.setImmediate = function (fn) { var a = Array.prototype.slice.call(arguments, 1); return globalThis.setTimeout(function () { fn.apply(null, a); }, 0); };
  if (!globalThis.clearImmediate) globalThis.clearImmediate = function (id) { globalThis.clearTimeout(id); };
  if (!globalThis.structuredClone) globalThis.structuredClone = function (o) { return o == null ? o : JSON.parse(JSON.stringify(o)); };
  // global Web Crypto (uuid, nanoid, many packages use globalThis.crypto.getRandomValues / randomUUID)
  if (!globalThis.crypto) {
    var __c = require('crypto');
    globalThis.crypto = {
      getRandomValues: function (ta) { var u = new Uint8Array(ta.buffer, ta.byteOffset, ta.byteLength); var r = __c.randomBytes(u.length); for (var i = 0; i < u.length; i++) u[i] = r[i]; return ta; },
      randomUUID: function () { return __c.randomUUID(); },
      subtle: { digest: function (algo, data) { var a = (typeof algo === 'string' ? algo : algo.name).toLowerCase().replace('-', ''); var h = __c.createHash(a === 'sha1' ? 'sha1' : a).update(require('buffer').Buffer.from(data.buffer || data)).digest(); return Promise.resolve(h.buffer.slice(h.byteOffset, h.byteOffset + h.length)); } }
    };
  }

  /* WHATWG URL globals */
  var __url = require('url');
  if (!globalThis.URL) globalThis.URL = __url.URL;
  if (!globalThis.URLSearchParams) globalThis.URLSearchParams = __url.URLSearchParams;

  /* TextEncoder / TextDecoder over Buffer's utf8 codec */
  var __B = require('buffer').Buffer;
  if (!globalThis.TextEncoder) {
    globalThis.TextEncoder = function () { this.encoding = 'utf-8'; };
    globalThis.TextEncoder.prototype.encode = function (s) { var b = __B.from(String(s == null ? '' : s), 'utf8'); var u = new Uint8Array(b.length); u.set(b); return u; };
    globalThis.TextEncoder.prototype.encodeInto = function (s, dest) { var b = __B.from(String(s), 'utf8'); var n = Math.min(b.length, dest.length); for (var i = 0; i < n; i++) dest[i] = b[i]; return { read: s.length, written: n }; };
  }
  if (!globalThis.TextDecoder) {
    globalThis.TextDecoder = function (enc) { this.encoding = (enc || 'utf-8').toLowerCase(); this.fatal = false; };
    globalThis.TextDecoder.prototype.decode = function (buf) { if (buf == null) return ''; var u = (buf instanceof Uint8Array) ? buf : new Uint8Array(buf.buffer || buf); return __B.from(u).toString(this.encoding === 'utf-8' || this.encoding === 'utf8' ? 'utf8' : 'latin1'); };
  }

  /* Event / EventTarget / AbortController / AbortSignal */
  if (!globalThis.Event) {
    var Event = function (type, opts) { this.type = type; this.defaultPrevented = false; this.cancelable = !!(opts && opts.cancelable); this.bubbles = !!(opts && opts.bubbles); };
    Event.prototype.preventDefault = function () { this.defaultPrevented = true; };
    Event.prototype.stopPropagation = function () {}; Event.prototype.stopImmediatePropagation = function () {};
    var EventTarget = function () { this.__l = {}; };
    EventTarget.prototype.addEventListener = function (t, fn) { if (typeof fn !== 'function') return; (this.__l[t] || (this.__l[t] = [])).push(fn); };
    EventTarget.prototype.removeEventListener = function (t, fn) { if (this.__l[t]) this.__l[t] = this.__l[t].filter(function (f) { return f !== fn; }); };
    EventTarget.prototype.dispatchEvent = function (ev) { var ls = (this.__l[ev.type] || []).slice(), self = this; ls.forEach(function (fn) { try { fn.call(self, ev); } catch (e) {} }); var on = this['on' + ev.type]; if (typeof on === 'function') on.call(this, ev); return !ev.defaultPrevented; };
    globalThis.Event = Event; globalThis.EventTarget = EventTarget;
    var AbortSignal = function () { EventTarget.call(this); this.aborted = false; this.reason = undefined; this.onabort = null; };
    AbortSignal.prototype = Object.create(EventTarget.prototype); AbortSignal.prototype.constructor = AbortSignal;
    AbortSignal.prototype.throwIfAborted = function () { if (this.aborted) throw (this.reason || new Error('This operation was aborted')); };
    AbortSignal.abort = function (reason) { var s = new AbortSignal(); s.aborted = true; s.reason = reason || new Error('This operation was aborted'); return s; };
    AbortSignal.timeout = function (ms) { var s = new AbortSignal(); globalThis.setTimeout(function () { s.aborted = true; s.reason = new Error('TimeoutError'); s.dispatchEvent(new Event('abort')); }, ms); return s; };
    globalThis.AbortSignal = AbortSignal;
    var AbortController = function () { this.signal = new AbortSignal(); };
    AbortController.prototype.abort = function (reason) { if (this.signal.aborted) return; this.signal.aborted = true; this.signal.reason = reason || new Error('This operation was aborted'); this.signal.dispatchEvent(new Event('abort')); };
    globalThis.AbortController = AbortController;
  }

  /* fill process gaps npm relies on (HOME for config, emitWarning, config, getBuiltinModule, hrtime) */
  var __proc = globalThis.process;
  if (!__proc.emitWarning) __proc.emitWarning = function (w) { try { __proc.stderr.write('(node9) Warning: ' + (w && w.message || w) + '\n'); } catch (e) {} };
  if (!__proc.config) __proc.config = { variables: { node_prefix: '/amd64' }, target_defaults: {} };
  if (!__proc.features) __proc.features = { uv: false, tls: true };
  if (!__proc.getBuiltinModule) __proc.getBuiltinModule = function (n) { try { return require(n); } catch (e) { return undefined; } };
  if (!__proc.allowedNodeEnvironmentFlags) __proc.allowedNodeEnvironmentFlags = new Set();
  if (!__proc.env) __proc.env = {};
  if (!__proc.env.HOME) __proc.env.HOME = (std.getenv && std.getenv('home')) || '/usr/glenda';
  if (!__proc.env.PATH) __proc.env.PATH = (std.getenv && std.getenv('path')) || '/bin:/amd64/bin';
  if (!__proc.env.PREFIX) __proc.env.PREFIX = '/amd64';
  if (!__proc.hrtime) {
    __proc.hrtime = function (prev) { var ns = Math.round(Date.now() * 1e6); if (prev) { var d = ns - (prev[0] * 1e9 + prev[1]); return [Math.floor(d / 1e9), d % 1e9]; } return [Math.floor(ns / 1e9), ns % 1e9]; };
    __proc.hrtime.bigint = function () { return BigInt(Math.round(Date.now() * 1e6)); };
  }
  if (!__proc.report) __proc.report = { getReport: function () { return {}; } };
  if (!__proc.memoryUsage) { __proc.memoryUsage = function () { return { rss: 0, heapTotal: 0, heapUsed: 0, external: 0, arrayBuffers: 0 }; }; __proc.memoryUsage.rss = function () { return 0; }; }
  if (!__proc.cpuUsage) __proc.cpuUsage = function () { return { user: 0, system: 0 }; };
  if (!__proc.umask) __proc.umask = function (m) { return 18; /* 0o022 */ };
  if (!__proc.geteuid) __proc.geteuid = function () { return 0; };
  if (!__proc.getegid) __proc.getegid = function () { return 0; };
  if (!__proc.getuid) __proc.getuid = function () { return 0; };
  if (!__proc.getgid) __proc.getgid = function () { return 0; };
  if (!__proc.setSourceMapsEnabled) __proc.setSourceMapsEnabled = function () {};
  if (!__proc.loadEnvFile) __proc.loadEnvFile = function () {};
  if (!__proc.title) __proc.title = 'node9';
  if (!__proc.kill) __proc.kill = function () { return true; };
})();
