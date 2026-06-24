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
    if(timeout >= 0){ tv.tv_sec = timeout/1000; tv.tv_usec = (timeout%1000)*1000; ptv = &tv; }
    n = select(maxfd+1, &rfds, &wfds, &efds, ptv);
    if(n <= 0) return n;
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
    return ready;
}
#endif
