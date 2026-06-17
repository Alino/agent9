#ifndef __APE_SHIM_LANGINFO_H
#define __APE_SHIM_LANGINFO_H
/*
 * Minimal <langinfo.h> for the 9front/APE CPython port. APE has no langinfo;
 * CPython's core only uses nl_langinfo(CODESET) (Python/fileutils.c,
 * Python/pylifecycle.c) to learn the locale encoding. Plan 9 is UTF-8
 * everywhere, so report that. The full nl_item set (used by the _locale
 * module, not in the core build) can be added later.
 */

typedef int nl_item;

#define CODESET 1

static char *
nl_langinfo(nl_item item)
{
    (void)item;
    return (char *)"UTF-8";
}

#endif /* __APE_SHIM_LANGINFO_H */
