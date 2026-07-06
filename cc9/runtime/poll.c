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
 *   - POLLOUT is reported always-ready. ponytail: Plan 9 pipe writes block
 *     only when the pipe is full and complete soon after; add writer-side
 *     readiness threads if a real workload ever stalls on this.
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
extern void n9_semacquire(int *, int);
extern void n9_semrelease(int *, int);
extern long n9_tsemacquire(int *, long);
extern long n9_errstr(char *, unsigned long);
extern int strncmp(const char *, const char *, unsigned long);
extern char *strstr(const char *, const char *);

#define PFD_MAX 64
#define PFD_BUF 8192

typedef struct {
	int fd;                 /* -1 = free slot */
	int flags;              /* O_NONBLOCK | O_CLOEXEC */
	int reader;             /* reader thread started */
	int dead;               /* fd closed; reader exits on next return */
	int eof, err;
	int lock;               /* binary sem, 1 = unlocked */
	int space;              /* reader waits here when the ring is full */
	int data;               /* blocking cc9_poll_read waiters */
	unsigned head, tail;    /* ring positions: head = fill, tail = drain */
	char buf[PFD_BUF];
} cc9_pfd;

static cc9_pfd tab[PFD_MAX];
static int tab_lock = 1;
static int poll_sem;
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
		char path[128]; int k = 0;
		while(pfx[k] && k < 100){ path[k] = pfx[k]; k++; }
		path[k++] = '.';
		int pid = getpid();
		for(int d = 100000; d; d /= 10) path[k++] = '0' + pid / d % 10;
		path[k] = 0;
		trace_fd = (int)n9_create(path, 1 /*OWRITE*/, 0666);
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

static unsigned ring_avail(cc9_pfd *p){ return p->head - p->tail; }
static unsigned ring_space(cc9_pfd *p){ return PFD_BUF - ring_avail(p); }

static void *reader_main(void *arg){
	cc9_pfd *p = arg;
	char tmp[2048];
	for(;;){
		while(!p->dead && ring_space(p) == 0)
			n9_semacquire(&p->space, 1);
		if(p->dead) break;
		unsigned want = ring_space(p);
		if(want > sizeof tmp) want = sizeof tmp;
		long r = n9_pread(p->fd, tmp, (long)want, -1);
		trace("rdthr", p->fd, r);
		if(r < 0 && was_interrupted() && !p->dead)
			continue;
		n9_semacquire(&p->lock, 1);
		if(r > 0){
			for(long i = 0; i < r; i++)
				p->buf[(p->head + i) % PFD_BUF] = tmp[i];
			p->head += (unsigned)r;
		} else if(r == 0){
			p->eof = 1;
		} else {
			p->err = 1;
		}
		n9_semrelease(&p->lock, 1);
		n9_semrelease(&poll_sem, 1);
		n9_semrelease(&p->data, 1);
		if(r <= 0) break;
	}
	/* dead (fd closed under us): free the slot; eof/err: keep it so
	 * cc9_poll_read can report 0/-1 until close() reclaims it. */
	n9_semacquire(&tab_lock, 1);
	if(p->dead) p->fd = -1;
	else p->reader = 0;
	n9_semrelease(&tab_lock, 1);
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
			p->fd = fd; p->flags = 0; p->reader = 0; p->dead = 0;
			p->eof = p->err = 0; p->lock = 1; p->space = 0; p->data = 0;
			p->head = p->tail = 0;
		}
	}
	if(p && start_reader && !p->reader){
		pthread_t t;
		if(pthread_create(&t, 0, reader_main, p) == 0){
			pthread_detach(t);
			p->reader = 1;
		}
	}
	n9_semrelease(&tab_lock, 1);
	return p;
}

/* ---- hooks for fs.c ---- */

int cc9_poll_owned(int fd){
	cc9_pfd *p = lookup(fd);
	return p && (p->reader || (p->flags & O_NONBLOCK));
}

long cc9_poll_read(int fd, void *buf, long n){
	cc9_pfd *p = lookup(fd);
	if(!p){ errno = EBADF; return -1; }
	if(!p->reader) ensure(fd, 1);
	char *d = buf;
	for(;;){
		n9_semacquire(&p->lock, 1);
		unsigned avail = ring_avail(p);
		if(avail > 0){
			long take = (long)avail < n ? (long)avail : n;
			for(long i = 0; i < take; i++)
				d[i] = p->buf[(p->tail + i) % PFD_BUF];
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

void cc9_poll_onclose(int fd){
	n9_semacquire(&tab_lock, 1);
	if(tab_inited)
		for(int i = 0; i < PFD_MAX; i++)
			if(tab[i].fd == fd && !tab[i].dead){
				if(tab[i].reader){
					tab[i].dead = 1;               /* reader reclaims on return */
					n9_semrelease(&tab[i].space, 1);
				} else
					tab[i].fd = -1;                /* no reader: free now */
			}
	n9_semrelease(&tab_lock, 1);
}

/* FD_CLOEXEC set? (consulted by execve before n9_exec) */
int cc9_poll_cloexec(int fd){
	cc9_pfd *p = lookup(fd);
	return p && (p->flags & O_CLOEXEC);
}

/* ---- POSIX surface ---- */

static long now_ms(void){
	struct timespec ts;
	clock_gettime(0, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
			if(fds[i].events & POLLOUT)
				fds[i].revents |= POLLOUT;         /* writes block-but-complete */
			if(fds[i].revents) ready++;
		}
		if(ready || timeout == 0) return ready;
		if(timeout < 0){
			n9_semacquire(&poll_sem, 1);
		} else {
			long left = deadline - now_ms();       /* stale tokens re-loop; keep the true deadline */
			if(left <= 0) return 0;
			long got = n9_tsemacquire(&poll_sem, left);
			if(got != 1) return 0;                 /* timed out */
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
		if(nfd >= 0 && cmd == F_DUPFD_CLOEXEC){
			cc9_pfd *np = ensure(nfd, 0);
			if(np) np->flags |= O_CLOEXEC;
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
	default:
		return 0;   /* locks etc: pretend success (single-writer norm) */
	}
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
