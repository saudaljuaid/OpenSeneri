// SPDX-License-Identifier: GPL-3.0-only
//! Moving a picture: scaling, positioning, and the arithmetic in between.
//!
//! Scaling a clip is the most-used operation in an editor after cutting, and
//! it is the one where "looks about right" hides the most. Two decisions
//! decide whether it is right, and both are the same shape as the ones
//! [`crate::composite`] already made.
//!
//! ## In what space?
//!
//! **Linear light.** A resampled pixel is a *weighted average of what was
//! there*, and an average is only meaningful over quantities that add — which
//! light does and code values do not. Averaging gamma-encoded values makes a
//! reduced picture darker than the one it came from, most visibly on fine
//! bright detail against dark: the highlights lose more than they should
//! because they were compressed before being averaged. So every sample is
//! decoded, weighted, summed and encoded back, exactly as `over` does.
//!
//! ## Straight or premultiplied?
//!
//! **Premultiplied, and nothing else.** Averaging straight samples across an
//! edge mixes the colour of pixels that are barely there with the colour of
//! pixels that are fully there, at equal weight — so a title over black picks
//! up a dark fringe from the transparent pixels beyond its edge, which held
//! whatever colour happened to be in the buffer. In premultiplied form a
//! transparent pixel contributes nothing to the colour sum because its colour
//! *is* nothing, which is the whole reason the form exists.
//!
//! ## The map runs backwards
//!
//! A caller says what they want — twice the size, shifted right — and that is
//! a **forward** map from source to destination. A resampler needs the
//! opposite: for each destination pixel, which part of the source landed on
//! it. So [`Mapping::new`] takes the forward map and inverts it, exactly,
//! because a rational two-by-two inverse is a determinant and four divisions.
//! A map that squashes the picture to a line has no inverse and is refused.
//!
//! ## Two filters, and what each is for
//!
//! [`Filter::Area`] gives every destination pixel the **exact area-weighted
//! average** of the source it covers. That is the correct answer for
//! *reduction*: a destination pixel really is standing in for a region, and
//! this is that region's mean. Point sampling instead is what makes a reduced
//! picture shimmer, because which source pixel each destination pixel happens
//! to land on changes as the picture moves.
//!
//! It is also, honestly, a poor filter for *enlargement*: a destination pixel
//! that falls entirely inside one source pixel gets that pixel's value and
//! nothing else, which is nearest-neighbour with extra arithmetic. That is
//! what [`Filter::Bilinear`] is for, and choosing between them is the
//! caller's rather than a heuristic's, because a heuristic that switched on a
//! scale factor would change a picture's look at the moment somebody dragged
//! past 100%.

use alloc::vec::Vec;

use media_editor_core::{FRACTION_BITS, Fixed, Rational};
use media_editor_media::{AlphaState, Frame, FrameDescription, PixelFormat};

use crate::convert::TransferTable;
use crate::status::{RenderStatus, Result};

/// How many bytes one `Rgba8` pixel occupies.
const CHANNELS: usize = 4;

/// Which of those bytes is the alpha.
const ALPHA: usize = 3;

/// The largest picture this will resample to or from, on either axis.
pub const MAX_EXTENT: usize = 16_384;

/// How many source rows one destination row may read.
///
/// Sixty-four, and the number is an argument rather than a limit somebody
/// picked. A band is what makes [`resample_row`] worth having: the row path
/// exists so that an export never holds a whole frame, and a band of *k* rows
/// holds *k* rows. Let *k* grow without a bound and a steep enough downscale
/// makes the band the frame, at which point scanning has bought nothing and is
/// merely slower. So a vertical downscale steeper than sixty-four to one is
/// refused (R-1.3) rather than quietly costing a frame — and a caller that
/// wants one can render whole frames, which is what it would be doing anyway.
///
/// It bounds nothing horizontally, because a destination row's preimage is one
/// row-shaped strip however wide the source is.
pub const MAX_BAND_ROWS: usize = 64;

/// How to weigh the source under a destination pixel.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Filter {
    /// The exact area-weighted mean of the source region a destination pixel
    /// covers. Right for reduction; nearest-neighbour in effect for
    /// enlargement, because a destination pixel inside one source pixel
    /// covers only that one.
    Area,
    /// The four samples around where a destination pixel's centre lands,
    /// weighted by how near it is to each. Right for enlargement; loses
    /// detail on reduction, because it looks at four samples however many the
    /// destination pixel actually spans.
    Bilinear,
}

/// An affine map from source pixel coordinates to destination ones.
///
/// Stored inverted, because that is the direction a resampler walks.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Mapping {
    /// The inverse map's linear part, row-major.
    inverse: [Rational; 4],
    /// The inverse map's translation.
    offset: (Rational, Rational),
    /// How much larger one destination pixel is than one source pixel, which
    /// is the determinant of the inverse and is what an area average divides
    /// by. Kept because it is exact and computing it per pixel would be four
    /// multiplications to arrive at the same number every time.
    footprint: Rational,
}

impl Mapping {
    /// From the forward map `(x, y) -> (a·x + b·y + e, c·x + d·y + f)`.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Singular`] for a map with no inverse — one that
    /// squashes the picture onto a line, where every destination pixel would
    /// draw from a region of no area — and [`RenderStatus::Time`] wrapping an
    /// overflow.
    pub fn new(linear: [Rational; 4], offset: (Rational, Rational)) -> Result<Self> {
        let [a, b, c, d] = linear;
        let determinant = a
            .checked_mul(d)
            .map_err(RenderStatus::Time)?
            .checked_sub(b.checked_mul(c).map_err(RenderStatus::Time)?)
            .map_err(RenderStatus::Time)?;
        if determinant.is_zero() {
            return Err(RenderStatus::Singular);
        }
        let invert = |value: Rational| value.checked_div(determinant).map_err(RenderStatus::Time);
        let inverse = [
            invert(d)?,
            invert(b.checked_neg().map_err(RenderStatus::Time)?)?,
            invert(c.checked_neg().map_err(RenderStatus::Time)?)?,
            invert(a)?,
        ];
        // The inverse also moves the origin: `-M⁻¹ · offset`.
        let shifted = (
            apply(inverse, offset)
                .0
                .checked_neg()
                .map_err(RenderStatus::Time)?,
            apply(inverse, offset)
                .1
                .checked_neg()
                .map_err(RenderStatus::Time)?,
        );
        let footprint = inverse[0]
            .checked_mul(inverse[3])
            .map_err(RenderStatus::Time)?
            .checked_sub(
                inverse[1]
                    .checked_mul(inverse[2])
                    .map_err(RenderStatus::Time)?,
            )
            .map_err(RenderStatus::Time)?;
        Ok(Self {
            inverse,
            offset: shifted,
            footprint: magnitude(footprint)?,
        })
    }

    /// A scale about the origin, with a translation after it.
    ///
    /// # Errors
    ///
    /// As [`Mapping::new`], and [`RenderStatus::Singular`] for a scale of
    /// nought on either axis.
    pub fn scaled(across: Rational, down: Rational, offset: (Rational, Rational)) -> Result<Self> {
        Self::new([across, Rational::ZERO, Rational::ZERO, down], offset)
    }

    /// A dimensionless map about a point of a picture, with a move in
    /// fractions of it.
    ///
    /// This is the shape a *transform* has, as opposed to the raw pixel-space
    /// map [`Mapping::new`] takes: the linear part means the same thing at
    /// every resolution, the move is a fraction rather than a pixel count, and
    /// it acts about a named point rather than about the corner. Scaling about
    /// the corner is what the arithmetic does if nobody decides otherwise, and
    /// it sends the picture sliding off to the lower right the moment somebody
    /// drags a scale slider.
    ///
    /// The anchor is in **fractions** of the picture, so a half and a half is
    /// the middle and nought is the top-left corner. It is not required to be
    /// inside the picture.
    ///
    /// ## Why the anchor cannot be folded into the offset
    ///
    /// It looks as though it could. Acting about `a` rather than about the
    /// centre `c` contributes `(a − c) − M(a − c)`, which is a translation,
    /// and the caller already passes one — so a model could add the two and
    /// this function would never need to know.
    ///
    /// That is true in **pixels** and false in **fractions**, which is the
    /// only place the model could do it. The vector from the centre to the
    /// anchor is `(W·Δx, H·Δy)`, and `M` mixes the two components; dividing
    /// the result back by `(W, H)` per axis does not undo that unless `M` is
    /// diagonal. So the folding is exact for a scale, exact for a move, and
    /// **wrong for every rotation** — which is to say, wrong for the case the
    /// anchor was added for. The anchor therefore arrives here, where the
    /// pixel dimensions are known and the arithmetic is done once.
    ///
    /// # Errors
    ///
    /// As [`Mapping::new`], and [`RenderStatus::OutsideDomain`] for a picture
    /// past [`MAX_EXTENT`].
    pub fn about(
        linear: [Rational; 4],
        offset: (Rational, Rational),
        anchor: (Rational, Rational),
        width: u32,
        height: u32,
    ) -> Result<Self> {
        if width == 0
            || height == 0
            || usize::try_from(width).map_err(|_| RenderStatus::OutsideDomain)? > MAX_EXTENT
            || usize::try_from(height).map_err(|_| RenderStatus::OutsideDomain)? > MAX_EXTENT
        {
            return Err(RenderStatus::OutsideDomain);
        }
        let across = Rational::from_integer(i64::from(width));
        let down = Rational::from_integer(i64::from(height));
        let pivot = (
            across.checked_mul(anchor.0).map_err(RenderStatus::Time)?,
            down.checked_mul(anchor.1).map_err(RenderStatus::Time)?,
        );
        // Acting about the pivot contributes `pivot - M·pivot` on each axis,
        // and the move is a fraction of the picture rather than a distance.
        let turned = apply(linear, pivot);
        let shifted = (
            pivot
                .0
                .checked_sub(turned.0)
                .map_err(RenderStatus::Time)?
                .checked_add(across.checked_mul(offset.0).map_err(RenderStatus::Time)?)
                .map_err(RenderStatus::Time)?,
            pivot
                .1
                .checked_sub(turned.1)
                .map_err(RenderStatus::Time)?
                .checked_add(down.checked_mul(offset.1).map_err(RenderStatus::Time)?)
                .map_err(RenderStatus::Time)?,
        );
        Self::new(linear, shifted)
    }

    /// Where a destination point came from in the source, for a test that
    /// needs to ask about a *point* rather than about a pixel.
    ///
    /// A pixel is a square and a fixed point is a point, so the one property
    /// that defines a pivot — that it does not move — cannot be asked of the
    /// resampled picture. Exposed rather than reimplemented in the test,
    /// because a test that recomputed the inverse would be checking its own
    /// arithmetic against itself.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an overflow.
    pub fn source_of_for_test(&self, x: Rational, y: Rational) -> Result<(Rational, Rational)> {
        self.source_of(x, y)
    }

    /// Where a destination point came from in the source.
    fn source_of(&self, x: Rational, y: Rational) -> Result<(Rational, Rational)> {
        let mapped = apply(self.inverse, (x, y));
        Ok((
            mapped
                .0
                .checked_add(self.offset.0)
                .map_err(RenderStatus::Time)?,
            mapped
                .1
                .checked_add(self.offset.1)
                .map_err(RenderStatus::Time)?,
        ))
    }
}

/// A two-by-two matrix times a point.
fn apply(matrix: [Rational; 4], point: (Rational, Rational)) -> (Rational, Rational) {
    let across = matrix[0]
        .checked_mul(point.0)
        .and_then(|held| held.checked_add(matrix[1].checked_mul(point.1)?))
        .unwrap_or(Rational::ZERO);
    let down = matrix[2]
        .checked_mul(point.0)
        .and_then(|held| held.checked_add(matrix[3].checked_mul(point.1)?))
        .unwrap_or(Rational::ZERO);
    (across, down)
}

/// A rational without its sign.
fn magnitude(value: Rational) -> Result<Rational> {
    if value.numerator() < 0 {
        value.checked_neg().map_err(RenderStatus::Time)
    } else {
        Ok(value)
    }
}

/// Move a premultiplied frame onto a new one.
///
/// Everything outside the source contributes nothing — no colour and no
/// coverage — so a picture scaled smaller than its frame arrives surrounded by
/// transparency rather than by an edge pixel smeared outwards.
///
/// # Errors
///
/// [`RenderStatus::AlphaRequired`] for a format with no alpha channel,
/// [`RenderStatus::WrongAlphaState`] for a frame that is not premultiplied,
/// [`RenderStatus::OutsideDomain`] for a picture past [`MAX_EXTENT`],
/// [`RenderStatus::OutOfMemory`], and [`RenderStatus::Time`] wrapping an
/// overflow.
pub fn resample(
    frame: &Frame,
    target: FrameDescription,
    mapping: Mapping,
    filter: Filter,
) -> Result<Frame> {
    let described = *frame.description();
    agreed(described, target)?;
    let source = Picture::new(frame)?;
    let (width, height) = extent(target)?;
    let table = TransferTable::build(described.colour())?;
    let mut out = Vec::new();
    out.try_reserve(
        width
            .checked_mul(height)
            .ok_or(RenderStatus::OutsideDomain)?
            .checked_mul(CHANNELS)
            .ok_or(RenderStatus::OutsideDomain)?,
    )
    .map_err(|_| RenderStatus::OutOfMemory)?;
    for y in 0..height {
        drawn(
            &source,
            &table,
            mapping,
            filter,
            Tile::row(y, width),
            y,
            &mut out,
        )?;
    }
    Ok(Frame::from_owned(target, out)?)
}

/// Move one destination row of a premultiplied frame, from a band of the
/// source rather than from all of it.
///
/// The whole row as one strip, which is what a map that takes horizontals to
/// horizontals needs and all it needs. A turn wants [`resample_strip`], and
/// this is that function with the strip filled in.
///
/// # Errors
///
/// As [`resample_strip`], plus [`RenderStatus::OutsideDomain`] for a row
/// outside the frame.
pub fn resample_row(
    band: Option<&Frame>,
    source: FrameDescription,
    from: usize,
    target: FrameDescription,
    mapping: Mapping,
    filter: Filter,
    row: usize,
) -> Result<Frame> {
    agreed(source, target)?;
    let (width, height) = extent(target)?;
    if row >= height {
        return Err(RenderStatus::OutsideDomain);
    }
    let mut out = Vec::new();
    out.try_reserve(
        width
            .checked_mul(CHANNELS)
            .ok_or(RenderStatus::OutsideDomain)?,
    )
    .map_err(|_| RenderStatus::OutOfMemory)?;
    resample_tile(
        band,
        source,
        from,
        mapping,
        filter,
        Tile::row(row, width),
        core::slice::from_mut(&mut out),
    )?;
    Ok(Frame::from_owned(one_row_of(target)?, out)?)
}

/// Move one tile of a premultiplied frame, appending each of its rows.
///
/// The band is what [`tile`] said this tile reads, fetched by whoever is
/// scanning; `from` is the first picture row it holds and `described` is the
/// **whole** picture it came out of, which is what tells a sample landing past
/// the band's edge whether it is off the picture or off the band.
///
/// `out` is one buffer per destination row of the tile, in order, and each
/// row's pixels are appended to its own. A rectangle is not contiguous in a
/// row-major picture, so a single buffer would need a stride and a caller that
/// understood it; a buffer a row is the shape the caller wants anyway, because
/// what it does next is hand those rows to a sink one at a time.
///
/// One description rather than a source and a target, because a transform
/// resamples into its source's own description and this exists for a
/// transform. A resample between two descriptions is [`resample`].
///
/// `None` is an **empty** band, which happens legitimately and often — a
/// picture moved off the top of its own frame contributes nothing to the rows
/// it has left — and the answer is a transparent tile rather than a refusal.
/// It is spelled as an absent frame rather than as a frame of no rows because
/// a picture with no rows is not a picture: [`media_editor_media::Geometry`]
/// refuses one, so there would be nothing for a caller to pass.
///
/// # Errors
///
/// Everything [`resample`] refuses, plus [`RenderStatus::RowOutsideBand`] for
/// a band that does not hold what this tile reads — which is a wrong answer
/// from [`tile`] rather than a bad argument from a caller —
/// [`RenderStatus::NotComposable`] for a band that does not describe the
/// picture it claims to be part of, and [`RenderStatus::OutsideDomain`] for a
/// tile outside the picture or a set of buffers that is not one a row.
pub fn resample_tile(
    band: Option<&Frame>,
    described: FrameDescription,
    from: usize,
    mapping: Mapping,
    filter: Filter,
    over: Tile,
    out: &mut [Vec<u8>],
) -> Result<()> {
    agreed(described, described)?;
    let (width, height) = extent(described)?;
    if over.rows.1 > height
        || over.columns.1 > width
        || over.rows.0 >= over.rows.1
        || over.columns.0 >= over.columns.1
        || out.len() != over.height()
    {
        return Err(RenderStatus::OutsideDomain);
    }
    let columns = over.width();
    for buffer in out.iter_mut() {
        buffer
            .try_reserve(
                columns
                    .checked_mul(CHANNELS)
                    .ok_or(RenderStatus::OutsideDomain)?,
            )
            .map_err(|_| RenderStatus::OutOfMemory)?;
    }
    let Some(band) = band else {
        for buffer in out.iter_mut() {
            buffer.resize(buffer.len() + columns * CHANNELS, 0);
        }
        return Ok(());
    };
    if band.description().geometry().width() != described.geometry().width()
        || band.description().format() != described.format()
        || band.description().colour() != described.colour()
        || band.description().alpha() != described.alpha()
    {
        // The band has to be the same picture, cut down. A band described
        // otherwise is a band of something else, and resampling it would
        // answer confidently about a picture nobody asked for.
        return Err(RenderStatus::NotComposable);
    }
    let held = Picture::banded(band, from, height)?;
    let table = TransferTable::build(described.colour())?;
    for (index, buffer) in out.iter_mut().enumerate() {
        drawn(
            &held,
            &table,
            mapping,
            filter,
            over,
            over.rows.0 + index,
            buffer,
        )?;
    }
    Ok(())
}

/// The checks a resample makes before it moves a pixel.
fn agreed(described: FrameDescription, target: FrameDescription) -> Result<()> {
    if described.format() != PixelFormat::Rgba8 || target.format() != PixelFormat::Rgba8 {
        return Err(RenderStatus::AlphaRequired);
    }
    if described.alpha() != Some(AlphaState::Premultiplied)
        || target.alpha() != Some(AlphaState::Premultiplied)
    {
        return Err(RenderStatus::WrongAlphaState);
    }
    if described.colour() != target.colour() {
        // Resampling is not a colour conversion, and doing both at once is how
        // a picture ends up converted twice or not at all (R-1.3).
        return Err(RenderStatus::NotComposable);
    }
    Ok(())
}

/// A description's extent in pixels, refused if it is not one this resamples.
fn extent(target: FrameDescription) -> Result<(usize, usize)> {
    let width =
        usize::try_from(target.geometry().width()).map_err(|_| RenderStatus::OutsideDomain)?;
    let height =
        usize::try_from(target.geometry().height()).map_err(|_| RenderStatus::OutsideDomain)?;
    if width == 0 || height == 0 || width > MAX_EXTENT || height > MAX_EXTENT {
        return Err(RenderStatus::OutsideDomain);
    }
    Ok((width, height))
}

/// The same picture, one row high.
fn one_row_of(description: FrameDescription) -> Result<FrameDescription> {
    FrameDescription::new(
        media_editor_media::Geometry::new(description.geometry().width(), 1)
            .map_err(RenderStatus::Media)?,
        description.format(),
        description.colour(),
        description.siting(),
        description.alpha(),
        description.pixel_aspect(),
    )
    .map_err(RenderStatus::Media)
}

/// One destination row, appended.
///
/// The one loop both a whole frame and a single row go through, so a scanned
/// picture and a rendered one cannot differ by a pixel: there is nothing for
/// them to differ in.
fn drawn(
    source: &Picture,
    table: &TransferTable,
    mapping: Mapping,
    filter: Filter,
    over: Tile,
    at: usize,
    out: &mut Vec<u8>,
) -> Result<()> {
    let row = i64::try_from(at).map_err(|_| RenderStatus::OutsideDomain)?;
    for x in over.columns.0..over.columns.1 {
        let column = i64::try_from(x).map_err(|_| RenderStatus::OutsideDomain)?;
        let pixel = match filter {
            Filter::Area => area_at(source, table, mapping, column, row)?,
            Filter::Bilinear => bilinear_at(source, table, mapping, column, row)?,
        };
        out.extend_from_slice(&pixel);
    }
    Ok(())
}

/// A source frame's samples, with its size — or a band of its rows, with the
/// size of the picture they came out of.
///
/// The second is what makes a resampled row possible, and the reason the two
/// are one type rather than two is that every sampler here asks the same two
/// questions: *is this position inside the picture* and *what is the sample
/// there*. A band answers the first exactly as a whole frame does — it knows
/// how tall the picture is even though it does not hold it — and answers the
/// second for the rows it holds. Outside those, it refuses.
struct Picture<'a> {
    packed: alloc::borrow::Cow<'a, [u8]>,
    width: i64,
    height: i64,
    /// The first row of the picture that `packed` holds.
    from: i64,
    /// One past the last row of the picture that `packed` holds.
    to: i64,
}

impl<'a> Picture<'a> {
    fn new(frame: &'a Frame) -> Result<Self> {
        let geometry = frame.description().geometry();
        let height = i64::from(geometry.height());
        Ok(Self {
            // Lent, not copied. Everything this resamples is `Rgba8`, which
            // is one plane, so this is always the borrow -- and a whole frame
            // is the largest thing the resampler touches.
            packed: frame.packed()?,
            width: i64::from(geometry.width()),
            height,
            from: 0,
            to: height,
        })
    }

    /// A band of a picture: rows `from` onwards of a picture `height` tall.
    fn banded(frame: &'a Frame, from: usize, height: usize) -> Result<Self> {
        let geometry = frame.description().geometry();
        let from = i64::try_from(from).map_err(|_| RenderStatus::OutsideDomain)?;
        let to = from
            .checked_add(i64::from(geometry.height()))
            .ok_or(RenderStatus::OutsideDomain)?;
        let height = i64::try_from(height).map_err(|_| RenderStatus::OutsideDomain)?;
        if to > height {
            // A band that claims rows the picture does not have is a band
            // computed against a different picture, and drawing from it would
            // produce a row nobody could explain.
            return Err(RenderStatus::RowOutsideBand);
        }
        Ok(Self {
            packed: frame.packed()?,
            width: i64::from(geometry.width()),
            height,
            from,
            to,
        })
    }

    /// One pixel, or nothing outside the picture.
    ///
    /// Nothing rather than the nearest edge pixel: a source that ran off its
    /// own edge would smear its last column outwards forever, and a picture
    /// scaled smaller than its frame would arrive with a streak instead of a
    /// border.
    ///
    /// Inside the picture and outside the band is neither of those, and is the
    /// one case that must not be answered: transparency is what a source
    /// legitimately returns past its own edge, so a band that came back too
    /// short would draw a hole in the picture and look like a picture with a
    /// hole in it.
    fn at(&self, x: i64, y: i64) -> Result<Option<[u8; CHANNELS]>> {
        if x < 0 || y < 0 || x >= self.width || y >= self.height {
            return Ok(None);
        }
        if y < self.from || y >= self.to {
            return Err(RenderStatus::RowOutsideBand);
        }
        let index = usize::try_from((y - self.from) * self.width + x)
            .map_err(|_| RenderStatus::OutsideDomain)?
            * CHANNELS;
        let held = self
            .packed
            .get(index..index + CHANNELS)
            .ok_or(RenderStatus::OutsideDomain)?;
        Ok(Some([held[0], held[1], held[2], held[3]]))
    }

    /// Whether a position lies inside the picture's extent.
    ///
    /// The extent is `[0, width) x [0, height)` in pixel coordinates, which is
    /// the region the picture covers — a different question from which sample
    /// is nearest, and the one that decides whether there is a picture here at
    /// all.
    fn holds(&self, x: Rational, y: Rational) -> Result<bool> {
        let inside = |value: Rational, limit: i64| -> Result<bool> {
            if value.numerator() < 0 {
                return Ok(false);
            }
            Ok(!value
                .checked_sub(Rational::from_integer(limit))
                .map_err(RenderStatus::Time)?
                .is_positive())
        };
        Ok(inside(x, self.width)? && inside(y, self.height)?)
    }

    /// One pixel, with the index brought back to the nearest that exists.
    ///
    /// Only for a position already known to be inside the extent: past the
    /// last sample's centre the best estimate of the signal is the last
    /// sample, and repeating it is what every reconstruction does at a
    /// boundary.
    fn clamped(&self, x: i64, y: i64) -> Result<[u8; CHANNELS]> {
        let x = x.clamp(0, self.width.saturating_sub(1));
        let y = y.clamp(0, self.height.saturating_sub(1));
        Ok(self.at(x, y)?.unwrap_or([0; CHANNELS]))
    }
}

/// A rectangle of destination pixels, taken back into the source.
///
/// An affine map sends a rectangle to a parallelogram, so this is exact and
/// has four corners however the picture is turned. One pixel is `(x, x + 1)`
/// by `(y, y + 1)`; a strip widens the columns; a **tile** widens both.
///
/// The vertical extent of this parallelogram is what a band has to hold, and
/// the two generalisations buy different things. Widening the columns is what
/// made a turn scannable at all — narrow the strip and the band shrinks.
/// Widening the rows is what stops it re-reading: one band that covers many
/// destination rows is fetched once and drawn from many times.
fn preimage_of(
    mapping: Mapping,
    columns: (i64, i64),
    rows: (i64, i64),
) -> Result<Vec<(Rational, Rational)>> {
    let mut preimage = Vec::new();
    preimage
        .try_reserve(4)
        .map_err(|_| RenderStatus::OutOfMemory)?;
    for (x, y) in [
        (columns.0, rows.0),
        (columns.1, rows.0),
        (columns.1, rows.1),
        (columns.0, rows.1),
    ] {
        preimage.push(mapping.source_of(Rational::from_integer(x), Rational::from_integer(y))?);
    }
    Ok(preimage)
}

/// Where a destination pixel's centre lands on the source's sample grid.
struct Landing {
    /// The centre, in source pixel coordinates.
    centre: (Rational, Rational),
    /// The sample above and to the left of it.
    corner: (i64, i64),
    /// How far past that sample the centre sits, per axis, from nought to one.
    fraction: (Rational, Rational),
}

/// Take a destination pixel's centre back to the source's sample grid.
///
/// The centre, because a pixel's value belongs at its middle rather than at
/// its corner. Sampling at the corner shifts the whole picture half a pixel up
/// and left, which is the single most common resampling bug and is invisible
/// until two versions of the same shot are compared. And the samples
/// themselves sit at pixel centres, so the grid this interpolates on is offset
/// by half a pixel from the pixel grid.
fn landing_of(mapping: Mapping, x: i64, y: i64) -> Result<Landing> {
    let half = Rational::new(1, 2).map_err(RenderStatus::Time)?;
    let shifted = |value: i64| -> Result<Rational> {
        Rational::from_integer(value)
            .checked_add(half)
            .map_err(RenderStatus::Time)
    };
    let centre = mapping.source_of(shifted(x)?, shifted(y)?)?;
    let across = centre.0.checked_sub(half).map_err(RenderStatus::Time)?;
    let down = centre.1.checked_sub(half).map_err(RenderStatus::Time)?;
    let left = across.floor().map_err(RenderStatus::Time)?;
    let top = down.floor().map_err(RenderStatus::Time)?;
    Ok(Landing {
        centre,
        corner: (left, top),
        fraction: (
            across
                .checked_sub(Rational::from_integer(left))
                .map_err(RenderStatus::Time)?,
            down.checked_sub(Rational::from_integer(top))
                .map_err(RenderStatus::Time)?,
        ),
    })
}

/// How many destination rows one tile may cover.
///
/// Sixteen, and the number is a trade rather than a limit. A tile of `h` rows
/// holds `h` rows of output while it is drawn, so `h` is memory; and it
/// divides the re-reading a turn does by roughly `h`, so `h` is also speed.
/// Sixteen rows of a 1,920-wide picture is 122,880 bytes — more than a
/// Phipia program is mapped at, which is why the *caller* decides how tall a
/// tile it can afford and this only says how tall one may be.
///
/// It is not [`MAX_BAND_ROWS`] and must not be confused with it. That bounds
/// the **source** rows one tile reads; this bounds the **destination** rows
/// one tile writes. A tile of sixteen rows may read sixty-four.
pub const MAX_TILE_ROWS: usize = 16;

/// A rectangle of a destination picture.
///
/// The unit a resampled picture is actually drawn in. A whole row is
/// `rows: (y, y + 1), columns: (0, width)`, and for a map that takes
/// horizontals to horizontals that is the only tile anybody needs. For a turn
/// it is not, and [`tile`] says why.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Tile {
    /// The destination rows, half-open.
    pub rows: (usize, usize),
    /// The destination columns, half-open.
    pub columns: (usize, usize),
}

impl Tile {
    /// One whole row.
    #[must_use]
    pub const fn row(row: usize, width: usize) -> Self {
        Self {
            rows: (row, row + 1),
            columns: (0, width),
        }
    }

    /// How many rows it covers.
    #[must_use]
    pub const fn height(&self) -> usize {
        self.rows.1.saturating_sub(self.rows.0)
    }

    /// How many columns it covers.
    #[must_use]
    pub const fn width(&self) -> usize {
        self.columns.1.saturating_sub(self.columns.0)
    }
}

/// The source rows one tile reads, inclusive at both ends and before any
/// clamping to the picture.
///
/// The arithmetic is not this function's: it is `preimage_of` and
/// `landing_of`, the same two the samplers use, so a band and the pixels drawn
/// inside it cannot disagree about which rows those are.
fn rows_under(mapping: Mapping, filter: Filter, over: Tile) -> Result<(i64, i64)> {
    let whole = |value: usize| i64::try_from(value).map_err(|_| RenderStatus::OutsideDomain);
    let (left, right) = (whole(over.columns.0)?, whole(over.columns.1)?);
    let (top, bottom) = (whole(over.rows.0)?, whole(over.rows.1)?);
    if right <= left || bottom <= top {
        return Err(RenderStatus::OutsideDomain);
    }
    match filter {
        Filter::Area => {
            let (_, first, _, last) = bounds(&preimage_of(mapping, (left, right), (top, bottom))?)?;
            Ok((first, last))
        }
        // Bilinear reads the sample above each centre and the one below it,
        // and no more. The source's vertical coordinate is affine in the
        // column and in the row, so over a rectangle its extremes are at the
        // four corner pixels — there is nothing in the middle to check.
        Filter::Bilinear => {
            let mut least = i64::MAX;
            let mut most = i64::MIN;
            for (x, y) in [
                (left, top),
                (right - 1, top),
                (left, bottom - 1),
                (right - 1, bottom - 1),
            ] {
                let at = landing_of(mapping, x, y)?.corner.1;
                least = least.min(at);
                most = most.max(at);
            }
            Ok((least, most.saturating_add(1)))
        }
    }
}

/// The source rows one tile reads, half-open and inside the picture.
///
/// Asked **before** the rows are fetched, which is the whole point: a caller
/// that scans gathers exactly this band and no more, and a caller that cannot
/// afford the band learns so before it has spent a row on it.
///
/// ## Why a tile and not a row
///
/// A destination row's preimage is a segment of the source. When the map takes
/// horizontals to horizontals that segment lies flat, so the whole row reads
/// one short band and there is nothing to slice. When it does not — a rotation,
/// a vertical shear — the segment has a slope, and a segment of slope `m`
/// across `w` columns crosses about `m · w` rows. For a picture of any size
/// that is more than a band may hold.
///
/// Narrowing the columns fixes that, and a **strip** — a tile one row tall —
/// is all it takes to make a turn drawable at all. What it does not fix is the
/// *re-reading*: neighbouring strips have overlapping bands, so a whole frame
/// drawn strip by strip fetches about `m · width` rows for every one of its
/// rows.
///
/// Widening the rows is what fixes that, and it is why this takes a rectangle.
/// One band that covers `h` destination rows is fetched once and drawn from
/// `h` times, so the re-reading falls by a factor of about `h` — for the price
/// of holding `h` rows of output. That is the trade [`MAX_TILE_ROWS`] bounds.
///
/// The band is empty when the tile's preimage misses the picture altogether,
/// and an empty band is not an error: there is nothing there, and a resampled
/// tile of nothing is transparent.
///
/// # Errors
///
/// [`RenderStatus::BandTooTall`] past [`MAX_BAND_ROWS`],
/// [`RenderStatus::OutsideDomain`] for a tile of no pixels, and
/// [`RenderStatus::Time`] wrapping an overflow.
pub fn tile(mapping: Mapping, filter: Filter, over: Tile, height: usize) -> Result<(usize, usize)> {
    let tall = i64::try_from(height).map_err(|_| RenderStatus::OutsideDomain)?;
    let (top, bottom) = rows_under(mapping, filter, over)?;
    let from = top.clamp(0, tall);
    let to = bottom.saturating_add(1).clamp(from, tall);
    let held = usize::try_from(to - from).map_err(|_| RenderStatus::OutsideDomain)?;
    if held > MAX_BAND_ROWS {
        return Err(RenderStatus::BandTooTall);
    }
    Ok((
        usize::try_from(from).map_err(|_| RenderStatus::OutsideDomain)?,
        usize::try_from(to).map_err(|_| RenderStatus::OutsideDomain)?,
    ))
}

/// The exact area-weighted mean of the source under one destination pixel.
fn area_at(
    source: &Picture,
    table: &TransferTable,
    mapping: Mapping,
    x: i64,
    y: i64,
) -> Result<[u8; CHANNELS]> {
    let preimage = preimage_of(mapping, (x, x + 1), (y, y + 1))?;
    let (left, top, right, bottom) = bounds(&preimage)?;
    let mut light = [Fixed::ZERO; CHANNELS];
    for row in top..=bottom {
        for column in left..=right {
            let Some(pixel) = source.at(column, row)? else {
                continue;
            };
            let share = overlap(&preimage, column, row)?;
            if share.is_zero() {
                continue;
            }
            let weight = Fixed::from_rational(
                share
                    .checked_div(mapping.footprint)
                    .map_err(RenderStatus::Time)?,
            )?;
            for channel in 0..CHANNELS {
                let value = if channel == ALPHA {
                    coverage_of(pixel[ALPHA])?
                } else {
                    table.decode(pixel[channel])
                };
                light[channel] = light[channel]
                    .checked_add(value.checked_mul(weight)?)
                    .map_err(RenderStatus::Time)?;
            }
        }
    }
    finish(table, light)
}

/// Four samples around where a destination pixel's centre lands.
fn bilinear_at(
    source: &Picture,
    table: &TransferTable,
    mapping: Mapping,
    x: i64,
    y: i64,
) -> Result<[u8; CHANNELS]> {
    let landing = landing_of(mapping, x, y)?;
    let (left, top) = landing.corner;
    let (fraction_x, fraction_y) = landing.fraction;

    // Outside the picture's *extent* is nothing at all. Inside it but beyond
    // the last sample's centre is a different thing entirely, and conflating
    // the two is what made the last column of an enlarged picture fade out:
    // the samples sit at pixel centres, so the outer half-pixel of a picture
    // lies beyond every sample and has no second one to interpolate towards.
    // There the reconstruction clamps to the edge sample, which is the best
    // estimate of a signal past its last measurement.
    if !source.holds(landing.centre.0, landing.centre.1)? {
        return Ok([0; CHANNELS]);
    }

    let mut light = [Fixed::ZERO; CHANNELS];
    for (dx, dy) in [(0_i64, 0_i64), (1, 0), (0, 1), (1, 1)] {
        let pixel = source.clamped(left + dx, top + dy)?;
        let weight_x = if dx == 0 {
            Rational::ONE
                .checked_sub(fraction_x)
                .map_err(RenderStatus::Time)?
        } else {
            fraction_x
        };
        let weight_y = if dy == 0 {
            Rational::ONE
                .checked_sub(fraction_y)
                .map_err(RenderStatus::Time)?
        } else {
            fraction_y
        };
        let weight =
            Fixed::from_rational(weight_x.checked_mul(weight_y).map_err(RenderStatus::Time)?)?;
        for channel in 0..CHANNELS {
            let value = if channel == ALPHA {
                coverage_of(pixel[ALPHA])?
            } else {
                table.decode(pixel[channel])
            };
            light[channel] = light[channel]
                .checked_add(value.checked_mul(weight)?)
                .map_err(RenderStatus::Time)?;
        }
    }
    finish(table, light)
}

/// Coverage as a fraction of one.
///
/// Alpha is not light and never passes through a transfer function: it is a
/// fraction of a pixel's area, so it is linear by definition.
fn coverage_of(code: u8) -> Result<Fixed> {
    Fixed::from_rational(Rational::new(i64::from(code), 255).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)
}

/// Encode an accumulated pixel back to bytes.
fn finish(table: &TransferTable, light: [Fixed; CHANNELS]) -> Result<[u8; CHANNELS]> {
    let mut out = [0_u8; CHANNELS];
    for channel in 0..ALPHA {
        out[channel] = table.encode(clamp(light[channel]));
    }
    // The same rounding the compositor uses for coverage, stated in one place
    // rather than left to whatever the multiplication happened to do.
    let scaled = clamp(light[ALPHA]).checked_mul(Fixed::from_integer(255)?)?;
    let half = 1_i64 << (FRACTION_BITS - 1);
    let rounded = scaled.raw().saturating_add(half) >> FRACTION_BITS;
    out[ALPHA] = u8::try_from(rounded.clamp(0, 255)).map_err(|_| RenderStatus::OutsideDomain)?;
    Ok(out)
}

/// A value brought back inside nought to one.
fn clamp(value: Fixed) -> Fixed {
    if value.raw() < 0 {
        return Fixed::ZERO;
    }
    if value.raw() > Fixed::ONE.raw() {
        return Fixed::ONE;
    }
    value
}

/// The whole-pixel box a polygon lies inside.
fn bounds(polygon: &[(Rational, Rational)]) -> Result<(i64, i64, i64, i64)> {
    let mut left = polygon[0].0;
    let mut right = polygon[0].0;
    let mut top = polygon[0].1;
    let mut bottom = polygon[0].1;
    for point in polygon {
        if point
            .0
            .checked_sub(left)
            .map_err(RenderStatus::Time)?
            .numerator()
            < 0
        {
            left = point.0;
        }
        if point
            .0
            .checked_sub(right)
            .map_err(RenderStatus::Time)?
            .is_positive()
        {
            right = point.0;
        }
        if point
            .1
            .checked_sub(top)
            .map_err(RenderStatus::Time)?
            .numerator()
            < 0
        {
            top = point.1;
        }
        if point
            .1
            .checked_sub(bottom)
            .map_err(RenderStatus::Time)?
            .is_positive()
        {
            bottom = point.1;
        }
    }
    Ok((
        left.floor().map_err(RenderStatus::Time)?,
        top.floor().map_err(RenderStatus::Time)?,
        right.floor().map_err(RenderStatus::Time)?,
        bottom.floor().map_err(RenderStatus::Time)?,
    ))
}

/// The exact area of one source pixel inside a preimage parallelogram.
///
/// Both are convex, so this is the shape rasteriser's own clip — and the
/// preimage's winding is whatever the map made of it, which under a mirror is
/// the other one. `shape::convex` measures it rather than being told, which is
/// exactly why a mask and a resampled pixel can share it.
fn overlap(preimage: &[(Rational, Rational)], x: i64, y: i64) -> Result<Rational> {
    match crate::shape::convex(preimage) {
        Ok(shape) => shape.coverage(x, y),
        // A preimage with no area is not an error here: an affine map cannot
        // produce one, because a singular map was refused when the mapping was
        // built. It is unreachable rather than impossible, so it answers
        // nought rather than asserting.
        Err(RenderStatus::DegenerateShape) => Ok(Rational::ZERO),
        Err(other) => Err(other),
    }
}
