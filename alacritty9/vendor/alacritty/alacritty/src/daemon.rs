#[cfg(target_os = "openbsd")]
use std::ffi::CStr;
#[cfg(not(any(windows, target_os = "plan9")))]
use std::ffi::CString;
use std::ffi::OsStr;
#[cfg(not(any(target_os = "macos", target_os = "openbsd", windows)))]
use std::fs;
use std::io;
#[cfg(not(any(windows, target_os = "plan9")))]
use std::os::unix::ffi::OsStringExt;
#[cfg(windows)]
use std::os::windows::process::CommandExt;
use std::process::{Command, Stdio};
#[cfg(target_os = "openbsd")]
use std::ptr;

#[rustfmt::skip]
#[cfg(not(any(windows, target_os = "plan9")))]
use {
    std::error::Error,
    std::os::unix::process::CommandExt,
    std::os::unix::io::RawFd,
    std::path::PathBuf,
};

#[cfg(target_os = "plan9")]
use {std::error::Error, std::path::PathBuf};

/// Plan 9 has no std::os::unix; other modules import this alias instead.
/// The value is a placeholder — there is no pty master fd on Plan 9.
#[cfg(target_os = "plan9")]
pub type RawFd = i32;

#[cfg(not(any(windows, target_os = "plan9")))]
use libc::pid_t;
#[cfg(windows)]
use windows_sys::Win32::System::Threading::{CREATE_NEW_PROCESS_GROUP, CREATE_NO_WINDOW};

#[cfg(target_os = "macos")]
use crate::macos;

/// Start a new process in the background.
#[cfg(windows)]
pub fn spawn_daemon<I, S>(program: &str, args: I) -> io::Result<()>
where
    I: IntoIterator<Item = S> + Copy,
    S: AsRef<OsStr>,
{
    // Setting all the I/O handles to null and setting the
    // CREATE_NEW_PROCESS_GROUP and CREATE_NO_WINDOW has the effect
    // that console applications will run without opening a new
    // console window.
    Command::new(program)
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .creation_flags(CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW)
        .spawn()
        .map(|_| ())
}

/// Start a new process in the background.
///
/// No fork/setsid on Plan 9 (different process model, no controlling tty to
/// detach from); a plain spawn with nulled stdio is already "in the
/// background".
#[cfg(target_os = "plan9")]
pub fn spawn_daemon<I, S>(
    program: &str,
    args: I,
    master_fd: RawFd,
    shell_pid: u32,
) -> io::Result<()>
where
    I: IntoIterator<Item = S> + Copy,
    S: AsRef<OsStr>,
{
    let mut command = Command::new(program);
    command.args(args).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
    if let Ok(cwd) = foreground_process_path(master_fd, shell_pid) {
        command.current_dir(cwd);
    }
    // ponytail: the child is never reaped (its wait record lingers until we
    // exit); harmless, revisit only if long sessions spawn many daemons.
    command.spawn().map(|_| ())
}

/// Start a new process in the background.
#[cfg(not(any(windows, target_os = "plan9")))]
pub fn spawn_daemon<I, S>(
    program: &str,
    args: I,
    master_fd: RawFd,
    shell_pid: u32,
) -> io::Result<()>
where
    I: IntoIterator<Item = S> + Copy,
    S: AsRef<OsStr>,
{
    let mut command = Command::new(program);
    command.args(args).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());

    let working_directory = foreground_process_path(master_fd, shell_pid)
        .ok()
        .and_then(|path| CString::new(path.into_os_string().into_vec()).ok());

    unsafe {
        command
            .pre_exec(move || {
                // POSIX.1-2017 describes `fork` as async-signal-safe with the following note:
                //
                // > While the fork() function is async-signal-safe, there is no way for
                // > an implementation to determine whether the fork handlers established by
                // > pthread_atfork() are async-signal-safe. [...] It is therefore undefined for the
                // > fork handlers to execute functions that are not async-signal-safe when fork()
                // > is called from a signal handler.
                //
                // POSIX.1-2024 removes this guarantee and introduces an async-signal-safe
                // replacement `_Fork`, which we'd like to use, but macOS doesn't support it yet.
                //
                // Since we aren't registering any fork handlers, and hopefully the OS doesn't
                // either, we're fine on systems compatible with POSIX.1-2017, which should be
                // enough for a long while. If this ever becomes a problem in the future, we should
                // be able to switch to `_Fork`.
                match libc::fork() {
                    -1 => return Err(io::Error::last_os_error()),
                    0 => (),
                    _ => libc::_exit(0),
                }

                // Copy foreground process' working directory, ignoring invalid paths.
                if let Some(working_directory) = working_directory.as_ref() {
                    libc::chdir(working_directory.as_ptr());
                }

                if libc::setsid() == -1 {
                    return Err(io::Error::last_os_error());
                }

                Ok(())
            })
            .spawn()?
            .wait()
            .map(|_| ())
    }
}

/// Get working directory of the shell.
///
/// Plan 9 has no process groups/tcgetpgrp; use the shell itself. The first
/// line of /proc/n/fd is the process's current directory.
#[cfg(target_os = "plan9")]
pub fn foreground_process_path(
    _master_fd: RawFd,
    shell_pid: u32,
) -> Result<PathBuf, Box<dyn Error>> {
    let fd = fs::read_to_string(format!("/proc/{shell_pid}/fd"))?;
    let cwd = fd.lines().next().ok_or("empty /proc/n/fd")?;
    Ok(PathBuf::from(cwd))
}

/// Get working directory of controlling process.
#[cfg(not(any(windows, target_os = "openbsd", target_os = "plan9")))]
pub fn foreground_process_path(
    master_fd: RawFd,
    shell_pid: u32,
) -> Result<PathBuf, Box<dyn Error>> {
    let mut pid = unsafe { libc::tcgetpgrp(master_fd) };
    if pid < 0 {
        pid = shell_pid as pid_t;
    }

    #[cfg(not(any(target_os = "macos", target_os = "freebsd")))]
    let link_path = format!("/proc/{pid}/cwd");
    #[cfg(target_os = "freebsd")]
    let link_path = format!("/compat/linux/proc/{}/cwd", pid);

    #[cfg(not(target_os = "macos"))]
    let cwd = fs::read_link(link_path)?;

    #[cfg(target_os = "macos")]
    let cwd = macos::proc::cwd(pid)?;

    Ok(cwd)
}

#[cfg(target_os = "openbsd")]
pub fn foreground_process_path(
    master_fd: RawFd,
    shell_pid: u32,
) -> Result<PathBuf, Box<dyn Error>> {
    let mut pid = unsafe { libc::tcgetpgrp(master_fd) };
    if pid < 0 {
        pid = shell_pid as pid_t;
    }
    let name = [libc::CTL_KERN, libc::KERN_PROC_CWD, pid];
    let mut buf = [0u8; libc::PATH_MAX as usize];
    let result = unsafe {
        libc::sysctl(
            name.as_ptr(),
            name.len().try_into().unwrap(),
            buf.as_mut_ptr() as *mut _,
            &mut buf.len() as *mut _,
            ptr::null_mut(),
            0,
        )
    };
    if result != 0 {
        Err(io::Error::last_os_error().into())
    } else {
        let foreground_path = unsafe { CStr::from_ptr(buf.as_ptr().cast()) }.to_str()?;
        Ok(PathBuf::from(foreground_path))
    }
}
