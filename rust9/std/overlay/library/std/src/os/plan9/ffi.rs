//! Plan 9-specific extensions to the primitives in the `std::ffi` module.
//!
//! Plan 9 paths are byte strings (slash-separated, no drive letters, UTF-8 by
//! convention but not by enforcement), exactly like unix — so `OsStr` here has
//! the same representation and the same extension trait applies verbatim.
//!
//! This exists because a good deal of portable Rust reaches for
//! `std::os::unix::ffi::OsStrExt` to get at path bytes (the `url` crate's
//! `from_file_path`, socket2's AF_UNIX paths, …). Plan 9 is not `unix`, so it
//! needs its own name for the same trait. Hermit does precisely this — see
//! `library/std/src/os/hermit/ffi.rs`, which re-exports the same module.
//!
//! # Examples
//!
//! ```ignore (plan9)
//! use std::ffi::OsStr;
//! use std::os::plan9::ffi::OsStrExt;
//!
//! let bytes = b"/net/tcp/clone";
//! let os_str = OsStr::from_bytes(bytes);
//! assert_eq!(os_str.as_bytes(), bytes);
//! ```

#![stable(feature = "rust1", since = "1.0.0")]

#[path = "../unix/ffi/os_str.rs"]
mod os_str;

#[stable(feature = "rust1", since = "1.0.0")]
pub use self::os_str::{OsStrExt, OsStringExt};
