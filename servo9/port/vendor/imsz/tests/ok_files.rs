fn get_testdata(fname: &str) -> std::path::PathBuf {
    let mut path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    path.push("testdata");
    path.push(fname);
    return path;
}


#[test]
fn avif() {
    let info = imsz::imsz_from_path(get_testdata("image.avif"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::AVIF);
            assert_eq!(info.format.name(), "AVIF");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn bmp() {
    let info = imsz::imsz_from_path(get_testdata("image.bmp"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::BMP);
            assert_eq!(info.format.name(), "BMP");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn dds() {
    let info = imsz::imsz_from_path(get_testdata("image.dds"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::DDS);
            assert_eq!(info.format.name(), "DDS");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn dib() {
    let info = imsz::imsz_from_path(get_testdata("image.dib"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::DIB);
            assert_eq!(info.format.name(), "DIB");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn exr() {
    let info = imsz::imsz_from_path(get_testdata("image.exr"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::OpenEXR);
            assert_eq!(info.format.name(), "OpenEXR");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn gif() {
    let info = imsz::imsz_from_path(get_testdata("image.gif"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::GIF);
            assert_eq!(info.format.name(), "GIF");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn heif() {
    let info = imsz::imsz_from_path(get_testdata("image.heif"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::HEIF);
            assert_eq!(info.format.name(), "HEIF");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn ico() {
    let info = imsz::imsz_from_path(get_testdata("image.ico"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::ICO);
            assert_eq!(info.format.name(), "ICO");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn ilbm() {
    let info = imsz::imsz_from_path(get_testdata("image.ilbm"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::ILBM);
            assert_eq!(info.format.name(), "ILBM");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn jp2() {
    let info = imsz::imsz_from_path(get_testdata("image.jp2"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::JP2K);
            assert_eq!(info.format.name(), "JPEG 2000");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn jpeg() {
    let info = imsz::imsz_from_path(get_testdata("image.jpeg"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::JPEG);
            assert_eq!(info.format.name(), "JPEG");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn pcx() {
    let info = imsz::imsz_from_path(get_testdata("image.pcx"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::PCX);
            assert_eq!(info.format.name(), "PCX");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn png() {
    let info = imsz::imsz_from_path(get_testdata("image.png"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::PNG);
            assert_eq!(info.format.name(), "PNG");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn psd() {
    let info = imsz::imsz_from_path(get_testdata("image.psd"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::PSD);
            assert_eq!(info.format.name(), "PSD");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn qoi() {
    let info = imsz::imsz_from_path(get_testdata("image.qoi"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::QOI);
            assert_eq!(info.format.name(), "QOI");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn tga() {
    let info = imsz::imsz_from_path(get_testdata("image.tga"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::TGA);
            assert_eq!(info.format.name(), "TGA");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn tiff_be() {
    let info = imsz::imsz_from_path(get_testdata("image_be.tiff"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::TIFF);
            assert_eq!(info.format.name(), "TIFF");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn tiff_le() {
    let info = imsz::imsz_from_path(get_testdata("image_le.tiff"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::TIFF);
            assert_eq!(info.format.name(), "TIFF");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn webp_lossless() {
    let info = imsz::imsz_from_path(get_testdata("image_lossless.webp"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::WEBP);
            assert_eq!(info.format.name(), "WebP");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn webp_lossless_vp8x() {
    let info = imsz::imsz_from_path(get_testdata("image_lossless_vp8x.webp"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::WEBP);
            assert_eq!(info.format.name(), "WebP");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn webp_lossy() {
    let info = imsz::imsz_from_path(get_testdata("image_lossy.webp"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::WEBP);
            assert_eq!(info.format.name(), "WebP");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn webp_lossy_vp8x() {
    let info = imsz::imsz_from_path(get_testdata("image_lossy_vp8x.webp"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::WEBP);
            assert_eq!(info.format.name(), "WebP");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}


#[test]
fn xcf() {
    let info = imsz::imsz_from_path(get_testdata("image.xcf"));
    match info {
        Ok(info) => {
            assert_eq!(info.format, imsz::ImFormat::XCF);
            assert_eq!(info.format.name(), "XCF");
            assert_eq!(info.width,  32);
            assert_eq!(info.height, 16);
        }
        Err(error) => {
            assert!(false, "{}", error);
        }
    }
}
