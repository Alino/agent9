// The Rust half of SpiderMonkey, for the plan9 target.
//
// libjs_static.a leaves two things undefined that mozjs_sys normally supplies
// from the Rust side (it is itself a Rust crate):
//
//  * encoding_mem_* — mfbt's Latin1.h routes all UTF-8/UTF-16/Latin1 conversion
//    into the encoding_rs crate. Referencing encoding_c_mem pulls its
//    #[no_mangle] extern "C" exports into this staticlib.
//  * install_rust_hooks — called from JS_Init via vm/Initialization.cpp.
//
// Nothing here calls encoding_c_mem; it is referenced for its exports alone.
extern crate encoding_c;
extern crate encoding_c_mem;

/// Matches mozjs_sys/src/lib.rs. Its body there is empty unless the
/// `oom_with_hook` feature is on (the panic hook next to it is commented out
/// upstream), so this is the real behaviour for our configuration rather than a
/// stub standing in for one.
#[no_mangle]
pub extern "C" fn install_rust_hooks() {}
