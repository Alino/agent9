/* libuv platform file for 9front-via-cc9 (neovim9). Compiled INTO libuv
 * alongside posix-poll.c / no-fsevents.c / no-proctitle.c / posix-hrtime.c
 * (the cygwin recipe). Sockets are ENOSYS in cc9, so the uv_tcp/uv_udp/
 * getaddrinfo surface fails at runtime with honest errors while everything
 * pipe/process/tty works. */
#include "uv.h"
#include "internal.h"

#include <string.h>
#include <unistd.h>

int uv_uptime(double* uptime) {
  *uptime = 0;   /* ponytail: nothing in nvim reads it; /dev/time if ever needed */
  return 0;
}

int uv_resident_set_memory(size_t* rss) {
  *rss = 0;
  return 0;
}

int uv_cpu_info(uv_cpu_info_t** cpu_infos, int* count) {
  *cpu_infos = NULL;
  *count = 0;
  return UV_ENOSYS;
}

uint64_t uv_get_constrained_memory(void) { return 0; }
uint64_t uv_get_available_memory(void) { return uv_get_free_memory(); }
uint64_t uv_get_free_memory(void) { return 256 * 1024 * 1024; }
uint64_t uv_get_total_memory(void) { return 1024ull * 1024 * 1024; }

void uv_loadavg(double avg[3]) { avg[0] = avg[1] = avg[2] = 0; }

int uv_interface_addresses(uv_interface_address_t** addresses, int* count) {
  *addresses = NULL;
  *count = 0;
  return 0;
}
/* uv_free_interface_addresses: generic impl in uv-common.c */

/* exepath: cc9 crt0 stashes argv[0]. Absolute -> as is; contains '/' ->
 * cwd-relative; bare name -> /bin (the 9front union bin). */
extern const char *__cc9_argv0;
int uv_exepath(char* buffer, size_t* size) {
  char tmp[512];
  const char *a0 = __cc9_argv0;
  if (a0[0] == '/') {
    strncpy(tmp, a0, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
  } else if (strchr(a0, '/')) {
    if (getcwd(tmp, sizeof(tmp)) == NULL)
      return UV_ENOENT;
    size_t n = strlen(tmp);
    snprintf(tmp + n, sizeof(tmp) - n, "/%s", a0);
  } else {
    snprintf(tmp, sizeof(tmp), "/bin/%s", a0);
  }
  size_t len = strlen(tmp);
  if (len >= *size)
    len = *size - 1;
  memcpy(buffer, tmp, len);
  buffer[len] = 0;
  *size = len;
  return 0;
}
