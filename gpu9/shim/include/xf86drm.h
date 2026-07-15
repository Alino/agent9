/*
 * xf86drm.h — minimal libdrm surface for iris on 9front.
 *
 * iris includes libdrm's OWN header (not just Mesa's vendored drm-uapi), but it
 * only touches 8 functions — and the one that matters, drmIoctl(), is a plain
 * ioctl(). cc9's runtime implements ioctl() and dispatches to gpu9 in-process,
 * so there is no kernel driver and no libdrm to port: this header plus that shim
 * IS the "kernel interface".
 *
 * What iris uses (measured, not guessed):
 *   drmIoctl x10        -> ioctl() -> gpu9            THE contract
 *   drmGetVersion/Free  -> a canned i915 version      (iris sanity-checks it)
 *   drmPrimeHandleToFD / drmPrimeFDToHandle -> dmabuf sharing; nothing to share
 *                          with on 9front (no compositor, no other GPU client)
 *   drmGetDeviceFromDevId / drmFreeDevice(s) -> PCI enumeration; gpu9 already
 *                          knows the one GPU it drives
 * The stubs fail cleanly (-1/ENOSYS) so callers take their error branch rather
 * than proceeding on a lie.
 */
#ifndef _XF86DRM_H_
#define _XF86DRM_H_

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _drmVersion {
	int     version_major;
	int     version_minor;
	int     version_patchlevel;
	int     name_len;
	char    *name;
	int     date_len;
	char    *date;
	int     desc_len;
	char    *desc;
} drmVersion, *drmVersionPtr;

typedef struct _drmPciBusInfo {
	uint16_t domain;
	uint8_t  bus, dev, func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmPciDeviceInfo {
	uint16_t vendor_id, device_id, subvendor_id, subdevice_id;
	uint8_t  revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;

#define DRM_BUS_PCI 0
#define DRM_DEVICE_GET_PCI_REVISION (1 << 0)

typedef struct _drmDevice {
	char **nodes;
	int available_nodes;
	int bustype;
	union { drmPciBusInfoPtr pci; } businfo;
	union { drmPciDeviceInfoPtr pci; } deviceinfo;
} drmDevice, *drmDevicePtr;

/* the whole kernel contract: cc9's ioctl() routes this to gpu9 */
extern int drmIoctl(int fd, unsigned long request, void *arg);

extern drmVersionPtr drmGetVersion(int fd);
extern void drmFreeVersion(drmVersionPtr v);

/* dmabuf sharing: nothing to share with on 9front — fail cleanly */
extern int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
extern int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);

/* PCI enumeration: gpu9 already knows its one GPU */
extern int  drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device);
extern int  drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
extern void drmFreeDevice(drmDevicePtr *device);
extern void drmFreeDevices(drmDevicePtr devices[], int count);

#ifdef __cplusplus
}
#endif
#endif
