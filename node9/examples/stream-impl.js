// node9-original real stream module — Readable/Writable/Duplex/Transform with backpressure,
// honoring Node's documented contract (push()->bool, write()->bool, 'drain'/'end'/'finish',
// pipe, pipeline, Readable.from, async iteration) for minipass/pacote interop.
// Exposed below as makeStream(EventEmitter, nextTick) so it can be tested standalone.
function makeStream(EventEmitter, nextTick) {
  function inherits(C, P) { C.prototype = Object.create(P.prototype); C.prototype.constructor = C; C.super_ = P; }

  /* ---------- Readable ---------- */
  function Readable(opts) {
    EventEmitter.call(this);
    opts = opts || {};
    this._rs = {
      buffer: [], length: 0, flowing: null, ended: false, endEmitted: false,
      reading: false, objectMode: !!opts.objectMode,
      highWaterMark: opts.highWaterMark != null ? opts.highWaterMark : (opts.objectMode ? 16 : 16384),
      destroyed: false, errored: null, resumeScheduled: false, readableListening: false,
    };
    if (opts.read) this._read = opts.read;
  }
  inherits(Readable, EventEmitter);
  Readable.prototype._read = function () {};
  Readable.prototype.push = function (chunk) {
    var s = this._rs;
    if (chunk === null) { s.ended = true; emitReadable(this); return false; }
    if (s.destroyed) return false;
    var size = s.objectMode ? 1 : (chunk.length || 0);
    s.buffer.push(chunk); s.length += size;
    if (s.flowing) drainFlowing(this);
    else this.emit('readable');
    return s.length < s.highWaterMark;
  };
  function emitReadable(self) {
    var s = self._rs;
    if (s.flowing) drainFlowing(self);
    else nextTick(function () { maybeEnd(self); });
  }
  function drainFlowing(self) {
    var s = self._rs;
    if (s.resumeScheduled) return;
    s.resumeScheduled = true;
    nextTick(function () {
      s.resumeScheduled = false;
      while (s.flowing && s.buffer.length) {
        var c = s.buffer.shift(); s.length -= s.objectMode ? 1 : (c.length || 0);
        self.emit('data', c);
      }
      if (s.flowing && s.length < s.highWaterMark && !s.ended) self._read(s.highWaterMark);
      maybeEnd(self);
    });
  }
  function maybeEnd(self) {
    var s = self._rs;
    if (s.ended && !s.endEmitted && s.buffer.length === 0) { s.endEmitted = true; self.emit('end'); }
  }
  Readable.prototype.read = function (n) {
    var s = this._rs;
    if (s.buffer.length === 0) { if (!s.ended && !s.reading) { s.reading = true; this._read(s.highWaterMark); s.reading = false; } maybeEnd(this); return null; }
    var c = s.buffer.shift(); s.length -= s.objectMode ? 1 : (c.length || 0);
    if (s.length < s.highWaterMark && !s.ended) { s.reading = true; this._read(s.highWaterMark); s.reading = false; }
    maybeEnd(this);
    return c;
  };
  Readable.prototype.resume = function () { var s = this._rs; if (s.flowing !== true) { s.flowing = true; drainFlowing(this); } return this; };
  Readable.prototype.pause = function () { this._rs.flowing = false; return this; };
  Readable.prototype.on = Readable.prototype.addListener = function (ev, fn) {
    EventEmitter.prototype.on.call(this, ev, fn);
    if (ev === 'data') { if (this._rs.flowing !== false) this.resume(); }
    if (ev === 'readable') this._rs.readableListening = true;
    return this;
  };
  Readable.prototype.pipe = function (dest, opts) {
    var self = this; opts = opts || {};
    function onData(chunk) { var ok = dest.write(chunk); if (ok === false) self.pause(); }
    function onDrain() { self.resume(); }
    function onEnd() { if (opts.end !== false) dest.end(); }
    self.on('data', onData);
    dest.on('drain', onDrain);
    self.on('end', onEnd);
    self.on('error', function (e) { if (dest.emit) dest.emit('error', e); });
    dest.emit && dest.emit('pipe', self);
    self.resume();
    return dest;
  };
  Readable.prototype.destroy = function (err) { var s = this._rs; if (s.destroyed) return this; s.destroyed = true; if (err) this.emit('error', err); this.emit('close'); return this; };
  Readable.prototype.setEncoding = function (enc) { this._rs.encoding = enc; return this; };
  Readable.prototype[Symbol.asyncIterator] = function () {
    var self = this, done = false, errored = null, pending = [], waiting = null;
    self.on('data', function (c) { if (waiting) { var w = waiting; waiting = null; w({ value: c, done: false }); } else pending.push(c); });
    self.on('end', function () { done = true; if (waiting) { var w = waiting; waiting = null; w({ value: undefined, done: true }); } });
    self.on('error', function (e) { errored = e; if (waiting) { var w = waiting; waiting = null; w(Promise.reject(e)); } });
    return { next: function () { return new Promise(function (res, rej) { if (errored) return rej(errored); if (pending.length) return res({ value: pending.shift(), done: false }); if (done) return res({ value: undefined, done: true }); waiting = res; }); }, [Symbol.asyncIterator]: function () { return this; } };
  };
  Readable.from = function (iterable, opts) {
    var r = new Readable(Object.assign({ objectMode: true }, opts));
    var it = iterable[Symbol.asyncIterator] ? iterable[Symbol.asyncIterator]() : iterable[Symbol.iterator]();
    var reading = false;
    r._read = function () {
      if (reading) return; reading = true;
      Promise.resolve(it.next()).then(function (res) {
        reading = false;
        if (res.done) r.push(null);
        else if (r.push(res.value)) r._read();
      }, function (e) { r.destroy(e); });
    };
    return r;
  };

  /* ---------- Writable ---------- */
  function Writable(opts) {
    EventEmitter.call(this);
    opts = opts || {};
    this._ws = {
      buffer: [], length: 0, writing: false, ended: false, finished: false, needDrain: false,
      objectMode: !!opts.objectMode, destroyed: false,
      highWaterMark: opts.highWaterMark != null ? opts.highWaterMark : (opts.objectMode ? 16 : 16384),
    };
    if (opts.write) this._write = opts.write;
    if (opts.final) this._final = opts.final;
  }
  inherits(Writable, EventEmitter);
  Writable.prototype._write = function (chunk, enc, cb) { cb(); };
  Writable.prototype.write = function (chunk, enc, cb) {
    var s = this._ws, self = this;
    if (typeof enc === 'function') { cb = enc; enc = null; }
    if (s.ended) { if (cb) cb(new Error('write after end')); return false; }
    var size = s.objectMode ? 1 : (chunk ? chunk.length : 0);
    s.length += size;
    var ret = s.length < s.highWaterMark;
    if (!ret) s.needDrain = true;
    if (s.writing) s.buffer.push({ chunk: chunk, enc: enc, cb: cb });
    else doWrite(self, chunk, enc, cb, size);
    return ret;
  };
  function doWrite(self, chunk, enc, cb, size) {
    var s = self._ws; s.writing = true;
    self._write(chunk, enc, function (err) {
      s.writing = false; s.length -= size;
      if (cb) cb(err);
      if (err) { self.emit('error', err); return; }
      if (s.buffer.length) { var n = s.buffer.shift(); doWrite(self, n.chunk, n.enc, n.cb, s.objectMode ? 1 : (n.chunk ? n.chunk.length : 0)); }
      else { if (s.needDrain) { s.needDrain = false; self.emit('drain'); } if (s.ended) finishWrite(self); }
    });
  }
  function finishWrite(self) {
    var s = self._ws; if (s.finished || s.writing || s.buffer.length) return;
    var done = function () { s.finished = true; self.emit('finish'); };
    if (self._final) self._final(done); else done();
  }
  Writable.prototype.end = function (chunk, enc, cb) {
    var s = this._ws;
    if (typeof chunk === 'function') { cb = chunk; chunk = null; }
    else if (typeof enc === 'function') { cb = enc; enc = null; }
    if (chunk != null) this.write(chunk, enc);
    s.ended = true;
    if (cb) this.on('finish', cb);
    if (!s.writing && s.buffer.length === 0) finishWrite(this);
    return this;
  };
  Writable.prototype.destroy = function (err) { var s = this._ws; if (s.destroyed) return this; s.destroyed = true; if (err) this.emit('error', err); this.emit('close'); return this; };
  Writable.prototype.cork = function () {}; Writable.prototype.uncork = function () {};
  Writable.prototype.setDefaultEncoding = function () { return this; };

  /* ---------- Duplex / Transform / PassThrough ---------- */
  function Duplex(opts) { Readable.call(this, opts); Writable.call(this, opts); if (opts && opts.read) this._read = opts.read; if (opts && opts.write) this._write = opts.write; }
  inherits(Duplex, Readable);
  Object.getOwnPropertyNames(Writable.prototype).forEach(function (k) { if (k !== 'constructor' && !Duplex.prototype[k]) Duplex.prototype[k] = Writable.prototype[k]; });
  Duplex.prototype.write = Writable.prototype.write;
  Duplex.prototype.end = Writable.prototype.end;

  function Transform(opts) { Duplex.call(this, opts); if (opts && opts.transform) this._transform = opts.transform; if (opts && opts.flush) this._flush = opts.flush; }
  inherits(Transform, Duplex);
  Transform.prototype._transform = function (chunk, enc, cb) { cb(null, chunk); };
  Transform.prototype._write = function (chunk, enc, cb) {
    var self = this;
    this._transform(chunk, enc, function (err, data) { if (data != null) self.push(data); cb(err); });
  };
  Transform.prototype._final = function (cb) {
    var self = this;
    if (this._flush) this._flush(function (err, data) { if (data != null) self.push(data); self.push(null); cb(err); });
    else { this.push(null); cb(); }
  };

  function PassThrough(opts) { Transform.call(this, opts); }
  inherits(PassThrough, Transform);

  /* ---------- pipeline / finished ---------- */
  function finished(stream, cb) {
    var called = false; function done(err) { if (called) return; called = true; cb(err); }
    stream.on('end', function () { done(); }); stream.on('finish', function () { done(); });
    stream.on('error', function (e) { done(e); }); stream.on('close', function () { done(); });
  }
  function pipeline() {
    var args = Array.prototype.slice.call(arguments);
    var cb = typeof args[args.length - 1] === 'function' ? args.pop() : function () {};
    var streams = args, last = streams[streams.length - 1];
    for (var i = 0; i < streams.length - 1; i++) streams[i].pipe(streams[i + 1]);
    finished(last, cb);
    return last;
  }

  return { Readable: Readable, Writable: Writable, Duplex: Duplex, Transform: Transform, PassThrough: PassThrough, Stream: Readable, pipeline: pipeline, finished: finished };
}
if (typeof module !== 'undefined') module.exports = { makeStream: makeStream };
globalThis.__makeStream = makeStream;
