/* node9-original: functional inspect + format for adapted Node modules. */
function inspect(v, opts) {
  var depth = (opts && typeof opts === 'object' && opts.depth != null) ? opts.depth : 2;
  var seen = [];
  function go(x, d) {
    if (x === null) return 'null';
    var t = typeof x;
    if (t === 'string') return d < 0 ? "'...'" : "'" + x.replace(/\n/g, '\\n') + "'";
    if (t === 'number' || t === 'boolean' || t === 'undefined') return String(x);
    if (t === 'bigint') return String(x) + 'n';
    if (t === 'function') return '[Function' + (x.name ? ': ' + x.name : ' (anonymous)') + ']';
    if (t === 'symbol') return x.toString();
    if (seen.indexOf(x) >= 0) return '[Circular *1]';
    if (d < 0) return Array.isArray(x) ? '[Array]' : '[Object]';
    seen.push(x); var r;
    if (Array.isArray(x)) r = '[ ' + x.map(function (e) { return go(e, d - 1); }).join(', ') + ' ]';
    else if (x instanceof Error) r = x.stack || (x.name + ': ' + x.message);
    else if (x instanceof Date) r = x.toISOString();
    else if (x instanceof RegExp) r = x.toString();
    else { var k = Object.keys(x); r = k.length ? '{ ' + k.map(function (kk) { return (/^[A-Za-z_$][\w$]*$/.test(kk) ? kk : JSON.stringify(kk)) + ': ' + go(x[kk], d - 1); }).join(', ') + ' }' : '{}'; }
    seen.pop(); return r;
  }
  if (typeof v === 'string' && (!opts || typeof opts !== 'object')) return v;
  return go(v, depth);
}
inspect.custom = Symbol.for('nodejs.util.inspect.custom');
inspect.colors = {}; inspect.styles = {}; inspect.defaultOptions = { depth: 2 };
function formatWithOptions(opts, ...args) {
  var i = 0, out = [];
  if (typeof args[0] === 'string') {
    i = 1;
    out.push(args[0].replace(/%[sdifjoOc%]/g, function (sp) {
      if (sp === '%%') return '%';
      if (i >= args.length) return sp;
      var a = args[i++];
      switch (sp) {
        case '%s': return typeof a === 'string' ? a : (typeof a === 'bigint' ? a + 'n' : (a === null || typeof a !== 'object' ? String(a) : inspect(a, { depth: 0 })));
        case '%d': case '%i': return typeof a === 'bigint' ? a + 'n' : String(parseInt(a, 10));
        case '%f': return String(parseFloat(a));
        case '%j': try { return JSON.stringify(a); } catch (e) { return '[Circular]'; }
        case '%o': case '%O': return inspect(a, opts);
        case '%c': return '';
        default: return sp;
      }
    }));
  }
  for (; i < args.length; i++) out.push(typeof args[i] === 'string' ? args[i] : inspect(args[i], opts));
  return out.join(' ');
}
function format() { return formatWithOptions.apply(null, [{}].concat(Array.prototype.slice.call(arguments))); }
function stripVTControlCharacters(s) { return String(s).replace(/\[[0-9;]*m/g, ''); }
function identicalSequenceRange() { return { len: 0, offset: 0 }; }
module.exports = { inspect, format, formatWithOptions, stripVTControlCharacters, identicalSequenceRange };
