//! Plan 9 stub for the config-file monitor.
//!
//! The `notify` crate (inotify/kqueue/ReadDirectoryChanges) has no Plan 9
//! backend and the platform has no file-change notification either; live
//! config reload is simply disabled. `new` returning `None` is the upstream
//! "nothing to watch" path, so no caller needs patching.

use std::path::PathBuf;

use winit::event_loop::EventLoopProxy;

use crate::event::Event;

pub struct ConfigMonitor;

impl ConfigMonitor {
    pub fn new(_paths: Vec<PathBuf>, _event_proxy: EventLoopProxy<Event>) -> Option<Self> {
        None
    }

    pub fn shutdown(self) {}

    pub fn needs_restart(&self, _files: &[PathBuf]) -> bool {
        false
    }
}
