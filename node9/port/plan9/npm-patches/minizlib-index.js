// node9 drop-in replacement for minizlib's commonjs index: minizlib relies on Node's
// internal zlib binding (._handle._processChunk), which node9 does not provide (it spun).
// Here Gunzip/Unzip are Minipass streams backed by node9's native libz inflater (gzip).
// Compression / raw-inflate / brotli / zstd are not implemented (not needed for npm install).
'use strict'
const { Minipass } = require('minipass')
const Buffer = require('buffer').Buffer
const nat = globalThis.__n9native

class ZlibError extends Error {
  constructor (err) { super('zlib: ' + (err && err.message || err)); this.code = 'ZLIB_ERROR'; this.errno = -1 }
}

class Gunzip extends Minipass {
  constructor (opts) {
    super(opts)
    this._h = nat.inflateCreate()
    if (this._h < 0) throw new ZlibError('inflateCreate failed')
    this._state = new Int32Array(2)
    this._outbuf = new Uint8Array(65536)
    this._zdone = false
  }
  _inflate (u8) {
    const parts = []
    let off = 0
    while (off < u8.length && !this._zdone) {
      const slice = off === 0 ? u8 : u8.subarray(off)
      const produced = nat.inflate(this._h, slice, this._outbuf, this._state)
      if (produced < 0) throw new ZlibError('inflate error')
      const consumed = this._state[0]
      this._zdone = this._state[1] === 1
      if (produced > 0) { const b = Buffer.alloc(produced); b.set(this._outbuf.subarray(0, produced)); parts.push(b) }
      if (consumed === 0 && produced === 0) break
      off += consumed
    }
    return parts
  }
  write (chunk, encoding, cb) {
    if (typeof encoding === 'function') { cb = encoding; encoding = undefined }
    let u8
    if (typeof chunk === 'string') u8 = Buffer.from(chunk, encoding || 'utf8')
    else if (chunk instanceof Uint8Array) u8 = chunk
    else u8 = Buffer.from(chunk)
    let ret = true
    try {
      const parts = this._inflate(u8)
      for (let i = 0; i < parts.length; i++) ret = super.write(parts[i])
    } catch (e) { this.emit('error', e); if (cb) cb(e); return false }
    if (cb) cb()
    return ret
  }
  end (chunk, encoding, cb) {
    if (typeof chunk === 'function') { cb = chunk; chunk = undefined }
    else if (typeof encoding === 'function') { cb = encoding; encoding = undefined }
    if (chunk != null) this.write(chunk, encoding)
    if (this._h >= 0) { try { nat.inflateDestroy(this._h) } catch (e) {} this._h = -1 }
    return super.end(cb)
  }
}

// gzip streams auto-detect a gzip header; node9's inflater is configured for gzip, so Unzip == Gunzip
class Unzip extends Gunzip {}

class Unsupported extends Minipass {
  constructor () { super(); throw new Error('minizlib: only Gunzip/Unzip are implemented on node9') }
}

let _zc = {}
try { _zc = require('zlib').constants || {} } catch (e) {}

module.exports = {
  constants: _zc,
  ZlibError,
  Zlib: Gunzip,
  Gzip: Unsupported, Deflate: Unsupported, Inflate: Unsupported,
  DeflateRaw: Unsupported, InflateRaw: Unsupported,
  Gunzip, Unzip,
  BrotliCompress: Unsupported, BrotliDecompress: Unsupported,
  ZstdCompress: Unsupported, ZstdDecompress: Unsupported,
}
