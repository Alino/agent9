/* elf2aout.c — C port of elf2aout.py, compiled with cc9 to run NATIVELY on 9front
 * (no python9 on the box). Converts a statically-linked x86_64 ELF laid out by
 * cc9/test/plan9.ld into a Plan 9 amd64 a.out. Completes the on-box toolchain
 * (clang -> ld.lld -> elf2aout, all on 9front).
 *
 * Plan 9 a.out (amd64): 40-byte big-endian header
 *   magic(0x8A97) text data bss syms entry32 spsz pcsz (8x u32) + u64 entry
 * File: [40B hdr][text][data]. Kernel maps text@0x200028, data@roundup(...,2MB).
 *
 * Usage: elf2aout in.elf out.aout
 */
extern int  n9_open(const char *, int);
extern int  n9_create(const char *, int, int);
extern long n9_seek(long long *, int, long long, int);
extern long n9_pread(int, void *, long, long long);
extern long n9_pwrite(int, const void *, long, long long);
extern int  n9_close(int);
extern void n9_exits(const char *);
extern void *malloc(unsigned long);

typedef unsigned long u64; typedef unsigned int u32; typedef unsigned short u16;

static void die(const char *m){ n9_pwrite(2, "elf2aout: ", 10, -1); int n=0; while(m[n])n++; n9_pwrite(2,m,n,-1); n9_pwrite(2,"\n",1,-1); n9_exits("elf2aout error"); }
static u16 rd16(unsigned char *b){ return b[0] | (b[1]<<8); }
static u32 rd32(unsigned char *b){ return (u32)b[0] | ((u32)b[1]<<8) | ((u32)b[2]<<16) | ((u32)b[3]<<24); }
static u64 rd64(unsigned char *b){ u64 v=0; for(int i=7;i>=0;i--) v=(v<<8)|b[i]; return v; }
/* big-endian store */
static void be32(unsigned char *p, u32 v){ p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static void be64(unsigned char *p, u64 v){ for(int i=0;i<8;i++) p[i]=(unsigned char)(v>>((7-i)*8)); }

int main(int argc, char **argv){
	if(argc < 3) die("usage: elf2aout in.elf out.aout");
	int fd = n9_open(argv[1], 0 /*OREAD*/);
	if(fd < 0) die("cannot open input");
	long long sz=0; n9_seek(&sz, fd, 0, 2 /*SEEK_END*/);
	if(sz <= 0) die("empty/seek-fail input");
	unsigned char *elf = (unsigned char *)malloc((u64)sz);
	if(!elf) die("oom reading elf");
	long got = n9_pread(fd, elf, (long)sz, 0); n9_close(fd);
	if(got != (long)sz) die("short read");

	if(!(elf[0]==0x7f && elf[1]=='E' && elf[2]=='L' && elf[3]=='F' && elf[4]==2 && elf[5]==1)) die("need LE 64-bit ELF");
	u64 e_entry = rd64(elf+0x18);
	u64 e_phoff = rd64(elf+0x20);
	u16 e_phentsize = rd16(elf+0x36);
	u16 e_phnum = rd16(elf+0x38);

	const u64 UTZERO=0x200000, HDRSZ=40, TEXTVA=UTZERO+HDRSZ, DATA_ALIGN=0x200000;
	u64 t_off=0,t_vaddr=0,t_filesz=0; int have_t=0;
	u64 d_off=0,d_vaddr=0,d_filesz=0,d_memsz=0; int have_d=0;
	for(u16 i=0;i<e_phnum;i++){
		unsigned char *o = elf + e_phoff + (u64)i*e_phentsize;
		if(rd32(o) != 1) continue;          /* PT_LOAD */
		u32 flags = rd32(o+0x04);
		u64 off=rd64(o+0x08), vaddr=rd64(o+0x10), filesz=rd64(o+0x20), memsz=rd64(o+0x28);
		if(flags & 0x1){ t_off=off; t_vaddr=vaddr; t_filesz=filesz; have_t=1; }
		else if(flags & 0x2){ d_off=off; d_vaddr=vaddr; d_filesz=filesz; d_memsz=memsz; have_d=1; }
	}
	if(!have_t) die("no R+X (text) segment");
	if(t_vaddr < TEXTVA) die("text vaddr < 0x200028");

	u64 tpad = t_vaddr - TEXTVA;            /* leading pad if linker bumped text base */
	u64 textsz = tpad + t_filesz;
	u64 datasz = 0, bss = 0;
	if(have_d){
		u64 expect = (TEXTVA + textsz + DATA_ALIGN - 1) & ~(DATA_ALIGN - 1);
		if(d_vaddr != expect) die("data vaddr != kernel-expected 2MB round");
		datasz = d_filesz; bss = d_memsz - d_filesz;
	}
	if(textsz>=(1ULL<<32) || datasz>=(1ULL<<32) || bss>=(1ULL<<32)) die("segment >= 4GB (32-bit a.out field overflow)");

	/* assemble: [40B hdr][text][data] */
	u64 outsz = HDRSZ + textsz + datasz;
	unsigned char *out = (unsigned char *)malloc(outsz);
	if(!out) die("oom output");
	for(u64 i=0;i<outsz;i++) out[i]=0;
	be32(out+0,  0x8A97);          /* magic */
	be32(out+4,  (u32)textsz);
	be32(out+8,  (u32)datasz);
	be32(out+12, (u32)bss);
	be32(out+16, 0);               /* syms */
	be32(out+20, (u32)(e_entry & 0xffffffff));
	be32(out+24, 0);               /* spsz */
	be32(out+28, 0);               /* pcsz */
	be64(out+32, e_entry);
	/* text (with leading pad already zeroed) */
	for(u64 i=0;i<t_filesz;i++) out[HDRSZ + tpad + i] = elf[t_off + i];
	/* data */
	for(u64 i=0;i<datasz;i++) out[HDRSZ + textsz + i] = elf[d_off + i];

	int ofd = n9_create(argv[2], 1 /*OWRITE*/, 0775);
	if(ofd < 0) die("cannot create output");
	long w = n9_pwrite(ofd, out, (long)outsz, 0); n9_close(ofd);
	if(w != (long)outsz) die("short write");
	n9_pwrite(1, "elf2aout: ok\n", 13, -1);
	n9_exits(0);
	return 0;
}
