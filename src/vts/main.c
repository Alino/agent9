/*
 * vts — Plan 9 console daemon ("vt server").
 *
 * Phase 1.3: spawns rc as a child process. Reads rc's output through the
 * VT100 parser into the cell buffer. External clients write keystrokes
 * to /1/cons; reads of /1/cells return binary diff frames.
 *
 *   vts            (default) spawn rc, run forever
 *   vts -n         no rc; useful for in-process tests of the 9P interface
 */
#include "compat.h"
#include "dat.h"
#include "fns.h"

#include <thread.h>

void
threadmain(int argc, char **argv)
{
	int spawn_rc = 1;
	extern char *vts_srvname;

	ARGBEGIN{
	case 'n':
		spawn_rc = 0;
		break;
	case 's':
		vts_srvname = EARGF(sysfatal("usage: vts [-n] [-s srvname]"));
		break;
	}ARGEND;
	USED(argc); USED(argv);

	print("vts: hello (build %s)%s\n", VTBUILD,
		spawn_rc ? "" : " (no rc; -n)");

	srvinit(spawn_rc);
	srvstart();

	/* In thread programs, threadmain returns and the runtime keeps
	 * other threads/procs alive. */
}
