/* node9: validators used by adapted Node modules. Behavior matches Node. */
const { codes } = require('internal/errors');
const { ERR_INVALID_ARG_TYPE, ERR_OUT_OF_RANGE } = codes;
function validateString(v, n) { if (typeof v !== 'string') throw ERR_INVALID_ARG_TYPE(n, 'string', v); }
function validateNumber(v, n) { if (typeof v !== 'number') throw ERR_INVALID_ARG_TYPE(n, 'number', v); }
function validateBoolean(v, n) { if (typeof v !== 'boolean') throw ERR_INVALID_ARG_TYPE(n, 'boolean', v); }
function validateFunction(v, n) { if (typeof v !== 'function') throw ERR_INVALID_ARG_TYPE(n, 'Function', v); }
function validateObject(v, n) { if (v === null || typeof v !== 'object' || Array.isArray(v)) throw ERR_INVALID_ARG_TYPE(n, 'Object', v); }
function validateInteger(v, n, min, max) {
  if (typeof v !== 'number') throw ERR_INVALID_ARG_TYPE(n, 'number', v);
  if (!Number.isInteger(v)) throw ERR_OUT_OF_RANGE(n, 'an integer', v);
  if (min != null && v < min || max != null && v > max) throw ERR_OUT_OF_RANGE(n, '>= ' + min + ' && <= ' + max, v);
}
function validateAbortSignal(v, n) { if (v !== undefined && (v === null || typeof v !== 'object' || !('aborted' in v))) throw ERR_INVALID_ARG_TYPE(n, 'AbortSignal', v); }
module.exports = { validateString, validateNumber, validateBoolean, validateFunction, validateObject, validateInteger, validateAbortSignal };

module.exports.validateInt32 = function (v, n, min, max) {
  min = min == null ? -2147483648 : min; max = max == null ? 2147483647 : max;
  if (typeof v !== 'number') throw codes.ERR_INVALID_ARG_TYPE(n, 'number', v);
  if (!Number.isInteger(v) || v < min || v > max) throw codes.ERR_OUT_OF_RANGE(n, '>= ' + min + ' && <= ' + max, v);
};

module.exports.validateOneOf = function(v,n,list){ if(list.indexOf(v)<0) throw codes.ERR_INVALID_ARG_VALUE(n,v,'must be one of: '+list.join(', ')); };
module.exports.validateStream = function(v,n){ if(v===null||typeof v!=='object') throw codes.ERR_INVALID_ARG_TYPE(n,'Stream',v); };
module.exports.validateArray = function(v,n){ if(!Array.isArray(v)) throw codes.ERR_INVALID_ARG_TYPE(n,'Array',v); };
