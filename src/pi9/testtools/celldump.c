/*
 * celldump — debug helper for pi9 / vts cell-stream protocol.
 *
 * Reads one frame from /n/vts/<session>/cells (default
 * /n/vts/1/cells), parses the binary header + cell-diff entries,
 * and prints (row, col) char fg bg attr for each cell.
 *
 * Build on plan9:
 *	6c -FTVw -o celldump.6 celldump.c
 *	6l -o celldump celldump.6
 *
 * Used in Phase 1 verification before vtwin is available. Once vtwin
 * can paint, we can compare visual render against celldump's
 * authoritative cell-state output.
 */
#include <u.h>
#include <libc.h>

enum {
	CD_HEADER = 22,
	CD_CELL = 12,
};

void
main(int argc, char **argv)
{
	int fd;
	uchar buf[131072];
	int n, i, ncells;
	char *path = "/n/vts/1/cells";
	if(argc > 1) path = argv[1];

	fd = open(path, OREAD);
	if(fd < 0) sysfatal("open %s: %r", path);
	n = read(fd, buf, sizeof buf);
	if(n < CD_HEADER) sysfatal("short read %d", n);

	uint magic = buf[0] | (buf[1]<<8) | (buf[2]<<16) | (buf[3]<<24);
	int rows = buf[6] | (buf[7]<<8);
	int cols = buf[8] | (buf[9]<<8);
	ncells = buf[10] | (buf[11]<<8) | (buf[12]<<16) | (buf[13]<<24);
	int curr = buf[14] | (buf[15]<<8);
	int curc = buf[16] | (buf[17]<<8);

	print("frame: n=%d magic=%#ux %dx%d ncells=%d cur=(%d,%d)\n",
		n, magic, rows, cols, ncells, curr, curc);

	uchar *p = buf + CD_HEADER;
	for(i = 0; i < ncells && (p - buf) + CD_CELL <= n; i++){
		int row = p[0] | (p[1]<<8);
		int col = p[2] | (p[3]<<8);
		uint r = p[4] | (p[5]<<8) | (p[6]<<16) | (p[7]<<24);
		int fg = p[8], bg = p[9], at = p[10];
		if(r >= 0x20 && r < 0x80)
			print("(%2d,%2d) %c fg=%d bg=%d a=%d\n", row, col, r, fg, bg, at);
		p += CD_CELL;
	}
	exits(nil);
}
