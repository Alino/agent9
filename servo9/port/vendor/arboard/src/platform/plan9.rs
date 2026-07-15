/*
Plan 9 (9front) clipboard, over /dev/snarf.

Plan 9's clipboard is a file. Reading /dev/snarf gets its contents; writing it
replaces them; truncating it clears. There is no selection ownership, no
X11-style negotiation, no daemon to talk to — so this backend is short not
because it is a stub but because the OS made it easy. alacritty9 already uses
/dev/snarf the same way.

Limits, honestly:
  * Text only. /dev/snarf is a byte file with no type negotiation, so images,
    HTML and file lists have nowhere to live and report Unsupported rather than
    silently dropping to plain text (which would look like data loss).
  * The snarf buffer is UTF-8 by convention; invalid bytes are reported as
    ConversionFailure rather than lossily mangled.
*/

use std::borrow::Cow;
use std::fs;
use std::io::Write;
use std::path::Path;

#[cfg(feature = "image-data")]
use crate::ImageData;
use crate::common::Error;

const SNARF: &str = "/dev/snarf";

fn io_err(e: std::io::Error) -> Error {
    Error::Unknown { description: format!("/dev/snarf: {e}") }
}

pub(crate) struct Clipboard {
    _private: (),
}

impl Clipboard {
    pub(crate) fn new() -> Result<Self, Error> {
        // Fail now rather than at the first copy: without /dev/snarf bound there
        // is no clipboard at all, and the caller should learn that up front.
        if !Path::new(SNARF).exists() {
            return Err(Error::ClipboardNotSupported);
        }
        Ok(Clipboard { _private: () })
    }
}

pub(crate) struct Get<'clipboard> {
    _clipboard: &'clipboard mut Clipboard,
}

impl<'clipboard> Get<'clipboard> {
    pub(crate) fn new(clipboard: &'clipboard mut Clipboard) -> Self {
        Get { _clipboard: clipboard }
    }

    pub(crate) fn text(self) -> Result<String, Error> {
        let bytes = fs::read(SNARF).map_err(io_err)?;
        String::from_utf8(bytes).map_err(|_| Error::ConversionFailure)
    }

    #[cfg(feature = "image-data")]
    pub(crate) fn image(self) -> Result<ImageData<'static>, Error> {
        // /dev/snarf carries bytes with no type tag: there is no way to say
        // "this is an image", and no way to know one is there.
        Err(Error::ContentNotAvailable)
    }

    pub(crate) fn html(self) -> Result<String, Error> {
        Err(Error::ContentNotAvailable)
    }

    pub(crate) fn file_list(self) -> Result<Vec<std::path::PathBuf>, Error> {
        Err(Error::ContentNotAvailable)
    }
}

pub(crate) struct Set<'clipboard> {
    _clipboard: &'clipboard mut Clipboard,
}

impl<'clipboard> Set<'clipboard> {
    pub(crate) fn new(clipboard: &'clipboard mut Clipboard) -> Self {
        Set { _clipboard: clipboard }
    }

    pub(crate) fn text<'a, T: Into<Cow<'a, str>>>(self, text: T) -> Result<(), Error> {
        let text = text.into();
        // Truncate-and-write: opening /dev/snarf for write replaces the buffer.
        let mut f = fs::File::create(SNARF).map_err(io_err)?;
        f.write_all(text.as_bytes()).map_err(io_err)?;
        Ok(())
    }

    pub(crate) fn html<'a, T: Into<Cow<'a, str>>>(
        self,
        _html: T,
        alt_text: Option<T>,
    ) -> Result<(), Error> {
        // No rich-text channel. Put the plain-text alternative on the clipboard
        // if the caller supplied one — that is what it is for — rather than
        // writing HTML markup where a user expects text.
        match alt_text {
            Some(alt) => self.text(alt),
            None => Err(Error::ConversionFailure),
        }
    }

    #[cfg(feature = "image-data")]
    pub(crate) fn image(self, _image: ImageData) -> Result<(), Error> {
        Err(Error::ConversionFailure)
    }

    pub(crate) fn file_list(self, _file_list: &[impl AsRef<Path>]) -> Result<(), Error> {
        Err(Error::ConversionFailure)
    }
}

pub(crate) struct Clear<'clipboard> {
    clipboard: &'clipboard mut Clipboard,
}

impl<'clipboard> Clear<'clipboard> {
    pub(crate) fn new(clipboard: &'clipboard mut Clipboard) -> Self {
        Clear { clipboard }
    }

    pub(crate) fn clear(self) -> Result<(), Error> {
        Set::new(self.clipboard).text("")
    }
}

// NB: no ClearExtLinux/GetExtLinux/SetExtLinux/LinuxClipboardKind here. Those are
// the X11 selection model (primary vs clipboard, selection ownership); Plan 9 has
// exactly one snarf buffer and no such concept. lib.rs only re-exports them under
// the linux cfg, so there is nothing to satisfy.
