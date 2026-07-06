//! crossfont 0.8.1 API shim for Plan 9.
//!
//! Real crossfont rasterizes via freetype+fontconfig on non-mac/win platforms;
//! neither exists on Plan 9. This shim keeps the exact public API alacritty
//! consumes but rasterizes with fontdue (pure Rust) using four bundled
//! Go Mono faces (BSD-3, see fonts/LICENSE).
//!
//! Public types (FontDesc, Style, Size, Metrics, RasterizedGlyph, Error, the
//! Rasterize trait, ...) are copied verbatim from crossfont 0.8.1 (Apache-2.0)
//! so alacritty's call sites compile unchanged.
//!
//! # Coordinate mapping (fontdue -> crossfont)
//!
//! crossfont follows freetype bitmap conventions, y-up baseline-relative
//! bearings, bitmap rows stored top row first:
//!   - `left` = horizontal distance from pen origin to bitmap's left edge
//!     (freetype `bitmap_left`).
//!   - `top`  = vertical distance from baseline UP to the bitmap's top edge
//!     (freetype `bitmap_top`); positive for anything above the baseline.
//!
//! fontdue's `Metrics` gives the bitmap's bounding box with `xmin`/`ymin`
//! being the offset of the bitmap's BOTTOM-LEFT corner from the origin on the
//! baseline (y positive up), plus `width`/`height`, and its coverage buffer is
//! also stored top row first. Therefore:
//!   - `left` = `xmin`
//!   - `top`  = `ymin + height`   (bottom edge offset + bitmap height = top edge)
//! e.g. 'g' with ymin = -3, height = 10 -> top = 7: bitmap top sits 7px above
//! the baseline, tail 3px below. alacritty then positions the glyph inside the
//! cell relative to the baseline using exactly these two numbers.

use std::collections::HashMap;
use std::fmt::{self, Display, Formatter};
use std::sync::atomic::{AtomicUsize, Ordering};

/// Max font size in pt (mirrors real crossfont; u32 storage with 6 fract digits).
const MAX_FONT_PT_SIZE: f32 = 3999.;

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct FontDesc {
    #[allow(dead_code)] // family names are meaningless on Plan 9; kept for Display/PartialEq
    name: String,
    style: Style,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Slant {
    Normal,
    Italic,
    Oblique,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Weight {
    Normal,
    Bold,
}

/// Style of font.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Style {
    Specific(String),
    Description { slant: Slant, weight: Weight },
}

impl fmt::Display for Style {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match *self {
            Style::Specific(ref s) => f.write_str(s),
            Style::Description { slant, weight } => {
                write!(f, "slant={:?}, weight={:?}", slant, weight)
            },
        }
    }
}

impl FontDesc {
    pub fn new<S>(name: S, style: Style) -> FontDesc
    where
        S: Into<String>,
    {
        FontDesc { name: name.into(), style }
    }
}

impl fmt::Display for FontDesc {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{} - {}", self.name, self.style)
    }
}

/// Identifier for a Font for use in maps/etc.
#[derive(Debug, Clone, Copy, Hash, PartialEq, Eq)]
pub struct FontKey {
    token: u32,
}

impl FontKey {
    /// Get next font key for given size.
    ///
    /// The generated key will be globally unique.
    pub fn next() -> FontKey {
        static TOKEN: AtomicUsize = AtomicUsize::new(0);

        FontKey { token: TOKEN.fetch_add(1, Ordering::SeqCst) as _ }
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub struct GlyphKey {
    pub character: char,
    pub font_key: FontKey,
    pub size: Size,
}

/// Font size stored as base and fraction.
#[derive(Debug, Copy, Clone, Hash, PartialEq, Eq, PartialOrd, Ord)]
pub struct Size(u32);

impl Size {
    /// Create a new `Size` from a f32 size in points.
    ///
    /// The font size is automatically clamped to supported range of `[1.; 3999.]` pt.
    pub fn new(size: f32) -> Size {
        let size = size.clamp(1., MAX_FONT_PT_SIZE);
        Size((size * Self::factor()) as u32)
    }

    /// Create a new `Size` from px.
    ///
    /// The value will be clamped to the pt range of [`Size::new`].
    pub fn from_px(size: f32) -> Self {
        let pt = size * 72. / 96.;
        Size::new(pt)
    }

    /// Scale font size by the given amount.
    pub fn scale(self, scale: f32) -> Self {
        Self::new(self.as_pt() * scale)
    }

    /// Get size in `px`.
    pub fn as_px(self) -> f32 {
        self.as_pt() * 96. / 72.
    }

    /// Get the size in `pt`.
    pub fn as_pt(self) -> f32 {
        (f64::from(self.0) / Size::factor() as f64) as f32
    }

    /// Scale factor between font "Size" type and point size.
    #[inline]
    fn factor() -> f32 {
        1_000_000.
    }
}

#[derive(Debug, Clone)]
pub struct RasterizedGlyph {
    pub character: char,
    pub width: i32,
    pub height: i32,
    pub top: i32,
    pub left: i32,
    pub advance: (i32, i32),
    pub buffer: BitmapBuffer,
}

#[derive(Clone, Debug)]
pub enum BitmapBuffer {
    /// RGB alphamask.
    Rgb(Vec<u8>),

    /// RGBA pixels with premultiplied alpha.
    Rgba(Vec<u8>),
}

impl Default for RasterizedGlyph {
    fn default() -> RasterizedGlyph {
        RasterizedGlyph {
            character: ' ',
            width: 0,
            height: 0,
            top: 0,
            left: 0,
            advance: (0, 0),
            buffer: BitmapBuffer::Rgb(Vec::new()),
        }
    }
}

#[derive(Debug, Copy, Clone)]
pub struct Metrics {
    pub average_advance: f64,
    pub line_height: f64,
    pub descent: f32,
    pub underline_position: f32,
    pub underline_thickness: f32,
    pub strikeout_position: f32,
    pub strikeout_thickness: f32,
}

/// Errors occuring when using the rasterizer.
#[derive(Debug)]
pub enum Error {
    /// Unable to find a font matching the description.
    FontNotFound(FontDesc),

    /// Unable to find metrics for a font face.
    MetricsNotFound,

    /// The glyph could not be found in any font.
    MissingGlyph(RasterizedGlyph),

    /// Requested an operation with a FontKey that isn't known to the rasterizer.
    UnknownFontKey,

    /// Error from platfrom's font system.
    PlatformError(String),
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        None
    }
}

impl Display for Error {
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        match self {
            Error::FontNotFound(font) => write!(f, "font {:?} not found", font),
            Error::MissingGlyph(glyph) => {
                write!(f, "glyph for character {:?} not found", glyph.character)
            },
            Error::UnknownFontKey => f.write_str("invalid font key"),
            Error::MetricsNotFound => f.write_str("metrics not found"),
            Error::PlatformError(err) => write!(f, "{}", err),
        }
    }
}

pub trait Rasterize {
    /// Create a new Rasterizer.
    fn new() -> Result<Self, Error>
    where
        Self: Sized;

    /// Get `Metrics` for the given `FontKey`.
    fn metrics(&self, _: FontKey, _: Size) -> Result<Metrics, Error>;

    /// Load the font described by `FontDesc` and `Size`.
    fn load_font(&mut self, _: &FontDesc, _: Size) -> Result<FontKey, Error>;

    /// Rasterize the glyph described by `GlyphKey`..
    fn get_glyph(&mut self, _: GlyphKey) -> Result<RasterizedGlyph, Error>;

    /// Kerning between two characters.
    fn kerning(&mut self, left: GlyphKey, right: GlyphKey) -> (f32, f32);
}

// ---------------------------------------------------------------------------
// Plan 9 rasterizer: fontdue + bundled Go Mono.
// ---------------------------------------------------------------------------

/// The four bundled faces, indexed by `bold as usize | (italic as usize) << 1`.
static FACE_DATA: [&[u8]; 4] = [
    include_bytes!("../fonts/Go-Mono.ttf"),
    include_bytes!("../fonts/Go-Mono-Bold.ttf"),
    include_bytes!("../fonts/Go-Mono-Italic.ttf"),
    include_bytes!("../fonts/Go-Mono-Bold-Italic.ttf"),
];

pub struct FontdueRasterizer {
    /// Parsed faces, lazily populated, indexed like `FACE_DATA`.
    faces: [Option<fontdue::Font>; 4],
    /// FontKey issued for each face index (one key per face, reused).
    keys: [Option<FontKey>; 4],
    /// Reverse map: issued FontKey -> face index.
    by_key: HashMap<FontKey, usize>,
}

pub use FontdueRasterizer as Rasterizer;

/// Pick one of the 4 bundled faces from a style.
///
/// There is no font database on Plan 9: the requested family name is ignored
/// and every family resolves to Go Mono; only (slant, weight) selects a face.
fn face_index(style: &Style) -> usize {
    let (bold, italic) = match style {
        Style::Description { slant, weight } => {
            (*weight == Weight::Bold, *slant != Slant::Normal)
        },
        Style::Specific(name) => {
            let name = name.to_lowercase();
            (name.contains("bold"), name.contains("italic") || name.contains("oblique"))
        },
    };
    bold as usize | (italic as usize) << 1
}

impl FontdueRasterizer {
    fn font(&self, key: FontKey) -> Result<&fontdue::Font, Error> {
        let index = *self.by_key.get(&key).ok_or(Error::UnknownFontKey)?;
        self.faces[index].as_ref().ok_or(Error::UnknownFontKey)
    }

    /// Convert one fontdue rasterization into a crossfont `RasterizedGlyph`.
    ///
    /// See the module docs for the bearing mapping: left = xmin,
    /// top = ymin + height (baseline-relative, y-up), buffer rows are
    /// top-first in both worlds. The 8-bit coverage is replicated x3 into
    /// `BitmapBuffer::Rgb` — crossfont's grayscale-as-RGB convention for
    /// non-LCD rendering (what freetype's render_mode=Normal path produces).
    fn to_glyph(character: char, metrics: fontdue::Metrics, coverage: Vec<u8>) -> RasterizedGlyph {
        let mut buffer = Vec::with_capacity(coverage.len() * 3);
        for c in coverage {
            buffer.extend_from_slice(&[c, c, c]);
        }
        RasterizedGlyph {
            character,
            width: metrics.width as i32,
            height: metrics.height as i32,
            top: metrics.ymin + metrics.height as i32,
            left: metrics.xmin,
            advance: (metrics.advance_width.round() as i32, 0),
            buffer: BitmapBuffer::Rgb(buffer),
        }
    }
}

impl Rasterize for FontdueRasterizer {
    fn new() -> Result<FontdueRasterizer, Error> {
        Ok(FontdueRasterizer {
            faces: [None, None, None, None],
            keys: [None; 4],
            by_key: HashMap::new(),
        })
    }

    fn load_font(&mut self, desc: &FontDesc, _size: Size) -> Result<FontKey, Error> {
        let index = face_index(&desc.style);
        if let Some(key) = self.keys[index] {
            return Ok(key);
        }
        let font = fontdue::Font::from_bytes(FACE_DATA[index], fontdue::FontSettings::default())
            .map_err(|e| Error::PlatformError(e.to_string()))?;
        let key = FontKey::next();
        self.faces[index] = Some(font);
        self.keys[index] = Some(key);
        self.by_key.insert(key, index);
        Ok(key)
    }

    fn metrics(&self, key: FontKey, size: Size) -> Result<Metrics, Error> {
        let font = self.font(key)?;
        let px = size.as_px();
        let line = font.horizontal_line_metrics(px).ok_or(Error::MetricsNotFound)?;

        // descent is negative below the baseline (freetype convention;
        // fontdue uses the same sign).
        let descent = line.descent;
        let line_height = f64::max(line.new_line_size as f64, (line.ascent - descent) as f64);
        let average_advance = font.metrics('m', px).advance_width as f64;

        // Go Mono's underline/strikeout tables aren't exposed through fontdue.
        // ponytail: heuristics mirroring crossfont's own bitmap-font fallback;
        // upgrade path is swash (reads the post/OS2 tables directly).
        let underline_position = descent / 2.;
        let underline_thickness = f32::max(px / 14., 1.);
        let strikeout_position = line.ascent * 0.3;
        let strikeout_thickness = underline_thickness;

        Ok(Metrics {
            average_advance,
            line_height,
            descent,
            underline_position,
            underline_thickness,
            strikeout_position,
            strikeout_thickness,
        })
    }

    fn get_glyph(&mut self, glyph_key: GlyphKey) -> Result<RasterizedGlyph, Error> {
        let font = self.font(glyph_key.font_key)?;
        let px = glyph_key.size.as_px();
        let index = font.lookup_glyph_index(glyph_key.character);
        let (metrics, coverage) = font.rasterize_indexed(index, px);
        let glyph = Self::to_glyph(glyph_key.character, metrics, coverage);

        // Index 0 is .notdef: the character isn't in the font. Return the
        // rasterized .notdef box inside MissingGlyph so alacritty's
        // glyph_cache fallback (show_missing) can still draw something.
        if index == 0 {
            return Err(Error::MissingGlyph(glyph));
        }
        Ok(glyph)
    }

    fn kerning(&mut self, left: GlyphKey, right: GlyphKey) -> (f32, f32) {
        let px = left.size.as_px();
        let kern = self
            .font(left.font_key)
            .ok()
            .and_then(|font| font.horizontal_kern(left.character, right.character, px))
            .unwrap_or(0.);
        (kern, 0.)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rasterize_m_all_styles() {
        let mut rasterizer = Rasterizer::new().unwrap();
        let size = Size::new(12.);
        let px = size.as_px();

        let styles = [
            (Slant::Normal, Weight::Normal),
            (Slant::Normal, Weight::Bold),
            (Slant::Italic, Weight::Normal),
            (Slant::Italic, Weight::Bold),
        ];

        let mut keys = Vec::new();
        for (slant, weight) in styles {
            let desc = FontDesc::new("anything", Style::Description { slant, weight });
            let key = rasterizer.load_font(&desc, size).unwrap();
            keys.push(key);

            let metrics = rasterizer.metrics(key, size).unwrap();
            assert!(metrics.line_height > 0., "line_height {}", metrics.line_height);
            assert!(metrics.descent < 0., "descent must be negative, got {}", metrics.descent);
            assert!(
                metrics.line_height > f64::from(-metrics.descent),
                "line_height {} vs descent {}",
                metrics.line_height,
                metrics.descent
            );
            assert!(metrics.average_advance > 0.);
            assert!(metrics.underline_position < 0., "underline below baseline");
            assert!(metrics.underline_thickness >= 1.);
            assert!(metrics.strikeout_position > 0., "strikeout above baseline");

            let glyph = rasterizer
                .get_glyph(GlyphKey { character: 'm', font_key: key, size })
                .unwrap();
            assert!(glyph.width > 0 && glyph.height > 0, "empty bitmap for 'm'");
            // 'm' sits on the baseline: top bearing positive, roughly x-height,
            // and never above the ascender.
            assert!(glyph.top > 0, "top bearing {} must be positive", glyph.top);
            let line = fontdue::Font::from_bytes(
                FACE_DATA[0],
                fontdue::FontSettings::default(),
            )
            .unwrap()
            .horizontal_line_metrics(px)
            .unwrap();
            assert!(
                (glyph.top as f32) <= line.ascent.ceil(),
                "top {} exceeds ascent {}",
                glyph.top,
                line.ascent
            );
            // 'm' has no descender: bitmap bottom is at/near the baseline.
            assert!(glyph.top >= glyph.height - 1, "'m' should not dip below baseline");
            assert!(glyph.advance.0 > 0);
            match &glyph.buffer {
                BitmapBuffer::Rgb(buf) => {
                    assert_eq!(buf.len(), (glyph.width * glyph.height * 3) as usize);
                    assert!(buf.iter().any(|&b| b > 0), "all-blank coverage");
                },
                BitmapBuffer::Rgba(_) => panic!("expected Rgb"),
            }
        }

        // 4 distinct faces get 4 distinct keys; reloading reuses the key.
        assert_eq!(keys.iter().collect::<std::collections::HashSet<_>>().len(), 4);
        let again = rasterizer
            .load_font(
                &FontDesc::new(
                    "other-family",
                    Style::Description { slant: Slant::Normal, weight: Weight::Normal },
                ),
                size,
            )
            .unwrap();
        assert_eq!(again, keys[0]);
    }

    #[test]
    fn specific_style_matching() {
        assert_eq!(face_index(&Style::Specific("Bold Italic".into())), 3);
        assert_eq!(face_index(&Style::Specific("bold".into())), 1);
        assert_eq!(face_index(&Style::Specific("Oblique".into())), 2);
        assert_eq!(face_index(&Style::Specific("Regular".into())), 0);
    }

    #[test]
    fn missing_glyph_carries_notdef() {
        let mut rasterizer = Rasterizer::new().unwrap();
        let size = Size::new(12.);
        let desc = FontDesc::new(
            "x",
            Style::Description { slant: Slant::Normal, weight: Weight::Normal },
        );
        let key = rasterizer.load_font(&desc, size).unwrap();
        // Go Mono has no CJK coverage.
        match rasterizer.get_glyph(GlyphKey { character: '\u{4e00}', font_key: key, size }) {
            Err(Error::MissingGlyph(glyph)) => {
                assert_eq!(glyph.character, '\u{4e00}');
                assert!(glyph.width > 0, ".notdef should render a visible box");
            },
            other => panic!("expected MissingGlyph, got {:?}", other.map(|g| g.character)),
        }
    }

    #[test]
    fn descender_bearing() {
        // 'g' dips below the baseline: top < height.
        let mut rasterizer = Rasterizer::new().unwrap();
        let size = Size::new(12.);
        let desc = FontDesc::new(
            "x",
            Style::Description { slant: Slant::Normal, weight: Weight::Normal },
        );
        let key = rasterizer.load_font(&desc, size).unwrap();
        let g = rasterizer.get_glyph(GlyphKey { character: 'g', font_key: key, size }).unwrap();
        assert!(g.top < g.height, "'g' tail must extend below baseline: top {} height {}", g.top, g.height);
        assert!(g.top > 0);
    }
}
