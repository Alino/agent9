/* cc9 poll(2) emulation — the readiness layer that backs libuv's posix-poll
 * backend (neovim9), plus fcntl(O_NONBLOCK/FD_CLOEXEC) and pipe2.
 *
 * Plan 9 I/O is blocking-only, so readiness is emulated per fd:
 *   - a reader pthread blocking-pread()s into a ring buffer,
 *   - poll() scans the buffers and, when nothing is ready, waits on ONE
 *     central counting semaphore (n9_tsemacquire gives timeouts for free;
 *     counting semantics close the check-then-wait race),
 *   - fs.c read() diverts to cc9_poll_read() for fds owned here: buffered
 *     bytes, else EOF/error, else EAGAIN (nonblocking) or wait (blocking),
 *   - O_NONBLOCK fds get a WRITE ring too: fs.c write() diverts to
 *     cc9_poll_write() (copy into ring, partial counts, EAGAIN when full); a
 *     writer pthread drains it with blocking pwrites, so POLLOUT is HONEST
 *     (ring has space) and two peers streaming large payloads at each other
 *     can no longer deadlock (the Ladybird IPC topology). Blocking fds keep
 *     direct write + always-ready POLLOUT (writes block-but-complete).
 *     ponytail: net9 datagram paths (sendto/sendmsg) bypass fs.c write and
 *     stay direct — their POLLOUT remains optimistic.
 *
 * A reader thread can linger blocked in pread after close() (Plan 9 has no
 * way to interrupt another proc's read short of a note); entries are marked
 * dead and reclaimed when the read returns. Bounded by PFD_MAX.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>

extern long n9_pread(int, void *, long, long long);
extern long n9_pwrite(int, const void *, long, long long);
extern long n9_pipe(int *);
extern long n9_dup(int, int);
extern long n9_close(int);
extern void n9_semacquire(int *, int);
extern void n9_semrelease(int *, int);
extern long n9_tsemacquire(int *, long);
extern long n9_errstr(char *, unsigned long);
extern int strncmp(const char *, const char *, unsigned long);
extern char *strstr(const char *, const char *);
extern void *malloc(unsigned long);
extern void free(void *);
extern void *memcpy(void *, const void *, unsigned long);

/* The poll table is the real ceiling on how many fds a process can poll, and
 * running out of it surfaces as EMFILE from fcntl/ioctl — "Too many open files"
 * with the OS nowhere near its own limit. At 256 Ladybird's RequestServer ran
 * out on a youtube watch page (a socket plus a pipe pair per in-flight request,
 * plus readers that linger on closed fds until their pread returns): 44 of 300
 * F_SETFL calls failed and every later request then failed to create its
 * response pipe. Slots are cheap now that the ring is bought by the reader
 * rather than by the claim, so the table is sized for a browser.
 * Gated by pollring_gate case 4. */
#define PFD_MAX 1024
/* Read rings START at 64 KiB and GROW ON DEMAND, doubling up to CC9_POLL_RING
 * (default: no growth at all, so a plain cc9 program behaves exactly as before).
 * A big read ring lets the per-fd reader thread buffer a whole large transfer,
 * so a slow event-loop consumer (ladybird's RequestServer->WebContent response
 * pipe) never backs the pipe up and stalls delivery.
 *
 * It MUST be a ceiling and not a fixed size: rings are per fd, and a fixed 8 MiB
 * across ladybird's ~64 curl connections reserved ~1 GiB in RequestServer alone,
 * which blew RLIMIT_AS and killed the process mid-page. Growth means only the
 * few fds carrying multi-MB bodies ever pay for the space.
 *
 * The WRITE ring is deliberately NOT growable. A full write ring is just EAGAIN,
 * which is the contract a non-blocking writer already handles by retrying; there
 * is no stall to fix, so there is no reason to spend the memory. */
#define PFD_WBUF 65536u
static unsigned pfd_buf = 65536;        /* initial read-ring size */
static unsigned pfd_max = 65536;        /* growth ceiling; CC9_POLL_RING raises it */
static void pfd_ring_init(void){
    static int done = 0; if(done) return; done = 1;
    extern char *getenv(const char *);
    char *e = getenv("CC9_POLL_RING");
    if(!e) return;
    unsigned long v = 0; for(char *p = e; *p >= '0' && *p <= '9'; p++) v = v*10 + (unsigned)(*p - '0');
    if(v >= 4096 && v <= 64UL*1024*1024) pfd_max = (unsigned)v;
    if(pfd_max < pfd_buf) pfd_buf = pfd_max;
}
#define PFD_BUF pfd_buf

/* An "infinite" poll (timeout<0) waits on poll_sem in bounded slices instead of
 * forever: readiness is recomputed from ring state every scan (poll is
 * level-triggered), so this re-scan is a pure safety net — a missed empty->
 * non-empty edge wake (or a reader/writer thread that lost its start race)
 * degrades to <=POLL_RESCAN_MS latency instead of a permanent hang. The common
 * case still returns immediately on the poll_sem token; the cost is a low-rate
 * idle wakeup. This closes the Ladybird IPC reuse-hang: a rare stranded message
 * that used to deadlock a WebContent reactor now drains on the next re-scan.
 * ponytail: 200ms is the ceiling on that stall; lower it if IPC latency shows. */
#define POLL_RESCAN_MS 200

typedef struct {
	int fd;                 /* -1 = free slot */
	int flags;              /* O_NONBLOCK | O_CLOEXEC */
	int reader;             /* reader thread started */
	int rfd;                /* private dup(2) the reader preads (-1 = none). Keying
	                         * the reader on the app fd NUMBER is the reuse-hang bug:
	                         * Plan 9 can't interrupt a blocked pread, so a reader
	                         * lingers after close(); if the fd number is then reused
	                         * (per-test IPC/shm/srv channels churn heavily), the
	                         * lingering reader preads the NEW channel into the OLD
	                         * (dead) slot while poll()'s lookup hands the reactor the
	                         * fresh empty slot -> the response strands, WebContent's
	                         * reactor never sees it, the test suite deadlocks. A
	                         * private dup decouples the reader from the app fd: it
	                         * drains the OLD channel harmlessly and the reused fd gets
	                         * its own fresh reader. */
	int dead;               /* fd closed; reader exits on next return */
	int eof, err;
	int lock;               /* binary sem, 1 = unlocked */
	int space;              /* reader waits here when the ring is full */
	int data;               /* blocking cc9_poll_read waiters */
	unsigned head, tail;    /* ring positions: head = fill, tail = drain */
	char *buf;              /* bufsz bytes, malloc'd when the slot is claimed */
	unsigned bufsz;         /* current read-ring size; grows toward pfd_max */
	/* write side (O_NONBLOCK fds only) */
	int writer;             /* writer thread started */
	int werr;               /* a drain pwrite failed; fd is done for */
	int wlock;              /* binary sem, 1 = unlocked */
	int wdata;              /* writer waits here when the ring is empty */
	int wdrain;             /* close-flush waiters; released per drain pass */
	unsigned whead, wtail;
	char *wbuf;             /* PFD_WBUF, malloc'd on first nonblocking write */
} cc9_pfd;

static cc9_pfd tab[PFD_MAX];
static int tab_lock = 1;
static int poll_sem;
static int poll_waiters;   /* reactors parked on poll_sem right now. A process can run
                            * MORE THAN ONE event loop (WebContent has 2), all sharing this
                            * one global poll_sem — so a single-token wake goes to ONE
                            * reactor, often the WRONG one, which rescans its own fds, finds
                            * nothing, and re-parks; the reactor that owns the ready fd only
                            * wakes on the bounded re-scan (200ms/msg -> IPC crawls, tests
                            * time out). Releasing poll_waiters tokens wakes ALL parked
                            * reactors so the owner is always among them; the others rescan
                            * and re-park (no token leak: each wake consumes one). */
static int tab_inited;

/* $CC9_POLL_TRACE=<path>.<pid> gets one line per event — ground-truth byte
 * accounting when a stream wedges. Off (fd<0) by default. */
extern char *getenv(const char *);
extern long n9_open(const char *, int);
extern long n9_create(const char *, int, unsigned int);
extern int getpid(void);
static int trace_fd = -2;
void cc9_trace(const char *, int, long);
static void trace(const char *op, int fd, long n){ cc9_trace(op, fd, n); }
void cc9_trace(const char *op, int fd, long n){
	if(trace_fd == -2){
		char *pfx = getenv("CC9_POLL_TRACE");
		if(!pfx) { trace_fd = -1; return; }
		/* CC9_POLL_TRACE=2 -> stderr, which a terminal captures with the child's
		 * output; the per-pid file needs a writable /tmp and has been unreliable. */
		if(pfx[0] == '2' && pfx[1] == 0) { trace_fd = 2; goto have_fd; }
		char path[128]; int k = 0;
		while(pfx[k] && k < 100){ path[k] = pfx[k]; k++; }
		path[k++] = '.';
		int pid = getpid();
		for(int d = 100000; d; d /= 10) path[k++] = '0' + pid / d % 10;
		path[k] = 0;
		trace_fd = (int)n9_create(path, 1 /*OWRITE*/, 0666);
	have_fd: ;
	}
	if(trace_fd < 0) return;
	char b[80]; int k = 0;
	while(*op) b[k++] = *op++;
	b[k++] = ' ';
	b[k++] = 'f'; b[k++] = 'd';
	b[k++] = '0' + (fd / 10) % 10; b[k++] = '0' + fd % 10;
	b[k++] = ' ';
	int neg = n < 0; unsigned long v = neg ? (unsigned long)-n : (unsigned long)n;
	if(neg) b[k++] = '-';
	char t[20]; int tn = 0;
	do { t[tn++] = '0' + v % 10; v /= 10; } while(v);
	while(tn) b[k++] = t[--tn];
	b[k++] = '\n';
	n9_pwrite(trace_fd, b, k, -1);
}

static void tab_init(void){
	/* runs under tab_lock */
	if(tab_inited) return;
	for(int i = 0; i < PFD_MAX; i++) tab[i].fd = -1;
	tab_inited = 1;
}

static cc9_pfd *lookup(int fd){
	if(!tab_inited) return 0;
	for(int i = 0; i < PFD_MAX; i++)
		if(tab[i].fd == fd && !tab[i].dead) return &tab[i];
	return 0;
}

/* interrupted syscall? (a note aborts pread; the reader must retry)
 * n9_errstr SWAPS the buffer with the kernel errstr: read it, test, then swap
 * the same buffer back so other errno paths still see the original string. */
static int was_interrupted(void){
	char e[128];
	e[0] = 0;
	n9_errstr(e, sizeof e);                    /* e = errstr, errstr = "" */
	int r = strstr(e, "interrupt") != 0;
	n9_errstr(e, sizeof e);                    /* restore */
	return r;
}

/* Wake every reactor parked on poll_sem, not just one (see poll_waiters). At
 * least one token so a reactor about to park doesn't miss it; extra tokens from
 * a racy count are drained by the spurious wake that consumes them. */
static void poll_wake(void){ int w = poll_waiters; n9_semrelease(&poll_sem, w > 0 ? w : 1); }

static unsigned ring_avail(cc9_pfd *p){ return p->head - p->tail; }
static unsigned ring_space(cc9_pfd *p){ return p->bufsz - ring_avail(p); }
static unsigned wring_avail(cc9_pfd *p){ return p->whead - p->wtail; }
static unsigned wring_space(cc9_pfd *p){ return PFD_WBUF - wring_avail(p); }

/* Double the read ring, up to pfd_max. Returns 1 if it grew.
 *
 * ONLY the reader thread may call this, and only between preads: the reader is
 * the sole writer of buf CONTENTS, so the move has to exclude just the consumer,
 * which copies out under p->lock. Growing re-linearizes the ring (tail lands at
 * 0), because head/tail are absolute counters reduced mod the size — changing
 * the size under a wrapped ring would otherwise reinterpret every live byte. */
static int ring_grow(cc9_pfd *p){
	if(p->bufsz >= pfd_max) return 0;
	unsigned nsz = p->bufsz * 2;
	if(nsz > pfd_max) nsz = pfd_max;
	char *nb = malloc(nsz);
	if(!nb) return 0;                       /* out of memory: fall back to blocking */
	n9_semacquire(&p->lock, 1);
	unsigned avail = ring_avail(p);
	unsigned off = p->tail % p->bufsz;
	unsigned cont = p->bufsz - off;
	if(cont > avail) cont = avail;
	memcpy(nb, p->buf + off, cont);
	if(avail > cont) memcpy(nb + cont, p->buf, avail - cont);
	char *ob = p->buf;
	p->buf = nb;
	p->bufsz = nsz;
	p->tail = 0;
	p->head = avail;
	n9_semrelease(&p->lock, 1);
	free(ob);
	trace("grow", p->fd, (long)nsz);
	return 1;
}

static void *reader_main(void *arg){
	cc9_pfd *p = arg;
	for(;;){
		/* Full ring: try to grow before parking. Growing here rather than at
		 * claim time is the whole point — only an fd that actually backs up
		 * (a multi-MB body against a slow consumer) ever costs more than 64 KiB. */
		while(!p->dead && ring_space(p) == 0)
			if(!ring_grow(p))
				n9_semacquire(&p->space, 1);
		if(p->dead) break;
		/* pread STRAIGHT into the ring's contiguous span (mirror writer_main):
		 * no bounce buffer, no byte-by-byte copy, no artificial 2K cap — one
		 * syscall drains up to a full window. ring_space is a conservative lower
		 * bound (the consumer only frees space), and the consumer never reads past
		 * head, so writing [head, head+want) without p->lock is safe; we take the
		 * lock only to advance head after the read completes. */
		unsigned space = ring_space(p);
		unsigned off = p->head % p->bufsz;
		unsigned cont = p->bufsz - off;             /* contiguous run to buffer end */
		unsigned want = space < cont ? space : cont;
		long r = n9_pread(p->rfd, p->buf + off, (long)want, -1);   /* private dup, not p->fd: immune to fd-number reuse */
		trace("rdthr", p->fd, r);
		if(r < 0 && was_interrupted() && !p->dead)
			continue;
		n9_semacquire(&p->lock, 1);
		int was_empty = ring_avail(p) == 0;
		if(r > 0){
			p->head += (unsigned)r;
		} else if(r == 0){
			p->eof = 1;
		} else {
			p->err = 1;
		}
		n9_semrelease(&p->lock, 1);
		/* Wake the poll reactor only on the empty->non-empty EDGE (plus eof/err).
		 * poll() is level-triggered — it reports POLLIN from ring_avail() under
		 * p->lock (poll.c ~397) — so a reactor already scanning/draining sees new
		 * bytes without a token, and poll_sem is a COUNTING semaphore so the edge
		 * wake can't be lost even if it races the reactor parking. This lets the
		 * reader build a backlog in the ring before the reactor runs, collapsing
		 * N per-segment cross-proc handoffs into one large drain (the throughput
		 * fix). p->data is still pulsed every pass for the blocking-read path. */
		if(was_empty || r <= 0)
			poll_wake();
		n9_semrelease(&p->data, 1);
		if(r <= 0) { trace("rdend", p->fd, r); break; }
	}
	/* dead (fd closed under us): the slot is freed by whoever set dead once
	 * BOTH threads are gone; here just drop our claim. eof/err: keep the slot
	 * so cc9_poll_read can report 0/-1 until close() reclaims it. */
	if(p->rfd >= 0 && p->rfd != p->fd) n9_close(p->rfd);   /* release the private dup we owned */
	n9_semacquire(&tab_lock, 1);
	p->reader = 0;
	p->rfd = -1;
	if(p->dead && !p->writer) p->fd = -1;
	n9_semrelease(&tab_lock, 1);
	return 0;
}

/* Drains the write ring with blocking pwrites. Ordering: single drainer per
 * fd, FIFO ring. On pwrite failure the fd is poisoned (werr) — pending bytes
 * are dropped, matching a peer-death mid-stream. */
static void *writer_main(void *arg){
	cc9_pfd *p = arg;
	for(;;){
		while(!p->dead && wring_avail(p) == 0)
			n9_semacquire(&p->wdata, 1);
		if(p->dead && wring_avail(p) == 0) break;
		unsigned tail = p->wtail;
		unsigned chunk = wring_avail(p);
		unsigned cont = PFD_WBUF - (tail % PFD_WBUF);   /* contiguous run */
		if(chunk > cont) chunk = cont;
		long r = n9_pwrite(p->fd, p->wbuf + (tail % PFD_WBUF), (long)chunk, -1);
		trace("wrthr", p->fd, r);
		if(r < 0 && was_interrupted() && !p->dead)
			continue;
		n9_semacquire(&p->wlock, 1);
		if(r > 0)
			p->wtail += (unsigned)r;
		else
			p->werr = 1;
		n9_semrelease(&p->wlock, 1);
		poll_wake();                      /* POLLOUT state changed; wake all reactors */
		n9_semrelease(&p->wdrain, 1);     /* wake a close-flush waiter */
		if(r <= 0) break;
	}
	n9_semacquire(&tab_lock, 1);
	p->writer = 0;
	if(p->dead && !p->reader) p->fd = -1;
	n9_semrelease(&tab_lock, 1);
	n9_semrelease(&p->wdrain, 1);
	return 0;
}

static cc9_pfd *ensure(int fd, int start_reader){
	n9_semacquire(&tab_lock, 1);
	tab_init();
	cc9_pfd *p = lookup(fd);
	if(!p){
		for(int i = 0; i < PFD_MAX; i++)
			if(tab[i].fd == -1){ p = &tab[i]; break; }
		if(p){
			pfd_ring_init();
			/* Slots are recycled across fds. Hand a grown ring back rather than
			 * letting it become the slot's permanent size — otherwise every slot
			 * that ever carried one big body keeps pfd_max reserved forever, which
			 * is the fixed-size allocation this growth scheme exists to avoid.
			 * The fresh ring is NOT allocated here: most slots are claimed by a
			 * bare fcntl(F_SETFL)/F_SETFD and never grow a reader, so buying a
			 * ring per claim would cost PFD_MAX * 64 KiB for nothing. */
			if(p->buf && p->bufsz > PFD_BUF){ free(p->buf); p->buf = 0; p->bufsz = 0; }
			p->fd = fd; p->flags = 0; p->reader = 0; p->rfd = -1; p->dead = 0;
			p->eof = p->err = 0; p->lock = 1; p->space = 0; p->data = 0;
			p->head = p->tail = 0;
			p->writer = 0; p->werr = 0; p->wlock = 1; p->wdata = 0;
			p->wdrain = 0; p->whead = p->wtail = 0;
		}
	}
	if(p && start_reader && !p->reader && !p->buf){
		p->buf = malloc(PFD_BUF);          /* the ring is the reader's, so buy it here */
		p->bufsz = p->buf ? PFD_BUF : 0;
	}
	if(p && start_reader && !p->reader && p->buf){
		p->rfd = (int)n9_dup(fd, -1);      /* reader preads this private dup, not the app fd */
		if(p->rfd < 0) p->rfd = fd;        /* dup exhausted: fall back to the app fd (old behavior) */
		pthread_t t;
		if(pthread_create(&t, 0, reader_main, p) == 0){
			pthread_detach(t);
			p->reader = 1;
		} else {
			if(p->rfd != fd) n9_close(p->rfd);
			p->rfd = -1;
		}
	}
	n9_semrelease(&tab_lock, 1);
	return p;
}

/* ---- hooks for fs.c ---- */

int cc9_poll_owned(int fd){
	cc9_pfd *p = lookup(fd);
	if(!p) return 0;
	if(p->reader || (p->flags & O_NONBLOCK)) return 1;
	/* The reader thread exits as soon as the kernel gives it EOF, which can happen
	 * with the tail of the transfer still sitting in the ring. Routing read() back to
	 * the kernel at that point returns 0 and throws those bytes away — a 220 KB HTTP
	 * body arrived as 4 KB. Keep owning the fd until the ring is drained and the
	 * eof/err the reader recorded has been reported through cc9_poll_read. No p->lock:
	 * this runs on every read(), and a stale answer only picks the ring path, which is
	 * the correct one whenever anything is left to hand back. */
	return p->head != p->tail || p->eof || p->err;
}

/* dup(2)/dup2(2): POSIX status flags (O_NONBLOCK) live on the OPEN FILE
 * DESCRIPTION and are shared with the dup; cc9 keys them per fd number, so
 * they'd silently vanish on the new fd (the RequestServer body pipe wedged a
 * whole event loop this way). Copy at dup time — the honest approximation
 * (post-dup mutations don't propagate; nothing we run does that).
 * FD_CLOEXEC is per-descriptor and correctly NOT copied. */
void cc9_poll_carry_dup(int oldfd, int newfd){
	cc9_pfd *op = lookup(oldfd);
	if(!op || !(op->flags & O_NONBLOCK)) return;
	cc9_pfd *np = ensure(newfd, 0);
	if(np) np->flags |= O_NONBLOCK;
}

/* Bytes already buffered for fd (FIONREAD); -1 if the fd isn't ours.
 * For UDP this can span datagrams (the ring is a byte stream) — fine for
 * one-query-per-socket users like a DNS client; a caller needing strict
 * per-datagram sizes must recvfrom into a max-size buffer instead. */
long cc9_poll_pending(int fd){
	cc9_pfd *p = lookup(fd);
	if(!p) return -1;
	/* Report only what the reader ring already holds. Do NOT start a reader
	 * here: ensure() spawns a thread via pthread_create, and FIONREAD is
	 * queried on the pthread-startup path itself — starting a reader from here
	 * recursed ensure -> pthread_create -> ... -> ensure into a stack overflow.
	 * With no reader yet, 0 bytes are buffered — an honest answer, and better
	 * than the old ENOTTY. poll()/read() start the reader when the fd is
	 * actually used, so a subsequent FIONREAD then reports the real fill. */
	if(!p->reader){
		if(!(p->flags & O_NONBLOCK)) return -1;
		return 0;
	}
	n9_semacquire(&p->lock, 1);
	long n = (long)ring_avail(p);
	n9_semrelease(&p->lock, 1);
	return n;
}

long cc9_poll_read(int fd, void *buf, long n){
	cc9_pfd *p = lookup(fd);
	if(!p){ errno = EBADF; return -1; }
	/* Do not restart a reader once the kernel has given us EOF (or an error): the
	 * ring still has to be drained first, and a fresh reader would only re-read 0. */
	if(!p->reader && !p->eof && !p->err) ensure(fd, 1);
	char *d = buf;
	for(;;){
		n9_semacquire(&p->lock, 1);
		unsigned avail = ring_avail(p);
		if(avail > 0){
			long take = (long)avail < n ? (long)avail : n;
			for(long i = 0; i < take; i++)
				d[i] = p->buf[(p->tail + i) % p->bufsz];
			p->tail += (unsigned)take;
			n9_semrelease(&p->lock, 1);
			n9_semrelease(&p->space, 1);   /* wake the reader if it was full */
			trace("read", fd, take);
			return take;
		}
		int eof = p->eof, err = p->err;
		n9_semrelease(&p->lock, 1);
		if(eof){ trace("eof", fd, 0); return 0; }
		if(err){ trace("err", fd, -1); errno = EIO; return -1; }
		if(p->flags & O_NONBLOCK){ trace("again", fd, -1); errno = EAGAIN; return -1; }
		n9_semacquire(&p->data, 1);        /* blocking: wait for the reader */
	}
}

/* Is write() for this fd routed through the write ring? */
int cc9_poll_wowned(int fd){
	cc9_pfd *p = lookup(fd);
	return p && (p->flags & O_NONBLOCK);
}

long cc9_poll_write(int fd, const void *buf, long n){
	cc9_pfd *p = lookup(fd);
	if(!p){ errno = EBADF; return -1; }
	if(p->werr){ errno = EPIPE; return -1; }
	if(!p->wbuf){
		n9_semacquire(&tab_lock, 1);
		if(!p->wbuf) p->wbuf = malloc(PFD_WBUF);
		n9_semrelease(&tab_lock, 1);
		if(!p->wbuf){ errno = ENOMEM; return -1; }
	}
	n9_semacquire(&p->wlock, 1);
	unsigned space = wring_space(p);
	if(space == 0){
		n9_semrelease(&p->wlock, 1);
		trace("wagain", fd, -1);
		errno = EAGAIN;
		return -1;
	}
	long take = (long)space < n ? (long)space : n;
	unsigned head = p->whead;
	for(long i = 0; i < take; i++)
		p->wbuf[(head + i) % PFD_WBUF] = ((const char *)buf)[i];
	p->whead += (unsigned)take;
	n9_semrelease(&p->wlock, 1);
	if(!p->writer){
		n9_semacquire(&tab_lock, 1);
		if(!p->writer){
			pthread_t t;
			if(pthread_create(&t, 0, writer_main, p) == 0){
				pthread_detach(t);
				p->writer = 1;
			}
		}
		n9_semrelease(&tab_lock, 1);
	}
	n9_semrelease(&p->wdata, 1);
	trace("wring", fd, take);
	return take;
}

void cc9_poll_onclose(int fd){
	/* flush: close() must not drop ring bytes the caller was told were
	 * written. Kick the writer and wait per drain pass. Ceiling: a peer that
	 * never reads keeps us here until it dies (its death fails the pwrite ->
	 * werr -> we stop waiting) — the same place a blocking write would sit. */
	for(;;){
		cc9_pfd *p = lookup(fd);
		if(!p || !p->writer || p->werr || wring_avail(p) == 0) break;
		n9_semrelease(&p->wdata, 1);
		n9_tsemacquire(&p->wdrain, 100);
	}
	n9_semacquire(&tab_lock, 1);
	if(tab_inited)
		for(int i = 0; i < PFD_MAX; i++)
			if(tab[i].fd == fd && !tab[i].dead){
				if(tab[i].reader || tab[i].writer){
					tab[i].dead = 1;               /* threads reclaim on return */
					n9_semrelease(&tab[i].space, 1);
					n9_semrelease(&tab[i].wdata, 1);
				} else
					tab[i].fd = -1;                /* no threads: free now */
			}
	n9_semrelease(&tab_lock, 1);
}

/* Drop a slot without flush or thread handshakes. For fork children about to
 * exec: the parent's reader/writer pthreads do NOT exist in the child, so any
 * flush wait would sleep forever; the table is just inherited bytes here. */
void cc9_poll_forget(int fd){
	if(!tab_inited) return;
	for(int i = 0; i < PFD_MAX; i++)
		if(tab[i].fd == fd){
			/* the private reader dup is internal; drop it so it can't ride the
			 * exec into the child as a stray open fd */
			if(tab[i].rfd >= 0 && tab[i].rfd != tab[i].fd) n9_close(tab[i].rfd);
			tab[i].rfd = -1;
			tab[i].fd = -1;
		}
}

/* FD_CLOEXEC set? (consulted by execve before n9_exec) */
int cc9_poll_cloexec(int fd){
	cc9_pfd *p = lookup(fd);
	return p && (p->flags & O_CLOEXEC);
}

/* Close every CLOEXEC fd (execve, replacing its old fixed 3..63 scan; the
 * table is the single source of CLOEXEC truth). Raw n9_close, no flush: on
 * the exec path there are no live rings to flush in this process image
 * (post-fork children have no threads; pre-exec state is discarded anyway). */
void cc9_poll_close_cloexec(void){
	if(!tab_inited) return;
	/* Private reader dups are NOT closed here — cc9_poll_child_reset() already
	 * did it, at fork, and closing them at exec time is actively wrong: by now
	 * the child's file actions (libuv's spawn shuffle, posix_spawn's dup2) have
	 * run, and a dup2 lands on the LOWEST free number — which is exactly the
	 * number a private dup just vacated. Closing "our" recorded rfd here then
	 * closes the child's freshly-installed stdio instead. That is what broke
	 * nvim: the TUI client spawned its --embed server, cc9 closed the server's
	 * RPC pipe on the way through execve, the server read EOF and exited, and
	 * the client tore the UI down having painted nothing but its init sequence. */
	for(int i = 0; i < PFD_MAX; i++){
		if(tab[i].fd >= 0 && (tab[i].flags & O_CLOEXEC)){
			n9_close(tab[i].fd);
			tab[i].fd = -1;
		}
	}
}

/* Called in the fork CHILD, before any file actions run. The child inherited a
 * copy of the table plus real open dups, but none of the reader/writer threads
 * that own them (fork copies one thread). So: close the inherited dups while
 * their numbers still mean what the table says, and clear the thread flags —
 * a slot left claiming reader=1 has nobody filling its ring, so the first read
 * would block forever. */
void cc9_poll_child_reset(void){
	if(!tab_inited) return;
	for(int i = 0; i < PFD_MAX; i++){
		if(tab[i].rfd >= 0 && tab[i].rfd != tab[i].fd) n9_close(tab[i].rfd);
		tab[i].rfd = -1;
		tab[i].reader = 0;
		tab[i].writer = 0;
	}
}

/* ---- POSIX surface ---- */

static long now_ms(void){
	struct timespec ts;
	clock_gettime(0, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Drain every write ring before the process dies (called from the exit
 * epilogue, BEFORE cc9_kill_threads).
 *
 * exit() does not close fds, so nothing else flushes these: the bytes a caller
 * was already told were written sit in the ring, and killing the drain threads
 * throws them away. That is how nvim lost its terminal-restore sequence on the
 * way out — the alternate screen was never left, so the editor's screen stayed
 * on the console, painted over the next shell prompt, and the terminal was left
 * in a state the user had to type through.
 *
 * Bounded by a whole-process budget: a peer that stopped reading must not be
 * able to hang exit (that is the same ceiling cc9_poll_onclose documents, but
 * shared across every fd rather than per-fd). */
void cc9_poll_flush_all(void){
	if(!tab_inited) return;
	long deadline = now_ms() + 1000;
	for(;;){
		int pending = 0;
		for(int i = 0; i < PFD_MAX; i++){
			cc9_pfd *p = &tab[i];
			if(p->fd < 0 || !p->writer || p->werr) continue;
			if(wring_avail(p) == 0) continue;
			pending = 1;
			n9_semrelease(&p->wdata, 1);      /* kick the drainer */
			n9_tsemacquire(&p->wdrain, 50);   /* wait one drain pass */
		}
		if(!pending || now_ms() >= deadline) return;
	}
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout){
	long deadline = timeout > 0 ? now_ms() + timeout : 0;
	for(;;){
		int ready = 0;
		for(nfds_t i = 0; i < nfds; i++){
			fds[i].revents = 0;
			if(fds[i].fd < 0) continue;
			if(fds[i].events & POLLIN){
				cc9_pfd *p = ensure(fds[i].fd, 1);
				if(!p){ fds[i].revents |= POLLNVAL; ready++; continue; }
				n9_semacquire(&p->lock, 1);
				unsigned avail = ring_avail(p);
				int eof = p->eof, err = p->err;
				n9_semrelease(&p->lock, 1);
				if(avail) fds[i].revents |= POLLIN;
				if(eof) fds[i].revents |= POLLIN | POLLHUP;
				if(err) fds[i].revents |= POLLERR;
			}
			if(fds[i].events & POLLOUT){
				cc9_pfd *p = lookup(fds[i].fd);
				if(p && (p->flags & O_NONBLOCK)){  /* ring-routed: honest */
					if(p->werr) fds[i].revents |= POLLERR;
					else if(!p->wbuf || wring_space(p) > 0) fds[i].revents |= POLLOUT;
				} else
					fds[i].revents |= POLLOUT;     /* blocking fd: writes block-but-complete */
			}
			if(fds[i].revents) ready++;
		}
		if(ready || timeout == 0) return ready;
		if(timeout < 0){
			__sync_fetch_and_add(&poll_waiters, 1);
			n9_tsemacquire(&poll_sem, POLL_RESCAN_MS);  /* bounded: re-scan even if an edge wake was missed */
			__sync_fetch_and_sub(&poll_waiters, 1);
		} else {
			long left = deadline - now_ms();       /* stale tokens re-loop; keep the true deadline */
			if(left <= 0) return 0;
			/* Cap each nap at POLL_RESCAN_MS like the infinite path does. Waiting the
			 * caller's FULL timeout here meant a missed edge wake went unnoticed for as
			 * long as the caller's next timer — minutes, for an app with a long idle
			 * timer — and the fd looked dead the whole time even though its ring had
			 * data. Re-scanning costs one wakeup every 200ms while waiting. */
			long nap = left < POLL_RESCAN_MS ? left : POLL_RESCAN_MS;
			__sync_fetch_and_add(&poll_waiters, 1);
			long got = n9_tsemacquire(&poll_sem, nap);
			__sync_fetch_and_sub(&poll_waiters, 1);
			if(got != 1 && nap >= left) return 0;  /* the real deadline passed */
			if(got != 1) continue;                 /* nap expired: re-scan the fds */
		}
	}
}

int fcntl(int fd, int cmd, ...){
	__builtin_va_list ap; __builtin_va_start(ap, cmd);
	long arg = __builtin_va_arg(ap, long);
	__builtin_va_end(ap);
	cc9_pfd *p;
	switch(cmd){
	case F_DUPFD:
	case F_DUPFD_CLOEXEC: {
		int nfd = (int)n9_dup(fd, -1);   /* ponytail: ignores the >=arg floor */
		if(nfd >= 0){
			extern void cc9_append_carry_dup(int, int);
			cc9_poll_carry_dup(fd, nfd);    /* O_NONBLOCK rides the description */
			cc9_append_carry_dup(fd, nfd);  /* O_APPEND too */
			if(cmd == F_DUPFD_CLOEXEC){
				cc9_pfd *np = ensure(nfd, 0);
				if(np) np->flags |= O_CLOEXEC;
			}
		}
		return nfd;
	}
	case F_GETFD:
		return cc9_poll_cloexec(fd) ? FD_CLOEXEC : 0;
	case F_SETFD:
		p = ensure(fd, 0);
		if(!p){ errno = EMFILE; return -1; }
		if(arg & FD_CLOEXEC) p->flags |= O_CLOEXEC; else p->flags &= ~O_CLOEXEC;
		return 0;
	case F_GETFL:
		/* access mode: report O_RDWR — Plan 9 pipes are bidirectional and we
		 * don't track open modes; claiming RDWR keeps libuv streams both
		 * readable and writable (a genuinely read-only fd still fails at
		 * write() time with a real error). */
		p = lookup(fd);
		return O_RDWR | (p ? (p->flags & O_NONBLOCK) : 0);
	case F_SETFL:
		p = ensure(fd, 0);
		if(!p){ errno = EMFILE; return -1; }
		if(arg & O_NONBLOCK) p->flags |= O_NONBLOCK; else p->flags &= ~O_NONBLOCK;
		return 0;
	/* Record locks: Plan 9 has NO POSIX byte-range locks, and cc9 does not fake
	 * one. This arm used to `return 0` for every lock cmd — telling two writers
	 * they each hold the exclusive lock. That is how a SQLite DB gets corrupted,
	 * silently, with no error anywhere to find it by.
	 *
	 * NOT built on DMEXCL (Plan 9's whole-file exclusive-open bit), deliberately.
	 * Checked against the vendored sqlite3.c: os_unix.c only ever locks byte
	 * RANGES — PENDING_BYTE, RESERVED_BYTE, SHARED_FIRST+SHARED_SIZE — and
	 * l_start==l_len==0 appears solely on the F_UNLCK release-all path. So DMEXCL
	 * would buy the one caller that matters nothing, while costing plenty: it is
	 * a PERSISTENT mode bit wstat'd onto the user's file (a crash leaves the DB
	 * permanently exclusive-use), and the kernel enforces it per open(2), so a
	 * second connection in OUR OWN process would be refused — breaking the
	 * single-process case that works today. Wrong granularity, wrong lifetime.
	 *
	 * So: refuse, loudly. ENOLCK is spec-legal for F_SETLK/F_SETLKW, and
	 * sqliteErrorFromPosixError() maps it to SQLITE_BUSY — SQLite reports
	 * "database is locked" instead of double-writing. A caller needing SQLite in
	 * one process can use its `unix-none` VFS, which bypasses fcntl entirely. */
	case F_GETLK: {
		/* NEVER return from F_GETLK without writing the struct: it is an
		 * out-parameter call, and a caller branching on stale stack memory is
		 * worse than any error. The -1/ENOLCK is the real answer ("cannot
		 * determine"); this fill is only defence for callers that ignore it, so
		 * it must not be the optimistic direction — claiming F_UNLCK would say
		 * "range is free, go ahead", the very lie we came here to delete. */
		struct flock *fl = (struct flock *)arg;
		if(fl){ fl->l_type = F_WRLCK; fl->l_pid = -1; }
		errno = ENOLCK;
		return -1;
	}
	case F_SETLK:
	case F_SETLKW: {
		struct flock *fl = (struct flock *)arg;
		if(fl && fl->l_type == F_UNLCK) return 0;   /* we hold none: an honest no-op */
		errno = ENOLCK;
		return -1;
	}
	default:
		/* Unknown cmd. cc9's <fcntl.h> defines only the commands handled above,
		 * so anything here is a number this target never named; the in-tree users
		 * of the old `return 0` (F_FULLFSYNC/F_BARRIERFSYNC/F_GETPATH/F_KINFO)
		 * are all __APPLE__-guarded and never compiled for Plan 9. EINVAL is what
		 * a real fcntl answers, and it lets optional-cmd callers fall back —
		 * libuv's uv__fs_fsync drops to fsync() on failure, where a "success"
		 * that synced nothing would have been a durability lie. */
		errno = EINVAL;
		return -1;
	}
}

/* Mark an fd close-on-exec from outside poll.c (open(2)'s O_CLOEXEC).
 * The poll table is the single source of CLOEXEC truth — an fd that never
 * reaches it survives exec no matter what flag the caller passed. */
void cc9_poll_mark_cloexec(int fd){
	cc9_pfd *p = ensure(fd, 0);
	if(p) p->flags |= O_CLOEXEC;
}

int pipe2(int fds[2], int flags){
	if(n9_pipe(fds) < 0){ errno = EMFILE; return -1; }
	if(flags){
		for(int i = 0; i < 2; i++){
			cc9_pfd *p = ensure(fds[i], 0);
			if(p) p->flags |= flags & (O_NONBLOCK | O_CLOEXEC);
		}
	}
	return 0;
}

/* ---- select(2) over poll() — fd_set surface for code written against the
 * BSD API (CPython's selectmodule, etc.). sys/select.h has the macros. */
struct cc9_timeval_sel { long tv_sec, tv_usec; };

int select(int nfds, unsigned long *rfds, unsigned long *wfds,
           unsigned long *efds, void *tvp){
	struct pollfd pfds[PFD_MAX];
	struct cc9_timeval_sel *tv = tvp;
	int n = 0, i, r, timeout = -1;

	if(nfds > PFD_MAX*8) nfds = PFD_MAX*8;   /* honest cap; PFD_MAX fds live here anyway */
	for(i = 0; i < nfds && n < PFD_MAX; i++){
		short ev = 0;
		if(rfds && (rfds[i/64] & (1UL << (i%64)))) ev |= POLLIN;
		if(wfds && (wfds[i/64] & (1UL << (i%64)))) ev |= POLLOUT;
		if(efds && (efds[i/64] & (1UL << (i%64)))) ev |= POLLPRI;
		if(ev){ pfds[n].fd = i; pfds[n].events = ev; pfds[n].revents = 0; n++; }
	}
	if(tv) timeout = (int)(tv->tv_sec*1000 + tv->tv_usec/1000);
	r = poll(pfds, n, timeout);
	if(r < 0) return -1;
	if(rfds) for(i = 0; i < (nfds+63)/64; i++) rfds[i] = 0;
	if(wfds) for(i = 0; i < (nfds+63)/64; i++) wfds[i] = 0;
	if(efds) for(i = 0; i < (nfds+63)/64; i++) efds[i] = 0;
	r = 0;
	for(i = 0; i < n; i++){
		int fd = pfds[i].fd, hit = 0;
		if(rfds && (pfds[i].revents & (POLLIN|POLLHUP|POLLERR))){ rfds[fd/64] |= 1UL << (fd%64); hit = 1; }
		if(wfds && (pfds[i].revents & (POLLOUT|POLLERR))){ wfds[fd/64] |= 1UL << (fd%64); hit = 1; }
		if(efds && (pfds[i].revents & POLLPRI)){ efds[fd/64] |= 1UL << (fd%64); hit = 1; }
		r += hit;
	}
	return r;
}
