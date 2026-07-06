//! Plan 9 shim for the `home` crate: the upstream crate has no arm for a
//! non-unix, non-windows OS. Plan 9 spells it $home (rc), but tolerate $HOME.

use std::path::PathBuf;

pub fn home_dir() -> Option<PathBuf> {
    std::env::var_os("home")
        .or_else(|| std::env::var_os("HOME"))
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
}
