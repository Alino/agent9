/* node9: minimal subset of Node's error system — enough for the modules we adapt.
   Produces TypeErrors/Errors carrying the matching .code, with Node-style messages. */
function makeNodeError(Base, code, msgFn) {
  return function () {
    var msg = msgFn.apply(null, arguments);
    var e = new Base(msg);
    e.code = code;
    e.name = Base.name + ' [' + code + ']';
    e.toString = function () { return this.name + ': ' + msg; };
    return e;
  };
}
function typeName(v) {
  if (v === null) return 'null';
  if (Array.isArray(v)) return 'Array';
  return typeof v;
}
var codes = {
  ERR_INVALID_ARG_TYPE: makeNodeError(TypeError, 'ERR_INVALID_ARG_TYPE', function (name, expected, actual) {
    if (!Array.isArray(expected)) expected = [expected];
    return 'The "' + name + '" argument must be of type ' + expected.join(' or ') +
      '. Received ' + (typeof actual === 'string' ? "type string ('" + actual + "')" : 'type ' + typeName(actual));
  }),
  ERR_INVALID_ARG_VALUE: makeNodeError(TypeError, 'ERR_INVALID_ARG_VALUE', function (name, value, reason) {
    return 'The argument \'' + name + '\' ' + (reason || 'is invalid') + '. Received ' + String(value);
  }),
  ERR_OUT_OF_RANGE: makeNodeError(RangeError, 'ERR_OUT_OF_RANGE', function (name, range, value) {
    return 'The value of "' + name + '" is out of range. It must be ' + range + '. Received ' + value;
  }),
  ERR_MISSING_ARGS: makeNodeError(TypeError, 'ERR_MISSING_ARGS', function () {
    return 'The "' + Array.prototype.join.call(arguments, '", "') + '" argument must be specified';
  }),
  ERR_UNHANDLED_ERROR: makeNodeError(Error, 'ERR_UNHANDLED_ERROR', function (err) {
    return 'Unhandled error.' + (err === undefined ? '' : ' (' + err + ')');
  }),
};
module.exports = { codes: codes };

/* node9 additions for os/fs adaptation */
function hideStackFrames(fn) { return fn; }
function ERR_SYSTEM_ERROR(ctx) { var e = new Error('A system error occurred'); e.code = 'ERR_SYSTEM_ERROR'; e.info = ctx; return e; }
ERR_SYSTEM_ERROR.HideStackFramesError = ERR_SYSTEM_ERROR;
module.exports.hideStackFrames = hideStackFrames;
module.exports.ERR_SYSTEM_ERROR = ERR_SYSTEM_ERROR;
Object.keys(codes).forEach(function (k) { module.exports[k] = codes[k]; });

module.exports.ERR_FALSY_VALUE_REJECTION = (function(){ var f=function(reason){ var e=new Error('Promise was rejected with a falsy value'); e.code='ERR_FALSY_VALUE_REJECTION'; e.reason=reason; return e; }; return f; })();
codes.ERR_FALSY_VALUE_REJECTION = module.exports.ERR_FALSY_VALUE_REJECTION;

['ERR_AMBIGUOUS_ARGUMENT','ERR_CONSTRUCT_CALL_REQUIRED','ERR_INVALID_RETURN_VALUE'].forEach(function(c){
  var f = function(){ var e=new TypeError(c+': '+Array.prototype.join.call(arguments,' ')); e.code=c; return e; };
  codes[c]=f; module.exports[c]=f;
});
