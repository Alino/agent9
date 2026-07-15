/* drmshim.c — the 8 libdrm functions iris actually calls. See xf86drm.h. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include "xf86drm.h"

extern int ioctl(int, unsigned long, void *);

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int ret;
	do {
		ret = ioctl(fd, request, arg);
	} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
	return ret;
}

/* iris checks the driver name/version to decide it is talking to i915. Report
 * what the kernel it expects would: gpu9 implements that same uapi. */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr v;
	(void)fd;
	v = calloc(1, sizeof *v);
	if (v == NULL)
		return NULL;
	v->version_major = 1;
	v->version_minor = 6;
	v->version_patchlevel = 0;
	v->name = strdup("i915");
	v->name_len = v->name ? strlen(v->name) : 0;
	v->date = strdup("20200114");
	v->date_len = v->date ? strlen(v->date) : 0;
	v->desc = strdup("gpu9 (Intel Gen8, userspace)");
	v->desc_len = v->desc ? strlen(v->desc) : 0;
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	if (v == NULL)
		return;
	free(v->name); free(v->date); free(v->desc);
	free(v);
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd; (void)handle; (void)flags; (void)prime_fd;
	errno = ENOSYS;		/* no dmabuf on 9front: nothing to share with */
	return -1;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd; (void)prime_fd; (void)handle;
	errno = ENOSYS;
	return -1;
}

int
drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{
	(void)dev_id; (void)flags; (void)device;
	errno = ENOSYS;		/* gpu9 drives one known GPU; no enumeration */
	return -1;
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd; (void)flags; (void)device;
	errno = ENOSYS;		/* gpu9 drives one known GPU; iris falls back */
	return -1;
}

void drmFreeDevice(drmDevicePtr *device) { (void)device; }
void drmFreeDevices(drmDevicePtr devices[], int count) { (void)devices; (void)count; }
