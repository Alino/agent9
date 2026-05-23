#include <u.h>
#include <libc.h>

/*
 * dumpwins — print state of all rio windows.
 *
 * Reads /mnt/wsys/wsys/<id>/{label,wctl} for each visible window and
 * prints one line per window. Used for debugging WM state without
 * resorting to screenshots + vision.
 *
 * wctl format: minx miny maxx maxy {current|notcurrent} {visible|hidden}
 *
 * NOTE: wctl reads BLOCK until the window has pending state changes.
 * A wctl=? in the output means the window is in a steady state — its
 * coordinates haven't changed recently.
 */
void
main(int argc, char *argv[])
{
	int i, fd, n;
	char path[256];
	Dir *d;
	long ndirs;

	USED(argc); USED(argv);

	fd = open("/mnt/wsys/wsys", OREAD);
	if(fd < 0){
		fprint(2, "dumpwins: open /mnt/wsys/wsys: %r\n");
		fprint(2, "  (need to mount /srv/rio.<user>.<pid> on /mnt/wsys first)\n");
		exits("open");
	}
	ndirs = dirreadall(fd, &d);
	close(fd);
	for(i = 0; i < ndirs; i++){
		if(!(d[i].mode & DMDIR))
			continue;

		char label[64] = "?";
		char wctl[256] = "?";

		snprint(path, sizeof path, "/mnt/wsys/wsys/%s/label", d[i].name);
		fd = open(path, OREAD);
		if(fd >= 0){
			n = read(fd, label, sizeof label - 1);
			if(n > 0) label[n] = 0;
			else label[0] = '?';
			close(fd);
		}

		snprint(path, sizeof path, "/mnt/wsys/wsys/%s/wctl", d[i].name);
		fd = open(path, OREAD);
		if(fd >= 0){
			n = read(fd, wctl, sizeof wctl - 1);
			if(n > 0) wctl[n] = 0;
			else wctl[0] = '?';
			close(fd);
		}
		print("id=%s label=%s wctl=%s\n", d[i].name, label, wctl);
	}
	exits(nil);
}
