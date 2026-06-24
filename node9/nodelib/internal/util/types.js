/* node9: util.types checkers. */
module.exports = {
  isDate:v=>v instanceof Date, isRegExp:v=>v instanceof RegExp, isNativeError:v=>v instanceof Error,
  isPromise:v=>v instanceof Promise, isMap:v=>v instanceof Map, isSet:v=>v instanceof Set,
  isArrayBuffer:v=>v instanceof ArrayBuffer, isTypedArray:v=>ArrayBuffer.isView(v)&&!(v instanceof DataView),
  isUint8Array:v=>v instanceof Uint8Array, isAnyArrayBuffer:v=>v instanceof ArrayBuffer,
  isProxy:()=>false, isModuleNamespaceObject:()=>false, isBoxedPrimitive:()=>false,
};
