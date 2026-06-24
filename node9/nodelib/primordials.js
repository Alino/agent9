/* node9 primordials shim — provides the intrinsic wrappers Node's lib/*.js expect.
   Original implementation for node9 (not adapted from Node); generates the standard
   `XPrototypeMethod` (uncurried) and `XStaticMethod` names from the live intrinsics. */
(function (g) {
  var p = Object.create(null);
  var call = Function.prototype.call;
  var bind = Function.prototype.bind;
  var uncurryThis = bind.bind(call); // uncurryThis(fn)(self, ...args) === fn.apply(self, args)
  p.uncurryThis = uncurryThis;
  function cap(s) { return s.charAt(0).toUpperCase() + s.slice(1); }

  var ctors = ['Object', 'Array', 'Function', 'Boolean', 'Number', 'String', 'Symbol',
    'BigInt', 'Math', 'JSON', 'Reflect', 'Date', 'RegExp', 'Error', 'TypeError',
    'RangeError', 'SyntaxError', 'ReferenceError', 'EvalError', 'URIError',
    'Promise', 'Map', 'Set', 'WeakMap', 'WeakSet', 'ArrayBuffer', 'SharedArrayBuffer',
    'DataView', 'Uint8Array', 'Int8Array', 'Uint16Array', 'Int16Array', 'Uint32Array',
    'Int32Array', 'Float32Array', 'Float64Array', 'Uint8ClampedArray', 'Proxy'];

  ctors.forEach(function (name) {
    var C = g[name];
    if (!C) return;
    p[name] = C;
    // static methods/props:  Object.keys -> ObjectKeys ; Number.MAX_VALUE -> NumberMAX_VALUE
    Object.getOwnPropertyNames(C).forEach(function (k) {
      if (k === 'prototype' || k === 'length' || k === 'name') return;
      var v;
      try { v = C[k]; } catch (e) { return; }
      if (typeof v === 'function') p[name + cap(k)] = v.bind(C);
      else p[name + (/^[A-Z]/.test(k) ? k : cap(k))] = v;
    });
    // prototype methods (uncurried):  String.prototype.slice -> StringPrototypeSlice(str, a, b)
    if (C.prototype) Object.getOwnPropertyNames(C.prototype).forEach(function (k) {
      if (k === 'constructor') return;
      var d;
      try { d = Object.getOwnPropertyDescriptor(C.prototype, k); } catch (e) { return; }
      if (d && typeof d.value === 'function') p[name + 'Prototype' + cap(k)] = uncurryThis(d.value);
      else if (d && typeof d.get === 'function') p[name + 'Prototype' + cap(k) + 'Get'] = uncurryThis(d.get);
    });
  });

  // Frequently-referenced explicit primordials
  p.globalThis = g;
  p.Symbol = g.Symbol;
  p.SymbolIterator = g.Symbol ? g.Symbol.iterator : undefined;
  p.SymbolFor = g.Symbol ? g.Symbol.for : undefined;
  p.SymbolAsyncIterator = g.Symbol ? g.Symbol.asyncIterator : undefined;
  p.SymbolToStringTag = g.Symbol ? g.Symbol.toStringTag : undefined;
  p.PromiseResolve = g.Promise.resolve.bind(g.Promise);
  p.PromiseReject = g.Promise.reject.bind(g.Promise);
  p.PromiseAll = g.Promise.all.bind(g.Promise);
  p.PromisePrototypeThen = uncurryThis(g.Promise.prototype.then);
  p.PromisePrototypeCatch = uncurryThis(g.Promise.prototype.catch);
  p.ReflectApply = g.Reflect.apply;
  p.ReflectOwnKeys = g.Reflect.ownKeys;
  // "Safe" collections — node uses these to avoid prototype tampering; plain ones are fine here
  p.SafeMap = g.Map; p.SafeSet = g.Set; p.SafeWeakMap = g.WeakMap; p.SafeWeakSet = g.WeakSet;
  p.SafePromise = g.Promise; p.SafeArrayIterator = function (a) { return a[Symbol.iterator](); };
  p.SafeStringIterator = function (s) { return s[Symbol.iterator](); };
  p.PromisePrototypeFinally = g.Promise.prototype.finally ? uncurryThis(g.Promise.prototype.finally) : undefined;
  p.NumberIsNaN = Number.isNaN; p.NumberIsInteger = Number.isInteger;
  p.MathFloor = Math.floor; p.MathMax = Math.max; p.MathMin = Math.min; p.MathAbs = Math.abs;

  if (typeof module !== 'undefined') module.exports = p;
  g.primordials = p;
})(globalThis);
