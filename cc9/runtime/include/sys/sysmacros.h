#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H
/* glibc device-number helpers. Plan 9 has no dev_t major/minor split; these
 * exist so code that reports a device id compiles. intel_perf only uses them to
 * name a DRM card node, which gpu9 does not have. */
#define major(x) ((int)(((x) >> 8) & 0xff))
#define minor(x) ((int)((x) & 0xff))
#define makedev(ma, mi) ((((ma) & 0xff) << 8) | ((mi) & 0xff))
#endif
