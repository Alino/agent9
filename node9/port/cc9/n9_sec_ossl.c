/* node9 native crypto/zlib/TLS for the cc9 build — OpenSSL (ssl9) + zlib.
 *
 * Same ABI as port/plan9/n9_sec.c (node9_native.h) so the marshalling layer and
 * boot.js are shared, with one unavoidable difference: TLS.
 *
 * Plan 9's libsec hands back a NEW FD carrying the cleartext stream, so the JS
 * side can treat a TLS socket exactly like a TCP one. OpenSSL has no such fd —
 * the cleartext only exists inside SSL_read/SSL_write. So this backend exposes
 * handles (n9_tls_read/write/close/pending/rawfd, declared in n9_tls.h) and
 * advertises `__n9native.tlsHandles`; boot.js picks the mode at runtime and both
 * builds keep working from one boot.js. Readability is still polled on the raw
 * TCP fd, which is what a TLS record arrives on.
 *
 * Certificates ARE verified here, unlike the libsec path (Plan 9's okCertificate
 * is thumbprint pinning and cannot validate a public-CA chain). The CA bundle is
 * $NODE9_CA_FILE, else /amd64/lib/node9/ca.pem. NODE9_TLS_INSECURE=1 skips
 * verification; missing bundle also degrades to unverified, loudly on stderr,
 * because failing every HTTPS request on a fresh install would be worse.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <zlib.h>

#include "node9_native.h"
#include "n9_tls.h"

/* ---------- hashes ---------- */

static const EVP_MD *md_for(int algo)
{
    switch (algo) {
    case 0: return EVP_sha256();
    case 1: return EVP_sha512();
    case 2: return EVP_sha1();
    case 3: return EVP_md5();
    default: return NULL;
    }
}

int n9_hash_dlen(int algo)
{
    const EVP_MD *md = md_for(algo);
    return md ? EVP_MD_size(md) : -1;
}

#define NH 64
static EVP_MD_CTX *htab[NH];
static int halgo[NH];

int n9_hash_create(int algo)
{
    const EVP_MD *md = md_for(algo);
    int i;
    if (!md) return -1;
    for (i = 0; i < NH; i++) {
        if (htab[i] == NULL) {
            EVP_MD_CTX *c = EVP_MD_CTX_new();
            if (!c) return -1;
            if (EVP_DigestInit_ex(c, md, NULL) != 1) { EVP_MD_CTX_free(c); return -1; }
            htab[i] = c;
            halgo[i] = algo;
            return i;
        }
    }
    return -1;
}

int n9_hash_update(int h, const unsigned char *p, size_t n)
{
    if (h < 0 || h >= NH || !htab[h]) return -1;
    return EVP_DigestUpdate(htab[h], p, n) == 1 ? 0 : -1;
}

int n9_hash_digest(int h, unsigned char *out)
{
    unsigned int len = 0;
    if (h < 0 || h >= NH || !htab[h]) return -1;
    if (EVP_DigestFinal_ex(htab[h], out, &len) != 1) len = 0;
    EVP_MD_CTX_free(htab[h]);
    htab[h] = NULL;
    return len ? (int)len : -1;
}

int n9_hmac(int algo, const unsigned char *key, size_t klen,
            const unsigned char *data, size_t dlen, unsigned char *out)
{
    const EVP_MD *md = md_for(algo);
    unsigned int len = 0;
    if (!md) return -1;
    if (!HMAC(md, key, (int)klen, data, dlen, out, &len)) return -1;
    return (int)len;
}

void n9_random_bytes(unsigned char *out, int n)
{
    if (RAND_bytes(out, n) == 1) return;
    /* RAND_bytes only fails if the pool cannot be seeded; there is no safe
     * fallback for a CSPRNG, so make it loud rather than return zeros. */
    fprintf(stderr, "node9: RAND_bytes failed — no secure randomness\n");
    abort();
}

/* ---------- zlib inflate (gzip) ---------- */

#define NZ 64
static z_stream *ztab[NZ];

int n9_inflate_create(void)
{
    int i;
    for (i = 0; i < NZ; i++) {
        if (ztab[i] == NULL) {
            z_stream *z = (z_stream *)calloc(1, sizeof(z_stream));
            if (!z) return -1;
            if (inflateInit2(z, 15 + 16) != Z_OK) { free(z); return -1; }   /* gzip */
            ztab[i] = z;
            return i;
        }
    }
    return -1;
}

int n9_inflate(int h, const unsigned char *in, size_t inlen, size_t *consumed,
               unsigned char *out, size_t outcap, int *done)
{
    z_stream *z;
    int r;
    if (h < 0 || h >= NZ || !ztab[h]) return -1;
    z = ztab[h];
    z->next_in = (Bytef *)in;
    z->avail_in = (uInt)inlen;
    z->next_out = (Bytef *)out;
    z->avail_out = (uInt)outcap;
    r = inflate(z, Z_NO_FLUSH);
    if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) return -1;
    *consumed = inlen - (size_t)z->avail_in;
    *done = (r == Z_STREAM_END);
    return (int)(outcap - (size_t)z->avail_out);
}

void n9_inflate_destroy(int h)
{
    if (h >= 0 && h < NZ && ztab[h]) {
        inflateEnd(ztab[h]);
        free(ztab[h]);
        ztab[h] = NULL;
    }
}

/* ---------- TLS client ---------- */

#define NT 64
/* Plan 9 has no non-blocking I/O: a read on a socket waits, and there is no
 * O_NONBLOCK to ask otherwise. Handing the fd to OpenSSL (SSL_set_fd) therefore
 * means SSL_read can block inside the event loop's read handler — which
 * deadlocks the moment the peer is waiting for a request still queued on our
 * side. So OpenSSL never sees the fd: it reads and writes memory BIOs, and the
 * JS side moves bytes between those BIOs and the socket when poll says it can.
 * Every SSL call then returns immediately, WANT_READ included. */
typedef struct {
    SSL *ssl;
    BIO *rbio;      /* ciphertext in  (we feed it from the socket) */
    BIO *wbio;      /* ciphertext out (we drain it to the socket) */
    int rawfd;
} N9Tls;
static N9Tls ttab[NT];

/* socket <-> BIO helpers, blocking, used only while connecting */
static int tls_flush_out(N9Tls *t)
{
    unsigned char buf[4096];
    int n, off, w;
    while ((n = BIO_read(t->wbio, buf, sizeof buf)) > 0) {
        for (off = 0; off < n; off += w) {
            w = (int)write(t->rawfd, buf + off, (size_t)(n - off));
            if (w <= 0) return -1;
        }
    }
    return 0;
}

static int tls_pump_in(N9Tls *t)
{
    unsigned char buf[4096];
    int n = (int)read(t->rawfd, buf, sizeof buf);
    if (n <= 0) return -1;
    return BIO_write(t->rbio, buf, n) == n ? 0 : -1;
}

static SSL_CTX *tls_ctx(void)
{
    static SSL_CTX *ctx;
    static int tried;
    const char *ca;
    if (tried) return ctx;
    tried = 1;
    SSL_library_init();
    SSL_load_error_strings();
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (getenv("NODE9_TLS_INSECURE")) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        return ctx;
    }
    ca = getenv("NODE9_CA_FILE");
    if (!ca) ca = "/amd64/lib/node9/ca.pem";
    if (SSL_CTX_load_verify_locations(ctx, ca, NULL) == 1) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    } else {
        fprintf(stderr, "node9: no CA bundle at %s — TLS certificates NOT verified "
                        "(install one or set NODE9_CA_FILE)\n", ca);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }
    return ctx;
}

int n9_tls_client(int tcpfd, const char *servername)
{
    SSL_CTX *ctx = tls_ctx();
    SSL *ssl;
    int i, slot = -1;

    if (!ctx) return -1;
    for (i = 0; i < NT; i++) if (!ttab[i].ssl) { slot = i; break; }
    if (slot < 0) return -1;

    ssl = SSL_new(ctx);
    if (!ssl) return -1;
    if (servername && *servername) {
        SSL_set_tlsext_host_name(ssl, servername);          /* SNI */
        /* verify the name too, not just the chain — a valid certificate for the
         * wrong host is exactly the attack chain-only checking misses */
        SSL_set1_host(ssl, servername);
    }
    ttab[slot].rbio = BIO_new(BIO_s_mem());
    ttab[slot].wbio = BIO_new(BIO_s_mem());
    if (!ttab[slot].rbio || !ttab[slot].wbio) {
        BIO_free_all(ttab[slot].rbio); BIO_free_all(ttab[slot].wbio);
        ttab[slot].rbio = ttab[slot].wbio = NULL;
        SSL_free(ssl);
        return -1;
    }
    BIO_set_mem_eof_return(ttab[slot].rbio, -1);   /* empty means "more later", not EOF */
    SSL_set_bio(ssl, ttab[slot].rbio, ttab[slot].wbio);
    SSL_set_connect_state(ssl);
    ttab[slot].ssl = ssl;
    ttab[slot].rawfd = tcpfd;

    /* Drive the handshake here, synchronously: connect() is already a blocking
     * step for the caller, and doing it now keeps the async path (below) to pure
     * memory operations. */
    for (;;) {
        int r = SSL_do_handshake(ssl);
        int err;
        if (tls_flush_out(&ttab[slot]) < 0) { r = -1; err = SSL_ERROR_SYSCALL; goto fail; }
        if (r == 1) break;
        err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_WANT_READ) {
            if (tls_pump_in(&ttab[slot]) < 0) goto fail;
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) continue;
    fail:
        {
            unsigned long e = ERR_get_error();
            long v = SSL_get_verify_result(ssl);
            if (v != X509_V_OK)
                fprintf(stderr, "node9: TLS verify failed for %s: %s\n",
                        servername ? servername : "?", X509_verify_cert_error_string(v));
            else if (e)
                fprintf(stderr, "node9: TLS handshake failed for %s: %s\n",
                        servername ? servername : "?", ERR_error_string(e, NULL));
        }
        SSL_free(ssl);      /* frees both BIOs */
        ttab[slot].ssl = NULL; ttab[slot].rbio = ttab[slot].wbio = NULL;
        return -1;
    }
    if (getenv("NODE9_TLS_DEBUG"))
        fprintf(stderr, "node9: tls connected %s %s fd=%d slot=%d\n",
                SSL_get_version(ssl), SSL_get_cipher(ssl), tcpfd, slot);
    return slot;
}

static void tls_dbg(const char *what, SSL *ssl, int n)
{
    if (!getenv("NODE9_TLS_DEBUG")) return;
    fprintf(stderr, "node9: tls %s n=%d sslerr=%d err=%s\n", what, n,
            SSL_get_error(ssl, n), ERR_error_string(ERR_peek_last_error(), NULL));
}

/* ciphertext from the socket -> OpenSSL */
int n9_tls_feed(int h, const unsigned char *buf, int len)
{
    if (h < 0 || h >= NT || !ttab[h].ssl) return -1;
    if (len <= 0) return 0;
    return BIO_write(ttab[h].rbio, buf, len) == len ? len : -1;
}

/* ciphertext from OpenSSL -> the socket (handshake records, acks, our writes) */
int n9_tls_pull(int h, unsigned char *buf, int len)
{
    int n;
    if (h < 0 || h >= NT || !ttab[h].ssl) return -1;
    n = BIO_read(ttab[h].wbio, buf, len);
    return n > 0 ? n : 0;
}

int n9_tls_read(int h, unsigned char *buf, int len)
{
    int n, err;
    if (h < 0 || h >= NT || !ttab[h].ssl) return -1;
    n = SSL_read(ttab[h].ssl, buf, len);
    if (n > 0) return n;
    err = SSL_get_error(ttab[h].ssl, n);
    if (err == SSL_ERROR_ZERO_RETURN) return 0;                  /* clean close */
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return -2;  /* retry */
    tls_dbg("read", ttab[h].ssl, n);
    return -1;
}

int n9_tls_write(int h, const unsigned char *buf, int len)
{
    int n, err;
    if (h < 0 || h >= NT || !ttab[h].ssl) return -1;
    n = SSL_write(ttab[h].ssl, buf, len);
    if (n > 0) return n;
    err = SSL_get_error(ttab[h].ssl, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return -2;
    tls_dbg("write", ttab[h].ssl, n);
    return -1;
}

/* Bytes already decrypted and waiting: a TLS record can carry several reads'
 * worth, so a poll on the raw fd would go quiet with data still buffered. */
int n9_tls_pending(int h)
{
    if (h < 0 || h >= NT || !ttab[h].ssl) return -1;
    return SSL_pending(ttab[h].ssl);
}

int n9_tls_rawfd(int h)
{
    if (h < 0 || h >= NT || !ttab[h].ssl) return -1;
    return ttab[h].rawfd;
}

void n9_tls_close(int h)
{
    if (h < 0 || h >= NT || !ttab[h].ssl) return;
    SSL_shutdown(ttab[h].ssl);
    SSL_free(ttab[h].ssl);          /* frees rbio and wbio too */
    ttab[h].ssl = NULL;
    ttab[h].rbio = ttab[h].wbio = NULL;
    close(ttab[h].rawfd);
    ttab[h].rawfd = -1;
}
