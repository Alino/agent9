#ifndef _SYS_IOCCOM_H
#define _SYS_IOCCOM_H
/* ioctl command encoding — pulled in by Mesa's vendored drm-uapi/drm.h through
 * llvmpipe's lp_texture.c. 9front has no ioctl; these macros only need to yield
 * compile-time constants (the DRM ioctl paths are never taken on 9front). */
#define _IOC(inout,group,num,len) (((inout)|(((len)&0x1fff)<<16)|((group)<<8)|(num)))
#define IOC_VOID  0x20000000
#define IOC_OUT   0x40000000
#define IOC_IN    0x80000000
#define IOC_INOUT (IOC_IN|IOC_OUT)
#define _IO(g,n)      _IOC(IOC_VOID,  (g), (n), 0)
#define _IOR(g,n,t)   _IOC(IOC_OUT,   (g), (n), sizeof(t))
#define _IOW(g,n,t)   _IOC(IOC_IN,    (g), (n), sizeof(t))
#define _IOWR(g,n,t)  _IOC(IOC_INOUT, (g), (n), sizeof(t))
#endif
