// Copyright 2015 The Servo Project Developers. See the COPYRIGHT
// file at the top-level directory of this distribution.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

#[cfg(all(
    not(feature = "force-inprocess"),
    any(
        target_os = "linux",
        target_os = "openbsd",
        target_os = "freebsd",
        target_os = "illumos",
    )
))]
mod unix;
#[cfg(all(
    not(feature = "force-inprocess"),
    any(
        target_os = "linux",
        target_os = "openbsd",
        target_os = "freebsd",
        target_os = "illumos",
    )
))]
mod os {
    pub use super::unix::*;
}

#[cfg(all(not(feature = "force-inprocess"), target_os = "macos"))]
mod macos;
#[cfg(all(not(feature = "force-inprocess"), target_os = "macos"))]
mod os {
    pub use super::macos::*;
}
#[cfg(all(not(feature = "force-inprocess"), target_os = "macos"))]
pub use macos::set_bootstrap_prefix;

#[cfg(all(not(feature = "force-inprocess"), target_os = "windows"))]
mod windows;
#[cfg(all(not(feature = "force-inprocess"), target_os = "windows"))]
mod os {
    pub use super::windows::*;
}

// Plan 9 has no SCM_RIGHTS (a connection cannot carry an fd) and no
// cross-process MAP_SHARED, so the unix backend is not implementable there.
// Inprocess is the only correct backend for it — not a preference, and not
// something a `force-inprocess` feature should have to express.
#[cfg(any(
    feature = "force-inprocess",
    target_os = "android",
    target_os = "ios",
    target_os = "plan9",
    target_os = "wasi",
    target_os = "unknown"
))]
mod inprocess;
#[cfg(any(
    feature = "force-inprocess",
    target_os = "android",
    target_os = "ios",
    target_os = "plan9",
    target_os = "wasi",
    target_os = "unknown"
))]
mod os {
    pub use super::inprocess::*;
}

pub use self::os::{channel, OsOpaqueIpcChannel};
pub use self::os::{OsIpcChannel, OsIpcOneShotServer, OsIpcReceiver, OsIpcReceiverSet};
pub use self::os::{OsIpcSelectionResult, OsIpcSender, OsIpcSharedMemory, OsTrySelectError};

#[cfg(test)]
mod test;
