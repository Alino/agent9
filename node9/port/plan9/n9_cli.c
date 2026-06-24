/* node9 qjs CLI — uses quickjs-libc's std/os modules.
   `qjs file.js` runs a script/module; bare `qjs` starts a minimal REPL. */
#include "node9_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "quickjs-libc.h"

/* Plan 9 enables FPU traps for divide-by-zero/overflow/invalid by default, so
   they fault ("suicide: sys: fp: ...") on real hardware — QEMU silently
   ignores them, which is why this only bit on bare metal (e.g. `npm help`).
   JavaScript/IEEE-754 require x/0 -> Infinity, 0/0 -> NaN, precision loss,
   etc. with no trap, so we DISABLE all FP exception traps at startup. getfcr/
   setfcr live in the native libc (declared in <libc.h>); APE has no
   <fpuctl.h>, so we declare them here. Plan 9's fcr uses ENABLE semantics
   (bit set = trap enabled), inverted from the raw amd64 MXCSR; bits 7..12
   (0x1f80) are the six exception enables, so clearing them disables all
   FP traps. (Verified empirically: setting them made `npm --version` fault
   on precision loss.) */
extern unsigned long getfcr(void);
extern void setfcr(unsigned long);

void n9_native_init(JSContext *ctx);   /* defined in n9_native.c */

/* --- DEBUG guard-byte allocator: aborts at the moment of an OOB write, printing the
   overflowed block's size. Enable by setting NODE9_GUARD; else use the default. --- */
#define N9G 32
static void *n9_gmalloc(void *o, size_t n){
    unsigned char *p = (unsigned char*)malloc(n + 2*N9G);
    if(!p) return 0;
    *((size_t*)p) = n;
    memset(p+sizeof(size_t), 0xAB, N9G-sizeof(size_t));
    memset(p+N9G+n, 0xCD, N9G);
    return p + N9G;
}
static void n9_gcheck(unsigned char *u){
    unsigned char *p = u - N9G; size_t n = *((size_t*)p); size_t i;
    for(i=sizeof(size_t);i<N9G;i++) if(p[i]!=0xAB){ fprintf(stderr,"N9HEAP UNDERFLOW sz=%lu\n",(unsigned long)n); abort(); }
    for(i=0;i<N9G;i++) if(p[N9G+n+i]!=0xCD){ fprintf(stderr,"N9HEAP OVERFLOW sz=%lu off=+%lu\n",(unsigned long)n,(unsigned long)i); abort(); }
}
static void n9_gfree(void *o, void *u){ if(!u) return; n9_gcheck((unsigned char*)u); free((unsigned char*)u - N9G); }
static void *n9_grealloc(void *o, void *u, size_t n){
    unsigned char *np;
    if(!u) return n9_gmalloc(o,n);
    if(n==0){ n9_gfree(o,u); return 0; }
    n9_gcheck((unsigned char*)u);
    np = (unsigned char*)realloc((unsigned char*)u - N9G, n + 2*N9G);
    if(!np) return 0;
    *((size_t*)np) = n;
    memset(np+sizeof(size_t), 0xAB, N9G-sizeof(size_t));
    memset(np+N9G+n, 0xCD, N9G);
    return np + N9G;
}
static size_t n9_gusable(const void *u){ return *((size_t*)((unsigned char*)u - N9G)); }
static void *n9_gcalloc(void *o, size_t c, size_t s){ size_t n=c*s; void *p=n9_gmalloc(o,n); if(p) memset(p,0,n); return p; }
static const JSMallocFunctions n9_gmf = { n9_gcalloc, n9_gmalloc, n9_gfree, n9_grealloc, n9_gusable };

/* Module specifier normalize: delegate to JS globalThis.__n9_resolve(base, name) so the
   node_modules walk + package.json "exports"/"imports" (ESM conditions) live in boot.js.
   Returns a js_malloc'd absolute path (or the bare name as a fallback). Lets quickjs's
   native import()/import resolve npm's ESM-only bundled deps (chalk, glob, ...). */
static char *n9_module_normalize(JSContext *ctx, const char *base_name, const char *name, void *opaque) {
    char *ret = NULL;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "__n9_resolve");
    if (JS_IsFunction(ctx, fn)) {
        JSValue args[2];
        args[0] = JS_NewString(ctx, base_name ? base_name : "");
        args[1] = JS_NewString(ctx, name ? name : "");
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
        if (JS_IsException(r)) { JSValue e = JS_GetException(ctx); JS_FreeValue(ctx, e); }
        else { const char *s = JS_ToCString(ctx, r); if (s) { size_t l = strlen(s) + 1; ret = js_malloc(ctx, l); if (ret) memcpy(ret, s, l); JS_FreeCString(ctx, s); } }
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, global);
    if (!ret) { size_t l = strlen(name) + 1; ret = js_malloc(ctx, l); if (ret) memcpy(ret, name, l); }
    return ret;
}

static int eval_buf(JSContext *ctx, const char *buf, size_t len, const char *fn, int is_main) {
    JSValue val;
    int ret = 0;
    if (JS_DetectModule(buf, len)) {
        val = JS_Eval(ctx, buf, len, fn, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(val)) {
            js_module_set_import_meta(ctx, val, 1, is_main);
            val = JS_EvalFunction(ctx, val);
        }
        val = js_std_await(ctx, val);
    } else {
        val = JS_Eval(ctx, buf, len, fn, JS_EVAL_TYPE_GLOBAL);
    }
    if (JS_IsException(val)) { js_std_dump_error(ctx); ret = -1; }
    JS_FreeValue(ctx, val);
    return ret;
}

static int eval_file(JSContext *ctx, const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) { perror(filename); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    size_t rd = fread(buf, 1, n, f); buf[rd] = 0; fclose(f);
    int ret = eval_buf(ctx, buf, rd, filename, 1);
    free(buf);
    return ret;
}

int main(int argc, char **argv) {
    setfcr(getfcr() & ~0x1f80); /* disable all FP traps: IEEE Inf/NaN, no Plan 9 suicide */
    JSRuntime *rt = getenv("NODE9_GUARD") ? JS_NewRuntime2(&n9_gmf, 0) : JS_NewRuntime();
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, n9_module_normalize, js_module_loader, js_module_check_attributes, NULL);
    JSContext *ctx = JS_NewContext(rt);
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    n9_native_init(ctx);   /* node9 native crypto/zlib/tls bindings -> globalThis.__n9native */
    js_std_add_helpers(ctx, argc - 1, argv + 1);

    /* expose std/os as globals (like qjs --std) */
    static const char *boot =
        "import * as std from 'std';\n"
        "import * as os from 'os';\n"
        "globalThis.std = std; globalThis.os = os;\n";
    eval_buf(ctx, boot, strlen(boot), "<boot>", 0);

    /* load the node9 standard library (require + Node-shaped builtins), if present */
    {
        const char *bp = getenv("NODE9_BOOT");
        if (!bp) bp = "/amd64/lib/node9/boot.js";
        FILE *bf = fopen(bp, "rb");
        if (bf) { fclose(bf); eval_file(ctx, bp); }
    }

    int ret = 0;
    if (argc >= 2) {
        ret = eval_file(ctx, argv[1]);
    } else {
        char line[8192];
        fprintf(stderr, "node9 qjs (quickjs-ng on 9front) — ctrl-D to exit\n");
        for (;;) {
            fputs("qjs> ", stderr); fflush(stderr);
            if (!fgets(line, sizeof line, stdin)) { fputc('\n', stderr); break; }
            JSValue v = JS_Eval(ctx, line, strlen(line), "<repl>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) js_std_dump_error(ctx);
            else if (!JS_IsUndefined(v)) {
                const char *s = JS_ToCString(ctx, v);
                if (s) { printf("%s\n", s); JS_FreeCString(ctx, s); }
            }
            JS_FreeValue(ctx, v);
            js_std_loop_once(ctx);
        }
    }
    js_std_loop(ctx);
    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return ret ? 1 : 0;
}
