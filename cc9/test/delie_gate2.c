/* delie_gate2 — verify the delegated cc9 de-lie fixes (threads/net/random). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <netdb.h>

extern unsigned int arc4random(void);
extern unsigned int arc4random_uniform(unsigned int);

static int pass = 0, fail = 0;
#define CHECK(name, cond) do { if (cond) { pass++; printf("PASS  %s\n", name); } \
    else { fail++; printf("FAIL  %s\n", name); } } while (0)

/* pthread_exit: a TSD destructor must run, and join must return. */
static int dtor_ran = 0;
static pthread_key_t key;
static void dtor(void *v) { (void)v; dtor_ran = 1; }
static void *exit_thread(void *a) {
    (void)a;
    pthread_setspecific(key, (void*)1);
    pthread_exit((void*)42);       /* must run the TSD dtor + terminate cleanly */
    return (void*)7;               /* never reached */
}

int main(void) {
    /* arc4random: print first value so a second run can confirm it's NOT fixed-seed. */
    unsigned long a = arc4random();
    printf("  arc4random first = %lu\n", a);
    int varied = 0; unsigned long prev = arc4random();
    for (int i = 0; i < 8; i++) { unsigned long x = arc4random(); if (x != prev) varied = 1; prev = x; }
    CHECK("arc4random varies (not a constant)", varied);
    unsigned int u = arc4random_uniform(100);
    CHECK("arc4random_uniform in range", u < 100);

    /* pthread_exit: TSD destructor runs + join gets the value. */
    pthread_key_create(&key, dtor);
    pthread_t t; void *rv = 0;
    pthread_create(&t, 0, exit_thread, 0);
    pthread_join(t, &rv);
    printf("  pthread_exit -> join rv=%ld, tsd dtor ran=%d\n", (long)rv, dtor_ran);
    CHECK("pthread_exit join value", (long)rv == 42);
    CHECK("pthread_exit runs TSD destructor", dtor_ran == 1);

    /* rwlock: recursive rdlock on one thread must NOT deadlock (was a plain mutex). */
    {
        pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
        int ok = 1;
        if (pthread_rwlock_rdlock(&rw) != 0) ok = 0;
        if (pthread_rwlock_rdlock(&rw) != 0) ok = 0;   /* 2nd read lock, same thread */
        pthread_rwlock_unlock(&rw);
        pthread_rwlock_unlock(&rw);
        /* a write lock now must succeed (all readers released) */
        if (pthread_rwlock_wrlock(&rw) != 0) ok = 0;
        pthread_rwlock_unlock(&rw);
        printf("  rwlock recursive-rdlock + wrlock ok=%d\n", ok);
        CHECK("rwlock supports recursive rdlock (no deadlock)", ok);
    }

    /* gethostbyname: must resolve now (/net/cs works) instead of always failing. */
    {
        struct hostent *h = gethostbyname("dns.google");
        printf("  gethostbyname(dns.google) -> %s\n", h ? h->h_name : "(null)");
        CHECK("gethostbyname resolves", h != 0 && h->h_addr_list && h->h_addr_list[0]);
    }

    printf("----\n%d passed, %d failed\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
