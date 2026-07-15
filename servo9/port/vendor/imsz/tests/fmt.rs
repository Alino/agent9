use imsz::ImFormat;

#[test]
fn fmt_debug() {
    let data = [
        (ImFormat::GIF,     "GIF"),
        (ImFormat::PNG,     "PNG"),
        (ImFormat::BMP,     "BMP"),
        (ImFormat::JPEG,    "JPEG"),
        (ImFormat::WEBP,    "WEBP"),
        (ImFormat::QOI,     "QOI"),
        (ImFormat::PSD,     "PSD"),
        (ImFormat::XCF,     "XCF"),
        (ImFormat::ICO,     "ICO"),
        (ImFormat::AVIF,    "AVIF"),
        (ImFormat::TIFF,    "TIFF"),
        (ImFormat::OpenEXR, "OpenEXR"),
        (ImFormat::PCX,     "PCX"),
        (ImFormat::TGA,     "TGA"),
        (ImFormat::DDS,     "DDS"),
        (ImFormat::HEIF,    "HEIF"),
        (ImFormat::JP2K,    "JP2K"),
        (ImFormat::DIB,     "DIB"),
        (ImFormat::VTF,     "VTF"),
        (ImFormat::ILBM,    "ILBM"),
    ];
    for (format, expected) in data {
        let actual = format!("{format:?}");
        assert_eq!(&actual, expected);
    }
}

#[test]
fn fmt_display() {
    let data = [
        (ImFormat::GIF,     "GIF"),
        (ImFormat::PNG,     "PNG"),
        (ImFormat::BMP,     "BMP"),
        (ImFormat::JPEG,    "JPEG"),
        (ImFormat::WEBP,    "WebP"),
        (ImFormat::QOI,     "QOI"),
        (ImFormat::PSD,     "PSD"),
        (ImFormat::XCF,     "XCF"),
        (ImFormat::ICO,     "ICO"),
        (ImFormat::AVIF,    "AVIF"),
        (ImFormat::TIFF,    "TIFF"),
        (ImFormat::OpenEXR, "OpenEXR"),
        (ImFormat::PCX,     "PCX"),
        (ImFormat::TGA,     "TGA"),
        (ImFormat::DDS,     "DDS"),
        (ImFormat::HEIF,    "HEIF"),
        (ImFormat::JP2K,    "JPEG 2000"),
        (ImFormat::DIB,     "DIB"),
        (ImFormat::VTF,     "VTF"),
        (ImFormat::ILBM,    "ILBM"),
    ];
    for (format, expected) in data {
        let actual = format!("{format}");
        assert_eq!(&actual, expected);
    }
}
