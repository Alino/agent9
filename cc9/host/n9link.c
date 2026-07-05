/*
 * n9link — a static ELF x86-64 -> Plan 9 amd64 a.out linker for rustc's
 * x86_64-unknown-plan9 output. Runs ON 9front (built by cc9) so rustc can link
 * self-hosted; also builds native for host testing.
 *
 * It reproduces exactly what rust9-ld + plan9.ld + elf2aout.py do:
 *   - read rustc's ELF relocatable objects + .rlib/.a archives (ar format),
 *   - pull archive members transitively to satisfy undefined symbols
 *     (whole-archive-group fixpoint, so circular core<->alloc<->builtins works),
 *   - dedup COMDAT (SHT_GROUP) sections (the cc9 C++/unwind runtime has ~1700),
 *   - merge input sections by name into text (.text/.rodata/.gcc_except_table/
 *     .eh_frame/.init_array/.fini_array) and data (.data/.data.rel.ro/.got/.bss/
 *     .cc9stack), laid out at text=0x200028, data=roundup(text_end,0x200000),
 *   - synthesize the boundary symbols the runtime needs,
 *   - apply the 5 reloc types rustc emits (64/32/32S/PC32/PLT32 -> S+A, S+A-P),
 *   - emit the 40-byte big-endian a.out header + text + data + a Plan 9 symtab.
 *
 * Usage:  n9link [gnu-ld/gcc-driver args: objects, archives, -o OUT, flags...]
 *   The cc9 substrate (libcc9cxx.a, libcc9m.a, the 3-symbol unwind shim object)
 *   is appended automatically; override the dir with $N9LINK_LIB.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

typedef uint64_t u64; typedef int64_t s64;
typedef uint32_t u32; typedef int32_t s32;
typedef uint16_t u16; typedef uint8_t u8;

/* ---- ELF64 ---- */
#define ET_REL 1
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_GROUP 17
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHF_TLS 0x400
#define GRP_COMDAT 1
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_SECTION 3
#define STT_FILE 4
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_PLT32 4
#define R_X86_64_GOTPCREL 9
#define R_X86_64_GOTPCRELX 41
#define R_X86_64_REX_GOTPCRELX 42

typedef struct { u8 ident[16]; u16 type, machine; u32 version; u64 entry, phoff, shoff;
    u32 flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { u32 name, type; u64 flags, addr, offset, size; u32 link, info; u64 align, entsize; } Shdr;
typedef struct { u32 name; u8 info, other; u16 shndx; u64 value, size; } Sym;
typedef struct { u64 offset, info; s64 addend; } Rela;
#define ELF_ST_BIND(i) ((i)>>4)
#define ELF_ST_TYPE(i) ((i)&0xf)
#define R_SYM(i)  ((u32)((i)>>32))
#define R_TYPE(i) ((u32)((i)&0xffffffff))

/* ---- little helpers ---- */
static void die(const char *m){ fprintf(stderr,"n9link: %s\n",m); exit(1); }
static void *xmalloc(size_t n){ void *p=calloc(1,n?n:1); if(!p) die("oom"); return p; }
static u32 rd32(const u8*p){ return p[0]|p[1]<<8|p[2]<<16|(u32)p[3]<<24; }

/* ---- input objects ---- */
typedef struct Obj {
    char *name;                 /* for diagnostics */
    u8 *img; long len;          /* whole ELF object image */
    Ehdr *eh;
    Shdr *sh;                   /* section headers (points into img) */
    int nsh;
    const char *shstr;          /* section-name strtab */
    Sym *sym; int nsym;         /* .symtab */
    const char *symstr;         /* symbol strtab */
    int symsh;                  /* index of .symtab section (-1 none) */
    /* per-section state */
    int *keep;                  /* [nsh] 1 if section goes into output */
    u64 *outaddr;               /* [nsh] final vaddr of the section */
    int *outseg;                /* [nsh] output segment class (see SEG_*) */
    int *relaof;                /* [nsh] index of the .rela section for section i, or -1 */
    int *got_ent;               /* [nsym] GOT-slot index for a *GOTPCREL* target symbol, or -1 */
    int included;               /* pulled into the link */
    struct Obj *next;
} Obj;
static int n_got;               /* number of GOT slots */
static u64 got_base;            /* vaddr of GOT slot 0 */

/* output segment classes (order = final layout order within text/data) */
enum { SEG_NONE=-1,
    SEG_TEXT, SEG_RODATA, SEG_EXCEPT, SEG_EHFRAME, SEG_INITARR, SEG_FINIARR, /* text */
    SEG_DATA, SEG_GOT, SEG_BSS, SEG_STACK,                                    /* data */
    SEG_MAX };
static int seg_is_text(int s){ return s>=SEG_TEXT && s<=SEG_FINIARR; }

/* classify an input section by name+flags -> output segment, or SEG_NONE to drop */
static int classify(const char *n, u32 type, u64 flags){
    if(type==SHT_NOBITS){
        if(!strcmp(n,".cc9stack")) return SEG_STACK;
        return SEG_BSS;                              /* .bss, .bss.* */
    }
    if(!(flags&SHF_ALLOC)) return SEG_NONE;          /* non-alloc: drop (.comment,.debug,...) */
    if(!strncmp(n,".text",5)) return SEG_TEXT;
    if(!strncmp(n,".rodata",7)) return SEG_RODATA;
    if(!strncmp(n,".gcc_except_table",17)) return SEG_EXCEPT;
    if(!strcmp(n,".eh_frame")) return SEG_EHFRAME;
    if(!strncmp(n,".init_array",11)) return SEG_INITARR;
    if(!strncmp(n,".fini_array",11)) return SEG_FINIARR;
    if(!strncmp(n,".data.rel.ro",12)) return SEG_DATA;
    if(!strncmp(n,".data",5)) return SEG_DATA;
    if(!strcmp(n,".got")||!strcmp(n,".got.plt")||!strcmp(n,".igot.plt")) return SEG_GOT;
    if(!strcmp(n,".eh_frame_hdr")) return SEG_NONE;  /* discarded */
    /* any other alloc section (rare) -> data to be safe */
    return SEG_DATA;
}

/* ---- global symbol table ---- */
typedef struct GSym {
    const char *name;
    Obj *obj;                   /* defining object (0 = undefined/synthetic) */
    int shndx;                  /* section index in obj (SHN_ABS for synthetic) */
    u64 value;                  /* st_value (offset in section) or absolute */
    int bind;                   /* STB_* of the def */
    int defined;
    u64 finalval;               /* resolved absolute address (filled at layout) */
    int needs_got;              /* referenced via *GOTPCREL* -> gets a GOT slot */
    u64 got_addr;               /* vaddr of this symbol's 8-byte GOT slot */
    struct GSym *next;
} GSym;

#define HN 65536
static GSym *ghash[HN];
static unsigned hashstr(const char *s){ unsigned h=2166136261u; while(*s){h^=(u8)*s++; h*=16777619u;} return h & (HN-1); }
static GSym *gget(const char *name){
    unsigned h=hashstr(name);
    for(GSym *g=ghash[h]; g; g=g->next) if(!strcmp(g->name,name)) return g;
    GSym *g=xmalloc(sizeof *g); g->name=name; g->next=ghash[h]; ghash[h]=g; return g;
}

static Obj *objs_head, *objs_tail;
static void add_obj_to_list(Obj *o){ if(objs_tail) objs_tail->next=o; else objs_head=o; objs_tail=o; o->next=0; }

/* parse an ELF object image into an Obj (does not resolve symbols yet) */
static Obj *parse_obj(const char *name, u8 *img, long len){
    if(len<64 || memcmp(img,"\177ELF",4) || img[4]!=2 /*ELF64*/ || img[5]!=1 /*LE*/) return 0;
    Ehdr *eh=(Ehdr*)img;
    if(eh->type!=ET_REL || eh->machine!=62 /*x86-64*/) return 0;
    Obj *o=xmalloc(sizeof *o);
    o->name=strdup(name); o->img=img; o->len=len; o->eh=eh;
    o->sh=(Shdr*)(img+eh->shoff); o->nsh=eh->shnum;
    o->shstr=(const char*)(img + o->sh[eh->shstrndx].offset);
    o->symsh=-1;
    for(int i=0;i<o->nsh;i++) if(o->sh[i].type==SHT_SYMTAB){ o->symsh=i; break; }
    if(o->symsh>=0){
        Shdr *ss=&o->sh[o->symsh];
        o->sym=(Sym*)(img+ss->offset); o->nsym=ss->size/sizeof(Sym);
        o->symstr=(const char*)(img + o->sh[ss->link].offset);
    } else { o->sym=0; o->nsym=0; o->symstr=""; }
    o->keep=xmalloc(sizeof(int)*o->nsh);
    o->outaddr=xmalloc(sizeof(u64)*o->nsh);
    o->outseg=xmalloc(sizeof(int)*o->nsh);
    o->relaof=xmalloc(sizeof(int)*o->nsh);
    o->got_ent=xmalloc(sizeof(int)*(o->nsym?o->nsym:1));
    for(int i=0;i<o->nsym;i++) o->got_ent[i]=-1;
    for(int i=0;i<o->nsh;i++){ o->outseg[i]=SEG_NONE; o->relaof[i]=-1; }
    /* map each .rela.X to its target section (sh_info) */
    for(int i=0;i<o->nsh;i++) if(o->sh[i].type==SHT_RELA && o->sh[i].info<(u32)o->nsh) o->relaof[o->sh[i].info]=i;
    return o;
}

static const char *secname(Obj *o,int i){ return o->shstr + o->sh[i].name; }
static const char *symname(Obj *o,Sym *s){ return o->symstr + s->name; }

/* Register an object's global defs; returns nothing. Locals stay object-private. */
static void register_defs(Obj *o){
    for(int i=1;i<o->nsym;i++){
        Sym *s=&o->sym[i];
        int bind=ELF_ST_BIND(s->info), typ=ELF_ST_TYPE(s->info);
        if(bind==STB_LOCAL) continue;
        if(typ==STT_FILE) continue;
        const char *nm=symname(o,s);
        if(!nm[0]) continue;
        if(s->shndx==SHN_UNDEF) continue;         /* undefined: not a def */
        GSym *g=gget(nm);
        if(g->defined){
            if(g->bind==STB_GLOBAL && bind==STB_GLOBAL) continue; /* keep first strong (common in archives) */
            if(g->bind==STB_GLOBAL) continue;      /* strong beats weak */
            /* existing weak, new strong -> replace */
        }
        g->obj=o; g->shndx=s->shndx; g->value=s->value; g->bind=bind; g->defined=1;
    }
}

/* Does this object define any currently-undefined-and-needed symbol? (archive pull test) */
static int obj_satisfies(Obj *o){
    for(int i=1;i<o->nsym;i++){
        Sym *s=&o->sym[i];
        if(ELF_ST_BIND(s->info)==STB_LOCAL) continue;
        if(s->shndx==SHN_UNDEF) continue;
        const char *nm=symname(o,s); if(!nm[0]) continue;
        GSym *g=gget(nm);
        if(g && !g->defined) return 1;   /* we have an undefined ref this obj can satisfy */
    }
    return 0;
}

/* record all undefined refs of an object as needed (defined=0 entries) */
static void note_undef(Obj *o){
    for(int i=1;i<o->nsym;i++){
        Sym *s=&o->sym[i];
        if(s->shndx!=SHN_UNDEF) continue;
        if(ELF_ST_BIND(s->info)==STB_LOCAL) continue;
        const char *nm=symname(o,s); if(!nm[0]) continue;
        gget(nm); /* ensure entry exists (defined stays 0 until satisfied) */
    }
}

static void include_obj(Obj *o){
    if(o->included) return;
    o->included=1;
    add_obj_to_list(o);
    register_defs(o);
    note_undef(o);
}

/* ---- archives (ar) ---- */
typedef struct { u8 *img; long len; char *name; } Arch;
static Arch *archs[256]; static int narch;

/* iterate ar members; for each ELF member, call fn(name,data,len) */
static long ar_num(const char *p,int n){ char b[16]; int i=0,j=0; for(;i<n;i++) if(p[i]!=' ') b[j++]=p[i]; b[j]=0; return b[0]?strtol(b,0,10):0; }

/* Pull members from all archives repeatedly until fixpoint (whole-group semantics). */
static void link_archives(void){
    int changed=1;
    while(changed){
        changed=0;
        for(int a=0;a<narch;a++){
            Arch *ar=archs[a];
            u8 *p=ar->img+8; u8 *end=ar->img+ar->len; /* skip "!<arch>\n" */
            const char *longnames=0; long longlen=0;
            while(p+60<=end){
                char nmf[17]; memcpy(nmf,p,16); nmf[16]=0;
                long msize=ar_num((char*)p+48,10);
                u8 *mdata=p+60;
                char mname[256]; mname[0]=0;
                if(!strncmp(nmf,"//",2)){ longnames=(const char*)mdata; longlen=msize; }
                else if(nmf[0]=='/' && nmf[1]>='0'&&nmf[1]<='9'){
                    long off=strtol(nmf+1,0,10);
                    if(longnames && off<longlen){ int j=0; while(off+j<longlen && longnames[off+j]!='\n' && longnames[off+j]!='/' && j<255){mname[j]=longnames[off+j];j++;} mname[j]=0; }
                } else if(nmf[0]!='/'){ int j=0; while(j<16 && nmf[j]!='/' && nmf[j]!=' '){mname[j]=nmf[j];j++;} mname[j]=0; }
                if(mname[0] && !(mname[0]=='_'&&mname[1]=='_')){ /* skip symbol-index members "/" "//" and __.SYMDEF */
                    Obj *o=parse_obj(mname,mdata,msize);
                    if(o && !o->included && obj_satisfies(o)){ include_obj(o); changed=1; }
                    else if(o) free(o); /* not needed yet; re-parsed next pass (cheap) */
                }
                p=mdata+msize; if(msize&1) p++; /* 2-byte align */
            }
        }
    }
}

/* ---- COMDAT dedup ---- */
/* For each object, process its SHT_GROUP sections. Keep the first group per
 * signature name globally; in later duplicates, drop the group's member sections. */
typedef struct GKey { const char *sig; struct GKey *next; } GKey;
static GKey *seen_groups;
static int group_seen(const char *sig){
    for(GKey *k=seen_groups;k;k=k->next) if(!strcmp(k->sig,sig)) return 1;
    GKey *k=xmalloc(sizeof *k); k->sig=sig; k->next=seen_groups; seen_groups=k; return 0;
}

/* Resolve the final absolute address a reloc's symbol refers to (locals, section
 * symbols, and globals alike). Must be called after layout+resolution. Sets
 * *undef=1 if a needed global is undefined. */
static u64 symval(Obj *o, u32 symi, int *undef){
    if(undef) *undef=0;
    Sym *s=&o->sym[symi];
    if(ELF_ST_TYPE(s->info)==STT_SECTION){
        if(s->shndx<(u16)o->nsh && o->keep[s->shndx]) return o->outaddr[s->shndx];
        return 0;
    }
    if(s->shndx==SHN_UNDEF || (ELF_ST_BIND(s->info)!=STB_LOCAL)){
        GSym *g=gget(symname(o,s));
        if(!g->defined){ if(undef && ELF_ST_BIND(s->info)!=STB_WEAK) *undef=1; return 0; }
        return g->finalval;
    }
    /* local defined symbol */
    if(s->shndx<(u16)o->nsh && o->keep[s->shndx]) return o->outaddr[s->shndx]+s->value;
    return s->value;
}

/* ---- main link ---- */
static u8 *rdfile(const char *path, long *plen){
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    u8 *b=xmalloc(n); if(fread(b,1,n,f)!=(size_t)n){ fclose(f); return 0; } fclose(f);
    *plen=n; return b;
}

/* output image buffers */
static u8 *timg; static u64 tsize, tvaddr=0x200028;
static u8 *dimg; static u64 dfilesz, dvaddr, dmemsz;
static u64 sym_of(const char*); /* fwd */

int main(int argc, char **argv){
    const char *out=0;
    const char *libdir=getenv("N9LINK_LIB"); if(!libdir) libdir="/usr/glenda/rust/cc9lib";
    char *inputs[4096]; int nin=0;

    /* ---- parse gnu-ld / gcc-driver args ---- */
    for(int i=1;i<argc;i++){
        char *a=argv[i];
        if(!strcmp(a,"-o")){ out=argv[++i]; continue; }
        if(!strncmp(a,"-o",2)&&a[2]){ out=a+2; continue; }
        if(!strcmp(a,"-L")){ i++; continue; }          /* lib search dir: we resolve full paths */
        if(!strncmp(a,"-L",2)) continue;
        if(!strncmp(a,"-Wl,",4)) continue;             /* driver->linker opts: all ignorable here */
        if(!strncmp(a,"-fuse-ld",8)) continue;
        if(!strcmp(a,"-no-pie")||!strcmp(a,"-nopie")||!strcmp(a,"-pie")) continue;
        if(!strcmp(a,"-nodefaultlibs")||!strcmp(a,"-nostdlib")||!strcmp(a,"-static")) continue;
        if(!strcmp(a,"-flavor")){ i++; continue; }
        if(!strcmp(a,"-z")){ i++; continue; }
        if(a[0]=='-') continue;                        /* any other flag: ignore */
        if(nin<4096) inputs[nin++]=a;                  /* an object or archive path */
    }
    if(!out) die("no -o output");

    /* ---- load the cc9 substrate after the user inputs (satisfies std's externs) ---- */
    char shim[512], libcxx[512], libm[512];
    snprintf(shim,sizeof shim,"%s/n9unwind.o",libdir);
    snprintf(libcxx,sizeof libcxx,"%s/libcc9cxx.a",libdir);
    snprintf(libm,sizeof libm,"%s/libcc9m.a",libdir);
    if(nin+3<4096){ inputs[nin++]=shim; inputs[nin++]=libcxx; inputs[nin++]=libm; }

    /* ---- pass 1: include all non-archive objects; queue archives ---- */
    for(int i=0;i<nin;i++){
        long len; u8 *img=rdfile(inputs[i],&len);
        if(!img){ fprintf(stderr,"n9link: cannot read %s\n",inputs[i]); return 1; }
        if(len>=8 && !memcmp(img,"!<arch>\n",8)){
            Arch *ar=xmalloc(sizeof *ar); ar->img=img; ar->len=len; ar->name=inputs[i];
            if(narch<256) archs[narch++]=ar;
        } else {
            Obj *o=parse_obj(inputs[i],img,len);
            if(!o){ fprintf(stderr,"n9link: not an ELF64 rel object: %s\n",inputs[i]); return 1; }
            include_obj(o);
        }
    }
    /* ---- pass 2: pull archive members to fixpoint ---- */
    link_archives();

    /* ---- COMDAT dedup + decide kept sections ---- */
    for(Obj *o=objs_head;o;o=o->next){
        for(int i=0;i<o->nsh;i++) o->keep[i]=0;
        /* process groups first: mark duplicate-group members for drop via keep=-1 sentinel */
    }
    /* mark group members: build per-object "in a dropped group" set */
    for(Obj *o=objs_head;o;o=o->next){
        for(int i=0;i<o->nsh;i++){
            if(o->sh[i].type!=SHT_GROUP) continue;
            /* signature = symbol named by sh_info in this object's symtab */
            if(o->symsh<0) continue;
            Sym *sig=&o->sym[o->sh[i].info];
            const char *signame=symname(o,sig);
            u32 *w=(u32*)(o->img + o->sh[i].offset);
            int nw=o->sh[i].size/4;
            int drop = (w[0]&GRP_COMDAT) ? group_seen(signame) : 0;
            if(drop){ for(int k=1;k<nw;k++){ if(w[k]<(u32)o->nsh) o->outseg[w[k]]=-2; } } /* -2 = dropped by comdat */
        }
    }
    /* classify + keep allocatable sections not dropped */
    for(Obj *o=objs_head;o;o=o->next){
        for(int i=0;i<o->nsh;i++){
            if(o->outseg[i]==-2){ o->keep[i]=0; continue; }   /* comdat-dropped */
            Shdr *s=&o->sh[i];
            if(s->type==SHT_RELA||s->type==SHT_SYMTAB||s->type==SHT_STRTAB||s->type==SHT_GROUP) continue;
            if(s->type==SHT_NOBITS){
                int seg=classify(secname(o,i),s->type,s->flags);
                if(seg!=SEG_NONE){ o->keep[i]=1; o->outseg[i]=seg; }
                continue;
            }
            if(!(s->flags&SHF_ALLOC)) continue;
            int seg=classify(secname(o,i),s->type,s->flags);
            if(seg==SEG_NONE) continue;
            o->keep[i]=1; o->outseg[i]=seg;
        }
    }

    /* ---- GOT pre-pass: assign an 8-byte slot per (object,symbol) *GOTPCREL* target.
     * Keyed by (obj,symidx) so LOCAL/section symbols (e.g. cranelift's anonymous
     * vtables/constants in .rodata) each get their own correctly-filled slot. ---- */
    for(Obj *o=objs_head;o;o=o->next) for(int i=0;i<o->nsh;i++) if(o->keep[i]){
        int rs=o->relaof[i]; if(rs<0) continue;
        Rela *r=(Rela*)(o->img+o->sh[rs].offset); int nr=o->sh[rs].size/sizeof(Rela);
        for(int k=0;k<nr;k++){
            u32 type=R_TYPE(r[k].info);
            if(type!=R_X86_64_GOTPCREL && type!=R_X86_64_GOTPCRELX && type!=R_X86_64_REX_GOTPCRELX) continue;
            u32 symi=R_SYM(r[k].info);
            if(o->got_ent[symi]<0) o->got_ent[symi]=n_got++;
        }
    }

    /* ---- layout: assign vaddrs, segment by segment in the fixed order ---- */
    /* text segments first (in-place from tvaddr), then data segments from 2MB-aligned dvaddr. */
    u64 addr=tvaddr;
    u64 eh_start=0,eh_end=0,init_start=0,init_end=0,fini_start=0,fini_end=0;
    for(int seg=SEG_TEXT; seg<=SEG_FINIARR; seg++){
        if(seg==SEG_EHFRAME){ addr=(addr+7)&~7ull; eh_start=addr; }
        if(seg==SEG_INITARR){ addr=(addr+7)&~7ull; init_start=addr; }
        if(seg==SEG_FINIARR){ fini_start=addr; }
        for(Obj *o=objs_head;o;o=o->next) for(int i=0;i<o->nsh;i++) if(o->keep[i]&&o->outseg[i]==seg){
            u64 al=o->sh[i].align?o->sh[i].align:1; addr=(addr+al-1)&~(al-1);
            o->outaddr[i]=addr; addr+=o->sh[i].size;
        }
        if(seg==SEG_EHFRAME) eh_end=addr;
        if(seg==SEG_INITARR) init_end=addr;
        if(seg==SEG_FINIARR) fini_end=addr;
    }
    u64 tend=addr; tsize=tend-tvaddr;

    /* data segment at next 2MB boundary */
    dvaddr=(tend + 0x200000-1) & ~0x1fffffull;
    addr=dvaddr;
    for(int seg=SEG_DATA; seg<=SEG_STACK; seg++){
        if(seg==SEG_BSS){
            /* synthetic GOT: file-backed, just before .bss */
            addr=(addr+7)&~7ull; got_base=addr; addr+=8ull*n_got;
            dfilesz=addr-dvaddr;    /* file-backed data (incl GOT) ends where bss begins */
        }
        for(Obj *o=objs_head;o;o=o->next) for(int i=0;i<o->nsh;i++) if(o->keep[i]&&o->outseg[i]==seg){
            u64 al=o->sh[i].align?o->sh[i].align:1; addr=(addr+al-1)&~(al-1);
            o->outaddr[i]=addr; addr+=o->sh[i].size;
        }
    }
    dmemsz=addr-dvaddr;
    u64 endaddr=addr;

    /* ---- synthetic/boundary symbols ---- */
    #define SYNTH(n,v) do{ GSym *g=gget(n); g->defined=1; g->obj=0; g->shndx=SHN_ABS; g->value=(v); g->finalval=(v); g->bind=STB_GLOBAL; }while(0)
    SYNTH("__eh_frame_start",eh_start); SYNTH("__eh_frame_end",eh_end);
    SYNTH("__init_array_start",init_start); SYNTH("__init_array_end",init_end);
    SYNTH("__fini_array_start",fini_start); SYNTH("__fini_array_end",fini_end);
    SYNTH("__eh_frame_hdr_start",0); SYNTH("__eh_frame_hdr_end",0);
    SYNTH("end",endaddr); SYNTH("_end",endaddr);
    SYNTH("edata",dvaddr+dfilesz); SYNTH("etext",tend);

    /* ---- resolve final values of all global defs ---- */
    for(int h=0;h<HN;h++) for(GSym *g=ghash[h];g;g=g->next){
        if(!g->defined) continue;
        if(g->obj==0) continue;                  /* synthetic: finalval already set */
        if(g->shndx==SHN_ABS){ g->finalval=g->value; continue; }
        if(g->shndx==SHN_COMMON){ g->finalval=0; continue; }
        if(g->shndx>=g->obj->nsh){ g->finalval=g->value; continue; }
        if(!g->obj->keep[g->shndx]){ g->finalval=g->value; continue; } /* def in dropped comdat: shouldn't be used */
        g->finalval = g->obj->outaddr[g->shndx] + g->value;
    }

    /* ---- build output images ---- */
    timg=xmalloc(tsize); dimg=xmalloc(dfilesz);
    for(Obj *o=objs_head;o;o=o->next) for(int i=0;i<o->nsh;i++) if(o->keep[i]){
        Shdr *s=&o->sh[i];
        if(s->type==SHT_NOBITS) continue;               /* bss/stack: no bytes */
        int seg=o->outseg[i];
        if(seg_is_text(seg)) memcpy(timg + (o->outaddr[i]-tvaddr), o->img+s->offset, s->size);
        else if(seg!=SEG_BSS && seg!=SEG_STACK) memcpy(dimg + (o->outaddr[i]-dvaddr), o->img+s->offset, s->size);
    }
    /* fill the synthetic GOT slots with resolved symbol addresses */
    for(Obj *o=objs_head;o;o=o->next) for(int si=0;si<o->nsym;si++) if(o->got_ent[si]>=0){
        int undef=0; u64 v=symval(o,si,&undef);
        if(undef) fprintf(stderr,"n9link: GOT target undefined: %s\n",symname(o,&o->sym[si]));
        memcpy(dimg + (got_base + 8ull*o->got_ent[si] - dvaddr), &v, 8);
    }

    /* ---- apply relocations ---- */
    for(Obj *o=objs_head;o;o=o->next) for(int i=0;i<o->nsh;i++) if(o->keep[i]){
        int rs=o->relaof[i]; if(rs<0) continue;
        Shdr *rsh=&o->sh[rs];
        Rela *r=(Rela*)(o->img+rsh->offset); int nr=rsh->size/sizeof(Rela);
        int seg=o->outseg[i]; int istext=seg_is_text(seg);
        u8 *base = istext ? timg + (o->outaddr[i]-tvaddr) : dimg + (o->outaddr[i]-dvaddr);
        u64 secaddr=o->outaddr[i];
        for(int k=0;k<nr;k++){
            u32 symi=R_SYM(r[k].info), type=R_TYPE(r[k].info);
            Sym *s=&o->sym[symi];
            u64 S;
            if(ELF_ST_TYPE(s->info)==STT_SECTION){
                /* section symbol: value = target section base */
                if(s->shndx<o->nsh && o->keep[s->shndx]) S=o->outaddr[s->shndx];
                else S=0;
            } else if(s->shndx==SHN_UNDEF || ELF_ST_BIND(s->info)!=STB_LOCAL){
                const char *nm=symname(o,s);
                GSym *g=gget(nm);
                if(!g->defined){
                    if(ELF_ST_BIND(s->info)==STB_WEAK) S=0;  /* unsatisfied weak ref -> 0 */
                    else { fprintf(stderr,"n9link: undefined symbol: %s (from %s)\n",nm,o->name); return 1; }
                } else S=g->finalval;
            } else {
                /* local symbol defined in this object */
                if(s->shndx<o->nsh && o->keep[s->shndx]) S=o->outaddr[s->shndx]+s->value;
                else S=s->value;
            }
            s64 A=r[k].addend;
            u64 P=secaddr + r[k].offset;
            u8 *loc=base + r[k].offset;
            switch(type){
            case R_X86_64_64: { u64 v=S+A; memcpy(loc,&v,8); } break;
            case R_X86_64_32: case R_X86_64_32S: { u32 v=(u32)(S+A); memcpy(loc,&v,4); } break;
            case R_X86_64_PC32: case R_X86_64_PLT32: { u32 v=(u32)(s64)(S+A-P); memcpy(loc,&v,4); } break;
            case R_X86_64_GOTPCREL: case R_X86_64_GOTPCRELX: case R_X86_64_REX_GOTPCRELX: {
                /* value points to this (obj,sym)'s GOT slot, PC-relative */
                u64 G = got_base + 8ull*o->got_ent[symi];
                u32 v=(u32)(s64)(G + A - P); memcpy(loc,&v,4);
            } break;
            default: fprintf(stderr,"n9link: unhandled reloc type %u in %s\n",type,o->name); return 1;
            }
        }
    }

    /* ---- Plan 9 symtab (for acid) ---- */
    /* format per elf2aout: 8-byte BE value + (0x80|type) + name + NUL. */
    u8 *symbuf=0; long symlen=0, symcap=0;
    for(int h=0;h<HN;h++) for(GSym *g=ghash[h];g;g=g->next){
        if(!g->defined || !g->name[0]) continue;
        u64 v=g->finalval; if(v==0) continue;
        char t = (v>=dvaddr) ? 'D' : 'T';
        long need=8+1+strlen(g->name)+1;
        if(symlen+need>symcap){ symcap=(symcap*2)+need+4096; symbuf=realloc(symbuf,symcap); if(!symbuf) die("oom"); }
        for(int b=7;b>=0;b--) symbuf[symlen++]=(v>>(b*8))&0xff;   /* big-endian */
        symbuf[symlen++]=0x80|(u8)t;
        strcpy((char*)symbuf+symlen,g->name); symlen+=strlen(g->name)+1;
    }

    /* ---- emit a.out ---- */
    u64 entry=gget("_start")->finalval;
    if(!gget("_start")->defined) die("no _start");
    u64 bss = dmemsz - dfilesz;
    FILE *f=fopen(out,"wb"); if(!f) die("cannot open output");
    u8 hdr[40];
    #define PUT32BE(o,val) do{u32 _v=(u32)(val); hdr[o]=_v>>24;hdr[o+1]=_v>>16;hdr[o+2]=_v>>8;hdr[o+3]=_v;}while(0)
    PUT32BE(0,0x8A97); PUT32BE(4,tsize); PUT32BE(8,dfilesz); PUT32BE(12,bss);
    PUT32BE(16,symlen); PUT32BE(20,entry&0xffffffff); PUT32BE(24,0); PUT32BE(28,0);
    for(int b=0;b<8;b++) hdr[32+b]=(entry>>((7-b)*8))&0xff;    /* entry BE 64 */
    fwrite(hdr,1,40,f);
    fwrite(timg,1,tsize,f);
    fwrite(dimg,1,dfilesz,f);
    if(symlen) fwrite(symbuf,1,symlen,f);
    fclose(f);
    chmod(out, 0775);   /* fopen creates 0666; the linked program must be runnable */
    fprintf(stderr,"n9link: %s  text=%llu data=%llu bss=%llu syms=%ld entry=0x%llx\n",
        out,(unsigned long long)tsize,(unsigned long long)dfilesz,(unsigned long long)bss,symlen,(unsigned long long)entry);
    return 0;
}
static u64 sym_of(const char*n){ return gget(n)->finalval; }
