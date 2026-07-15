// The gate: does the layer Servo actually imports build for plan9?
//
// Driving SpiderMonkey's configure by hand (servo9/_work) proves the C++ engine
// compiles. mozjs_sys proves build.rs + bindgen + jsglue + ICU work. This crate
// goes one further: `js` (package `mozjs`) is the safe wrapper Servo names in its
// own Cargo.toml, so this is the first thing here that Servo would literally
// depend on.
//
// Symbols are referenced so they stay linked; failure should be a build error,
// not an empty artifact that looks like success.

use js::jsapi::JS_ShutDown;
use js::rust::JSEngine;

/// Boot the engine through mozjs's safe wrapper and shut it down.
///
/// JSEngine::init() is what Servo calls. It goes through JS_Init (inline in
/// jsapi.h, hence exported by jsglue.cpp) and brings up the GC, the atom tables
/// and the self-hosted builtins.
pub fn engine_boots() -> bool {
    match JSEngine::init() {
        Ok(engine) => {
            drop(engine);
            true
        }
        Err(_) => false,
    }
}

/// Shut down without the wrapper, to keep the raw jsapi binding linked too.
pub unsafe fn raw_shutdown() {
    JS_ShutDown();
}

/// The bindings must describe PLAN 9, not the host that ran bindgen. Had bindgen
/// parsed jsapi.h against macOS/arm64 headers it would still compile — and then
/// get every struct offset wrong at runtime. Assert the shape at compile time so
/// that failure is loud and early instead of a fault on the box.
const _: () = {
    assert!(core::mem::size_of::<usize>() == 8, "plan9/cc9 is LP64");
    assert!(
        core::mem::size_of::<js::jsapi::JS::Value>() == 8,
        "JS::Value must be a 64-bit NaN-boxed word"
    );
};
