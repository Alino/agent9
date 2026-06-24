/* node9: subset of internal/util used by adapted Node modules. */
function getLazy(fn){ var c=false,v; return function(){ if(!c){v=fn();c=true;} return v; }; }
const kEmptyObject = Object.freeze(Object.create(null));
function spliceOne(list,index){ for(;index+1<list.length;index++)list[index]=list[index+1]; list.pop(); }
function promisify(fn){ return function(){ var a=Array.prototype.slice.call(arguments),self=this; return new Promise(function(res,rej){ a.push(function(e,v){ if(e)rej(e); else res(v); }); fn.apply(self,a); }); }; }
promisify.custom = Symbol.for('nodejs.util.promisify.custom');
function deprecate(fn){ return fn; }
function getSystemErrorName(){ return 'Unknown system error'; }
function getSystemErrorMessage(){ return 'unknown error'; }
function getSystemErrorMap(){ return new Map(); }
function convertProcessSignalToExitCode(){ return 1; }
function defineLazyProperties(target, id, keys){ keys.forEach(function(k){ Object.defineProperty(target,k,{configurable:true,enumerable:true,get:function(){ return require(id)[k]; }}); }); }
function getCIDR(){ return null; }
module.exports = { getLazy, kEmptyObject, spliceOne, promisify, deprecate, getSystemErrorName, getSystemErrorMessage, getSystemErrorMap, convertProcessSignalToExitCode, defineLazyProperties, getCIDR };

module.exports.setOwnProperty = function(obj, key, value){ return Object.defineProperty(obj, key, { __proto__:null, value:value, writable:true, enumerable:true, configurable:true }); };
module.exports.SideEffectFreeRegExpPrototypeExec = function(re, s){ return re.exec(s); };
module.exports.emitExperimentalWarning = function(){};
module.exports.kEnumerableProperty = { __proto__:null, enumerable:true, configurable:true };

module.exports.isError = function(v){ return v instanceof Error || Object.prototype.toString.call(v)==='[object Error]'; };

module.exports.isErrorStackTraceLimitWritable = function(){ try { var d=Object.getOwnPropertyDescriptor(Error,'stackTraceLimit'); if(d===undefined)return Object.isExtensible(Error); return d.writable!==false || (d.set!==undefined); } catch(e){ return false; } };
module.exports.overrideStackTrace = new (globalThis.WeakMap||function(){this.set=function(){};this.has=function(){return false;};})();
