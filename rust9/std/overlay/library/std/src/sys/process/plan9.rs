//! Plan 9 process spawning over cc9's rfork(RFPROC|RFFDG) + exec + await.
//! Focused MVP: spawn a program with args/env/cwd and inherited/nulled stdio,
//! then wait for it. Piped stdio (MakePipe) is not yet wired (returns an error),
//! so `Command::status()` works; `Command::output()` needs the pipe follow-up.
use super::env::{CommandEnv, CommandEnvs, CommandResolvedEnvs};
pub use crate::ffi::OsString as EnvKey;
use crate::ffi::{CString, OsStr, OsString};
use crate::num::NonZero;
use crate::path::Path;
use crate::process::StdioPipes;
use crate::sys::fs::File;
use crate::{fmt, io};

// cc9 process/exec/wait primitives (see cc9/runtime/posix_llvm.c).
unsafe extern "C" {
    fn n9_rfork(flags: i32) -> i64;
    fn n9_exec(path: *const u8, argv: *const *const u8) -> i64;
    fn n9_exits(msg: *const u8);
    fn waitpid(pid: i32, status: *mut i32, opts: i32) -> i32;
    #[link_name = "getpid"]
    fn c_getpid() -> i32;
    fn chdir(path: *const u8) -> i32;
    fn dup2(oldfd: i32, newfd: i32) -> i32;
    fn open(path: *const u8, flags: i32, ...) -> i32;
    fn close(fd: i32) -> i32;
    fn write(fd: i32, buf: *const u8, n: usize) -> isize;
    fn setenv(name: *const u8, value: *const u8, overwrite: i32) -> i32;
}

const RFPROC: i32 = 0x10;
const RFFDG: i32 = 0x04;

////////////////////////////////////////////////////////////////////////////////
// Command
////////////////////////////////////////////////////////////////////////////////

pub struct Command {
    program: OsString,
    args: Vec<OsString>,
    env: CommandEnv,
    cwd: Option<OsString>,
    stdin: Option<Stdio>,
    stdout: Option<Stdio>,
    stderr: Option<Stdio>,
}

#[derive(Debug)]
pub enum Stdio {
    Inherit,
    Null,
    MakePipe,
    ParentStdout,
    ParentStderr,
    Fd(i32), // a raw fd to dup onto the child's descriptor (e.g. a piped ChildPipe)
    #[allow(dead_code)]
    InheritFile(File),
}

impl Command {
    pub fn new(program: &OsStr) -> Command {
        Command {
            program: program.to_owned(),
            args: vec![program.to_owned()],
            env: Default::default(),
            cwd: None,
            stdin: None,
            stdout: None,
            stderr: None,
        }
    }

    pub fn arg(&mut self, arg: &OsStr) {
        self.args.push(arg.to_owned());
    }
    pub fn env_mut(&mut self) -> &mut CommandEnv {
        &mut self.env
    }
    pub fn cwd(&mut self, dir: &OsStr) {
        self.cwd = Some(dir.to_owned());
    }
    pub fn stdin(&mut self, stdin: Stdio) {
        self.stdin = Some(stdin);
    }
    pub fn stdout(&mut self, stdout: Stdio) {
        self.stdout = Some(stdout);
    }
    pub fn stderr(&mut self, stderr: Stdio) {
        self.stderr = Some(stderr);
    }
    pub fn get_program(&self) -> &OsStr {
        &self.program
    }
    pub fn get_args(&self) -> CommandArgs<'_> {
        let mut iter = self.args.iter();
        iter.next();
        CommandArgs { iter }
    }
    pub fn get_envs(&self) -> CommandEnvs<'_> {
        self.env.iter()
    }
    pub fn get_env_clear(&self) -> bool {
        self.env.does_clear()
    }
    pub fn get_resolved_envs(&self) -> CommandResolvedEnvs {
        CommandResolvedEnvs::new(self.env.capture())
    }
    pub fn get_current_dir(&self) -> Option<&Path> {
        self.cwd.as_ref().map(|cs| Path::new(cs))
    }

    pub fn spawn(
        &mut self,
        default: Stdio,
        _needs_stdin: bool,
    ) -> io::Result<(Process, StdioPipes)> {
        // Pre-build everything in the PARENT so the child (which shares a COW copy
        // of memory after rfork) only makes raw syscalls — no allocation, no locks.
        let program = cstr(&self.program)?;
        let mut argv_owned: Vec<CString> = Vec::with_capacity(self.args.len() + 1);
        for a in &self.args {
            argv_owned.push(cstr(a)?);
        }
        let mut argv: Vec<*const u8> = argv_owned.iter().map(|c| c.as_ptr() as *const u8).collect();
        argv.push(core::ptr::null());

        let cwd = match &self.cwd {
            Some(d) => Some(cstr(d)?),
            None => None,
        };
        // env: cc9 shares /env across rfork unless RFENVG; apply the deltas via
        // setenv in the child (writes /env/NAME). Capture (key,val) C strings up front.
        let mut envs: Vec<(CString, CString)> = Vec::new();
        for (k, v) in self.get_envs() {
            if let Some(v) = v {
                envs.push((cstr(k)?, cstr(v)?));
            }
        }

        // Resolve each stream to a (dup-from-fd -> std-fd) plan + close lists, and
        // build any parent-side pipe ends. All allocation happens HERE, in the parent.
        let mut child_dups: [(i32, i32); 3] = [(-1, 0), (-1, 1), (-1, 2)];
        let mut child_close: Vec<i32> = Vec::new();
        let mut parent_close: Vec<i32> = Vec::new();
        let mut sp = StdioPipes { stdin: None, stdout: None, stderr: None };
        for (idx, (opt, std_fd)) in
            [(&self.stdin, 0), (&self.stdout, 1), (&self.stderr, 2)].into_iter().enumerate()
        {
            match opt.as_ref().unwrap_or(&default) {
                Stdio::Inherit => {}
                Stdio::ParentStdout => child_dups[idx] = (1, std_fd),
                Stdio::ParentStderr => child_dups[idx] = (2, std_fd),
                Stdio::Fd(fd) => {
                    child_dups[idx] = (*fd, std_fd);
                    child_close.push(*fd);
                }
                Stdio::Null => {
                    let fd = unsafe { open(c"/dev/null".as_ptr() as *const u8, 2) };
                    if fd < 0 {
                        return Err(io::Error::last_os_error());
                    }
                    child_dups[idx] = (fd, std_fd);
                    child_close.push(fd);
                    parent_close.push(fd);
                }
                Stdio::MakePipe => {
                    let (p0, p1) = crate::sys::pipe::pipe()?;
                    let child_end = p0.into_fd();
                    child_dups[idx] = (child_end, std_fd);
                    child_close.push(child_end);
                    child_close.push(p1.fd());
                    parent_close.push(child_end);
                    match idx {
                        0 => sp.stdin = Some(p1),
                        1 => sp.stdout = Some(p1),
                        _ => sp.stderr = Some(p1),
                    }
                }
                Stdio::InheritFile(_) => {
                    return Err(io::const_error!(
                        io::ErrorKind::Unsupported,
                        "file stdio not yet supported on plan9"
                    ));
                }
            }
        }

        // NOTE: env_clear() is NOT honored. RFCENVG gives the child a fresh empty
        // env group, but after it cc9's setenv() no longer populates the child's
        // /env (the deltas vanish) — worse than ignoring the clear. Left as a gap
        // pending a cc9 runtime fix for /env after RFCENVG.
        let pid = unsafe { n9_rfork(RFPROC | RFFDG) };
        if pid < 0 {
            return Err(io::Error::last_os_error());
        }
        if pid == 0 {
            // CHILD: raw syscalls only (no allocation, no locks).
            unsafe {
                for &(from, to) in &child_dups {
                    if from >= 0 && from != to {
                        dup2(from, to);
                    }
                }
                for &fd in &child_close {
                    close(fd);
                }
                if let Some(ref d) = cwd {
                    chdir(d.as_ptr() as *const u8);
                }
                for (k, v) in &envs {
                    setenv(k.as_ptr() as *const u8, v.as_ptr() as *const u8, 1);
                }
                n9_exec(program.as_ptr() as *const u8, argv.as_ptr());
                n9_exits(c"exec failed".as_ptr() as *const u8);
                loop {}
            }
        }
        // PARENT: close the child's ends so EOF works.
        for &fd in &parent_close {
            unsafe { close(fd) };
        }
        Ok((Process { pid: pid as i32, status: None }, sp))
    }
}

fn cstr(s: &OsStr) -> io::Result<CString> {
    CString::new(s.as_encoded_bytes())
        .map_err(|_| io::const_error!(io::ErrorKind::InvalidInput, "nul byte in command component"))
}

pub fn output(cmd: &mut Command) -> io::Result<(ExitStatus, Vec<u8>, Vec<u8>)> {
    let (mut process, mut pipes) = cmd.spawn(Stdio::MakePipe, false)?;
    drop(pipes.stdin.take());
    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    match (pipes.stdout.take(), pipes.stderr.take()) {
        (Some(out), Some(err)) => read_output(out, &mut stdout, err, &mut stderr)?,
        (Some(out), None) => {
            out.read_to_end(&mut stdout)?;
        }
        (None, Some(err)) => {
            err.read_to_end(&mut stderr)?;
        }
        (None, None) => {}
    }
    let status = process.wait()?;
    Ok((status, stdout, stderr))
}

impl From<ChildPipe> for Stdio {
    fn from(pipe: ChildPipe) -> Stdio {
        Stdio::Fd(pipe.into_fd())
    }
}
impl From<io::Stdout> for Stdio {
    fn from(_: io::Stdout) -> Stdio {
        Stdio::ParentStdout
    }
}
impl From<io::Stderr> for Stdio {
    fn from(_: io::Stderr) -> Stdio {
        Stdio::ParentStderr
    }
}
impl From<File> for Stdio {
    fn from(file: File) -> Stdio {
        Stdio::InheritFile(file)
    }
}

impl fmt::Debug for Command {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}", self.args[0])?;
        for arg in &self.args[1..] {
            write!(f, " {arg:?}")?;
        }
        Ok(())
    }
}

#[derive(PartialEq, Eq, Clone, Copy, Debug, Default)]
pub struct ExitStatus(i32);

impl ExitStatus {
    pub fn exit_ok(&self) -> Result<(), ExitStatusError> {
        // WIFEXITED && code == 0 (our waitpid encodes exit code as (code&0xff)<<8;
        // signals are small ints 4/6).
        if self.0 == 0 { Ok(()) } else { Err(ExitStatusError(self.0)) }
    }
    pub fn code(&self) -> Option<i32> {
        // Exit codes are encoded in the high byte; signals in the low byte.
        if self.0 & 0x7f == 0 { Some((self.0 >> 8) & 0xff) } else { None }
    }
    pub fn signal(&self) -> Option<i32> {
        let s = self.0 & 0x7f;
        if s != 0 { Some(s) } else { None }
    }
}

impl fmt::Display for ExitStatus {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some(c) = self.code() {
            write!(f, "exit status: {c}")
        } else {
            write!(f, "terminated by signal {}", self.0 & 0x7f)
        }
    }
}

#[derive(PartialEq, Eq, Clone, Copy, Debug)]
pub struct ExitStatusError(i32);

impl Into<ExitStatus> for ExitStatusError {
    fn into(self) -> ExitStatus {
        ExitStatus(self.0)
    }
}
impl ExitStatusError {
    pub fn code(self) -> Option<NonZero<i32>> {
        NonZero::new((self.0 >> 8) & 0xff)
    }
}

#[derive(PartialEq, Eq, Clone, Copy, Debug)]
pub struct ExitCode(u8);

impl ExitCode {
    pub const SUCCESS: ExitCode = ExitCode(0);
    pub const FAILURE: ExitCode = ExitCode(1);
    pub fn as_i32(&self) -> i32 {
        self.0 as i32
    }
}
impl From<u8> for ExitCode {
    fn from(code: u8) -> Self {
        Self(code)
    }
}

pub struct Process {
    pid: i32,
    status: Option<ExitStatus>, // set once reaped (wait or try_wait); await(2) yields a record only once
}

/// Builds `/proc/<pid><suffix>` into `path` without allocating (suffix must be
/// NUL-terminated). Used for the kill note and the try_wait liveness probe.
fn proc_path(pid: i32, suffix: &[u8], path: &mut [u8; 32]) {
    let mut i = 0;
    for &b in b"/proc/" {
        path[i] = b;
        i += 1;
    }
    let mut tmp = [0u8; 10];
    let mut n = pid as u32;
    let mut j = 0;
    if n == 0 {
        tmp[0] = b'0';
        j = 1;
    } else {
        while n > 0 {
            tmp[j] = b'0' + (n % 10) as u8;
            n /= 10;
            j += 1;
        }
    }
    while j > 0 {
        j -= 1;
        path[i] = tmp[j];
        i += 1;
    }
    for &b in suffix {
        path[i] = b;
        i += 1;
    }
}

impl Process {
    pub fn id(&self) -> u32 {
        self.pid as u32
    }
    pub fn kill(&mut self) -> io::Result<()> {
        // Plan 9: write "kill" to /proc/<pid>/note. Build the path on the stack.
        let mut path = [0u8; 32];
        proc_path(self.pid, b"/note\0", &mut path);
        let fd = unsafe { open(path.as_ptr(), 1) }; // O_WRONLY
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        unsafe {
            write(fd, b"kill".as_ptr(), 4);
            close(fd);
        }
        Ok(())
    }
    pub fn wait(&mut self) -> io::Result<ExitStatus> {
        if let Some(s) = self.status {
            return Ok(s);
        }
        let mut status: i32 = 0;
        let r = unsafe { waitpid(self.pid, &mut status, 0) };
        if r < 0 {
            return Err(io::Error::last_os_error());
        }
        self.status = Some(ExitStatus(status));
        Ok(ExitStatus(status))
    }
    pub fn try_wait(&mut self) -> io::Result<Option<ExitStatus>> {
        if let Some(s) = self.status {
            return Ok(Some(s));
        }
        // Plan 9 has no non-blocking await, but an exited process vanishes from
        // /proc immediately (its wait record queues in the parent separately) —
        // so /proc/<pid> gone means waitpid() returns without blocking. Caveat:
        // pid reuse by an unrelated process reads as "still running" until the
        // next poll after that process too is gone; conservative, never wrong-reap.
        let mut path = [0u8; 32];
        proc_path(self.pid, b"/status\0", &mut path);
        let fd = unsafe { open(path.as_ptr(), 0) }; // O_RDONLY
        if fd >= 0 {
            unsafe { close(fd) };
            return Ok(None);
        }
        self.wait().map(Some)
    }
}

pub struct CommandArgs<'a> {
    iter: crate::slice::Iter<'a, OsString>,
}
impl<'a> Iterator for CommandArgs<'a> {
    type Item = &'a OsStr;
    fn next(&mut self) -> Option<&'a OsStr> {
        self.iter.next().map(|os| &**os)
    }
    fn size_hint(&self) -> (usize, Option<usize>) {
        self.iter.size_hint()
    }
}
impl<'a> ExactSizeIterator for CommandArgs<'a> {
    fn len(&self) -> usize {
        self.iter.len()
    }
    fn is_empty(&self) -> bool {
        self.iter.is_empty()
    }
}
impl<'a> fmt::Debug for CommandArgs<'a> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_list().entries(self.iter.clone()).finish()
    }
}

pub type ChildPipe = crate::sys::pipe::Pipe;

pub fn read_output(
    out: ChildPipe,
    stdout: &mut Vec<u8>,
    err: ChildPipe,
    stderr: &mut Vec<u8>,
) -> io::Result<()> {
    // Drain stderr on a helper thread so a child that fills one pipe's buffer
    // while we're blocked reading the other can't deadlock us (no poll/select
    // or non-blocking reads on Plan 9 — a thread is the only concurrent read).
    let handle = crate::thread::Builder::new().spawn(move || {
        let mut v = Vec::new();
        let r = err.read_to_end(&mut v);
        (v, r)
    })?;
    let r_out = out.read_to_end(stdout);
    let (v, r_err) = handle
        .join()
        .map_err(|_| io::const_error!(io::ErrorKind::Uncategorized, "stderr reader panicked"))?;
    *stderr = v;
    r_out?;
    r_err?;
    Ok(())
}

pub fn getpid() -> u32 {
    unsafe { c_getpid() as u32 }
}
