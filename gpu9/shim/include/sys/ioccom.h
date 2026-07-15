/*
 * sys/ioccom.h — ioctl request encoding for the gpu9 shim.
 *
 * drm-uapi/drm.h takes its "one of the BSDs" branch on 9front (no __linux__)
 * and includes this. But iris itself was compiled in a Linux container, where
 * drm.h took the __linux__ branch and used asm-generic's _IOC encoding. The
 * DRM_IOCTL_I915_* request NUMBERS are baked into the iris archive with that
 * encoding, so this shim MUST reproduce it exactly — a BSD _IOC layout would
 * make iris and gpu9 disagree on every request code.
 *
 * Linux asm-generic/ioctl.h layout:  dir[31:30] size[29:16] type[15:8] nr[7:0]
 */
#ifndef _GPU9_SYS_IOCCOM_H_
#define _GPU9_SYS_IOCCOM_H_

#define _IOC_NRBITS	8
#define _IOC_TYPEBITS	8
#define _IOC_SIZEBITS	14
#define _IOC_DIRBITS	2

#define _IOC_NRSHIFT	0
#define _IOC_TYPESHIFT	(_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT	(_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT	(_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE	0U
#define _IOC_WRITE	1U
#define _IOC_READ	2U

#define _IOC(dir,type,nr,size) \
	(((dir)  << _IOC_DIRSHIFT)  | \
	 ((type) << _IOC_TYPESHIFT) | \
	 ((nr)   << _IOC_NRSHIFT)   | \
	 ((size) << _IOC_SIZESHIFT))

#define _IO(type,nr)		_IOC(_IOC_NONE,(type),(nr),0)
#define _IOR(type,nr,size)	_IOC(_IOC_READ,(type),(nr),sizeof(size))
#define _IOW(type,nr,size)	_IOC(_IOC_WRITE,(type),(nr),sizeof(size))
#define _IOWR(type,nr,size)	_IOC(_IOC_READ|_IOC_WRITE,(type),(nr),sizeof(size))

#endif
