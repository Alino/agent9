/* node9 — minimal qjs: eval a JS file (or a default expr) and print the result.
   Links against the QuickJS engine only (no quickjs-libc yet). */
#include "node9_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static void dump_exception(JSContext *ctx) {
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    fprintf(stderr, "Uncaught: %s\n", s ? s : "(unknown)");
    if (s) JS_FreeCString(ctx, s);
    JSValue stk = JS_GetPropertyStr(ctx, e, "stack");
    if (!JS_IsUndefined(stk)) {
        const char *st = JS_ToCString(ctx, stk);
        if (st) { fprintf(stderr, "%s\n", st); JS_FreeCString(ctx, st); }
    }
    JS_FreeValue(ctx, stk);
    JS_FreeValue(ctx, e);
}

static int eval_buf(JSContext *ctx, const char *buf, size_t len, const char *fn) {
    JSValue v = JS_Eval(ctx, buf, len, fn, JS_EVAL_TYPE_GLOBAL);
    int ret = 0;
    if (JS_IsException(v)) { dump_exception(ctx); ret = -1; }
    else {
        const char *s = JS_ToCString(ctx, v);
        if (s) { printf("%s\n", s); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, v);
    return ret;
}

int main(int argc, char **argv) {
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "JS_NewRuntime failed\n"); return 2; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContext failed\n"); return 2; }
    int ret;
    if (argc >= 2) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        char *buf = malloc(n + 1);
        size_t rd = fread(buf, 1, n, f); buf[rd] = 0; fclose(f);
        ret = eval_buf(ctx, buf, rd, argv[1]);
        free(buf);
    } else {
        const char *code = "var s=0; for(var i=1;i<=10;i++) s+=i; `sum1..10=${s}, sqrt2=${Math.sqrt(2)}, json=${JSON.stringify({a:[1,2,3]})}`";
        ret = eval_buf(ctx, code, strlen(code), "<builtin>");
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return ret ? 1 : 0;
}
