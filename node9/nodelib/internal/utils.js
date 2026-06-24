/* node9: stream type checkers (stubs — node9 streams are minimal). */
var no=function(){return false;};
module.exports = { isReadable:no, isWritable:no, isStream:no, isDuplexStream:no, isReadableStream:no, isWritableStream:no, isNodeStream:no, isIterable:function(o){return o!=null&&typeof o[Symbol.iterator]==='function';}, isDestroyed:no, isDisturbed:no, isErrored:no };
