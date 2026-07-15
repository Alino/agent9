//! Functionality common to backends using EGL displays.

use crate::egl::Egl;

#[cfg(not(any(target_os = "windows", target_os = "plan9")))]
use libc::{dlopen, dlsym, RTLD_LAZY};
use std::ffi::{CStr, CString};
use std::mem;
use std::os::raw::c_void;
use std::sync::LazyLock;
#[cfg(target_os = "windows")]
use winapi::shared::minwindef::HMODULE;
#[cfg(target_os = "windows")]
use winapi::um::libloaderapi;

thread_local! {
    pub static EGL_FUNCTIONS: Egl = Egl::load_with(get_proc_address);
}

#[cfg(target_os = "windows")]
static EGL_LIBRARY: LazyLock<EGLLibraryWrapper> = LazyLock::new(|| unsafe {
    let module = libloaderapi::LoadLibraryA(c"libEGL.dll".as_ptr());
    EGLLibraryWrapper(module)
});

#[cfg(target_env = "ohos")]
static EGL_POTENTIAL_SO_NAMES: [&CStr; 1] = [c"libEGL.so"];

#[cfg(not(any(target_os = "windows", target_os = "macos", target_env = "ohos", target_os = "plan9")))]
static EGL_POTENTIAL_SO_NAMES: [&CStr; 2] = [c"libEGL.so.1", c"libEGL.so"];

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "plan9")))]
static EGL_LIBRARY: LazyLock<EGLLibraryWrapper> = LazyLock::new(|| {
    for soname in EGL_POTENTIAL_SO_NAMES {
        unsafe {
            let handle = dlopen(soname.as_ptr(), RTLD_LAZY);
            if !handle.is_null() {
                return EGLLibraryWrapper(handle);
            }
        }
    }
    panic!("Unable to load the libEGL shared object");
});

#[cfg(target_os = "windows")]
struct EGLLibraryWrapper(HMODULE);
#[cfg(not(any(target_os = "windows", target_os = "plan9")))]
struct EGLLibraryWrapper(*mut c_void);

#[cfg(not(target_os = "plan9"))]
unsafe impl Send for EGLLibraryWrapper {}
#[cfg(not(target_os = "plan9"))]
unsafe impl Sync for EGLLibraryWrapper {}

#[cfg(target_os = "windows")]
fn get_proc_address(symbol_name: &str) -> *const c_void {
    unsafe {
        let symbol_name: CString = CString::new(symbol_name).unwrap();
        let symbol_ptr = symbol_name.as_ptr();
        libloaderapi::GetProcAddress(EGL_LIBRARY.0, symbol_ptr).cast()
    }
}

#[cfg(not(any(target_os = "windows", target_os = "plan9")))]
fn get_proc_address(symbol_name: &str) -> *const c_void {
    unsafe {
        let symbol_name: CString = CString::new(symbol_name).unwrap();
        let symbol_ptr = symbol_name.as_ptr();
        dlsym(EGL_LIBRARY.0, symbol_ptr).cast_const()
    }
}

// Plan 9 has no dynamic linker: gl9's EGL (Mesa + the gl9egl seam) is linked
// straight into the binary, so there is nothing to dlopen and nothing to dlsym.
// gl9egl's own eglGetProcAddress is the resolver instead — it answers for the EGL
// core, the EGL extensions it implements, and (by forwarding to
// OSMesaGetProcAddress) the GL entry points, so it can serve every name the
// generated loader asks for, including "eglGetProcAddress" itself.
//
// Do NOT reintroduce a symbol table here. An earlier version listed gl9egl's
// exports in this file and only consulted eglGetProcAddress as a fallback, which
// meant the list had to be kept in step with gl9egl by hand — and it silently was
// not: EGL_EXTENSION_FUNCTIONS resolves through egl.GetProcAddress at runtime
// rather than through this function, so entries added here never reached it.
// gl9egl owns the mapping; ask it.
#[cfg(target_os = "plan9")]
mod plan9_egl {
    use std::os::raw::{c_char, c_void};

    unsafe extern "C" {
        pub fn eglGetProcAddress(name: *const c_char) -> *mut c_void;
    }
}

#[cfg(target_os = "plan9")]
fn get_proc_address(symbol_name: &str) -> *const c_void {
    let p = unsafe {
        let c: CString = CString::new(symbol_name).unwrap();
        plan9_egl::eglGetProcAddress(c.as_ptr()).cast_const()
    };
    // A null becomes a bare "egl function was not loaded" panic from the generated
    // bindings — or, for the extension fn pointers, a call straight to address 0 —
    // and neither says which symbol was missing. Name it.
    if p.is_null() && std::env::var_os("SURFMAN_EGL_TRACE").is_some() {
        eprintln!("surfman: EGL symbol unresolved: {symbol_name}");
    }
    p
}

pub(crate) unsafe fn lookup_egl_extension(name: &CStr) -> *mut c_void {
    EGL_FUNCTIONS.with(|egl| mem::transmute(egl.GetProcAddress(name.as_ptr())))
}
