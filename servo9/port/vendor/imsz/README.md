# imsz

[![Test Status](https://img.shields.io/github/actions/workflow/status/panzi/imsz/test.yml?branch=main)](https://github.com/panzi/imsz/actions/workflows/test.yml)
[![License](https://img.shields.io/github/license/panzi/imsz)](https://github.com/panzi/imsz/blob/main/LICENSE)
[Reference](https://panzi.github.io/imsz/imsz) – [C API](https://panzi.github.io/imsz/c)

Get width and height from an image file reading as few bytes as possible.

This is a fork of [scardine/imsz](https://github.com/scardine/imsz) that adds
support for more file formats, but also breaks API compatibility in order to be
more idiomatic Rust. It also provides a C library wrapper. For more information
on how this started see the mentioned link.

The library itself has zero dependencies, but the example binary uses
[clap](https://crates.io/crates/clap).

## Usage

There is a simple example binary:

```bash
$ cargo run -q --example imsz testdata/image.gif
testdata/image.gif: GIF, 32 x 16

$ cargo run -q --example imsz -- --help
imsz 0.4.1
Paulo Scardine <paulo@scardine.com.br>, Mathias Panzenböck <grosser.meister.morti@gmx.net>

USAGE:
    imsz [FILES]...

ARGS:
    <FILES>...    

OPTIONS:
    -h, --help       Print help information
    -V, --version    Print version information
```

The relevant parts:

```Rust
use imsz::imsz;

let info = imsz(filename)?;
println!("{}: {}, {} x {}", filename, info.format, info.width, info.height);
// testdata/image.gif: GIF, 32 x 16

// or for already opened files:
let info = imsz(File::open(filename)?);

// or for in memory buffers:
let info = imsz(b"\x89PNG\r\n\x1a\n...");

// or for *anything* implementing Read and Seek:
use imsz::imsz_from_reader;

let mut file = BufReader::new(File::open(filename)?);
let info = imsz_from_reader(&mut file)?;
```

## Supported File Formats

* AVIF
* BMP
* DDS
* DIB
* GIF
* HEIC/HEIF
* ICO
* ILBM
* JPEG
* JPEG 2000
* PCX
* PNG
* PSD
* OpenEXR
* QOI
* TGA
* TIFF
* VTF
* WEBP
* XCF

No guarantees of correct or complete implementation are made.

## Related Work

* [scardine/imsz](https://github.com/scardine/imsz) – original Rust library from which this is a fork
* [panzi/get_image_size.py](https://github.com/panzi/get_image_size.py) – a very similar library in pure Python
* [StackOverflow answer](https://stackoverflow.com/a/19035508/277767) – the start of it all
