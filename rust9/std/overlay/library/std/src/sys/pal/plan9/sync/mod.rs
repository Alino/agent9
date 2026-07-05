#![allow(unsafe_op_in_unsafe_fn)]
//! PAL-level real synchronization for plan9, over cc9's pthread primitives
//! (rfork(RFMEM) + Plan 9 semaphores). Consumed by the `pthread` Mutex/Condvar
//! wrappers and the `pthread` Parker.
mod condvar;
mod mutex;

pub use condvar::Condvar;
pub use mutex::Mutex;
