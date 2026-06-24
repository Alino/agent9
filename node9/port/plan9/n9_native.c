/* node9 native bindings — quickjs marshalling layer. Exposes crypto/zlib/tls
   (implemented in n9_sec.c over libsec/libz) to JS as globalThis.__n9native.
   Bytes cross as typed arrays (no per-call string copies). */
#include "quickjs.h"
#include "node9_native.h"
#include <stddef.h>

/* pointer to the bytes of a TypedArray arg (offset-adjusted); *plen = byte length */
static unsigned char *n9_u8(JSContext *ctx, JSValueConst v, size_t *plen) {
    size_t off, len, bpe, absz;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    unsigned char *base;
    if (JS_IsException(ab)) return 0;
    base = JS_GetArrayBuffer(ctx, &absz, ab);
    JS_FreeValue(ctx, ab);
    if (!base) return 0;
    *plen = len;
    return base + off;
}

static JSValue js_hashCreate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int algo = 0; JS_ToInt32(ctx, &algo, argv[0]);
    return JS_NewInt32(ctx, n9_hash_create(algo));
}
static JSValue js_hashUpdate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int h = 0; size_t len = 0; unsigned char *p;
    JS_ToInt32(ctx, &h, argv[0]);
    p = n9_u8(ctx, argv[1], &len);
    if (!p) return JS_ThrowTypeError(ctx, "hashUpdate: expected typed array");
    return JS_NewInt32(ctx, n9_hash_update(h, p, len));
}
static JSValue js_hashDigest(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int h = 0; size_t len = 0; unsigned char *p;
    JS_ToInt32(ctx, &h, argv[0]);
    p = n9_u8(ctx, argv[1], &len);
    if (!p) return JS_ThrowTypeError(ctx, "hashDigest: expected typed array");
    return JS_NewInt32(ctx, n9_hash_digest(h, p));
}
static JSValue js_hashDlen(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int algo = 0; JS_ToInt32(ctx, &algo, argv[0]);
    return JS_NewInt32(ctx, n9_hash_dlen(algo));
}
static JSValue js_hmac(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int algo = 0; size_t kl = 0, dl = 0, ol = 0; unsigned char *k, *d, *o;
    JS_ToInt32(ctx, &algo, argv[0]);
    k = n9_u8(ctx, argv[1], &kl); d = n9_u8(ctx, argv[2], &dl); o = n9_u8(ctx, argv[3], &ol);
    if (!k || !d || !o) return JS_ThrowTypeError(ctx, "hmac: expected typed arrays");
    return JS_NewInt32(ctx, n9_hmac(algo, k, kl, d, dl, o));
}
static JSValue js_randomBytes(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    size_t len = 0; unsigned char *p = n9_u8(ctx, argv[0], &len);
    if (!p) return JS_ThrowTypeError(ctx, "randomBytes: expected typed array");
    n9_random_bytes(p, (int)len);
    return JS_UNDEFINED;
}
static JSValue js_inflateCreate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    return JS_NewInt32(ctx, n9_inflate_create());
}
/* inflate(h, inU8, outU8, stateI32[2]) -> produced; state[0]=consumed, state[1]=done */
static JSValue js_inflate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int h = 0, done = 0, produced; size_t inlen = 0, outlen = 0, slen = 0, consumed = 0;
    unsigned char *in, *out; int *st;
    JS_ToInt32(ctx, &h, argv[0]);
    in = n9_u8(ctx, argv[1], &inlen);
    out = n9_u8(ctx, argv[2], &outlen);
    st = (int *)n9_u8(ctx, argv[3], &slen);
    if (!in || !out || !st || slen < 8) return JS_ThrowTypeError(ctx, "inflate: bad args");
    produced = n9_inflate(h, in, inlen, &consumed, out, outlen, &done);
    st[0] = (int)consumed; st[1] = done;
    return JS_NewInt32(ctx, produced);
}
static JSValue js_inflateDestroy(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int h = 0; JS_ToInt32(ctx, &h, argv[0]); n9_inflate_destroy(h); return JS_UNDEFINED;
}
static JSValue js_tlsClient(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    int fd = 0; const char *sni; JSValue r;
    JS_ToInt32(ctx, &fd, argv[0]);
    sni = JS_ToCString(ctx, argv[1]);
    r = JS_NewInt32(ctx, n9_tls_client(fd, sni ? sni : ""));
    if (sni) JS_FreeCString(ctx, sni);
    return r;
}

void n9_native_init(JSContext *ctx) {
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue o = JS_NewObject(ctx);
#define FN(name, fn, n) JS_SetPropertyStr(ctx, o, name, JS_NewCFunction(ctx, fn, name, n))
    FN("hashCreate", js_hashCreate, 1);
    FN("hashUpdate", js_hashUpdate, 2);
    FN("hashDigest", js_hashDigest, 2);
    FN("hashDlen", js_hashDlen, 1);
    FN("hmac", js_hmac, 4);
    FN("randomBytes", js_randomBytes, 1);
    FN("inflateCreate", js_inflateCreate, 0);
    FN("inflate", js_inflate, 4);
    FN("inflateDestroy", js_inflateDestroy, 1);
    FN("tlsClient", js_tlsClient, 2);
#undef FN
    JS_SetPropertyStr(ctx, g, "__n9native", o);
    JS_FreeValue(ctx, g);
}
