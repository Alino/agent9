use imsz::{imsz, ImError, ImFormat, ImInfo, ImResult};

fn expect_broken(data: &[u8], expect_format: ImFormat) {
    let actual = imsz(data);

    if let Err(ImError::ParserError(format)) = actual {
        if format == expect_format {
            return;
        }
    }

    let expected: ImResult<ImInfo> = Err(ImError::ParserError(expect_format));
    assert!(false, " expected: {expected:?}\n   actual: {actual:?}");
}

#[test]
fn broken_avif() {
    let _ = imsz(b"\0\0\0\0ftypavif");
    let broken_images = [
        b"\0\0\0\0ftypavif" as &[u8],
        b"\x00\x00\x00\x0cftypavif",
        b"\x00\x00\x00\x0cftypavif\x00\x00\x00\x14meta....\x00\x00\x00\x08iprp",
        b"\x00\x00\x00\x0cftypavif\x00\x00\x00\x1cmeta....\x00\x00\x00\x10iprp\x00\x00\x00\x08ipco",
        b"\x00\x00\x00\x0cftypavif\x00\x00\x00$meta....\x00\x00\x00\x18iprp\x00\x00\x00\x10ipco\x00\x00\x00\x08ispe",
        b"\x00\x00\x00\x0cftypavif\x00\x00\x00/meta....\x00\x00\x00#iprp\x00\x00\x00\x1bipco\x00\x00\x00\x13ispe..........."
    ];
    for data in broken_images {
        expect_broken(data, ImFormat::AVIF);
    }
}

#[test]
fn broken_heif() {
    let _ = imsz(b"\0\0\0\0ftypheic");
    let broken_images = [
        b"\0\0\0\0ftypheic" as &[u8],
        b"\x00\x00\x00\x0cftypheic",
        b"\x00\x00\x00\x0cftypheic\x00\x00\x00\x14meta....\x00\x00\x00\x08iprp",
        b"\x00\x00\x00\x0cftypheic\x00\x00\x00\x1cmeta....\x00\x00\x00\x10iprp\x00\x00\x00\x08ipco",
        b"\x00\x00\x00\x0cftypheic\x00\x00\x00$meta....\x00\x00\x00\x18iprp\x00\x00\x00\x10ipco\x00\x00\x00\x08ispe",
        b"\x00\x00\x00\x0cftypheic\x00\x00\x00/meta....\x00\x00\x00#iprp\x00\x00\x00\x1bipco\x00\x00\x00\x13ispe..........."
    ];
    for data in broken_images {
        expect_broken(data, ImFormat::HEIF);
    }
}

#[test]
fn broken_gif() {
    let broken_images = [
        b"GIF87a" as &[u8],
        b"GIF89a...",
    ];
    for data in broken_images {
        expect_broken(data, ImFormat::GIF);
    }
}

#[test]
fn broken_png() {
    let broken_images = [
        b"\x89PNG\r\n\x1a\n" as &[u8],
        b"\x89PNG\r\n\x1a\n\x00\x00\x00\x07IHDR.......",
    ];
    for data in broken_images {
        expect_broken(data, ImFormat::PNG);
    }
}

#[test]
fn broken_bmp() {
    let broken_images = [
        b"BM\x00\x00\x00\x0e\0\0\0\0\0\0\0\0" as &[u8],
        b"BM\x00\x00\x00\x19\0\0\0\0\x00\x00\x00\x0b...........",
    ];
    for data in broken_images {
        expect_broken(data, ImFormat::BMP);
    }
}

#[test]
fn broken_ilbm() {
    let broken_images = [
        b"FORM\x00\x00\x00\x80ILBMBMHD\x00\x00\x00\x12\x02\x80\x01\xe0" as &[u8],
        b"FORM\x00\x00\x00\x1ePBM BMHD\x00\x00\x00\x14\x02\x80\x01\xe0",
    ];
    for data in broken_images {
        expect_broken(data, ImFormat::ILBM);
    }
}
