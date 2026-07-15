//! Plan 9 filesystem over cc9's POSIX-over-9P slice (open/read/write/lseek/
//! stat/fstat/mkdir/remove/rename + opendir/readdir/closedir). cc9 exposes a
//! Linux-glibc-shaped `struct stat`, so we mirror it and map the mode bits.
use crate::ffi::{CStr, OsStr, OsString, c_char, c_void};
use crate::fmt;
use crate::fs::TryLockError;
use crate::io::{self, BorrowedCursor, IoSlice, IoSliceMut, SeekFrom};
use crate::path::{Path, PathBuf};
pub use crate::sys::fs::common::{Dir, copy, exists, remove_dir_all};
use crate::sys::time::{SystemTime, UNIX_EPOCH};
use crate::sys::unsupported;
use crate::time::Duration;

#[repr(C)]
#[derive(Clone, Copy)]
struct CTimespec {
    tv_sec: i64,
    tv_nsec: i64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct CStat {
    st_dev: u64,
    st_ino: u64,
    st_mode: u32,
    st_nlink: u32,
    st_uid: u32,
    st_gid: u32,
    st_rdev: u64,
    st_size: i64,
    st_blksize: i64,
    st_blocks: i64,
    st_atim: CTimespec,
    st_mtim: CTimespec,
    st_ctim: CTimespec,
}

#[repr(C)]
struct CDirent {
    d_ino: u64,
    d_type: u8,
    d_name: [c_char; 256],
}

unsafe extern "C" {
    fn open(path: *const u8, flags: i32, ...) -> i32;
    #[link_name = "close"]
    fn c_close(fd: i32) -> i32;
    #[link_name = "read"]
    fn c_read(fd: i32, buf: *mut u8, n: usize) -> isize;
    #[link_name = "write"]
    fn c_write(fd: i32, buf: *const u8, n: usize) -> isize;
    #[link_name = "lseek"]
    fn c_lseek(fd: i32, off: i64, whence: i32) -> i64;
    #[link_name = "fstat"]
    fn c_fstat(fd: i32, st: *mut CStat) -> i32;
    #[link_name = "stat"]
    fn c_stat(path: *const u8, st: *mut CStat) -> i32;
    #[link_name = "mkdir"]
    fn c_mkdir(path: *const u8, mode: u32) -> i32;
    #[link_name = "unlink"]
    fn c_unlink(path: *const u8) -> i32;
    #[link_name = "rmdir"]
    fn c_rmdir(path: *const u8) -> i32;
    #[link_name = "rename"]
    fn c_rename(from: *const u8, to: *const u8) -> i32;
    #[link_name = "ftruncate"]
    fn c_ftruncate(fd: i32, len: i64) -> i32;
    #[link_name = "dup"]
    fn c_dup(fd: i32) -> i32;
    #[link_name = "chmod"]
    fn c_chmod(path: *const u8, mode: u32) -> i32;
    #[link_name = "fchmod"]
    fn c_fchmod(fd: i32, mode: u32) -> i32;
    // wstat-based mtime setters (Plan 9 cannot set atime — it is kernel-owned).
    fn cc9_set_mtime(path: *const u8, secs: u64) -> i32;
    fn cc9_fset_mtime(fd: i32, secs: u64) -> i32;
    #[link_name = "opendir"]
    fn c_opendir(path: *const u8) -> *mut c_void;
    #[link_name = "readdir"]
    fn c_readdir(dir: *mut c_void) -> *mut CDirent;
    #[link_name = "closedir"]
    fn c_closedir(dir: *mut c_void) -> i32;
}

// O_* flags (cc9 fcntl.h).
const O_RDONLY: i32 = 0;
const O_WRONLY: i32 = 1;
const O_RDWR: i32 = 2;
const O_CREAT: i32 = 0x100;
const O_TRUNC: i32 = 0x200;
const O_EXCL: i32 = 0x400;
const O_APPEND: i32 = 0x800;

// mode bits.
const S_IFMT: u32 = 0o170000;
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const S_IFLNK: u32 = 0o120000;

fn cpath(p: &Path) -> Vec<u8> {
    let mut v = p.as_os_str().as_encoded_bytes().to_vec();
    v.push(0);
    v
}

fn cvt(r: i32) -> io::Result<()> {
    if r < 0 { Err(io::Error::last_os_error()) } else { Ok(()) }
}

fn systime(ts: &CTimespec) -> SystemTime {
    let d = Duration::new(ts.tv_sec as u64, ts.tv_nsec as u32);
    UNIX_EPOCH.checked_add_duration(&d).unwrap_or(UNIX_EPOCH)
}

pub struct File {
    fd: i32,
}

#[derive(Clone)]
pub struct FileAttr {
    stat: CStat,
}

pub struct ReadDir {
    dir: *mut c_void,
    root: PathBuf,
}

// `dir` is a cc9 `DIR *`. The raw pointer makes ReadDir auto-!Send/!Sync, which
// blocks tokio::fs (its ReadDir must cross threads to reach the blocking pool).
// Upstream's unix backend has exactly this and does exactly this — see
// `unsafe impl Send for DirStream` in library/std/src/sys/fs/unix.rs.
//
// SAFETY: the DIR is owned solely by this ReadDir (opened in readdir(), closed
// in Drop, never aliased), so moving it between threads transfers sole
// ownership. Sync holds because every method that touches `dir` takes &mut self:
// a shared &ReadDir cannot reach the stream at all.
unsafe impl Send for ReadDir {}
unsafe impl Sync for ReadDir {}

pub struct DirEntry {
    name: OsString,
    root: PathBuf,
    dtype: u8,
}

#[derive(Clone, Debug)]
pub struct OpenOptions {
    read: bool,
    write: bool,
    append: bool,
    truncate: bool,
    create: bool,
    create_new: bool,
}

#[derive(Copy, Clone, Debug, Default)]
pub struct FileTimes {
    modified: Option<SystemTime>,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct FilePermissions {
    mode: u32,
}

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct FileType {
    mode: u32,
}

#[derive(Debug)]
pub struct DirBuilder {}

impl FileAttr {
    pub fn size(&self) -> u64 {
        self.stat.st_size as u64
    }
    pub fn perm(&self) -> FilePermissions {
        FilePermissions { mode: self.stat.st_mode }
    }
    pub fn file_type(&self) -> FileType {
        FileType { mode: self.stat.st_mode }
    }
    pub fn modified(&self) -> io::Result<SystemTime> {
        Ok(systime(&self.stat.st_mtim))
    }
    pub fn accessed(&self) -> io::Result<SystemTime> {
        Ok(systime(&self.stat.st_atim))
    }
    pub fn created(&self) -> io::Result<SystemTime> {
        Ok(systime(&self.stat.st_ctim))
    }
}

impl FilePermissions {
    pub fn readonly(&self) -> bool {
        self.mode & 0o222 == 0
    }
    pub fn set_readonly(&mut self, readonly: bool) {
        if readonly {
            self.mode &= !0o222;
        } else {
            self.mode |= 0o222;
        }
    }
}

impl FileTimes {
    // atime is kernel-owned on Plan 9 (wstat cannot set it); accept and ignore,
    // the POSIX-degrade a filesystem without settable atime allows.
    pub fn set_accessed(&mut self, _t: SystemTime) {}
    pub fn set_modified(&mut self, t: SystemTime) {
        self.modified = Some(t);
    }
}

fn mtime_secs(t: SystemTime) -> io::Result<u64> {
    t.sub_time(&UNIX_EPOCH)
        .map(|d| d.as_secs())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "mtime before the epoch"))
}

impl FileType {
    pub fn is_dir(&self) -> bool {
        self.mode & S_IFMT == S_IFDIR
    }
    pub fn is_file(&self) -> bool {
        self.mode & S_IFMT == S_IFREG
    }
    pub fn is_symlink(&self) -> bool {
        self.mode & S_IFMT == S_IFLNK
    }
}

impl fmt::Debug for ReadDir {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ReadDir").field("root", &self.root).finish()
    }
}

impl Iterator for ReadDir {
    type Item = io::Result<DirEntry>;
    fn next(&mut self) -> Option<io::Result<DirEntry>> {
        loop {
            let ent = unsafe { c_readdir(self.dir) };
            if ent.is_null() {
                return None;
            }
            let name_bytes = unsafe { CStr::from_ptr((*ent).d_name.as_ptr()) }.to_bytes();
            if name_bytes == b"." || name_bytes == b".." {
                continue;
            }
            let name =
                unsafe { OsStr::from_encoded_bytes_unchecked(name_bytes).to_os_string() };
            return Some(Ok(DirEntry {
                name,
                root: self.root.clone(),
                dtype: unsafe { (*ent).d_type },
            }));
        }
    }
}

impl Drop for ReadDir {
    fn drop(&mut self) {
        unsafe { c_closedir(self.dir) };
    }
}

impl DirEntry {
    pub fn path(&self) -> PathBuf {
        self.root.join(&self.name)
    }
    pub fn file_name(&self) -> OsString {
        self.name.clone()
    }
    pub fn metadata(&self) -> io::Result<FileAttr> {
        stat(&self.path())
    }
    pub fn file_type(&self) -> io::Result<FileType> {
        match self.dtype {
            4 => Ok(FileType { mode: S_IFDIR }),
            8 => Ok(FileType { mode: S_IFREG }),
            10 => Ok(FileType { mode: S_IFLNK }),
            _ => self.metadata().map(|m| m.file_type()),
        }
    }
}

impl OpenOptions {
    pub fn new() -> OpenOptions {
        OpenOptions {
            read: false,
            write: false,
            append: false,
            truncate: false,
            create: false,
            create_new: false,
        }
    }
    pub fn read(&mut self, read: bool) {
        self.read = read;
    }
    pub fn write(&mut self, write: bool) {
        self.write = write;
    }
    pub fn append(&mut self, append: bool) {
        self.append = append;
    }
    pub fn truncate(&mut self, truncate: bool) {
        self.truncate = truncate;
    }
    pub fn create(&mut self, create: bool) {
        self.create = create;
    }
    pub fn create_new(&mut self, create_new: bool) {
        self.create_new = create_new;
    }

    fn flags(&self) -> i32 {
        let mut f = if self.read && (self.write || self.append) {
            O_RDWR
        } else if self.write || self.append {
            O_WRONLY
        } else {
            O_RDONLY
        };
        if self.append {
            f |= O_APPEND;
        }
        if self.truncate {
            f |= O_TRUNC;
        }
        if self.create {
            f |= O_CREAT;
        }
        if self.create_new {
            f |= O_CREAT | O_EXCL;
        }
        f
    }
}

impl File {
    pub fn open(path: &Path, opts: &OpenOptions) -> io::Result<File> {
        let p = cpath(path);
        let mode: u32 = 0o666;
        let fd = unsafe { open(p.as_ptr(), opts.flags(), mode) };
        if fd < 0 { Err(io::Error::last_os_error()) } else { Ok(File { fd }) }
    }

    pub fn file_attr(&self) -> io::Result<FileAttr> {
        let mut st: CStat = unsafe { core::mem::zeroed() };
        cvt(unsafe { c_fstat(self.fd, &mut st) })?;
        Ok(FileAttr { stat: st })
    }

    pub fn fsync(&self) -> io::Result<()> {
        Ok(())
    }
    pub fn datasync(&self) -> io::Result<()> {
        Ok(())
    }
    pub fn lock(&self) -> io::Result<()> {
        Ok(())
    }
    pub fn lock_shared(&self) -> io::Result<()> {
        Ok(())
    }
    pub fn try_lock(&self) -> Result<(), TryLockError> {
        Ok(())
    }
    pub fn try_lock_shared(&self) -> Result<(), TryLockError> {
        Ok(())
    }
    pub fn unlock(&self) -> io::Result<()> {
        Ok(())
    }

    pub fn truncate(&self, size: u64) -> io::Result<()> {
        cvt(unsafe { c_ftruncate(self.fd, size as i64) })
    }

    pub fn read(&self, buf: &mut [u8]) -> io::Result<usize> {
        let n = unsafe { c_read(self.fd, buf.as_mut_ptr(), buf.len()) };
        if n < 0 { Err(io::Error::last_os_error()) } else { Ok(n as usize) }
    }

    pub fn read_vectored(&self, bufs: &mut [IoSliceMut<'_>]) -> io::Result<usize> {
        for b in bufs {
            if !b.is_empty() {
                return self.read(b);
            }
        }
        Ok(0)
    }

    pub fn is_read_vectored(&self) -> bool {
        false
    }

    pub fn read_buf(&self, cursor: BorrowedCursor<'_, u8>) -> io::Result<()> {
        crate::io::default_read_buf(|buf| self.read(buf), cursor)
    }

    pub fn write(&self, buf: &[u8]) -> io::Result<usize> {
        let n = unsafe { c_write(self.fd, buf.as_ptr(), buf.len()) };
        if n < 0 { Err(io::Error::last_os_error()) } else { Ok(n as usize) }
    }

    pub fn write_vectored(&self, bufs: &[IoSlice<'_>]) -> io::Result<usize> {
        for b in bufs {
            if !b.is_empty() {
                return self.write(b);
            }
        }
        Ok(0)
    }

    pub fn is_write_vectored(&self) -> bool {
        false
    }

    pub fn flush(&self) -> io::Result<()> {
        Ok(())
    }

    pub fn seek(&self, pos: SeekFrom) -> io::Result<u64> {
        let (whence, off) = match pos {
            SeekFrom::Start(n) => (0, n as i64),
            SeekFrom::End(n) => (2, n),
            SeekFrom::Current(n) => (1, n),
        };
        let r = unsafe { c_lseek(self.fd, off, whence) };
        if r < 0 { Err(io::Error::last_os_error()) } else { Ok(r as u64) }
    }

    pub fn size(&self) -> Option<io::Result<u64>> {
        Some(self.file_attr().map(|a| a.size()))
    }

    pub fn tell(&self) -> io::Result<u64> {
        let off = unsafe { c_lseek(self.fd, 0, 1) };
        if off < 0 { Err(io::Error::last_os_error()) } else { Ok(off as u64) }
    }

    pub fn duplicate(&self) -> io::Result<File> {
        let fd = unsafe { c_dup(self.fd) };
        if fd < 0 { Err(io::Error::last_os_error()) } else { Ok(File { fd }) }
    }

    pub fn set_permissions(&self, perm: FilePermissions) -> io::Result<()> {
        cvt(unsafe { c_fchmod(self.fd, perm.mode & 0o777) })
    }

    pub fn set_times(&self, times: FileTimes) -> io::Result<()> {
        match times.modified {
            Some(t) => cvt(unsafe { cc9_fset_mtime(self.fd, mtime_secs(t)?) }),
            None => Ok(()),
        }
    }
}

impl Drop for File {
    fn drop(&mut self) {
        unsafe { c_close(self.fd) };
    }
}

impl fmt::Debug for File {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("File").field("fd", &self.fd).finish()
    }
}

impl DirBuilder {
    pub fn new() -> DirBuilder {
        DirBuilder {}
    }
    pub fn mkdir(&self, p: &Path) -> io::Result<()> {
        cvt(unsafe { c_mkdir(cpath(p).as_ptr(), 0o777) })
    }
}

pub fn readdir(p: &Path) -> io::Result<ReadDir> {
    let cp = cpath(p);
    let dir = unsafe { c_opendir(cp.as_ptr()) };
    if dir.is_null() {
        Err(io::Error::last_os_error())
    } else {
        Ok(ReadDir { dir, root: p.to_path_buf() })
    }
}

pub fn unlink(p: &Path) -> io::Result<()> {
    cvt(unsafe { c_unlink(cpath(p).as_ptr()) })
}

pub fn rename(old: &Path, new: &Path) -> io::Result<()> {
    match cvt(unsafe { c_rename(cpath(old).as_ptr(), cpath(new).as_ptr()) }) {
        Ok(()) => Ok(()),
        // Plan 9's rename is a `wstat` that only changes the final path component,
        // so it cannot move a file across directories (cc9 returns EXDEV). POSIX
        // rename can, and callers rely on it — notably rustc's atomic metadata
        // write, which renames a temp-subdir file onto the final output. Fall back
        // to copy + remove, which is what POSIX rename degrades to here.
        Err(_) if old.parent() != new.parent() => {
            copy(old, new)?;
            unlink(old)
        }
        Err(e) => Err(e),
    }
}

pub fn set_perm(p: &Path, perm: FilePermissions) -> io::Result<()> {
    cvt(unsafe { c_chmod(cpath(p).as_ptr(), perm.mode & 0o777) })
}

pub fn rmdir(p: &Path) -> io::Result<()> {
    cvt(unsafe { c_rmdir(cpath(p).as_ptr()) })
}

pub fn readlink(_p: &Path) -> io::Result<PathBuf> {
    unsupported()
}

pub fn symlink(_original: &Path, _link: &Path) -> io::Result<()> {
    unsupported()
}

pub fn link(_src: &Path, _dst: &Path) -> io::Result<()> {
    unsupported()
}

pub fn stat(p: &Path) -> io::Result<FileAttr> {
    let cp = cpath(p);
    let mut st: CStat = unsafe { core::mem::zeroed() };
    cvt(unsafe { c_stat(cp.as_ptr(), &mut st) })?;
    Ok(FileAttr { stat: st })
}

pub fn lstat(p: &Path) -> io::Result<FileAttr> {
    // Plan 9 has no POSIX symlinks; lstat == stat.
    stat(p)
}

pub fn set_times(p: &Path, times: FileTimes) -> io::Result<()> {
    match times.modified {
        Some(t) => cvt(unsafe { cc9_set_mtime(cpath(p).as_ptr(), mtime_secs(t)?) }),
        None => Ok(()),
    }
}

pub fn set_times_nofollow(p: &Path, times: FileTimes) -> io::Result<()> {
    // No symlinks on Plan 9: nofollow == follow.
    set_times(p, times)
}

pub fn canonicalize(p: &Path) -> io::Result<PathBuf> {
    // Plan 9 has no symlinks, so the canonical form is just the absolute path
    // lexically normalized (`.`/`..` resolved), verified to exist like realpath.
    use crate::path::Component;
    let abs;
    let p = if p.is_absolute() {
        p
    } else {
        abs = crate::sys::paths::getcwd()?.join(p);
        &abs
    };
    let mut out = PathBuf::from("/");
    for c in p.components() {
        match c {
            Component::RootDir | Component::CurDir | Component::Prefix(_) => {}
            Component::ParentDir => {
                out.pop();
            }
            Component::Normal(s) => out.push(s),
        }
    }
    stat(&out)?;
    Ok(out)
}
