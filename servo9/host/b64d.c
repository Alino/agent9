/* b64d IN OUT — base64 decode, for kencc on the box. Companion to ship.py.
 *
 * Why this exists: listen1 gives us an rc shell that carries text safely but
 * mangles raw binary, and cc9/host/deliver.py's answer (emit the whole artifact
 * as a C `uchar[]` and compile it with 6c) costs ~6 bytes of source per byte
 * shipped — fine for a 500KB test, hopeless for a 13MB one. So ship base64 text
 * through the same channel and decode it here. This program is small and fixed
 * in size no matter how big the payload is.
 */
#include <u.h>
#include <libc.h>

static int
b64val(int c)
{
	if(c >= 'A' && c <= 'Z')
		return c - 'A';
	if(c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if(c >= '0' && c <= '9')
		return c - '0' + 52;
	if(c == '+')
		return 62;
	if(c == '/')
		return 63;
	return -1;	/* '=', newlines, anything else: skip */
}

static uchar buf[64*1024];
static uchar ob[48*1024];

void
main(int argc, char **argv)
{
	int in, out, v, nbits, no;
	long n, i;
	ulong acc;

	if(argc != 3){
		fprint(2, "usage: b64d in out\n");
		exits("usage");
	}
	if((in = open(argv[1], OREAD)) < 0)
		sysfatal("open %s: %r", argv[1]);
	if((out = create(argv[2], OWRITE, 0644)) < 0)
		sysfatal("create %s: %r", argv[2]);

	acc = 0;
	nbits = 0;
	no = 0;
	while((n = read(in, buf, sizeof buf)) > 0){
		for(i = 0; i < n; i++){
			if((v = b64val(buf[i])) < 0)
				continue;
			acc = (acc << 6) | v;
			nbits += 6;
			if(nbits >= 8){
				nbits -= 8;
				ob[no++] = (acc >> nbits) & 0xff;
				if(no == sizeof ob){
					if(write(out, ob, no) != no)
						sysfatal("write: %r");
					no = 0;
				}
			}
		}
	}
	/* Trailing bits from '=' padding are < 8 and correctly dropped. */
	if(no > 0 && write(out, ob, no) != no)
		sysfatal("write: %r");
	close(out);
	close(in);
	exits(nil);
}
