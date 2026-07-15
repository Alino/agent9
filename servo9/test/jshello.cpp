/* The mozjs-on-plan9 gate: boot a real SpiderMonkey, evaluate JavaScript, print
 * the result. If this runs on 9front, the engine's GC, interpreter, self-hosted
 * builtins and atom tables all work — which is the whole question. The loop and
 * the string interpolation are deliberate: they exercise the interpreter and
 * the self-hosted code path, not just JS_Init.
 */
#include <js/Initialization.h>
#include <jsapi.h>
#include <js/CompilationAndEvaluation.h>
#include <js/Conversions.h>
#include <js/RealmOptions.h>
#include <js/SourceText.h>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>  // cc9_have_wx
#include "jit/JitOptions.h"

static JSClass GlobalClass = {"global", JSCLASS_GLOBAL_FLAGS,
                              &JS::DefaultGlobalClassOps};

int main() {
  // The whole point of the runtime check: one binary, two kernels.
  std::printf("cc9_have_wx (kernel W^X gate): %s\n",
              cc9_have_wx() ? "YES" : "no");

  if (!JS_Init()) {
    std::printf("JS_Init failed\n");
    return 1;
  }

  // Read AFTER JS_Init: jit::InitializeJit is what consults cc9_have_wx and
  // decides. Reading it earlier would report the default, not the decision.
  std::printf("JIT backend: %s\n",
              js::jit::JitOptions.disableJitBackend ? "DISABLED (interpreter)"
                                                    : "ENABLED (baseline+ion)");

  JSContext* cx = JS_NewContext(32L * 1024 * 1024);
  if (!cx || !JS::InitSelfHostedCode(cx)) {
    std::printf("context/selfhosted init failed\n");
    return 1;
  }

  // Scoped: JSAutoRealm's destructor calls leaveRealm() on the context, so it
  // must run BEFORE JS_DestroyContext. Letting it destruct at end of main (i.e.
  // after the context is gone) is a use-after-free.
  {
    JS::RealmOptions options;
    JS::RootedObject global(
        cx, JS_NewGlobalObject(cx, &GlobalClass, nullptr,
                               JS::FireOnNewGlobalHook, options));
    if (!global) {
      std::printf("global failed\n");
      return 1;
    }

    JSAutoRealm ar(cx, global);

    // Run hot enough to force tier-up: Baseline warms at ~100 calls and Ion at
    // ~1500, so a few hundred thousand iterations guarantees that if the JIT is
    // enabled, the answer below was computed by JIT-emitted machine code rather
    // than the interpreter. The result is checked, so a miscompile shows up as a
    // wrong number instead of passing quietly.
    static const char code[] =
        "function f(n) { let s = 0; for (let i = 1; i <= n; i++) s += i * i; return s; }"
        "let acc = 0;"
        "for (let k = 0; k < 200000; k++) acc = f(8);"
        "const expect = 204;"
        "if (acc !== expect) throw new Error('WRONG: ' + acc + ' != ' + expect);"
        "const who = ['SpiderMonkey', 140, 'on', '9front'].join(' ');"
        "`${who}: sum of squares 1..8 = ${acc} (verified over 200k calls)`";

    JS::SourceText<mozilla::Utf8Unit> source;
    if (!source.init(cx, code, std::strlen(code),
                     JS::SourceOwnership::Borrowed)) {
      std::printf("source init failed\n");
      return 1;
    }

    JS::CompileOptions opts(cx);
    opts.setFileAndLine("plan9", 1);

    JS::RootedValue rval(cx);
    if (!JS::Evaluate(cx, opts, source, &rval)) {
      std::printf("evaluate failed\n");
      return 1;
    }

    JS::RootedString str(cx, JS::ToString(cx, rval));
    JS::UniqueChars chars = JS_EncodeStringToUTF8(cx, str);
    std::printf("%s\n", chars.get());
  }

  JS_DestroyContext(cx);
  JS_ShutDown();
  std::printf("clean shutdown\n");
  return 0;
}
