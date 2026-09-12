// SPDX-License-Identifier: GPL-3.0-only
//! A face, and text drawn as exact coverage.
//!
//! Text arrived last for a reason recorded three milestones ago: "offline
//! media renders a slate but cannot say which media is missing — that is text
//! on a frame, and text needs a font". A font could not be taken from
//! anywhere. Every outline format worth reading is a parser and a hinting
//! engine, and every free face worth shipping is somebody else's licence to
//! reconcile with GPL-3.0-only on a platform that cannot mount a file system
//! to load one. So Media Editor writes its own, and what it writes is decided by
//! what it already has.
//!
//! ## A glyph is disjoint convex pieces, because that is what is exact
//!
//! The shape rasteriser computes the **exact area** of a pixel lying inside a
//! convex region. A letter is not convex. But a letter *is* a small number of
//! convex pieces, and the area of a union of pieces whose interiors do not
//! overlap is the sum of their areas — exactly, with no reasoning about
//! antialiasing at all.
//!
//! So every glyph here is authored as pieces that **touch but never overlap**,
//! and coverage is their sum. That is not a convention the drawing hopes for:
//! [`shape::quantise`] refuses a coverage above full, so a face whose pieces
//! overlapped enough to fill a pixel past one would be *refused* rather than
//! drawn wrong. And the tests go further and measure every pair's intersection
//! area exactly, which catches an overlap far too small to fill a pixel.
//!
//! The consequence worth having is that this face is exact at every size. There
//! is no bitmap, no hinting, no grid fitting and no size at which it stops
//! being the same letter — a title at 4K and the same title on a proxy are the
//! same shape, measured the same way.
//!
//! ## What it does not do, and why
//!
//! **No curves.** Every piece is a straight-edged quadrilateral or triangle.
//! A curve would be a polygonal approximation of a curve, which is a decision
//! about how many segments — a number that would have to be defended, would
//! change with size, and would make "the same shape at every size" false.
//!
//! **Three heights, not one.** Capitals were the whole face to begin with, and
//! a capital runs from the cap line to the baseline and does nothing else —
//! one measurement. Lowercase needed three more: an x-height the bodies sit
//! on, ascenders that reach the cap line, and descenders that hang below the
//! baseline. That is why it was deferred as "a second set of metrics" rather
//! than as more of the same drawing, and the deferral was right about the
//! work. The metrics are named here and a test measures the glyphs against
//! them, so they describe the face rather than decorating it.
//!
//! **Monospaced.** Every glyph advances by the same amount. The first things
//! drawn are a timecode and a digest, and a proportional face makes a counting
//! number dance in place while it counts.
//!
//! ## The design grid
//!
//! Half-units, sixteen to the em vertically. A glyph is drawn in a box ten
//! half-units wide and sixteen tall — five by eight whole units — with a
//! stroke two half-units thick, and the pen advances twelve. Every design
//! coordinate below is an integer in that grid, so the face is exact rational
//! data rather than a table of decimals somebody rounded.

use alloc::vec::Vec;

use media_editor_core::Rational;

use crate::shape::{self, Shape};
use crate::status::{RenderStatus, Result};

/// Half-units to the em.
pub const GRID: i64 = 16;

/// How far the pen moves for every glyph, in half-units.
pub const ADVANCE: i64 = 12;

/// How wide a glyph's drawing box is, in half-units.
pub const BOX_WIDTH: i64 = 10;

/// The baseline, in half-units below the cap line.
///
/// Also the em: a capital runs from nought to here and that is all it does,
/// which is what made capitals alone "one set of metrics" and lowercase a
/// second one.
pub const BASELINE: i64 = 16;

/// Where the body of a lowercase letter starts, in half-units.
///
/// Ten of the sixteen, which is the ratio a grotesque of this weight wants:
/// much smaller and the lowercase reads as small capitals, much larger and the
/// ascenders stop being visible as ascenders.
pub const X_LINE: i64 = 6;

/// How far below the baseline a descender reaches, in half-units.
pub const DESCENDER: i64 = 5;

/// Cap line to the next line's cap line, in half-units.
///
/// The em plus the descender plus one half-unit of air. Set at the em instead
/// — which is what "line height equals font size" means everywhere it is
/// offered — and every `g` in one line touches every `A` in the next.
pub const LINE_SPACING: i64 = BASELINE + DESCENDER + 1;

/// The longest run this draws.
///
/// A slate's caption, not a screenplay. Bounded because everything here is
/// (R-5.1), and low enough that the whole run's pieces stay a list worth
/// scanning per pixel.
pub const MAX_TEXT: usize = 128;

/// A corner, in ems: x to the right, y down from the cap line.
type Point = (Rational, Rational);

/// A quadrilateral with two horizontal edges: what every stroke here is.
///
/// Slanted or upright, a stroke in this face is a shape with a flat top and a
/// flat bottom and two straight sides. That is enough for every letter and it
/// is convex by construction, which is what the rasteriser needs — and it
/// gives a stroke two edges that can be *evaluated* at a height, which is what
/// makes a crossbar between two slanted legs exact instead of eyeballed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Band {
    top: (i16, i16),
    bottom: (i16, i16),
    y0: i16,
    y1: i16,
}

/// An upright stroke: a rectangle, in half-units.
const fn rect(x0: i16, x1: i16, y0: i16, y1: i16) -> Band {
    Band {
        top: (x0, x1),
        bottom: (x0, x1),
        y0,
        y1,
    }
}

/// A slanted stroke, in half-units.
const fn leg(top: (i16, i16), bottom: (i16, i16), y0: i16, y1: i16) -> Band {
    Band {
        top,
        bottom,
        y0,
        y1,
    }
}

impl Band {
    /// Where an edge of this band sits at a height, as an exact fraction.
    ///
    /// `at0` and `at1` are the edge's x at the band's top and bottom, and the
    /// answer is the straight line between them. This is what a crossbar is
    /// computed from, so that a bar meeting two slanted legs *abuts* them
    /// rather than approximately reaching them: a gap would show as a hairline
    /// and an overlap would be refused.
    fn edge_at(self, at0: i16, at1: i16, y: Rational) -> Result<Rational> {
        let span = i64::from(self.y1 - self.y0);
        if span == 0 {
            return Err(RenderStatus::DegenerateShape);
        }
        let from = whole(i64::from(at0))?;
        let travel = whole(i64::from(at1 - at0))?;
        let along = y
            .checked_sub(whole(i64::from(self.y0))?)
            .map_err(RenderStatus::Time)?
            .checked_div(whole(span)?)
            .map_err(RenderStatus::Time)?;
        from.checked_add(travel.checked_mul(along).map_err(RenderStatus::Time)?)
            .map_err(RenderStatus::Time)
    }

    /// This band's right edge at a height.
    fn right_at(self, y: Rational) -> Result<Rational> {
        self.edge_at(self.top.1, self.bottom.1, y)
    }

    /// This band's left edge at a height.
    fn left_at(self, y: Rational) -> Result<Rational> {
        self.edge_at(self.top.0, self.bottom.0, y)
    }

    /// The corners, in ems, going round.
    fn corners(self) -> Result<Vec<Point>> {
        let mut out = Vec::new();
        out.try_reserve(4).map_err(|_| RenderStatus::OutOfMemory)?;
        for (x, y) in [
            (self.top.0, self.y0),
            (self.top.1, self.y0),
            (self.bottom.1, self.y1),
            (self.bottom.0, self.y1),
        ] {
            out.push((em(i64::from(x))?, em(i64::from(y))?));
        }
        Ok(out)
    }
}

/// A count of half-units as a fraction of the em.
fn em(half_units: i64) -> Result<Rational> {
    Rational::new(half_units, GRID).map_err(RenderStatus::Time)
}

/// A count of half-units as a whole number, for arithmetic in grid space.
fn whole(half_units: i64) -> Result<Rational> {
    Rational::new(half_units, 1).map_err(RenderStatus::Time)
}

/// The pieces of a glyph that are not plain strokes.
///
/// Three glyphs in this face are not four numbers and a height: an `A` has a
/// crossbar whose sides *are* the legs it meets, a `1` has a triangular flag,
/// and a comma has a tail that starts where its dot ends. They are named here
/// rather than stored as coordinates because two of them are computed from
/// other pieces, and a coordinate copied out of a computation is a coordinate
/// that goes stale when the computation changes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Extra {
    /// Nothing beyond the strokes.
    None,
    /// The crossbar between an `A`'s two legs.
    Apex,
    /// The triangular flag of a `1`.
    Flag,
}

/// One character's shape: pieces that touch but never overlap.
///
/// A **view onto static data**, not a copy of it. The face is a table of
/// strokes in a design grid of small integers, which lives in the image's
/// read-only data; a glyph turns its own strokes into exact rational corners
/// when somebody draws it, and only the characters in a run ever pay for that.
///
/// The face used to build every glyph's corners up front, and the measurement
/// that changed it is in the platform contract: `Face::stencil` was the
/// largest single function in the whole program at 23,807 bytes, because every
/// coordinate of every glyph was an *instruction* that stored it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Glyph {
    bands: &'static [Band],
    extra: Extra,
}

impl Glyph {
    /// The pieces, each a convex polygon in em units.
    ///
    /// Computed here rather than held, which is what makes the face a table
    /// rather than a program that builds one.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::OutOfMemory`], or [`RenderStatus::Time`] wrapping an
    /// overflow.
    pub fn pieces(&self) -> Result<Vec<Vec<Point>>> {
        let mut out = Vec::new();
        out.try_reserve(self.len())
            .map_err(|_| RenderStatus::OutOfMemory)?;
        for band in self.bands {
            out.push(band.corners()?);
        }
        match self.extra {
            Extra::None => {}
            Extra::Apex => out.push(between(self.bands[0], self.bands[1], 10, 12)?),
            Extra::Flag => out.push(triangle([(2, 0), (4, 0), (4, 4)])?),
        }
        Ok(out)
    }

    /// How many pieces this is drawn from.
    #[must_use]
    pub fn len(&self) -> usize {
        self.bands.len() + usize::from(!matches!(self.extra, Extra::None))
    }

    /// Whether this glyph draws nothing, which the space does.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The exact total area of this glyph, in square ems.
    ///
    /// A sum, which is only the glyph's real area because the pieces do not
    /// overlap — so this doubles as the number the disjointness tests measure
    /// against.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::OutOfMemory`], or [`RenderStatus::Time`] wrapping an
    /// overflow.
    pub fn area(&self) -> Result<Rational> {
        let mut total = Rational::ZERO;
        for piece in self.pieces()? {
            total = total
                .checked_add(polygon_area(&piece)?)
                .map_err(RenderStatus::Time)?;
        }
        Ok(total)
    }
}

/// The area of a simple polygon, by the shoelace sum, halved once.
fn polygon_area(corners: &[Point]) -> Result<Rational> {
    let mut twice = Rational::ZERO;
    for (index, current) in corners.iter().enumerate() {
        let next = corners[(index + 1) % corners.len()];
        twice = twice
            .checked_add(
                current
                    .0
                    .checked_mul(next.1)
                    .map_err(RenderStatus::Time)?
                    .checked_sub(next.0.checked_mul(current.1).map_err(RenderStatus::Time)?)
                    .map_err(RenderStatus::Time)?,
            )
            .map_err(RenderStatus::Time)?;
    }
    let half = Rational::new(1, 2).map_err(RenderStatus::Time)?;
    let signed = twice.checked_mul(half).map_err(RenderStatus::Time)?;
    Ok(if signed.is_positive() {
        signed
    } else {
        Rational::ZERO
            .checked_sub(signed)
            .map_err(RenderStatus::Time)?
    })
}

/// A repertoire of glyphs.
///
/// Holds nothing. The face is a table in the image's read-only data and this
/// is a handle to it — so obtaining one costs no allocation, cannot fail, and
/// does not have to be kept about and passed around to be cheap.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Face;

impl Face {
    /// The one face Media Editor has: capitals, lowercase, digits, punctuation.
    #[must_use]
    pub const fn stencil() -> Self {
        Self
    }

    /// Whether this face can set a character.
    #[must_use]
    pub fn has(self, character: char) -> bool {
        STENCIL.iter().any(|(held, _, _)| *held == character)
    }

    /// The glyph for a character.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::NoSuchGlyph`], which is deliberate. A face that
    /// substituted a box for a character it does not have would put a
    /// rectangle on a slate and call it a message; a face that substituted
    /// nothing would put a gap there. Both are a picture that says something
    /// other than what it was given, which is the one thing a slate must not
    /// do.
    pub fn glyph(self, character: char) -> Result<Glyph> {
        STENCIL
            .iter()
            .find(|(held, _, _)| *held == character)
            .map(|(_, bands, extra)| Glyph {
                bands,
                extra: *extra,
            })
            .ok_or(RenderStatus::NoSuchGlyph)
    }

    /// Every character this face sets, in the order it was built.
    #[must_use]
    pub fn repertoire(self) -> Vec<char> {
        STENCIL.iter().map(|(held, _, _)| *held).collect()
    }

    /// How wide a run is, in ems.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::NoSuchGlyph`], [`RenderStatus::TextTooLong`] past
    /// [`MAX_TEXT`], and [`RenderStatus::Time`] wrapping an overflow.
    pub fn measure(self, text: &str) -> Result<Rational> {
        let count = self.count(text)?;
        Rational::new(
            count
                .checked_mul(ADVANCE)
                .ok_or(RenderStatus::TextTooLong)?,
            GRID,
        )
        .map_err(RenderStatus::Time)
    }

    /// How wide a run's *ink* is, in ems.
    ///
    /// Narrower than [`Face::measure`] by one glyph's trailing side bearing:
    /// the pen advances past the last letter and nobody sees the space it
    /// moved through. Centring on the advance leaves a line visibly left of
    /// centre, by half a side bearing, on every caption — small enough to look
    /// like a rounding and large enough to see.
    ///
    /// It is the *box*, not the outline: a caption ending in a full stop has
    /// its own ink stopping short of the box, and chasing that would make
    /// centring depend on which letters a line happens to end with.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::NoSuchGlyph`], [`RenderStatus::TextTooLong`], and
    /// [`RenderStatus::Time`] wrapping an overflow.
    pub fn inked(self, text: &str) -> Result<Rational> {
        let count = self.count(text)?;
        if count == 0 {
            return Ok(Rational::ZERO);
        }
        Rational::new(
            (count - 1)
                .checked_mul(ADVANCE)
                .and_then(|held| held.checked_add(BOX_WIDTH))
                .ok_or(RenderStatus::TextTooLong)?,
            GRID,
        )
        .map_err(RenderStatus::Time)
    }

    /// How many characters a run sets, refusing one this face cannot.
    fn count(self, text: &str) -> Result<i64> {
        let mut count = 0_i64;
        for character in text.chars() {
            self.glyph(character)?;
            count += 1;
            if count > i64::try_from(MAX_TEXT).unwrap_or(i64::MAX) {
                return Err(RenderStatus::TextTooLong);
            }
        }
        Ok(count)
    }
}

/// A run of text placed on a picture, ready to be measured pixel by pixel.
#[derive(Clone, Debug)]
pub struct Run {
    pieces: Vec<Shape>,
    bounds: Vec<(i64, i64, i64, i64)>,
}

impl Run {
    /// How many convex pieces the whole run came to.
    #[must_use]
    pub fn len(&self) -> usize {
        self.pieces.len()
    }

    /// Whether this run draws nothing.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.pieces.is_empty()
    }

    /// The exact area of one pixel covered by this run.
    ///
    /// A sum over the pieces, which is the run's true coverage because the
    /// pieces do not overlap — within a glyph by how the face is authored,
    /// and between glyphs because the pen advances further than a glyph is
    /// wide.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an overflow.
    pub fn coverage(&self, x: i64, y: i64) -> Result<Rational> {
        let mut total = Rational::ZERO;
        for (piece, bounds) in self.pieces.iter().zip(&self.bounds) {
            // A run of forty characters is two hundred pieces and a pixel
            // touches at most a handful. Clipping a square against every one
            // of them would be correct and would make drawing a caption cost
            // the length of the caption at every pixel of the frame.
            let (left, right, top, floor) = *bounds;
            if x < left || x > right || y < top || y > floor {
                continue;
            }
            total = total
                .checked_add(piece.coverage(x, y)?)
                .map_err(RenderStatus::Time)?;
        }
        Ok(total)
    }

    /// This run's coverage over a whole picture, one byte per pixel.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::OutsideDomain`] for a size past
    /// [`shape::MAX_EXTENT`] or with no pixels in it,
    /// [`RenderStatus::OutOfMemory`], and — the one worth naming —
    /// [`RenderStatus::OutsideDomain`] again from [`shape::quantise`] if a
    /// pixel is covered *more than once*, which is a face whose pieces
    /// overlap.
    pub fn plane(&self, width: usize, height: usize) -> Result<Vec<u8>> {
        self.plane_rows(width, height, 0, height)
    }

    /// One row of the coverage, placed against the whole picture.
    ///
    /// A run is laid out against the frame it belongs to — the em is a
    /// fraction of the height and the pen starts at a fraction of the width —
    /// so type drawn into a frame one row high would be a different size in a
    /// different place. Placed against the full extent, one row rasterised,
    /// which is what a mask and a wipe do and for the same reason.
    ///
    /// Exact, because coverage is a function of position: a glyph's pieces are
    /// convex and the area of one against a pixel needs nothing from the row
    /// above it.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::OutsideDomain`] for an empty or oversized extent, or a
    /// row past the bottom.
    pub fn plane_row(&self, width: usize, height: usize, row: usize) -> Result<Vec<u8>> {
        if row >= height {
            return Err(RenderStatus::OutsideDomain);
        }
        self.plane_rows(width, height, row, row + 1)
    }

    /// A run of rows, placed against the whole picture.
    ///
    /// One rasteriser for both forms, so a row and a frame cannot disagree
    /// about where a letter falls.
    fn plane_rows(&self, width: usize, height: usize, from: usize, to: usize) -> Result<Vec<u8>> {
        if width == 0 || height == 0 || width > shape::MAX_EXTENT || height > shape::MAX_EXTENT {
            return Err(RenderStatus::OutsideDomain);
        }
        let mut out = Vec::new();
        out.try_reserve(
            width
                .checked_mul(to.saturating_sub(from))
                .ok_or(RenderStatus::OutsideDomain)?,
        )
        .map_err(|_| RenderStatus::OutOfMemory)?;
        for y in from..to {
            for x in 0..width {
                let row = i64::try_from(y).map_err(|_| RenderStatus::OutsideDomain)?;
                let column = i64::try_from(x).map_err(|_| RenderStatus::OutsideDomain)?;
                out.push(shape::quantise(self.coverage(column, row)?)?);
            }
        }
        Ok(out)
    }
}

/// Place a run of text, in pixels.
///
/// `size` is the em, in pixels. `origin` is where the pen starts: the left of
/// the first glyph, on the **cap line** — the top of a capital letter rather
/// than the baseline, because every glyph in this face is drawn from the cap
/// line down and a baseline would be a second number to keep true.
///
/// # Errors
///
/// [`RenderStatus::SizeNotPositive`], [`RenderStatus::NoSuchGlyph`],
/// [`RenderStatus::TextTooLong`], and whatever the rasteriser refuses.
pub fn place(face: Face, text: &str, size: Rational, origin: (Rational, Rational)) -> Result<Run> {
    if !size.is_positive() {
        return Err(RenderStatus::SizeNotPositive);
    }
    face.count(text)?;
    let step = Rational::new(ADVANCE, GRID)
        .map_err(RenderStatus::Time)?
        .checked_mul(size)
        .map_err(RenderStatus::Time)?;
    let mut pieces = Vec::new();
    let mut bounds = Vec::new();
    let mut pen = origin.0;
    for character in text.chars() {
        for piece in face.glyph(character)?.pieces()? {
            let mut corners = Vec::new();
            corners
                .try_reserve(piece.len())
                .map_err(|_| RenderStatus::OutOfMemory)?;
            for (x, y) in &piece {
                corners.push((
                    pen.checked_add(x.checked_mul(size).map_err(RenderStatus::Time)?)
                        .map_err(RenderStatus::Time)?,
                    origin
                        .1
                        .checked_add(y.checked_mul(size).map_err(RenderStatus::Time)?)
                        .map_err(RenderStatus::Time)?,
                ));
            }
            bounds.push(box_of(&corners));
            pieces.push(shape::convex(&corners)?);
        }
        pen = pen.checked_add(step).map_err(RenderStatus::Time)?;
    }
    Ok(Run { pieces, bounds })
}

/// The pixels a polygon can touch: left, right, top, bottom, inclusive.
///
/// Tight rather than generous, and that took a control to notice. The obvious
/// bound rounds the far edge *up*, which is one pixel too many: a piece ending
/// at 6.875 has its last ink in pixel 6, and one ending at exactly 7.0 also
/// has its last ink in pixel 6, because pixel 7 is the square from 7 to 8. So
/// the far edge is the last pixel *strictly before* the edge. A control that
/// took one off the generous version changed no answer, which is how the slack
/// was found.
fn box_of(corners: &[Point]) -> (i64, i64, i64, i64) {
    let mut left = corners[0].0;
    let mut right = left;
    let mut top = corners[0].1;
    let mut floor = top;
    for (x, y) in corners {
        if *x < left {
            left = *x;
        }
        if *x > right {
            right = *x;
        }
        if *y < top {
            top = *y;
        }
        if *y > floor {
            floor = *y;
        }
    }
    (
        floor_of(left),
        ceil_of(right) - 1,
        floor_of(top),
        ceil_of(floor) - 1,
    )
}

/// The pixel column or row a coordinate falls in.
fn floor_of(value: Rational) -> i64 {
    value.numerator().div_euclid(value.denominator())
}

/// The pixel a coordinate falls in, rounded the other way.
fn ceil_of(value: Rational) -> i64 {
    let numerator = value.numerator();
    let denominator = value.denominator();
    if numerator.rem_euclid(denominator) == 0 {
        numerator.div_euclid(denominator)
    } else {
        numerator.div_euclid(denominator) + 1
    }
}

/// Points in grid space, as a piece in em units.
fn scaled(points: &[(Rational, Rational)]) -> Result<Vec<Point>> {
    let grid = whole(GRID)?;
    let mut out = Vec::new();
    out.try_reserve(points.len())
        .map_err(|_| RenderStatus::OutOfMemory)?;
    for (x, y) in points {
        out.push((
            x.checked_div(grid).map_err(RenderStatus::Time)?,
            y.checked_div(grid).map_err(RenderStatus::Time)?,
        ));
    }
    Ok(out)
}

/// A bar filling the gap between two slanted strokes, exactly.
///
/// The bar's own sides are the strokes' facing edges, evaluated at the bar's
/// top and bottom. That is the whole reason a stroke here is a shape with two
/// evaluable edges rather than a list of corners: a crossbar authored by hand
/// would either leave a hairline or overlap, and one of those is invisible
/// until somebody renders the letter large.
fn between(left: Band, right: Band, y0: i64, y1: i64) -> Result<Vec<Point>> {
    let top = whole(y0)?;
    let bottom = whole(y1)?;
    scaled(&[
        (left.right_at(top)?, top),
        (right.left_at(top)?, top),
        (right.left_at(bottom)?, bottom),
        (left.right_at(bottom)?, bottom),
    ])
}

/// A triangle, in half-units.
fn triangle(corners: [(i64, i64); 3]) -> Result<Vec<Point>> {
    let mut points = Vec::new();
    points
        .try_reserve(3)
        .map_err(|_| RenderStatus::OutOfMemory)?;
    for (x, y) in corners {
        points.push((whole(x)?, whole(y)?));
    }
    scaled(&points)
}

/// Every glyph this face sets: its character, its strokes, and whatever it has
/// that is not a stroke.
///
/// A `static`, and that is the point. Written as a table built inside a
/// function, this was the largest single item in the whole program — 23,807
/// bytes, more than `Edit::apply` — because a coordinate in a function body is
/// an *instruction that stores a coordinate*, and there are some two thousand
/// of them here. As data it is data.
///
/// Every coordinate is an integer on the half-unit grid, so the face is exact
/// rational data rather than a table of rounded decimals. The one place a
/// coordinate is *not* an integer is an `A`'s crossbar, which is computed from
/// the two legs it meets rather than measured off a drawing — see [`Extra`].
static STENCIL: &[(char, &[Band], Extra)] = &[
    (' ', &[], Extra::None),
    (
        'B',
        &[
            rect(0, 2, 0, 16),
            rect(2, 8, 0, 2),
            rect(2, 8, 7, 9),
            rect(2, 8, 14, 16),
            rect(8, 10, 2, 7),
            rect(8, 10, 9, 14),
        ],
        Extra::None,
    ),
    (
        'C',
        &[
            rect(0, 2, 0, 16),
            rect(2, 8, 0, 2),
            rect(2, 8, 14, 16),
            rect(8, 10, 0, 4),
            rect(8, 10, 12, 16),
        ],
        Extra::None,
    ),
    (
        'D',
        &[
            rect(0, 2, 0, 16),
            rect(2, 8, 0, 2),
            rect(2, 8, 14, 16),
            rect(8, 10, 2, 14),
        ],
        Extra::None,
    ),
    (
        'E',
        &[
            rect(0, 2, 0, 16),
            rect(2, 10, 0, 2),
            rect(2, 8, 7, 9),
            rect(2, 10, 14, 16),
        ],
        Extra::None,
    ),
    (
        'F',
        &[rect(0, 2, 0, 16), rect(2, 10, 0, 2), rect(2, 8, 7, 9)],
        Extra::None,
    ),
    (
        'G',
        &[
            rect(0, 2, 0, 16),
            rect(2, 8, 0, 2),
            rect(8, 10, 0, 4),
            rect(5, 8, 8, 10),
            rect(8, 10, 8, 14),
            rect(2, 10, 14, 16),
        ],
        Extra::None,
    ),
    (
        'H',
        &[rect(0, 2, 0, 16), rect(8, 10, 0, 16), rect(2, 8, 7, 9)],
        Extra::None,
    ),
    (
        'I',
        &[
            rect(4, 6, 0, 16),
            rect(2, 4, 0, 2),
            rect(6, 8, 0, 2),
            rect(2, 4, 14, 16),
            rect(6, 8, 14, 16),
        ],
        Extra::None,
    ),
    (
        'J',
        &[rect(6, 8, 0, 14), rect(0, 8, 14, 16), rect(0, 2, 10, 14)],
        Extra::None,
    ),
    (
        'K',
        &[
            rect(0, 2, 0, 16),
            leg((8, 10), (2, 4), 0, 8),
            leg((2, 4), (8, 10), 8, 16),
        ],
        Extra::None,
    ),
    ('L', &[rect(0, 2, 0, 16), rect(2, 10, 14, 16)], Extra::None),
    (
        'M',
        &[
            rect(0, 2, 0, 16),
            rect(8, 10, 0, 16),
            leg((2, 4), (4, 5), 0, 9),
            leg((6, 8), (5, 6), 0, 9),
        ],
        Extra::None,
    ),
    (
        'N',
        &[
            rect(0, 2, 0, 16),
            rect(8, 10, 0, 16),
            leg((2, 4), (6, 8), 0, 16),
        ],
        Extra::None,
    ),
    (
        'O',
        &[
            rect(0, 2, 0, 16),
            rect(8, 10, 0, 16),
            rect(2, 8, 0, 2),
            rect(2, 8, 14, 16),
        ],
        Extra::None,
    ),
    (
        'P',
        &[
            rect(0, 2, 0, 16),
            rect(2, 8, 0, 2),
            rect(8, 10, 2, 7),
            rect(2, 8, 7, 9),
        ],
        Extra::None,
    ),
    (
        'Q',
        &[
            rect(0, 2, 0, 16),
            rect(8, 10, 0, 16),
            rect(2, 8, 0, 2),
            rect(2, 8, 14, 16),
            leg((5, 7), (6, 8), 10, 14),
        ],
        Extra::None,
    ),
    (
        'R',
        &[
            rect(0, 2, 0, 16),
            rect(2, 8, 0, 2),
            rect(8, 10, 2, 7),
            rect(2, 8, 7, 9),
            leg((4, 6), (8, 10), 9, 16),
        ],
        Extra::None,
    ),
    (
        'S',
        &[
            rect(0, 2, 0, 7),
            rect(2, 10, 0, 2),
            rect(0, 8, 7, 9),
            rect(8, 10, 9, 14),
            rect(0, 10, 14, 16),
        ],
        Extra::None,
    ),
    ('T', &[rect(0, 10, 0, 2), rect(4, 6, 2, 16)], Extra::None),
    (
        'U',
        &[rect(0, 2, 0, 14), rect(8, 10, 0, 14), rect(0, 10, 14, 16)],
        Extra::None,
    ),
    (
        'V',
        &[leg((0, 2), (4, 5), 0, 16), leg((8, 10), (5, 6), 0, 16)],
        Extra::None,
    ),
    (
        'W',
        &[
            leg((0, 2), (2, 3), 0, 16),
            leg((4, 5), (3, 4), 0, 16),
            leg((5, 6), (6, 7), 0, 16),
            leg((8, 10), (7, 8), 0, 16),
        ],
        Extra::None,
    ),
    (
        'X',
        &[
            leg((0, 2), (4, 5), 0, 8),
            leg((8, 10), (5, 6), 0, 8),
            leg((4, 5), (0, 2), 8, 16),
            leg((5, 6), (8, 10), 8, 16),
        ],
        Extra::None,
    ),
    (
        'Y',
        &[
            leg((0, 2), (4, 5), 0, 8),
            leg((8, 10), (5, 6), 0, 8),
            rect(4, 6, 8, 16),
        ],
        Extra::None,
    ),
    (
        'Z',
        &[
            rect(0, 10, 0, 2),
            leg((6, 8), (2, 4), 2, 14),
            rect(0, 10, 14, 16),
        ],
        Extra::None,
    ),
    (
        '0',
        &[
            rect(0, 2, 0, 16),
            rect(8, 10, 0, 16),
            rect(2, 8, 0, 2),
            rect(2, 8, 14, 16),
            leg((5, 7), (3, 5), 3, 13),
        ],
        Extra::None,
    ),
    (
        '2',
        &[
            rect(0, 2, 2, 5),
            rect(2, 8, 0, 2),
            rect(8, 10, 2, 7),
            leg((8, 10), (0, 2), 7, 14),
            rect(0, 10, 14, 16),
        ],
        Extra::None,
    ),
    (
        '3',
        &[
            rect(2, 10, 0, 2),
            rect(8, 10, 2, 14),
            rect(3, 8, 7, 9),
            rect(2, 10, 14, 16),
            rect(0, 2, 2, 5),
            rect(0, 2, 11, 14),
        ],
        Extra::None,
    ),
    (
        '4',
        &[
            rect(6, 8, 0, 16),
            leg((4, 6), (0, 2), 0, 11),
            rect(0, 6, 11, 13),
            rect(8, 10, 11, 13),
        ],
        Extra::None,
    ),
    (
        '5',
        &[
            rect(0, 10, 0, 2),
            rect(0, 2, 2, 7),
            rect(0, 8, 7, 9),
            rect(8, 10, 9, 14),
            rect(0, 10, 14, 16),
            rect(0, 2, 11, 14),
        ],
        Extra::None,
    ),
    (
        '6',
        &[
            rect(0, 2, 0, 16),
            rect(2, 8, 0, 2),
            rect(8, 10, 0, 4),
            rect(2, 8, 7, 9),
            rect(8, 10, 7, 14),
            rect(2, 10, 14, 16),
        ],
        Extra::None,
    ),
    (
        '7',
        &[rect(0, 10, 0, 2), leg((6, 8), (2, 4), 2, 16)],
        Extra::None,
    ),
    (
        '8',
        &[
            rect(0, 2, 0, 16),
            rect(8, 10, 0, 16),
            rect(2, 8, 0, 2),
            rect(2, 8, 7, 9),
            rect(2, 8, 14, 16),
        ],
        Extra::None,
    ),
    (
        '9',
        &[
            rect(8, 10, 0, 16),
            rect(0, 2, 0, 9),
            rect(2, 8, 0, 2),
            rect(2, 8, 7, 9),
            rect(2, 8, 14, 16),
        ],
        Extra::None,
    ),
    ('.', &[rect(4, 6, 13, 16)], Extra::None),
    ('-', &[rect(2, 8, 7, 9)], Extra::None),
    (':', &[rect(4, 6, 4, 7), rect(4, 6, 11, 14)], Extra::None),
    ('/', &[leg((6, 8), (2, 4), 0, 16)], Extra::None),
    (
        '(',
        &[
            leg((5, 7), (2, 4), 0, 5),
            rect(2, 4, 5, 11),
            leg((2, 4), (5, 7), 11, 16),
        ],
        Extra::None,
    ),
    (
        ')',
        &[
            leg((3, 5), (6, 8), 0, 5),
            rect(6, 8, 5, 11),
            leg((6, 8), (3, 5), 11, 16),
        ],
        Extra::None,
    ),
    // Lowercase. Three heights rather than one, which is the whole of
    // why it was not simply more of the same work: a capital runs from
    // the cap line to the baseline and that is all it does, while
    // these sit on an x-height at six, reach up to the cap line, and
    // hang below the baseline to twenty-one.
    (
        'a',
        &[
            rect(0, 2, 8, 16),
            rect(8, 10, 6, 16),
            rect(2, 8, 6, 8),
            rect(2, 8, 14, 16),
        ],
        Extra::None,
    ),
    (
        'b',
        &[
            rect(0, 2, 0, 16),
            rect(2, 8, 6, 8),
            rect(8, 10, 8, 14),
            rect(2, 8, 14, 16),
        ],
        Extra::None,
    ),
    (
        'c',
        &[rect(0, 2, 6, 16), rect(2, 8, 6, 8), rect(2, 8, 14, 16)],
        Extra::None,
    ),
    (
        'd',
        &[
            rect(8, 10, 0, 16),
            rect(2, 8, 6, 8),
            rect(0, 2, 8, 14),
            rect(2, 8, 14, 16),
        ],
        Extra::None,
    ),
    (
        'e',
        &[
            rect(0, 2, 6, 16),
            rect(2, 8, 6, 8),
            rect(8, 10, 8, 10),
            rect(2, 10, 10, 12),
            rect(2, 8, 14, 16),
        ],
        Extra::None,
    ),
    (
        'f',
        &[
            rect(4, 6, 0, 16),
            rect(6, 10, 0, 2),
            rect(2, 4, 6, 8),
            rect(6, 8, 6, 8),
        ],
        Extra::None,
    ),
    (
        'g',
        &[
            rect(0, 2, 6, 12),
            rect(2, 8, 6, 8),
            rect(8, 10, 6, 19),
            rect(2, 8, 10, 12),
            rect(0, 10, 19, 21),
            rect(0, 2, 17, 19),
        ],
        Extra::None,
    ),
    (
        'h',
        &[rect(0, 2, 0, 16), rect(2, 8, 6, 8), rect(8, 10, 8, 16)],
        Extra::None,
    ),
    ('i', &[rect(4, 6, 6, 16), rect(4, 6, 2, 4)], Extra::None),
    (
        'j',
        &[rect(4, 6, 2, 4), rect(4, 6, 6, 19), rect(0, 6, 19, 21)],
        Extra::None,
    ),
    (
        'k',
        &[
            rect(0, 2, 0, 16),
            leg((8, 10), (2, 4), 6, 11),
            leg((2, 4), (8, 10), 11, 16),
        ],
        Extra::None,
    ),
    ('l', &[rect(4, 6, 0, 16), rect(6, 8, 14, 16)], Extra::None),
    (
        'm',
        &[
            rect(0, 2, 6, 16),
            rect(2, 8, 6, 8),
            rect(4, 6, 8, 16),
            rect(8, 10, 8, 16),
        ],
        Extra::None,
    ),
    (
        'n',
        &[rect(0, 2, 6, 16), rect(2, 8, 6, 8), rect(8, 10, 8, 16)],
        Extra::None,
    ),
    (
        'o',
        &[
            rect(0, 2, 6, 16),
            rect(8, 10, 6, 16),
            rect(2, 8, 6, 8),
            rect(2, 8, 14, 16),
        ],
        Extra::None,
    ),
    (
        'p',
        &[
            rect(0, 2, 6, 21),
            rect(2, 8, 6, 8),
            rect(8, 10, 8, 12),
            rect(2, 8, 12, 14),
        ],
        Extra::None,
    ),
    (
        'q',
        &[
            rect(8, 10, 6, 21),
            rect(2, 8, 6, 8),
            rect(0, 2, 8, 12),
            rect(2, 8, 12, 14),
        ],
        Extra::None,
    ),
    (
        'r',
        &[rect(0, 2, 6, 16), rect(2, 8, 6, 8), rect(8, 10, 8, 10)],
        Extra::None,
    ),
    (
        's',
        &[
            rect(0, 2, 6, 10),
            rect(2, 10, 6, 8),
            rect(0, 10, 10, 12),
            rect(8, 10, 12, 14),
            rect(0, 10, 14, 16),
        ],
        Extra::None,
    ),
    (
        't',
        &[
            rect(4, 6, 2, 16),
            rect(2, 4, 6, 8),
            rect(6, 8, 6, 8),
            rect(6, 8, 14, 16),
        ],
        Extra::None,
    ),
    (
        'u',
        &[rect(0, 2, 6, 14), rect(8, 10, 6, 14), rect(0, 10, 14, 16)],
        Extra::None,
    ),
    (
        'v',
        &[leg((0, 2), (4, 5), 6, 16), leg((8, 10), (5, 6), 6, 16)],
        Extra::None,
    ),
    (
        'w',
        &[
            leg((0, 2), (2, 3), 6, 16),
            leg((4, 5), (3, 4), 6, 16),
            leg((5, 6), (6, 7), 6, 16),
            leg((8, 10), (7, 8), 6, 16),
        ],
        Extra::None,
    ),
    (
        'x',
        &[
            leg((0, 2), (4, 5), 6, 11),
            leg((8, 10), (5, 6), 6, 11),
            leg((4, 5), (0, 2), 11, 16),
            leg((5, 6), (8, 10), 11, 16),
        ],
        Extra::None,
    ),
    (
        'y',
        &[
            leg((0, 2), (4, 5), 6, 13),
            leg((8, 10), (5, 6), 6, 13),
            rect(4, 6, 13, 19),
            rect(0, 6, 19, 21),
        ],
        Extra::None,
    ),
    (
        'z',
        &[
            rect(0, 10, 6, 8),
            leg((6, 8), (2, 4), 8, 14),
            rect(0, 10, 14, 16),
        ],
        Extra::None,
    ),
    (
        'A',
        &[leg((4, 5), (0, 2), 0, 16), leg((5, 6), (8, 10), 0, 16)],
        Extra::Apex,
    ),
    (
        '1',
        &[rect(4, 6, 0, 16), rect(2, 4, 14, 16), rect(6, 8, 14, 16)],
        Extra::Flag,
    ),
    (
        ',',
        &[rect(4, 6, 13, 16), leg((4, 6), (2, 4), 16, 19)],
        Extra::None,
    ),
];

/// The smallest em, in pixels, this will set a caption at.
///
/// A stroke here is an eighth of the em, so nine pixels puts it just over one
/// — the point at which a letter has a stroke rather than a shadow of one.
/// Below four it is half a pixel and the whole face is grey, which a test
/// measures; nine is comfortably clear of that and still small enough to set a
/// digest across a proxy.
///
/// A slate whose caption is a grey smear has told the viewer something false
/// about how much it knows, so below this a caption is **not drawn** rather
/// than drawn illegibly.
pub const MIN_CAPTION_EM: i64 = 9;

/// The largest em, as a fraction of the frame's height.
///
/// Without it a short caption on a wide frame becomes a billboard: eight
/// characters of a digest fitted across 4K would be six hundred pixels tall.
const MAX_CAPTION_HEIGHT: (i64, i64) = (1, 6);

/// How much of the width a caption leaves as margin, either side.
const MARGIN: (i64, i64) = (1, 16);

/// Where the cap line sits, as a fraction of the height.
const CAP_LINE: (i64, i64) = (5, 8);

/// A caption placed across a frame, or nothing if it cannot be read there.
///
/// Centred, on a cap line five eighths of the way down, at the largest size
/// that fits the width with a margin and stays under an eighth of the height.
/// `Ok(None)` means the frame is too small to set this legibly — which is a
/// picture's answer, not a failure.
///
/// # Errors
///
/// [`RenderStatus::NoSuchGlyph`], [`RenderStatus::TextTooLong`],
/// [`RenderStatus::OutsideDomain`] for a size past the rasteriser's extent,
/// and [`RenderStatus::Time`] wrapping an overflow.
pub fn caption(text: &str, width: usize, height: usize) -> Result<Option<Run>> {
    let face = Face::stencil();
    let across = i64::try_from(width).map_err(|_| RenderStatus::OutsideDomain)?;
    let down = i64::try_from(height).map_err(|_| RenderStatus::OutsideDomain)?;
    let measured = face.inked(text)?;
    if measured.is_zero() {
        // A caption of nothing. There is no size at which that draws anything,
        // and dividing by its width would be worse than saying so. A caption
        // of *spaces* is a different case and goes through: it has a width, it
        // is placed, and it draws nothing because a space has no pieces —
        // which is the same answer arrived at without a special case.
        return Ok(None);
    }
    let usable = whole(across)?
        .checked_mul(Rational::new(MARGIN.1 - 2 * MARGIN.0, MARGIN.1).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)?;
    let fitting = usable.checked_div(measured).map_err(RenderStatus::Time)?;
    let ceiling = whole(down)?
        .checked_mul(
            Rational::new(MAX_CAPTION_HEIGHT.0, MAX_CAPTION_HEIGHT.1)
                .map_err(RenderStatus::Time)?,
        )
        .map_err(RenderStatus::Time)?;
    let size = if fitting < ceiling { fitting } else { ceiling };
    if size < whole(MIN_CAPTION_EM)? {
        return Ok(None);
    }
    let half = Rational::new(1, 2).map_err(RenderStatus::Time)?;
    let middle = whole(across)?
        .checked_mul(half)
        .map_err(RenderStatus::Time)?;
    let baseline = whole(down)?
        .checked_mul(Rational::new(CAP_LINE.0, CAP_LINE.1).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)?;
    // The cap line sits an em above where the baseline goes, so the caption's
    // *centre* is half an em above that -- which is what `centred` wants.
    let centre = baseline
        .checked_sub(size.checked_mul(half).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)?;
    centred(face, text, size, (middle, centre)).map(Some)
}

/// A line of type with its middle at a point, in pixels.
///
/// The middle rather than the corner, because that is what anybody placing a
/// line actually chooses: a title an eighth from the top and centred across is
/// two fractions of the frame, and working out where its top-left corner lands
/// is arithmetic nobody should have to do to say that.
///
/// Centred on the **ink** — see [`Face::inked`] — and on the em vertically,
/// which is cap line to baseline. Descenders hang below it and the comma is
/// the only glyph here that has one.
///
/// # Errors
///
/// [`RenderStatus::SizeNotPositive`], [`RenderStatus::NoSuchGlyph`],
/// [`RenderStatus::TextTooLong`], and whatever the rasteriser refuses.
pub fn centred(
    face: Face,
    text: &str,
    size: Rational,
    centre: (Rational, Rational),
) -> Result<Run> {
    if !size.is_positive() {
        return Err(RenderStatus::SizeNotPositive);
    }
    let half = Rational::new(1, 2).map_err(RenderStatus::Time)?;
    let run_width = face
        .inked(text)?
        .checked_mul(size)
        .map_err(RenderStatus::Time)?;
    let left = centre
        .0
        .checked_sub(run_width.checked_mul(half).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)?;
    let cap = centre
        .1
        .checked_sub(size.checked_mul(half).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)?;
    place(face, text, size, (left, cap))
}

/// How the lines of a block sit against one another.
///
/// Named here as well as in the model, for the reason [`crate::resample`]
/// names its filters twice: the model owns the decision, because which
/// alignment somebody chose is part of their project, and the renderer owns
/// what the decision *means*. They are siblings and neither may depend on the
/// other.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Alignment {
    /// Every line starts at the block's left edge.
    Left,
    /// Every line is centred on the block's middle.
    Centre,
    /// Every line ends at the block's right edge.
    Right,
}

impl Run {
    /// Take another run's pieces into this one.
    ///
    /// Only correct because the runs do not overlap, which for the lines of a
    /// block is [`LINE_SPACING`] being wider than the em plus the descender —
    /// asserted by a test rather than assumed here, because if it were ever
    /// false the coverage of a `g` and the `A` below it would sum past full
    /// and the whole card would be *refused* rather than drawn heavy.
    fn absorb(&mut self, other: Self) -> Result<()> {
        self.pieces
            .try_reserve(other.pieces.len())
            .map_err(|_| RenderStatus::OutOfMemory)?;
        self.bounds
            .try_reserve(other.bounds.len())
            .map_err(|_| RenderStatus::OutOfMemory)?;
        self.pieces.extend(other.pieces);
        self.bounds.extend(other.bounds);
        Ok(())
    }
}

/// A block of type: several lines, aligned against one another.
///
/// The block's *centre* goes at `centre`, which is what makes moving a card
/// and re-aligning it two different gestures. Its width is the widest line's
/// and its height runs from the first line's cap to the last line's baseline —
/// so a two-line card straddles the point it was placed at rather than hanging
/// below it.
///
/// # Errors
///
/// [`RenderStatus::SizeNotPositive`], [`RenderStatus::NoSuchGlyph`],
/// [`RenderStatus::TextTooLong`], and whatever the rasteriser refuses.
pub fn block(
    face: Face,
    lines: &[&str],
    size: Rational,
    centre: (Rational, Rational),
    alignment: Alignment,
) -> Result<Run> {
    if !size.is_positive() {
        return Err(RenderStatus::SizeNotPositive);
    }
    let half = Rational::new(1, 2).map_err(RenderStatus::Time)?;
    let step = Rational::new(LINE_SPACING, GRID)
        .map_err(RenderStatus::Time)?
        .checked_mul(size)
        .map_err(RenderStatus::Time)?;

    let mut widest = Rational::ZERO;
    for line in lines {
        let width = face
            .inked(line)?
            .checked_mul(size)
            .map_err(RenderStatus::Time)?;
        if width > widest {
            widest = width;
        }
    }
    // The block runs from the first line's cap to the last line's baseline:
    // one em, plus a step for each line after the first. The descender of the
    // last line hangs below that, deliberately -- a card of "PHIP" and a card
    // of "Phip" sit in the same place, which they would not if the block's
    // height depended on which letters happened to be in it.
    let count = i64::try_from(lines.len().max(1)).map_err(|_| RenderStatus::TextTooLong)?;
    let tall = step
        .checked_mul(whole(count - 1)?)
        .map_err(RenderStatus::Time)?
        .checked_add(size)
        .map_err(RenderStatus::Time)?;
    let left = centre
        .0
        .checked_sub(widest.checked_mul(half).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)?;
    let mut cap = centre
        .1
        .checked_sub(tall.checked_mul(half).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)?;

    let mut run = Run {
        pieces: Vec::new(),
        bounds: Vec::new(),
    };
    for line in lines {
        let width = face
            .inked(line)?
            .checked_mul(size)
            .map_err(RenderStatus::Time)?;
        let start = match alignment {
            Alignment::Left => left,
            Alignment::Centre => left
                .checked_add(
                    widest
                        .checked_sub(width)
                        .map_err(RenderStatus::Time)?
                        .checked_mul(half)
                        .map_err(RenderStatus::Time)?,
                )
                .map_err(RenderStatus::Time)?,
            Alignment::Right => left
                .checked_add(widest.checked_sub(width).map_err(RenderStatus::Time)?)
                .map_err(RenderStatus::Time)?,
        };
        run.absorb(place(face, line, size, (start, cap))?)?;
        cap = cap.checked_add(step).map_err(RenderStatus::Time)?;
    }
    Ok(run)
}

/// A title: lines somebody chose the size, the place and the alignment of.
///
/// Unlike [`caption`] there is no floor and no cap. A caption's size is chosen
/// *for* the reader, so it has to refuse to be illegible; a title's size is
/// the editor's own decision, and a program that quietly declined to draw
/// somebody's title because it judged it too small would be worse than one
/// that drew it.
///
/// `size` is the em as a fraction of the frame's **height**, and `across` and
/// `down` place the middle of the line as fractions of the width and height.
/// Fractions rather than pixels, so a title laid out on a proxy is the same
/// title on the finish.
///
/// # Errors
///
/// [`RenderStatus::SizeNotPositive`], [`RenderStatus::NoSuchGlyph`],
/// [`RenderStatus::TextTooLong`], [`RenderStatus::OutsideDomain`] for a frame
/// past the rasteriser's extent, and [`RenderStatus::Time`] wrapping an
/// overflow.
pub fn title(
    lines: &[&str],
    size: Rational,
    across: Rational,
    down: Rational,
    alignment: Alignment,
    width: usize,
    height: usize,
) -> Result<Run> {
    let face = Face::stencil();
    let columns = whole(i64::try_from(width).map_err(|_| RenderStatus::OutsideDomain)?)?;
    let rows = whole(i64::try_from(height).map_err(|_| RenderStatus::OutsideDomain)?)?;
    let em = rows.checked_mul(size).map_err(RenderStatus::Time)?;
    block(
        face,
        lines,
        em,
        (
            columns.checked_mul(across).map_err(RenderStatus::Time)?,
            rows.checked_mul(down).map_err(RenderStatus::Time)?,
        ),
        alignment,
    )
}
