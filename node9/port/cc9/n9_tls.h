#ifndef NODE9_TLS_H
#define NODE9_TLS_H
/* Handle-based TLS, for backends that cannot hand back a cleartext fd (OpenSSL).
 * The Plan 9 libsec backend returns a real fd from n9_tls_client() and does not
 * implement these; boot.js checks `__n9native.tlsHandles` and picks the mode. */
#include <stddef.h>

int  n9_tls_feed(int h, const unsigned char *buf, int len);  /* socket ciphertext -> TLS */
int  n9_tls_pull(int h, unsigned char *buf, int len);        /* TLS ciphertext -> socket */
int  n9_tls_read(int h, unsigned char *buf, int len);   /* >0 bytes, 0 EOF, -2 retry, -1 error */
int  n9_tls_write(int h, const unsigned char *buf, int len);  /* >0 written, -2 retry, -1 error */
int  n9_tls_pending(int h);   /* bytes already decrypted (a record may hold several reads) */
int  n9_tls_rawfd(int h);     /* the TCP fd — what readability is polled on */
void n9_tls_close(int h);

#endif
