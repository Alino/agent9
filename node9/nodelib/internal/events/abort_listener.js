/* node9: AbortSignal listener helper — stubbed (no AbortController integration yet). */
function addAbortListener(signal, listener) {
  if (signal && typeof signal.addEventListener === 'function') { signal.addEventListener('abort', listener, { once: true }); return { __proto__: null, [Symbol.dispose]() { signal.removeEventListener('abort', listener); } }; }
  return { __proto__: null, [Symbol.dispose]() {} };
}
module.exports = { addAbortListener };
