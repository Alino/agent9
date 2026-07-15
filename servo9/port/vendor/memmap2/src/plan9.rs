//! Plan 9 (9front) backend: a mapping is a private heap copy of the file.
//!
//! Plan 9 has no file-backed mmap — cc9's mmap shim serves anonymous memory only
//! (segattach), and the kernel offers nothing to map a file with. Upstream
//! memmap2 therefore compiled its `stub.rs` here (plan9 is neither unix nor
//! windows), where every constructor returns `Unsupported`. That failure is
//! silent at build time and expensive at run time: Servo's font loader does
//! `File::open(..).and_then(|f| Mmap::map(&f))`, so every local font "failed to
//! load" and layout reported "Could not find font" with the font sitting right
//! there on disk.
//!
//! Reading the range into the heap gives each mapping kind exactly what its
//! contract allows us to give:
//!   - `map` / `map_copy_read_only`: a read-only view. Correct, with one honest
//!     ceiling — writes to the file AFTER the map are not reflected (a real
//!     shared mapping would show them). Callers that need that coherence need a
//!     real mmap, which this platform does not have.
//!   - `map_copy`: private copy-on-write — a heap copy IS that, exactly.
//!   - `map_anon`: zeroed private memory — a zeroed buffer IS that, exactly.
//!   - `map_mut` (shared write-back) and `map_exec`: cannot be honoured, and a
//!     lie here corrupts data or crashes later, so they stay `Unsupported`.
//!
//! The buffer is a `Box<[u8]>`, not a `Vec`: `ptr()` hands the address out and
//! the caller holds it for the mapping's lifetime, so the storage must never
//! reallocate or move.

use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};

pub struct MmapInner {
    buf: Box<[u8]>,
    writable: bool,
}

fn read_range(file: &File, offset: u64, len: usize) -> io::Result<Box<[u8]>> {
    // Seek+Read work through &File, but they move the shared cursor, and the
    // caller's File is borrowed, not owned — put the cursor back where it was.
    let mut f = file;
    let old = f.seek(SeekFrom::Current(0))?;
    let result = (|| {
        f.seek(SeekFrom::Start(offset))?;
        let mut buf = vec![0u8; len];
        f.read_exact(&mut buf)?;
        Ok(buf.into_boxed_slice())
    })();
    let _ = f.seek(SeekFrom::Start(old));
    result
}

impl MmapInner {
    pub fn map(
        len: usize,
        file: &File,
        offset: u64,
        _populate: bool,
        _no_reserve: bool,
    ) -> io::Result<MmapInner> {
        Ok(MmapInner { buf: read_range(file, offset, len)?, writable: false })
    }

    pub fn map_exec(
        _len: usize,
        _file: &File,
        _offset: u64,
        _populate: bool,
        _no_reserve: bool,
    ) -> io::Result<MmapInner> {
        // Executable file mappings need real mmap + executable pages; heap
        // memory is NX on a stock 9front kernel. Refuse rather than fault later.
        Err(io::ErrorKind::Unsupported.into())
    }

    pub fn map_mut(
        _len: usize,
        _file: &File,
        _offset: u64,
        _populate: bool,
        _no_reserve: bool,
    ) -> io::Result<MmapInner> {
        // Shared write-back: changes must reach the file and other mappings. A
        // heap copy silently DROPS every write on unmap — worse than failing.
        Err(io::ErrorKind::Unsupported.into())
    }

    pub fn map_copy(
        len: usize,
        file: &File,
        offset: u64,
        _populate: bool,
        _no_reserve: bool,
    ) -> io::Result<MmapInner> {
        Ok(MmapInner { buf: read_range(file, offset, len)?, writable: true })
    }

    pub fn map_copy_read_only(
        len: usize,
        file: &File,
        offset: u64,
        _populate: bool,
        _no_reserve: bool,
    ) -> io::Result<MmapInner> {
        Ok(MmapInner { buf: read_range(file, offset, len)?, writable: false })
    }

    pub fn map_anon(
        len: usize,
        _stack: bool,
        _populate: bool,
        _huge: Option<u8>,
        _no_reserve: bool,
    ) -> io::Result<MmapInner> {
        Ok(MmapInner { buf: vec![0u8; len].into_boxed_slice(), writable: true })
    }

    pub fn flush(&self, _offset: usize, _len: usize) -> io::Result<()> {
        // Nothing here is file-backed (map_mut is refused), so there is nothing
        // a flush could write back.
        Ok(())
    }

    pub fn flush_async(&self, _offset: usize, _len: usize) -> io::Result<()> {
        Ok(())
    }

    pub fn make_read_only(&mut self) -> io::Result<()> {
        // Dropping write access from a private buffer breaks no contract; the
        // "protection" is advisory (we cannot change page permissions), which is
        // fine — safe Rust cannot reach mut_ptr on the read-only wrapper types.
        self.writable = false;
        Ok(())
    }

    pub fn make_exec(&mut self) -> io::Result<()> {
        Err(io::ErrorKind::Unsupported.into())
    }

    pub fn make_mut(&mut self) -> io::Result<()> {
        // Upgrading a `map()` to shared write-back is exactly map_mut; refuse
        // for the same reason. (A map_copy that was made read-only stays
        // demotable-only: we cannot tell the two apart here, and allowing the
        // upgrade on a former map() would lose writes silently.)
        Err(io::ErrorKind::Unsupported.into())
    }

    #[inline]
    pub fn ptr(&self) -> *const u8 {
        self.buf.as_ptr()
    }

    #[inline]
    pub fn mut_ptr(&mut self) -> *mut u8 {
        debug_assert!(self.writable, "mut_ptr on a read-only plan9 mapping");
        self.buf.as_mut_ptr()
    }

    #[inline]
    pub fn len(&self) -> usize {
        self.buf.len()
    }
}

pub fn file_len(file: &File) -> io::Result<u64> {
    Ok(file.metadata()?.len())
}
