/* node9 native bindings — portable C ABI between the quickjs marshalling layer
   (n9_native.c) and the Plan 9 libsec/libz layer (n9_sec.c). No quickjs or <u.h>
   types cross this boundary, so the two TUs never collide on type definitions. */
#ifndef NODE9_NATIVE_H
#define NODE9_NATIVE_H
#include <stddef.h>

/* crypto (libsec). algo: 0=sha256 1=sha512 2=sha1 3=md5 */
int  n9_hash_dlen(int algo);                 /* digest length in bytes, or -1 */
int  n9_hash_create(int algo);               /* -> handle (>=0) or -1 */
int  n9_hash_update(int h, const unsigned char *p, size_t n);  /* 0 ok, -1 bad handle */
int  n9_hash_digest(int h, unsigned char *out); /* writes n9_hash_dlen bytes, frees handle; ret len or -1 */
int  n9_hmac(int algo, const unsigned char *key, size_t klen,
             const unsigned char *data, size_t dlen, unsigned char *out); /* ret digest len or -1 */
void n9_random_bytes(unsigned char *out, int n);   /* libsec genrandom (CSPRNG) */

/* zlib inflate, gzip-aware (libz) */
int  n9_inflate_create(void);                /* -> handle or -1 */
/* feed in[0..inlen), write to out[0..outcap); sets *consumed and *done(=1 at stream end);
   returns bytes produced, or -1 on error */
int  n9_inflate(int h, const unsigned char *in, size_t inlen, size_t *consumed,
                unsigned char *out, size_t outcap, int *done);
void n9_inflate_destroy(int h);

/* TLS client (libsec tlsClient + CA verify). tcpfd is an open /net/tcp data fd.
   Returns a new fd carrying the encrypted stream (usable by os.read/write/setReadHandler),
   or <0 on handshake/verify failure. */
int  n9_tls_client(int tcpfd, const char *servername);

#endif
