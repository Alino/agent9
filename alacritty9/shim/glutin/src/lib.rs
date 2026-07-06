//! Plan 9 shim for glutin 0.32 — the glutin public API (the subset Alacritty
//! uses) implemented by direct `extern "C"` calls into gl9egl, the minimal
//! statically-linked libEGL over OSMesa (gl9/port/plan9/egl/gl9egl.c).
//!
//! Why a shim: 9front has no dynamic linking, so real glutin's EGL backend
//! (libloading/dlopen) and its windows/unix-gated build.rs cannot work with
//! `target_os = "plan9"`. Module paths, type names, enum variants (including
//! the `Egl` variants alacritty pattern-matches on) and method signatures
//! mirror real glutin 0.32.3; everything else is simplified.

pub mod api;
pub mod config;
pub mod context;
pub mod display;
pub mod error;
pub mod prelude;
pub mod surface;

/// Platform-specific extensions. Real glutin has `platform::x11`; on Plan 9
/// there is none, the module exists only for path fidelity.
pub mod platform {}
