//! Get image width and height reading as few bytes as possible.
//! 
//! [GitHub](https://github.com/panzi/imsz) – [C API Reference](https://panzi.github.io/imsz/c)
//! 
//! Example:
//! ```
//! # use std::io::BufReader;
//! # use std::fs::File;
//! # fn main() -> imsz::ImResult<()> {
//! use imsz::imsz;
//! 
//! let fname = "testdata/image.gif";
//! let info = imsz(fname)?;
//! println!("{}: {}, {} x {}", fname, info.format, info.width, info.height);
//! // testdata/image.gif: GIF, 32 x 16
//! 
//! // alternatively if you have something implementing std::io::Read and std::io::Seek:
//! use imsz::imsz_from_reader;
//! 
//! let mut file = BufReader::new(File::open(fname)?);
//! let info = imsz_from_reader(&mut file)?;
//! # Ok(())
//! # }
//! ```

use std::fs::File;
use std::io::{Read, Seek, SeekFrom, BufReader};

#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum ImFormat {
    /// Graphics Interchange Format files in version GIF87a or GIF89a.
    GIF     =  1,

    /// Portable Network Graphics files. Requires the first chunk to be `IHDR`.
    PNG     =  2,

    /// Windows Bitmap, both for Windows 2.0 (BITMAPCOREHEADER) and for newer
    /// versions (BITMAPINFOHEADER).
    BMP     =  3,

    /// Joint Photographic Experts Group files.
    JPEG    =  4,

    /// WebP files. Supported sub-formats: `VP8 `, `VP8L`, `VP8X`.
    WEBP    =  5,

    /// Quite OK Image format files.
    QOI     =  6,

    /// Adobe Photoshop files.
    PSD     =  7,

    /// GIMP files.
    XCF     =  8,

    /// ICO files can contain multiple images. This returns the dimensions of
    /// the biggest image in the file.
    ICO     =  9,

    /// AV1 Image File Format.
    AVIF    = 10,

    /// Tag Image File Format. Supports big endian and little endian TIFF files.
    TIFF    = 11,

    /// OpenEXR files.
    OpenEXR = 12,

    /// PiCture eXchange files.
    PCX     = 13,

    /// TARGA (Truevision Advanced Raster Graphics Adapter) files.
    /// 
    /// Only if the file ends in `b"TRUEVISION-XFILE.\0"` since otherwise there
    /// is no good way to detect TGA files. Note that this string is optional
    /// to this file format and thus there can be TGA files that aren't supported
    /// by this library.
    TGA     = 14,

    /// DirectDraw Surface files.
    DDS     = 15,

    /// HEIC/HEIF files. These are extremely similar to AVIF and use the same
    /// parsing code.
    HEIF    = 16,

    /// JPEG 2000 files.
    JP2K    = 17,

    /// Device-Independent Bitmap files.
    DIB     = 18,

    /// Valve Texture Format.
    VTF     = 19,

    /// Interleaved Bitmap files, including Planar Bitmap variant.
    ILBM    = 20,
}

impl ImFormat {
    pub const fn name(&self) -> &'static str {
        match self {
            Self::GIF     => "GIF",
            Self::PNG     => "PNG",
            Self::BMP     => "BMP",
            Self::JPEG    => "JPEG",
            Self::WEBP    => "WebP",
            Self::QOI     => "QOI",
            Self::PSD     => "PSD",
            Self::XCF     => "XCF",
            Self::ICO     => "ICO",
            Self::AVIF    => "AVIF",
            Self::TIFF    => "TIFF",
            Self::OpenEXR => "OpenEXR",
            Self::PCX     => "PCX",
            Self::TGA     => "TGA",
            Self::DDS     => "DDS",
            Self::HEIF    => "HEIF",
            Self::JP2K    => "JPEG 2000",
            Self::DIB     => "DIB",
            Self::VTF     => "VTF",
            Self::ILBM    => "ILBM",
        }
    }
}

impl std::fmt::Display for ImFormat {
    #[inline]
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.name().fmt(f)
    }
}

/// The width, height and format of an image.
#[derive(Debug, Clone)]
pub struct ImInfo {
    pub width:  u64,
    pub height: u64,
    pub format: ImFormat,
}

#[derive(Debug)]
pub enum ImError {
    /// If there was an IO error reading the image file this error is returend.
    IO(std::io::Error),

    /// If the image format couldn't be detected this error is returend.
    UnknownFormat,

    /// If the image format was detected, but then something went wrong parsing
    /// the file this error is returned.
    ParserError(ImFormat),
}

impl std::fmt::Display for ImError {
    #[inline]
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::IO(error) => error.fmt(f),
            Self::UnknownFormat => "Unknown Format".fmt(f),
            Self::ParserError(format) => write!(f, "Error parsing {format} image")
        }
    }
}

impl std::error::Error for ImError {
    #[inline]
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::IO(error) => Some(error),
            _ => None
        }
    }
}

impl From<std::io::Error> for ImError {
    #[inline]
    fn from(error: std::io::Error) -> Self {
        ImError::IO(error)
    }
}

pub type ImResult<T> = std::result::Result<T, ImError>;

trait Ratio<T: Sized> {
    fn value<R>(&self) -> R::Output
    where R: Sized, R: std::ops::Div, R: From<T>;
}

impl Ratio<u32> for (u32, u32) {
    #[inline]
    fn value<R>(&self) -> R::Output
    where R: Sized, R: std::ops::Div, R: From<u32> {
        let (a, b) = *self;
        let x: R = a.into();
        let y: R = b.into();
        x / y
    }
}

impl Ratio<i32> for (i32, i32) {
    #[inline]
    fn value<R>(&self) -> R::Output
    where R: Sized, R: std::ops::Div, R: From<i32> {
        let (a, b) = *self;
        let x: R = a.into();
        let y: R = b.into();
        x / y
    }
}

trait BinaryReader {
    #[inline]
    fn read_u8(reader: &mut impl Read) -> std::io::Result<u8> {
        let mut buf = [0u8];
        reader.read_exact(&mut buf)?;
        return Ok(buf[0]);
    }

    #[inline]
    fn read_uchar(reader: &mut impl Read) -> std::io::Result<u8> {
        let mut buf = [0u8];
        reader.read_exact(&mut buf)?;
        return Ok(buf[0]);
    }

    #[inline]
    fn read_i8(reader: &mut impl Read) -> std::io::Result<i8> {
        let mut buf = [0u8];
        reader.read_exact(&mut buf)?;
        return Ok(buf[0] as i8);
    }

    #[inline]
    fn read_ichar(reader: &mut impl Read) -> std::io::Result<i8> {
        let mut buf = [0u8];
        reader.read_exact(&mut buf)?;
        return Ok(buf[0] as i8);
    }

    fn get_u32(buf: [u8; 4]) -> u32;

    fn read_u16(reader: &mut impl Read) -> std::io::Result<u16>;
    fn read_u32(reader: &mut impl Read) -> std::io::Result<u32>;
    fn read_uratio(reader: &mut impl Read) -> std::io::Result<(u32, u32)>;

    fn read_i16(reader: &mut impl Read) -> std::io::Result<i16>;
    fn read_i32(reader: &mut impl Read) -> std::io::Result<i32>;
    fn read_iratio(reader: &mut impl Read) -> std::io::Result<(i32, i32)>;

    fn read_f32(reader: &mut impl Read) -> std::io::Result<f32>;
    fn read_f64(reader: &mut impl Read) -> std::io::Result<f64>;
}

struct LittleEndianReader;
struct BigEndianReader;

impl BinaryReader for LittleEndianReader {
    #[inline]
    fn get_u32(buf: [u8; 4]) -> u32 {
        return u32::from_le_bytes(buf);
    }

    #[inline]
    fn read_u16(reader: &mut impl Read) -> std::io::Result<u16> {
        let mut buf = [0u8; 2];
        reader.read_exact(&mut buf)?;
        return Ok(u16::from_le_bytes(buf));
    }

    #[inline]
    fn read_u32(reader: &mut impl Read) -> std::io::Result<u32> {
        let mut buf = [0u8; 4];
        reader.read_exact(&mut buf)?;
        return Ok(u32::from_le_bytes(buf));
    }

    #[inline]
    fn read_uratio(reader: &mut impl Read) -> std::io::Result<(u32, u32)> {
        let mut buf = [0u8; 8];
        reader.read_exact(&mut buf)?;
        return Ok((
            u32::from_le_bytes([ buf[0], buf[1], buf[2], buf[3] ]),
            u32::from_le_bytes([ buf[4], buf[5], buf[6], buf[7] ]),
        ));
    }

    #[inline]
    fn read_i16(reader: &mut impl Read) -> std::io::Result<i16> {
        let mut buf = [0u8; 2];
        reader.read_exact(&mut buf)?;
        return Ok(i16::from_le_bytes(buf));
    }

    #[inline]
    fn read_i32(reader: &mut impl Read) -> std::io::Result<i32> {
        let mut buf = [0u8; 4];
        reader.read_exact(&mut buf)?;
        return Ok(i32::from_le_bytes(buf));
    }

    #[inline]
    fn read_iratio(reader: &mut impl Read) -> std::io::Result<(i32, i32)> {
        let mut buf = [0u8; 8];
        reader.read_exact(&mut buf)?;
        return Ok((
            i32::from_le_bytes([ buf[0], buf[1], buf[2], buf[3] ]),
            i32::from_le_bytes([ buf[4], buf[5], buf[6], buf[7] ]),
        ));
    }

    #[inline]
    fn read_f32(reader: &mut impl Read) -> std::io::Result<f32> {
        let mut buf = [0u8; 4];
        reader.read_exact(&mut buf)?;
        return Ok(f32::from_le_bytes(buf));
    }

    #[inline]
    fn read_f64(reader: &mut impl Read) -> std::io::Result<f64> {
        let mut buf = [0u8; 8];
        reader.read_exact(&mut buf)?;
        return Ok(f64::from_le_bytes(buf));
    }
}

impl BinaryReader for BigEndianReader {
    #[inline]
    fn get_u32(buf: [u8; 4]) -> u32 {
        return u32::from_be_bytes(buf);
    }

    #[inline]
    fn read_u16(reader: &mut impl Read) -> std::io::Result<u16> {
        let mut buf = [0u8; 2];
        reader.read_exact(&mut buf)?;
        return Ok(u16::from_be_bytes(buf));
    }

    #[inline]
    fn read_u32(reader: &mut impl Read) -> std::io::Result<u32> {
        let mut buf = [0u8; 4];
        reader.read_exact(&mut buf)?;
        return Ok(u32::from_be_bytes(buf));
    }

    #[inline]
    fn read_uratio(reader: &mut impl Read) -> std::io::Result<(u32, u32)> {
        let mut buf = [0u8; 8];
        reader.read_exact(&mut buf)?;
        return Ok((
            u32::from_be_bytes([ buf[0], buf[1], buf[2], buf[3] ]),
            u32::from_be_bytes([ buf[4], buf[5], buf[6], buf[7] ]),
        ));
    }

    #[inline]
    fn read_i16(reader: &mut impl Read) -> std::io::Result<i16> {
        let mut buf = [0u8; 2];
        reader.read_exact(&mut buf)?;
        return Ok(i16::from_be_bytes(buf));
    }

    #[inline]
    fn read_i32(reader: &mut impl Read) -> std::io::Result<i32> {
        let mut buf = [0u8; 4];
        reader.read_exact(&mut buf)?;
        return Ok(i32::from_be_bytes(buf));
    }

    #[inline]
    fn read_iratio(reader: &mut impl Read) -> std::io::Result<(i32, i32)> {
        let mut buf = [0u8; 8];
        reader.read_exact(&mut buf)?;
        return Ok((
            i32::from_be_bytes([ buf[0], buf[1], buf[2], buf[3] ]),
            i32::from_be_bytes([ buf[4], buf[5], buf[6], buf[7] ]),
        ));
    }

    #[inline]
    fn read_f32(reader: &mut impl Read) -> std::io::Result<f32> {
        let mut buf = [0u8; 4];
        reader.read_exact(&mut buf)?;
        return Ok(f32::from_be_bytes(buf));
    }

    #[inline]
    fn read_f64(reader: &mut impl Read) -> std::io::Result<f64> {
        let mut buf = [0u8; 8];
        reader.read_exact(&mut buf)?;
        return Ok(f64::from_be_bytes(buf));
    }
}

macro_rules! array2 {
    ($data:expr, $offset:expr) => {
        [ $data[$offset], $data[$offset + 1] ]
    };
}

macro_rules! array4 {
    ($data:expr, $offset:expr) => {
        [ $data[$offset], $data[$offset + 1], $data[$offset + 2], $data[$offset + 3] ]
    };
}

macro_rules! map_err {
    ($fmt:expr, $expr:expr) => {
        if let Err(_) = $expr {
            return Err(ImError::ParserError($fmt));
        }
    };

    ($fmt:ident $expr:expr) => {
        map_err!(ImFormat::$fmt, $expr);
    };
}

macro_rules! map_expr {
    ($fmt:ident $expr:expr) => {
        match $expr {
            Err(_) => return Err(ImError::ParserError(ImFormat::$fmt)),
            Ok(value) => value
        }
    };
}

fn find_riff_chunk<R>(reader: &mut R, name: &[u8; 4], chunk_size: u64, format: ImFormat) -> ImResult<u64>
where R: Read, R: Seek {
    let mut sub_chunk_size;
    let mut buf = [0u8; 8];
    let mut offset = 0;

    loop {
        if offset > chunk_size {
            return Err(ImError::ParserError(format));
        }
        if let Err(_) = reader.read_exact(&mut buf) {
            return Err(ImError::ParserError(format));
        }
        sub_chunk_size = u32::from_be_bytes(array4!(&buf, 0)) as u64;
        if sub_chunk_size < 8 {
            return Err(ImError::ParserError(format));
        }
        if &buf[4..8] == name {
            break;
        }
        offset += sub_chunk_size;
        if let Err(_) = reader.seek(SeekFrom::Current(sub_chunk_size as i64 - 8)) {
            return Err(ImError::ParserError(format));
        }
    }

    return Ok(sub_chunk_size);
}

fn parse_tiff<BR, R>(reader: &mut R, preamble: &[u8]) -> ImResult<ImInfo>
where BR: BinaryReader, R: Read, R: Seek {
    let ifd_offset = BR::get_u32(array4!(preamble, 4));
    map_err!(TIFF reader.seek(SeekFrom::Start(ifd_offset as u64)));

    let ifd_entry_count = map_expr!(TIFF BR::read_u16(reader)) as u32;
    // 2 bytes: TagId + 2 bytes: type + 4 bytes: count of values + 4
    // bytes: value offset
    let mut width:  Option<u64> = None;
    let mut height: Option<u64> = None;

    for index in 0..ifd_entry_count {
        // sizeof ifd_entry_count = 2
        let entry_offset = ifd_offset + 2 + index * 12;
        map_err!(TIFF reader.seek(SeekFrom::Start(entry_offset as u64)));
        let tag = map_expr!(TIFF BR::read_u16(reader));

        // 256 ... width
        // 257 ... height
        if tag == 256 || tag == 257 {
            // if type indicates that value fits into 4 bytes, value
            // offset is not an offset but value itself
            let ftype = map_expr!(TIFF BR::read_u16(reader));
            map_err!(TIFF reader.seek(SeekFrom::Start(entry_offset as u64 + 8)));
            let value: u64 = match ftype {
                 1 => map_expr!(TIFF BR::read_u8(reader)).into(),
                 2 => map_expr!(TIFF BR::read_uchar(reader)).into(),
                 3 => map_expr!(TIFF BR::read_u16(reader)).into(),
                 4 => map_expr!(TIFF BR::read_u32(reader)).into(),
                 5 => map_expr!(TIFF BR::read_uratio(reader)).value::<u64>(),
                 6 => map_expr!(TIFF BR::read_i8(reader)).max(0) as u64,
                 7 => map_expr!(TIFF BR::read_ichar(reader)).max(0) as u64,
                 8 => map_expr!(TIFF BR::read_i16(reader)).max(0) as u64,
                 9 => map_expr!(TIFF BR::read_i32(reader)).max(0) as u64,
                10 => map_expr!(TIFF BR::read_iratio(reader)).value::<i64>().max(0) as u64,
                11 => map_expr!(TIFF BR::read_f32(reader)) as u64,
                12 => map_expr!(TIFF BR::read_f64(reader)) as u64,
                _ => return Err(ImError::ParserError(ImFormat::TIFF))
            };

            if tag == 256 {
                if let Some(height) = height {
                    return Ok(ImInfo {
                        format: ImFormat::TIFF,
                        width: value,
                        height,
                    });
                }
                width = Some(value);
            } else {
                if let Some(width) = width {
                    return Ok(ImInfo {
                        format: ImFormat::TIFF,
                        width,
                        height: value,
                    });
                }
                height = Some(value);
            }
        }
    }

    return Err(ImError::ParserError(ImFormat::TIFF));
}

#[inline]
fn is_tga<R>(file: &mut R) -> std::io::Result<bool>
where R: Read, R: Seek {
    file.seek(SeekFrom::End(-18))?;
    let mut buf = [0u8; 18];
    file.read_exact(&mut buf)?;
    return Ok(&buf == b"TRUEVISION-XFILE.\0");
}

/// Trait to provide generic [imsz()] function for paths, buffers, and readers.
pub trait Imsz {
    fn imsz(self) -> ImResult<ImInfo>;
}

impl Imsz for &str {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_path(self);
    }
}

impl Imsz for &String {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_path(self.as_str());
    }
}

impl Imsz for String {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_path(self.as_str());
    }
}

impl Imsz for &std::ffi::OsStr {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_path(self);
    }
}

impl Imsz for &std::ffi::OsString {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_path(self.as_os_str());
    }
}

impl Imsz for std::ffi::OsString {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_path(self.as_os_str());
    }
}

impl Imsz for &std::path::Path {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_path(self);
    }
}

impl Imsz for &std::path::PathBuf {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_path(self.as_path());
    }
}

impl Imsz for std::path::PathBuf {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_path(self.as_path());
    }
}

impl Imsz for &[u8] {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_reader(&mut std::io::Cursor::new(self));
    }
}

impl<const LEN: usize> Imsz for [u8; LEN] {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_reader(&mut std::io::Cursor::new(&self[..]));
    }
}

impl<const LEN: usize> Imsz for &[u8; LEN] {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_reader(&mut std::io::Cursor::new(&self[..]));
    }
}

impl Imsz for &mut std::fs::File {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_reader(&mut BufReader::new(self));
    }
}

impl Imsz for std::fs::File {
    #[inline]
    fn imsz(mut self) -> ImResult<ImInfo> {
        return imsz_from_reader(&mut BufReader::new(&mut self));
    }
}

impl Imsz for std::io::Stdin {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return (&self).imsz();
    }
}

/// Plan 9: `Imsz for &Stdin` is unavailable.
///
/// The impl below turns stdin into a seekable `File` via `File::from_raw_fd`.
/// rust9's plan9 std does not implement the fd traits for `fs::File` yet (its
/// File is not FileDesc-based), so that route does not exist here. Rather than
/// fake it, say so: reading an image from stdin is not something Servo does, and
/// `imsz_from_reader` — the API Servo actually uses — is unaffected.
///
/// (Note the impl below also builds a `File` from a borrowed fd, which closes
/// stdin when dropped. Not a pattern worth reproducing.)
#[cfg(target_os = "plan9")]
impl Imsz for &std::io::Stdin {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        Err(ImError::IO(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "imsz: reading images from stdin is not supported on plan9",
        )))
    }
}

#[cfg(any(target_family="unix", target_family="windows"))]
impl Imsz for &std::io::Stdin {
    /// WARNING: This looses already buffered input!
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        let lock = self.lock();

        #[cfg(target_family="unix")]
        let mut seekable_stdin = unsafe {
            use std::os::unix::io::{AsRawFd, FromRawFd};
            std::fs::File::from_raw_fd(lock.as_raw_fd())
        };

        #[cfg(target_family="windows")]
        let mut seekable_stdin = unsafe {
            use std::os::windows::io::{AsRawHandle, FromRawHandle};
            std::fs::File::from_raw_handle(lock.as_raw_handle())
        };

        let result = imsz_from_reader(&mut BufReader::new(&mut seekable_stdin));

        // Be sure the lock is released *after* all my IO happened.
        drop(lock);

        return result;
    }
}

impl Imsz for &mut std::io::Cursor<&[u8]> {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_reader(self);
    }
}

impl Imsz for std::io::Cursor<&[u8]> {
    #[inline]
    fn imsz(mut self) -> ImResult<ImInfo> {
        return imsz_from_reader(&mut self);
    }
}

impl<const LEN: usize> Imsz for std::io::Cursor<&[u8; LEN]> {
    #[inline]
    fn imsz(mut self) -> ImResult<ImInfo> {
        return imsz_from_reader(&mut self);
    }
}

impl<const LEN: usize> Imsz for std::io::Cursor<[u8; LEN]> {
    #[inline]
    fn imsz(mut self) -> ImResult<ImInfo> {
        return imsz_from_reader(&mut self);
    }
}

impl<R> Imsz for &mut std::io::BufReader<R> where R: Read, R: Seek {
    #[inline]
    fn imsz(self) -> ImResult<ImInfo> {
        return imsz_from_reader(self);
    }
}

impl<R> Imsz for std::io::BufReader<R> where R: Read, R: Seek {
    #[inline]
    fn imsz(mut self) -> ImResult<ImInfo> {
        return imsz_from_reader(&mut self);
    }
}

/// Read width and height of an image.
/// 
/// `input` can be a file path, a byte buffer, a file reader, or a buffered reader.
#[inline]
pub fn imsz(input: impl Imsz) -> ImResult<ImInfo> {
    return input.imsz();
}

/// Read width and height of an image.
#[inline]
pub fn imsz_from_path(path: impl AsRef<std::path::Path>) -> ImResult<ImInfo> {
    let mut reader = BufReader::new(File::open(path)?);
    return imsz_from_reader(&mut reader);
}

/// Read width and height of an image.
/// 
/// Some file formats (like JPEG) need repeated small reads, so passing a
/// `std::io::BufReader` is recommended.
pub fn imsz_from_reader<R>(file: &mut R) -> ImResult<ImInfo>
where R: Read, R: Seek {
    let mut preamble = [0u8; 30];

    let size = file.read(&mut preamble)?;

    if size >= 6 && (&preamble[..6] == b"GIF87a" || &preamble[..6] == b"GIF89a") {
        // GIF
        if size < 10 {
            return Err(ImError::ParserError(ImFormat::GIF));
        }
        let w = u16::from_le_bytes(array2!(preamble, 6));
        let h = u16::from_le_bytes(array2!(preamble, 8));

        return Ok(ImInfo {
            format: ImFormat::GIF,
            width:  w as u64,
            height: h as u64,
        });
    } else if size >= 8 && preamble.starts_with(b"\x89PNG\r\n\x1a\n") {
        // PNG
        if size < 24 {
            return Err(ImError::ParserError(ImFormat::PNG));
        }

        let chunk_size = u32::from_be_bytes(array4!(preamble, 8));
        if chunk_size < 8 || &preamble[12..16] != b"IHDR" {
            return Err(ImError::ParserError(ImFormat::PNG));
        }

        let w = u32::from_be_bytes(array4!(preamble, 16));
        let h = u32::from_be_bytes(array4!(preamble, 20));

        return Ok(ImInfo {
            format: ImFormat::PNG,
            width:  w as u64,
            height: h as u64,
        });
    } else if size >= 10 && preamble.starts_with(b"BM") && &preamble[6..10] == b"\0\0\0\0" {
        // BMP
        let file_size = u32::from_le_bytes(array4!(preamble, 2));
        let min_size = (file_size as usize).min(size);
        if min_size < 22 {
            return Err(ImError::ParserError(ImFormat::BMP));
        }

        let header_size = u32::from_le_bytes(array4!(preamble, 14));
        if header_size == 12 {
            // Windows 2.0 BITMAPCOREHEADER
            let w = i16::from_le_bytes(array2!(preamble, 18));
            let h = i16::from_le_bytes(array2!(preamble, 20));

            return Ok(ImInfo {
                format: ImFormat::BMP,
                width:  w as u64,
                // h is negative when stored upside down
                height: h.abs() as u64,
            });
        } else {
            if min_size < 26 || header_size <= 12 {
                return Err(ImError::ParserError(ImFormat::BMP));
            }
            let w = i32::from_le_bytes(array4!(preamble, 18));
            let h = i32::from_le_bytes(array4!(preamble, 22));

            return Ok(ImInfo {
                format: ImFormat::BMP,
                width:  w as u64,
                // h is negative when stored upside down
                height: h.abs() as u64
            });
        }
    } else if size >= 3 && &preamble[..2] == b"\xff\xd8" {
        // JPEG
        map_err!(JPEG file.seek(SeekFrom::Start(3)));
        let mut buf1: [u8; 1] = [ preamble[2] ];
        let mut buf2: [u8; 2] = [0; 2];
        let mut buf4: [u8; 4] = [0; 4];
        while buf1[0] != b'\xda' && buf1[0] != 0 {
            while buf1[0] != b'\xff' {
                map_err!(JPEG file.read_exact(&mut buf1));
            }
            while buf1[0] == b'\xff' {
                map_err!(JPEG file.read_exact(&mut buf1));
            }
            if buf1[0] >= 0xc0 && buf1[0] <= 0xc3 {
                map_err!(JPEG file.seek(SeekFrom::Current(3)));
                map_err!(JPEG file.read_exact(&mut buf4));
                let h = u16::from_be_bytes(array2!(buf4, 0));
                let w = u16::from_be_bytes(array2!(buf4, 2));

                return Ok(ImInfo {
                    format: ImFormat::JPEG,
                    width:  w as u64,
                    height: h as u64,
                });
            }
            map_err!(JPEG file.read_exact(&mut buf2));
            let b = u16::from_be_bytes(buf2);
            let offset = (b - 2) as i64;
            map_err!(JPEG file.seek(SeekFrom::Current(offset)));
            map_err!(JPEG file.read_exact(&mut buf1));
        }
        return Err(ImError::ParserError(ImFormat::JPEG));
    } else if size >= 30 && preamble.starts_with(b"RIFF") && &preamble[8..12] == b"WEBP" {
        // WEBP
        let hdr = &preamble[12..16];
        if hdr == b"VP8L" {
            let b0 = preamble[21];
            let b1 = preamble[22];
            let b2 = preamble[23];
            let b3 = preamble[24];

            let w = 1u32 + ((((b1 & 0x3F) as u32) << 8) | b0 as u32);
            let h = 1u32 + ((((b3 & 0xF) as u32) << 10) | ((b2 as u32) << 2) | ((b1 & 0xC0) as u32 >> 6));

            return Ok(ImInfo {
                format: ImFormat::WEBP,
                width:  w as u64,
                height: h as u64,
            });
        } else if hdr == b"VP8 " {
            let b0 = preamble[23];
            let b1 = preamble[24];
            let b2 = preamble[25];
            if b0 != 0x9d || b1 != 0x01 || b2 != 0x2a {
                return Err(ImError::ParserError(ImFormat::WEBP));
            }
            let w = u16::from_le_bytes(array2!(preamble, 26));
            let h = u16::from_le_bytes(array2!(preamble, 28));
            return Ok(ImInfo {
                format: ImFormat::WEBP,
                width:  w as u64 & 0x3ffff,
                height: h as u64 & 0x3ffff,
            });
        } else if hdr == b"VP8X" {
            let w1 = preamble[24] as u32;
            let w2 = preamble[25] as u32;
            let w3 = preamble[26] as u32;
            let h1 = preamble[27] as u32;
            let h2 = preamble[28] as u32;
            let h3 = preamble[29] as u32;

            let width  = (w1 | w2 << 8 | w3 << 16) + 1;
            let height = (h1 | h2 << 8 | h3 << 16) + 1;

            return Ok(ImInfo {
                format: ImFormat::WEBP,
                width:  width  as u64,
                height: height as u64,
            });
        }
        return Err(ImError::ParserError(ImFormat::WEBP));
    } else if size >= 12 && (&preamble[4..12] == b"ftypavif" || &preamble[4..12] == b"ftypheic") {
        // AVIF and HEIF
        let format = if &preamble[8..12] == b"avif" {
            ImFormat::AVIF
        } else {
            ImFormat::HEIF
        };

        let ftype_size = u32::from_be_bytes(array4!(preamble, 0));
        if ftype_size < 12 {
            return Err(ImError::ParserError(format));
        }
        map_err!(format, file.seek(SeekFrom::Start(ftype_size as u64)));

        // chunk nesting: meta > iprp > ipco > ispe
        let chunk_size = find_riff_chunk(file, b"meta", u64::MAX, format)?;
        if chunk_size < 12 {
            return Err(ImError::ParserError(format));
        }
        map_err!(format, file.seek(SeekFrom::Current(4)));
        let chunk_size = find_riff_chunk(file, b"iprp", chunk_size - 12, format)?;
        let chunk_size = find_riff_chunk(file, b"ipco", chunk_size -  8, format)?;
        let chunk_size = find_riff_chunk(file, b"ispe", chunk_size -  8, format)?;

        if chunk_size < 12 {
            return Err(ImError::ParserError(format));
        }

        let mut buf = [0u8; 12];
        map_err!(format, file.read_exact(&mut buf));

        let w = u32::from_be_bytes(array4!(buf, 4));
        let h = u32::from_be_bytes(array4!(buf, 8));

        return Ok(ImInfo {
            format,
            width:  w as u64,
            height: h as u64,
        });
    } else if size >= 24 && preamble.starts_with(b"\0\0\0\x0CjP  ") && &preamble[16..24] == b"ftypjp2 " {
        // JPEG 2000
        let chunk_size = u32::from_be_bytes(array4!(preamble, 12));
        map_err!(JP2K file.seek(SeekFrom::Start(12 + chunk_size as u64)));
        let chunk_size = find_riff_chunk(file, b"jp2h", u64::MAX, ImFormat::JP2K)?;
        let chunk_size = find_riff_chunk(file, b"ihdr", chunk_size, ImFormat::JP2K)?;

        if chunk_size < 8 {
            return Err(ImError::ParserError(ImFormat::JP2K));
        }

        let mut buf = [0u8; 8];
        map_err!(JP2K file.read_exact(&mut buf));

        let h = u32::from_be_bytes(array4!(buf, 0));
        let w = u32::from_be_bytes(array4!(buf, 4));

        return Ok(ImInfo {
            format: ImFormat::JP2K,
            width:  w as u64,
            height: h as u64,
        });
    } else if size >= 8 && (preamble.starts_with(b"II*\0") || preamble.starts_with(b"MM\0*")) {
        // TIFF
        if preamble.starts_with(b"MM") {
            // big endian
            return parse_tiff::<BigEndianReader, R>(file, &preamble[..size]);
        } else {
            // little endian
            return parse_tiff::<LittleEndianReader, R>(file, &preamble[..size]);
        }
    } else if size >= 14 && preamble.starts_with(b"qoif") {
        // QOI
        let w = u32::from_be_bytes(array4!(preamble, 4));
        let h = u32::from_be_bytes(array4!(preamble, 8));

        return Ok(ImInfo {
            format: ImFormat::QOI,
            width:  w as u64,
            height: h as u64,
        });
    } else if size >= 22 && preamble.starts_with(b"8BPS\0\x01\0\0\0\0\0\0") {
        // PSD
        let h = u32::from_be_bytes(array4!(preamble, 14));
        let w = u32::from_be_bytes(array4!(preamble, 18));

        return Ok(ImInfo {
            format: ImFormat::PSD,
            width:  w as u64,
            height: h as u64,
        });
    } else if size >= 22 && preamble.starts_with(b"gimp xcf ") && preamble[13] == 0 {
        // XCF
        let w = u32::from_be_bytes(array4!(preamble, 14));
        let h = u32::from_be_bytes(array4!(preamble, 18));

        return Ok(ImInfo {
            format: ImFormat::XCF,
            width:  w as u64,
            height: h as u64,
        });
    } else if size >= 6 && preamble.starts_with(b"\0\0\x01\0") {
        // ICO
        let count = u16::from_le_bytes(array2!(preamble, 4));
        map_err!(ICO file.seek(SeekFrom::Start(6)));

        let mut buf = [0u8; 16];
        let mut width:  u32 = 0;
        let mut height: u32 = 0;
        for _ in 0..count {
            map_err!(ICO file.read_exact(&mut buf));
            let w = buf[0] as u32;
            let h = buf[1] as u32;
            if w >= width && h >= height {
                width  = w;
                height = h;
            }
        }

        return Ok(ImInfo {
            format: ImFormat::ICO,
            width:  width  as u64,
            height: height as u64,
        });
    } else if size > 8 && preamble.starts_with(b"\x76\x2f\x31\x01") && (preamble[4] == 0x01 || preamble[4] == 0x02) {
        // OpenEXR
        // https://www.openexr.com/documentation/openexrfilelayout.pdf
        map_err!(OpenEXR file.seek(SeekFrom::Start(8)));

        let mut name_buf = Vec::new();
        let mut type_buf = Vec::new();
        let mut buf1 = [0u8];
        let mut buf4 = [0u8; 4];

        loop {
            name_buf.clear();
            loop {
                map_err!(OpenEXR file.read_exact(&mut buf1));
                let byte = buf1[0];
                if byte == 0 {
                    break;
                }
                name_buf.push(byte);
            }

            if name_buf.is_empty() {
                break;
            }

            type_buf.clear();
            loop {
                map_err!(OpenEXR file.read_exact(&mut buf1));
                let byte = buf1[0];
                if byte == 0 {
                    break;
                }
                type_buf.push(byte);
            }

            map_err!(OpenEXR file.read_exact(&mut buf4));
            let size = u32::from_le_bytes(buf4);

            if &name_buf == b"displayWindow" {
                if &type_buf != b"box2i" || size != 16 {
                    return Err(ImError::ParserError(ImFormat::OpenEXR));
                }

                let mut box_buf = [0u8; 16];
                map_err!(OpenEXR file.read_exact(&mut box_buf));

                let x1 = i32::from_le_bytes(array4!(box_buf,  0)) as i64;
                let y1 = i32::from_le_bytes(array4!(box_buf,  4)) as i64;
                let x2 = i32::from_le_bytes(array4!(box_buf,  8)) as i64;
                let y2 = i32::from_le_bytes(array4!(box_buf, 12)) as i64;

                let width  = x2 - x1 + 1;
                let height = y2 - y1 + 1;

                if width <= 0 || height <= 0 {
                    return Err(ImError::ParserError(ImFormat::OpenEXR));
                }

                return Ok(ImInfo {
                    format: ImFormat::OpenEXR,
                    width:  width  as u64,
                    height: height as u64,
                });
            } else {
                map_err!(OpenEXR file.seek(SeekFrom::Current(size as i64)));
            }
        }

        return Err(ImError::ParserError(ImFormat::OpenEXR));
    } else if size >= 30 && preamble[0] == 0x0A && preamble[1] < 6 && (preamble[3] == 1 || preamble[3] == 2 || preamble[3] == 4 || preamble[3] == 8) {
        // PCX
        let x1 = u16::from_le_bytes(array2!(preamble,  4)) as i64;
        let y1 = u16::from_le_bytes(array2!(preamble,  6)) as i64;
        let x2 = u16::from_le_bytes(array2!(preamble,  8)) as i64;
        let y2 = u16::from_le_bytes(array2!(preamble, 10)) as i64;

        let width  = x2 - x1 + 1;
        let height = y2 - y1 + 1;

        if width <= 0 || height <= 0 {
            return Err(ImError::ParserError(ImFormat::PCX));
        }

        return Ok(ImInfo {
            format: ImFormat::PCX,
            width:  width  as u64,
            height: height as u64,
        });
    } else if size >= 30 && preamble.starts_with(b"DDS \x7C\0\0\0") && (u32::from_le_bytes(array4!(preamble, 8)) & 0x1007) != 0 {
        // DDS
        // http://doc.51windows.net/directx9_sdk/graphics/reference/DDSFileReference/ddsfileformat.htm
        // https://docs.microsoft.com/en-us/windows/win32/direct3ddds/dds-header

        let h = u32::from_le_bytes(array4!(preamble, 12));
        let w = u32::from_le_bytes(array4!(preamble, 16));

        return Ok(ImInfo {
            format: ImFormat::DDS,
            width:  w as u64,
            height: h as u64,
        });
    } else if size >= 14 && preamble.starts_with(b"\x28\0\0\0") && &preamble[12..14] == b"\x01\0" && preamble[15] == 0 {
        // DIB
        let w = i32::from_le_bytes(array4!(preamble, 4));
        let h = i32::from_le_bytes(array4!(preamble, 8));

        return Ok(ImInfo {
            format: ImFormat::DIB,
            width:  w as u64,
            height: h.abs() as u64,
        });
    } else if size >= 20 && preamble.starts_with(b"VTF\0") {
        // VTF
        let header_size = u32::from_le_bytes(array4!(preamble, 12));
        let w = u16::from_le_bytes(array2!(preamble, 16));
        let h = u16::from_le_bytes(array2!(preamble, 18));
        if header_size < 10 {
            return Err(ImError::ParserError(ImFormat::VTF));
        }

        return Ok(ImInfo {
            format: ImFormat::VTF,
            width:  w as u64,
            height: h as u64,
        });
    } else if size >= 24 && preamble.starts_with(b"FORM") && matches!(&preamble[8..12], b"ILBM"|b"PBM ") && &preamble[12..16] == b"BMHD" {
        let chunk_len = u32::from_be_bytes(array4!(preamble, 4));
        if chunk_len < 32 {
            // need at least room for full header chunk
            return Err(ImError::ParserError(ImFormat::ILBM));
        }

        let bmhd_chunk_len = u32::from_be_bytes(array4!(preamble, 16));
        if bmhd_chunk_len < 20 {
            // need at least room for full header data
            return Err(ImError::ParserError(ImFormat::ILBM));
        }

        let w = u16::from_be_bytes(array2!(preamble, 20));
        let h = u16::from_be_bytes(array2!(preamble, 22));

        return Ok(ImInfo {
            format: ImFormat::ILBM,
            width:  w as u64,
            height: h as u64,
        })
    } else if size >= 30 && preamble[1] < 2 && preamble[2] < 12 && is_tga(file)? {
        // TGA
        let w = u16::from_le_bytes(array2!(preamble, 12));
        let h = u16::from_le_bytes(array2!(preamble, 14));

        return Ok(ImInfo {
            format: ImFormat::TGA,
            width:  w as u64,
            height: h as u64,
        });
    }
    return Err(ImError::UnknownFormat);
}
