/* qjsbench — compile a JS file N times, then round-trip its bytecode.
 *
 * Same source compiled by both toolchains (kencc/APE and cc9/clang) so the
 * numbers compare like for like. No internal clock: wall time is measured
 * outside (rc's `time`), which avoids trusting two different gettimeofday
 * implementations.
 *
 *   qjsbench FILE [iterations]      default 3
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static char *slurp(const char *path, size_t *lenp)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    *lenp = fread(buf, 1, (size_t)n, f);
    buf[*lenp] = 0;
    fclose(f);
    return buf;
}

/* Compiling a module still resolves its import specifiers, so give the engine a
   loader that hands back an empty module for anything. Nothing is ever linked
   or run here — the point is to measure parse + compile of one file. */
static int stub_init(JSContext *ctx, JSModuleDef *m) { (void)ctx; (void)m; return 0; }

static JSModuleDef *stub_loader(JSContext *ctx, const char *name, void *opaque)
{
    (void)opaque;
    return JS_NewCModule(ctx, name, stub_init);
}

int main(int argc, char **argv)
{
    JSRuntime *rt;
    JSContext *ctx;
    char *src;
    size_t len;
    int iters = 3, i, run_mode = 0;
    const char *path;

    if (argc < 2) { fprintf(stderr, "usage: qjsbench [-run] FILE [iters]\n"); return 2; }

    {   /* -run FILE: execute the file as a script (measures the INTERPRETER);
           default: compile it N times (measures the PARSER) */
        int argi = 1;
        if (!strcmp(argv[1], "-run")) { run_mode = 1; argi = 2; }
        if (argc <= argi) { fprintf(stderr, "usage: qjsbench [-run] FILE [iters]\n"); return 2; }
        path = argv[argi];
        if (argc > argi + 1) iters = atoi(argv[argi + 1]);
        src = slurp(path, &len);
    }
    if (!src) { fprintf(stderr, "cannot read %s\n", path); return 1; }

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    if (rt) JS_SetModuleLoaderFunc(rt, NULL, stub_loader, NULL);
    if (!rt || !ctx) { fprintf(stderr, "runtime init failed\n"); return 1; }

    printf("file=%s bytes=%lu iters=%d mode=%s\n", path, (unsigned long)len, iters,
           run_mode ? "run" : "compile");
    fflush(stdout);

    if (run_mode) {
        JSValue v = JS_Eval(ctx, src, len, path, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx);
            const char *s2 = JS_ToCString(ctx, e);
            fprintf(stderr, "run failed: %s\n", s2 ? s2 : "?");
            return 1;
        }
        {   const char *s2 = JS_ToCString(ctx, v);
            printf("result=%s\n", s2 ? s2 : "(none)");
        }
        return 0;
    }

    for (i = 0; i < iters; i++) {
        JSValue v = JS_Eval(ctx, src, len, path,
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(v)) {
            JSValue e = JS_GetException(ctx);
            const char *s = JS_ToCString(ctx, e);
            fprintf(stderr, "compile failed: %s\n", s ? s : "?");
            return 1;
        }
        if (i == 0) {
            /* the bytecode round trip that faults under kencc */
            size_t olen = 0;
            uint8_t *out = JS_WriteObject(ctx, &olen, v, JS_WRITE_OBJ_BYTECODE);
            printf("write: %s len=%lu\n", out ? "ok" : "FAILED", (unsigned long)olen);
            fflush(stdout);
            if (out) {
                JSValue r = JS_ReadObject(ctx, out, olen, JS_READ_OBJ_BYTECODE);
                printf("read: %s\n", JS_IsException(r) ? "EXCEPTION" : "ok");
                fflush(stdout);
                JS_FreeValue(ctx, r);
                js_free(ctx, out);
            }
        }
        JS_FreeValue(ctx, v);
    }
    printf("compiled %d times\n", iters);
    fflush(stdout);
    /* deliberately skip JS_FreeContext/FreeRuntime: teardown cost is not what
       is being measured, and one build asserts on a leak the other tolerates */
    return 0;
}
