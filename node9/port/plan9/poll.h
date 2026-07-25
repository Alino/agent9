#ifndef NODE9_POLL_H
#define NODE9_POLL_H
/* poll() implemented over APE's select() — APE multiplexes fds with Plan 9 helper
   procs internally, i.e. this IS the rfork-based native event loop, via a tested path. */
#include <sys/time.h>
#include <sys/select.h>
struct pollfd { int fd; short events; short revents; };
typedef unsigned long nfds_t;
#define POLLIN 1
#define POLLPRI 2
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLNVAL 32
/* NODE9_POLLTRACE=1 -> /tmp/n9poll: one line per call, "<ms> to=<timeout> in=<fd:ev,..>
   n=<select ret> out=<fd:revents,..>". Only for debugging the event loop. */
static void n9_polltrace(struct pollfd *fds, nfds_t nfds, int timeout, int n, int after){
    static int checked = 0, on = 0;
    FILE *f;
    nfds_t i;
    struct timeval now;
    if(!checked){ checked = 1; on = getenv("NODE9_POLLTRACE") != 0; }
    if(!on) return;
    f = fopen("/tmp/n9poll", "a");
    if(!f) return;
    gettimeofday(&now, 0);
    fprintf(f, "%ld.%03ld %s to=%d n=%d fds=", (long)now.tv_sec, (long)(now.tv_usec/1000),
            after ? "post" : "pre", timeout, n);
    for(i = 0; i < nfds; i++)
        fprintf(f, "%d:%d/%d,", fds[i].fd, fds[i].events, after ? fds[i].revents : 0);
    fprintf(f, "\n");
    fclose(f);
}

/* Same switch: log what the event loop decided BEFORE it gets as far as poll(). */
static void n9_looptrace(const char *what, int a, int b){
    static int checked = 0, on = 0;
    FILE *f;
    struct timeval now;
    if(!checked){ checked = 1; on = getenv("NODE9_POLLTRACE") != 0; }
    if(!on) return;
    f = fopen("/tmp/n9poll", "a");
    if(!f) return;
    gettimeofday(&now, 0);
    fprintf(f, "%ld.%03ld LOOP %s a=%d b=%d\n", (long)now.tv_sec, (long)(now.tv_usec/1000), what, a, b);
    fclose(f);
}

static int poll(struct pollfd *fds, nfds_t nfds, int timeout){
    fd_set rfds, wfds, efds;
    struct timeval tv, *ptv;
    int maxfd, n, ready;
    nfds_t i;
    FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
    maxfd = -1;
    for(i = 0; i < nfds; i++){
        fds[i].revents = 0;
        if(fds[i].fd < 0) continue;
        if(fds[i].events & POLLIN)  FD_SET(fds[i].fd, &rfds);
        if(fds[i].events & POLLOUT) FD_SET(fds[i].fd, &wfds);
        FD_SET(fds[i].fd, &efds);
        if(fds[i].fd > maxfd) maxfd = fds[i].fd;
    }
    ptv = 0;
    if(timeout >= 0){
        /* APE's select() multiplexes with helper procs, which it cannot even start within a
         * zero timeout: select(tv=0) returns 0 without reporting fds that are already
         * readable. An event loop that polls with 0 (because a timer is due right now)
         * would therefore never service its sockets. Give it 1ms of real time instead. */
        if(timeout == 0) timeout = 1;
        tv.tv_sec = timeout/1000; tv.tv_usec = (timeout%1000)*1000; ptv = &tv;
    }
    n9_polltrace(fds, nfds, timeout, -1, 0);
    n = select(maxfd+1, &rfds, &wfds, &efds, ptv);
    if(n <= 0){ n9_polltrace(fds, nfds, timeout, n, 1); return n; }
    ready = 0;
    for(i = 0; i < nfds; i++){
        short re = 0;
        if(fds[i].fd < 0) continue;
        if(FD_ISSET(fds[i].fd, &rfds)) re |= POLLIN;
        if(FD_ISSET(fds[i].fd, &wfds)) re |= POLLOUT;
        if(FD_ISSET(fds[i].fd, &efds)) re |= POLLERR;
        re &= (fds[i].events | POLLERR | POLLHUP | POLLNVAL);
        fds[i].revents = re;
        if(re) ready++;
    }
    n9_polltrace(fds, nfds, timeout, n, 1);
    return ready;
}
#endif
