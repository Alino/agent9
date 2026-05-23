/*
 * keysniff.c — read raw bytes + decoded runes from cons, log both.
 *
 * Diagnostic tool for discovering what key combos produce on a given
 * keyboard / VNC client setup. Run inside the VM:
 *
 *     keysniff
 *     <press whatever keys you want to identify>
 *     <press Delete to exit>
 *
 * Then: cat /tmp/keysniff.log
 *
 * Output includes raw byte sequences AND decoded plan9 runes so you
 * can see when a key produces non-UTF-8 garbage (collapsed to U+FFFD
 * by chartorune but the raw bytes might still be unique).
 *
 * Used to discover that Mac+VNC sends Cmd+C as 0x03 and Cmd+V as 0x16
 * (the VNC client maps Cmd → Ctrl). See wiki/log.md entry for the
 * full story.
 */
#include <u.h>
#include <libc.h>

void
main(void)
{
	Rune r;
	uchar buf[16];
	int n, i, lf;

	lf = create("/tmp/keysniff.log", OWRITE|OTRUNC, 0666);
	if(lf < 0){
		fprint(2, "keysniff: cannot create log: %r\n");
		exits("log");
	}

	print("keysniff: press keys (Cmd+C, Option+V, F-keys, etc).\n");
	print("Output saved to /tmp/keysniff.log with raw bytes.\n");
	print("Press Delete to quit.\n");
	fprint(lf, "=== keysniff started ===\n");

	while((n = read(0, buf, sizeof buf)) > 0){
		fprint(lf, "READ %d bytes:", n);
		for(i = 0; i < n; i++)
			fprint(lf, " %02x", buf[i]);
		fprint(lf, "\n");
		print("READ %d bytes:", n);
		for(i = 0; i < n; i++)
			print(" %02x", buf[i]);
		print("\n");

		i = 0;
		while(i < n){
			int w = chartorune(&r, (char*)buf+i);
			fprint(lf, "  rune 0x%04x dec=%d", r, r);
			print("  rune 0x%04x dec=%d", r, r);
			if(r >= 0x20 && r < 0x7f){
				fprint(lf, " char='%C'", r);
				print(" char='%C'", r);
			}
			fprint(lf, "\n");
			print("\n");
			i += w;
		}
	}
	close(lf);
	exits(nil);
}
