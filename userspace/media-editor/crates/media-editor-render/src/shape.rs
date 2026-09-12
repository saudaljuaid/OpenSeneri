// SPDX-License-Identifier: GPL-3.0-only
//! Shapes, and the exact area of a pixel inside one.
//!
//! M4.5 recorded the reason wipes were not built: *a wipe needs a shape and a
//! shape needs a rasteriser*. This is that rasteriser, and it is the same
//! primitive a mask needs, so it is built once.
//!
//! ## Coverage is an area, not a sample
//!
//! The usual rasteriser asks "is the pixel's centre inside the shape" and gets
//! a jagged edge, or asks it at sixteen sub-positions and gets sixteen
//! possible answers instead of two hundred and fifty-six. This one computes
//! the **exact area** of the pixel square that lies inside the shape, as a
//! rational, and quantises once at the very end.
//!
//! That is worth the arithmetic for a reason that shows up immediately: a wipe
//! is a straight line crossing a whole frame over a second, so the *same* edge
//! is drawn at a thousand slightly different offsets. Sampled coverage makes
//! the edge crawl — each pixel flips between two values at a different moment
//! — while exact area makes it slide, because the area under a line is a
//! continuous function of where the line is and a sample is not.
//!
//! ## Two implementations, one of them to be checked against
//!
//! A convex shape is an intersection of half-planes, and coverage is computed
//! by clipping the pixel square against each in turn and taking the shoelace
//! area of what is left. That is general: one edge is a wipe, four are a
//! rectangle, many are a mask.
//!
//! There is also a closed form for the one-edge case — the area of a unit
//! square under a line, in three lines of arithmetic — and it is *not* what
//! the rasteriser calls. It exists to be compared against, pixel for pixel,
//! over a whole frame at several angles. Two implementations that share no
//! code and agree everywhere is a much stronger statement than one
//! implementation and a test that agrees with it, which is the same argument
//! [`crate::lut`] makes for keeping trilinear interpolation alive.
//!
//! ## What it refuses
//!
//! Every coordinate is an exact rational and every clip is exact, which means
//! denominators grow: an intersection point is a ratio of products of the
//! inputs, and the shoelace area multiplies those together again. A shape
//! whose arithmetic will not fit in a rational is **refused by name** rather
//! than quietly evaluated in floating point. There is no floating point here
//! to fall back to, which is the point.

use alloc::vec::Vec;

use media_editor_core::Rational;

use crate::status::{RenderStatus, Result};

/// The most edges one shape may have.
///
/// A wipe is one, a rectangle four, a rounded corner as many as it is worth
/// approximating with. Sixty-four is past anything an interface offers and is
/// a bound a hostile project file cannot talk its way past (R-11.2).
pub const MAX_EDGES: usize = 64;

/// The largest picture this will rasterise a shape over, on either axis.
pub const MAX_EXTENT: usize = 16_384;

/// Full coverage, as a byte.
pub const FULL: u8 = 255;

/// One half-plane: every point where `a·x + b·y <= c`.
///
/// The inequality is not strict, so the boundary line belongs to the region.
/// **Nothing here can tell the difference**, and that was established by
/// trying: making it strict changes no coverage anywhere, because a line has
/// no area and the clipper puts back as a crossing point exactly the corner
/// the strict test would have dropped. It is written down so that a later
/// `contains(point)` — which *can* tell — does not have to decide it again,
/// and it is recorded as a decision without a consequence rather than dressed
/// up as one with one.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Edge {
    a: Rational,
    b: Rational,
    c: Rational,
}

impl Edge {
    /// The half-plane `a·x + b·y <= c`.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::DegenerateEdge`] if `a` and `b` are both zero, which
    /// names no line: the condition is then either always true or always
    /// false, and a shape built out of one is not the shape anybody drew.
    pub fn new(a: Rational, b: Rational, c: Rational) -> Result<Self> {
        if a.is_zero() && b.is_zero() {
            return Err(RenderStatus::DegenerateEdge);
        }
        Ok(Self { a, b, c })
    }

    /// The same line, keeping the other side.
    ///
    /// `a·x + b·y <= c` becomes `-a·x - b·y <= -c`, which is `a·x + b·y >= c`.
    /// The boundary belongs to both, which is why the two coverages sum to
    /// exactly one rather than to one minus the line.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an overflow.
    pub fn complement(self) -> Result<Self> {
        Ok(Self {
            a: self.a.checked_neg().map_err(RenderStatus::Time)?,
            b: self.b.checked_neg().map_err(RenderStatus::Time)?,
            c: self.c.checked_neg().map_err(RenderStatus::Time)?,
        })
    }

    /// How far a point is outside, in the units the coefficients are in.
    ///
    /// Negative or zero is inside. This is not a distance — it is not divided
    /// by the normal's length — and nothing here needs it to be, because every
    /// use compares two of them against each other.
    fn outside(self, x: Rational, y: Rational) -> Result<Rational> {
        self.a
            .checked_mul(x)
            .map_err(RenderStatus::Time)?
            .checked_add(self.b.checked_mul(y).map_err(RenderStatus::Time)?)
            .map_err(RenderStatus::Time)?
            .checked_sub(self.c)
            .map_err(RenderStatus::Time)
    }
}

/// A convex region: everything inside all of its edges at once.
///
/// Convex because that is what a clip against a sequence of half-planes
/// produces, and because a non-convex mask is a *union* of convex ones rather
/// than a different kind of shape. Nothing here needs to know that; whatever
/// unions them does.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Shape {
    edges: Vec<Edge>,
}

impl Shape {
    /// A shape from its edges.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::ShapeTooComplex`] past [`MAX_EDGES`], and
    /// [`RenderStatus::DegenerateShape`] for no edges at all — which would be
    /// the whole plane, and is better said by not having a shape.
    pub fn new(edges: Vec<Edge>) -> Result<Self> {
        if edges.is_empty() {
            return Err(RenderStatus::DegenerateShape);
        }
        if edges.len() > MAX_EDGES {
            return Err(RenderStatus::ShapeTooComplex);
        }
        Ok(Self { edges })
    }

    /// One half-plane, which is what a wipe is.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::OutOfMemory`].
    pub fn half_plane(edge: Edge) -> Result<Self> {
        let mut edges = Vec::new();
        edges
            .try_reserve(1)
            .map_err(|_| RenderStatus::OutOfMemory)?;
        edges.push(edge);
        Ok(Self { edges })
    }

    /// An axis-aligned rectangle, from its left, top, right and bottom.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::DegenerateShape`] if it encloses nothing — a rectangle
    /// whose right is not past its left has no area, and a mask of nothing is
    /// a mistake rather than an empty picture.
    pub fn rectangle(
        left: Rational,
        top: Rational,
        right: Rational,
        bottom: Rational,
    ) -> Result<Self> {
        let wide = right.checked_sub(left).map_err(RenderStatus::Time)?;
        let tall = bottom.checked_sub(top).map_err(RenderStatus::Time)?;
        if !wide.is_positive() || !tall.is_positive() {
            return Err(RenderStatus::DegenerateShape);
        }
        let one = Rational::ONE;
        let zero = Rational::ZERO;
        let minus = Rational::from_integer(-1);
        let mut edges = Vec::new();
        edges
            .try_reserve(4)
            .map_err(|_| RenderStatus::OutOfMemory)?;
        edges.push(Edge::new(
            minus,
            zero,
            minus.checked_mul(left).map_err(RenderStatus::Time)?,
        )?);
        edges.push(Edge::new(one, zero, right)?);
        edges.push(Edge::new(
            zero,
            minus,
            minus.checked_mul(top).map_err(RenderStatus::Time)?,
        )?);
        edges.push(Edge::new(zero, one, bottom)?);
        Ok(Self { edges })
    }

    /// The edges, in the order they are clipped against.
    #[must_use]
    pub fn edges(&self) -> &[Edge] {
        &self.edges
    }

    /// The exact area of one pixel that lies inside this shape.
    ///
    /// The pixel at column `x` and row `y` is the square from `(x, y)` to
    /// `(x + 1, y + 1)`, so the answer is between nought and one inclusive.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an overflow, which is a shape whose
    /// arithmetic a rational cannot carry — see the module note.
    pub fn coverage(&self, x: i64, y: i64) -> Result<Rational> {
        let mut polygon = square(x, y)?;
        for edge in &self.edges {
            polygon = clip(&polygon, *edge)?;
            if polygon.len() < 3 {
                // Clipped away to a point, a segment or nothing. This is an
                // early exit and **not** a correctness guard: the shoelace sum
                // over fewer than three vertices is already nought, and
                // clipping an empty polygon again yields an empty one, so
                // removing this changes no answer. It is here because a
                // sixty-four edge mask over a large frame would otherwise keep
                // clipping nothing, sixty-three more times, for every pixel
                // outside it. Tried, and it is worth the two lines.
                return Ok(Rational::ZERO);
            }
        }
        area(&polygon)
    }
}

/// The pixel square, counter-clockwise in a coordinate system whose `y` runs
/// downward — which is a picture's, and is why this order looks clockwise if
/// read as mathematics.
fn square(x: i64, y: i64) -> Result<Vec<(Rational, Rational)>> {
    let left = Rational::from_integer(x);
    let top = Rational::from_integer(y);
    let right = Rational::from_integer(x.checked_add(1).ok_or(RenderStatus::OutsideDomain)?);
    let bottom = Rational::from_integer(y.checked_add(1).ok_or(RenderStatus::OutsideDomain)?);
    let mut out = Vec::new();
    out.try_reserve(4).map_err(|_| RenderStatus::OutOfMemory)?;
    out.push((left, top));
    out.push((right, top));
    out.push((right, bottom));
    out.push((left, bottom));
    Ok(out)
}

/// Sutherland and Hodgman's clip of a convex polygon against one half-plane.
///
/// Every vertex is kept if it is inside, and every edge that crosses the line
/// contributes the crossing point. The crossing is found by the ratio of the
/// two endpoints' signed values, which is exact: a rational divided by a
/// rational, with no square root and no iteration anywhere.
fn clip(polygon: &[(Rational, Rational)], edge: Edge) -> Result<Vec<(Rational, Rational)>> {
    let mut out = Vec::new();
    out.try_reserve(polygon.len() + 1)
        .map_err(|_| RenderStatus::OutOfMemory)?;
    for (index, current) in polygon.iter().enumerate() {
        let previous = polygon[(index + polygon.len() - 1) % polygon.len()];
        let here = edge.outside(current.0, current.1)?;
        let there = edge.outside(previous.0, previous.1)?;
        let keeps = !here.is_positive();
        let kept = !there.is_positive();
        if keeps != kept {
            out.push(crossing(previous, *current, there, here)?);
        }
        if keeps {
            out.push(*current);
        }
    }
    Ok(out)
}

/// Where the segment from `from` to `to` crosses the line the two signed
/// values are measured against.
///
/// The two values have opposite signs by construction — the caller only calls
/// this when the endpoints are on different sides — so their difference is
/// non-zero and the division cannot be by nought.
fn crossing(
    from: (Rational, Rational),
    to: (Rational, Rational),
    at_from: Rational,
    at_to: Rational,
) -> Result<(Rational, Rational)> {
    let span = at_from.checked_sub(at_to).map_err(RenderStatus::Time)?;
    let fraction = at_from.checked_div(span).map_err(RenderStatus::Time)?;
    Ok((
        along(from.0, to.0, fraction)?,
        along(from.1, to.1, fraction)?,
    ))
}

/// A fraction of the way from one coordinate to another.
fn along(from: Rational, to: Rational, fraction: Rational) -> Result<Rational> {
    let span = to.checked_sub(from).map_err(RenderStatus::Time)?;
    from.checked_add(span.checked_mul(fraction).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)
}

/// The shoelace area of a polygon, as a positive rational.
///
/// The sign says which way round the vertices run, and an area does not have
/// one, so it is taken away. Halving happens once, at the end, rather than per
/// term — which keeps the denominators one factor of two smaller through the
/// whole sum.
fn area(polygon: &[(Rational, Rational)]) -> Result<Rational> {
    let mut total = Rational::ZERO;
    for (index, current) in polygon.iter().enumerate() {
        let next = polygon[(index + 1) % polygon.len()];
        let term = current
            .0
            .checked_mul(next.1)
            .map_err(RenderStatus::Time)?
            .checked_sub(next.0.checked_mul(current.1).map_err(RenderStatus::Time)?)
            .map_err(RenderStatus::Time)?;
        total = total.checked_add(term).map_err(RenderStatus::Time)?;
    }
    if !total.is_positive() {
        total = total.checked_neg().map_err(RenderStatus::Time)?;
    }
    total
        .checked_div(Rational::from_integer(2))
        .map_err(RenderStatus::Time)
}

/// The area of one pixel under a line, in closed form.
///
/// This is the independent check described in the module note, and nothing in
/// the rasteriser calls it. For a half-plane `a·x + b·y <= c` and the unit
/// square, with `d` the room left at the square's near corner:
///
/// ```text
///   area = [ d² - max(0, d - |a|)² - max(0, d - |b|)² ] / (2·|a|·|b|)
/// ```
///
/// Reflecting the square about either of its centre lines turns a negative
/// coefficient positive and does not change an area, which is why the absolute
/// values are all it takes to cover every orientation. When either coefficient
/// is nought the line is axis-aligned, the formula would divide by nought, and
/// the answer is the clamped ratio along the other axis.
///
/// # Errors
///
/// [`RenderStatus::Time`] wrapping an overflow.
pub fn half_plane_coverage(edge: Edge, x: i64, y: i64) -> Result<Rational> {
    let a = magnitude(edge.a)?;
    let b = magnitude(edge.b)?;
    // The value at the corner the normal points away from, which is where the
    // covered region starts: c minus the smallest a·x + b·y over the square.
    let least = smallest(edge, x, y)?;
    let room = edge.c.checked_sub(least).map_err(RenderStatus::Time)?;
    if !room.is_positive() {
        return Ok(Rational::ZERO);
    }
    let reach = a.checked_add(b).map_err(RenderStatus::Time)?;
    if !reach
        .checked_sub(room)
        .map_err(RenderStatus::Time)?
        .is_positive()
    {
        return Ok(Rational::ONE);
    }
    if a.is_zero() || b.is_zero() {
        return room.checked_div(reach).map_err(RenderStatus::Time);
    }
    let whole = squared(room)?;
    let past_a = squared(beyond(room, a)?)?;
    let past_b = squared(beyond(room, b)?)?;
    let top = whole
        .checked_sub(past_a)
        .map_err(RenderStatus::Time)?
        .checked_sub(past_b)
        .map_err(RenderStatus::Time)?;
    let bottom = a
        .checked_mul(b)
        .map_err(RenderStatus::Time)?
        .scale(2)
        .map_err(RenderStatus::Time)?;
    top.checked_div(bottom).map_err(RenderStatus::Time)
}

/// The smallest `a·x + b·y` over the pixel square, which is at whichever
/// corner each coefficient's sign picks.
fn smallest(edge: Edge, x: i64, y: i64) -> Result<Rational> {
    let low_x = Rational::from_integer(if edge.a.is_positive() { x } else { x + 1 });
    let low_y = Rational::from_integer(if edge.b.is_positive() { y } else { y + 1 });
    edge.a
        .checked_mul(low_x)
        .map_err(RenderStatus::Time)?
        .checked_add(edge.b.checked_mul(low_y).map_err(RenderStatus::Time)?)
        .map_err(RenderStatus::Time)
}

/// A rational without its sign.
fn magnitude(value: Rational) -> Result<Rational> {
    if value.numerator() < 0 {
        value.checked_neg().map_err(RenderStatus::Time)
    } else {
        Ok(value)
    }
}

/// `max(0, room - limit)`.
fn beyond(room: Rational, limit: Rational) -> Result<Rational> {
    let past = room.checked_sub(limit).map_err(RenderStatus::Time)?;
    if past.is_positive() {
        Ok(past)
    } else {
        Ok(Rational::ZERO)
    }
}

/// A rational times itself.
fn squared(value: Rational) -> Result<Rational> {
    value.checked_mul(value).map_err(RenderStatus::Time)
}

/// The shape a wipe covers when its edge is a fraction of the way across.
///
/// The direction says which way the covered region *grows*; the fraction says
/// how far. At nought the edge sits on the corner the direction points away
/// from and covers nothing; at one it has passed the opposite corner and
/// covers everything. Both ends are reached exactly, so a wipe that has not
/// started shows none of the incoming clip and one that has finished shows
/// none of the outgoing one — the same property the dissolve's fraction has,
/// arrived at by geometry instead of by arithmetic.
///
/// The direction's *length* carries no meaning: `(2, 0)` and `(1, 0)` are the
/// same wipe, because the fraction sets the edge's position rather than the
/// vector's length doing it.
///
/// # Errors
///
/// [`RenderStatus::DegenerateEdge`] for a direction that is nowhere,
/// [`RenderStatus::OutsideDomain`] for a picture with no pixels or a fraction
/// outside nought to one, and [`RenderStatus::Time`] wrapping an overflow.
pub fn sweeping(
    across: Rational,
    down: Rational,
    fraction: Rational,
    width: usize,
    height: usize,
) -> Result<Shape> {
    if width == 0 || height == 0 || width > MAX_EXTENT || height > MAX_EXTENT {
        return Err(RenderStatus::OutsideDomain);
    }
    if fraction.numerator() < 0
        || fraction
            .checked_sub(Rational::ONE)
            .map_err(RenderStatus::Time)?
            .is_positive()
    {
        return Err(RenderStatus::OutsideDomain);
    }
    let (least, most) = extremes(across, down, width, height)?;
    let travelled = most.checked_sub(least).map_err(RenderStatus::Time)?;
    let offset = least
        .checked_add(
            travelled
                .checked_mul(fraction)
                .map_err(RenderStatus::Time)?,
        )
        .map_err(RenderStatus::Time)?;
    Shape::half_plane(Edge::new(across, down, offset)?)
}

/// The smallest and largest `across·x + down·y` over the whole picture.
///
/// Both are at corners, and which two is decided by the signs. Taking them
/// from the corners rather than from a search is exact and is four
/// multiplications.
fn extremes(
    across: Rational,
    down: Rational,
    width: usize,
    height: usize,
) -> Result<(Rational, Rational)> {
    let right =
        Rational::from_integer(i64::try_from(width).map_err(|_| RenderStatus::OutsideDomain)?);
    let bottom =
        Rational::from_integer(i64::try_from(height).map_err(|_| RenderStatus::OutsideDomain)?);
    let mut least = None;
    let mut most = None;
    for (x, y) in [
        (Rational::ZERO, Rational::ZERO),
        (right, Rational::ZERO),
        (Rational::ZERO, bottom),
        (right, bottom),
    ] {
        let value = across
            .checked_mul(x)
            .map_err(RenderStatus::Time)?
            .checked_add(down.checked_mul(y).map_err(RenderStatus::Time)?)
            .map_err(RenderStatus::Time)?;
        least = Some(match least {
            None => value,
            Some(held) => lower(held, value)?,
        });
        most = Some(match most {
            None => value,
            Some(held) => upper(held, value)?,
        });
    }
    Ok((
        least.ok_or(RenderStatus::OutsideDomain)?,
        most.ok_or(RenderStatus::OutsideDomain)?,
    ))
}

/// How far a wipe's edge travels across the picture, in the edge's own units.
fn travel(across: Rational, down: Rational, width: usize, height: usize) -> Result<Rational> {
    let (least, most) = extremes(across, down, width, height)?;
    most.checked_sub(least).map_err(RenderStatus::Time)
}

/// Whichever of two rationals is smaller.
fn lower(one: Rational, other: Rational) -> Result<Rational> {
    if other
        .checked_sub(one)
        .map_err(RenderStatus::Time)?
        .is_positive()
    {
        Ok(one)
    } else {
        Ok(other)
    }
}

/// Whichever of two rationals is larger.
fn upper(one: Rational, other: Rational) -> Result<Rational> {
    if other
        .checked_sub(one)
        .map_err(RenderStatus::Time)?
        .is_positive()
    {
        Ok(other)
    } else {
        Ok(one)
    }
}

/// A shape's coverage over a whole picture, one byte per pixel.
///
/// # Errors
///
/// [`RenderStatus::OutsideDomain`] for a size past [`MAX_EXTENT`] or with no
/// pixels in it, [`RenderStatus::OutOfMemory`], and [`RenderStatus::Time`]
/// wrapping an overflow.
pub fn plane(shape: &Shape, width: usize, height: usize) -> Result<Vec<u8>> {
    plane_rows(shape, width, height, 0..height)
}

/// A range of a shape's rows, rasterised against the whole frame.
fn plane_rows(
    shape: &Shape,
    width: usize,
    height: usize,
    rows: core::ops::Range<usize>,
) -> Result<Vec<u8>> {
    if width == 0 || height == 0 || width > MAX_EXTENT || height > MAX_EXTENT {
        return Err(RenderStatus::OutsideDomain);
    }
    let mut out = Vec::new();
    out.try_reserve(
        width
            .checked_mul(rows.len())
            .ok_or(RenderStatus::OutsideDomain)?,
    )
    .map_err(|_| RenderStatus::OutOfMemory)?;
    for y in rows {
        for x in 0..width {
            let row = i64::try_from(y).map_err(|_| RenderStatus::OutsideDomain)?;
            let column = i64::try_from(x).map_err(|_| RenderStatus::OutsideDomain)?;
            out.push(quantise(shape.coverage(column, row)?)?);
        }
    }
    Ok(out)
}

/// A coverage in nought to one, as a byte.
///
/// Rounded half away from zero — the same rounding the curves use, stated in
/// one place rather than left to whatever the division happened to do.
///
/// # Errors
///
/// [`RenderStatus::OutsideDomain`] for a coverage outside nought to one, which
/// no area of a unit square can be and which therefore means the arithmetic
/// above went wrong rather than the picture being unusual.
pub fn quantise(coverage: Rational) -> Result<u8> {
    let numerator = coverage.numerator();
    let denominator = coverage.denominator();
    if numerator < 0 || numerator > denominator {
        return Err(RenderStatus::OutsideDomain);
    }
    let scaled = numerator
        .checked_mul(i64::from(FULL))
        .ok_or(RenderStatus::Time(media_editor_core::CoreStatus::Overflow))?;
    let rounded = (scaled * 2 + denominator) / (denominator * 2);
    u8::try_from(rounded).map_err(|_| RenderStatus::OutsideDomain)
}

/// A wipe whose edge fades rather than cutting.
///
/// Two parallel lines with a linear ramp between them: fully covered on the
/// near side of the first, not at all past the second, and a straight run in
/// between. The band is measured in the edge's own units, which is why nothing
/// constructs one of these directly — [`sweeping_soft`] takes a softness as a
/// fraction of the wipe's travel, where the units cancel.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Feather {
    edge: Edge,
    band: Rational,
}

impl Feather {
    /// A ramp of `band` units, centred on an edge.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::DegenerateShape`] for a band of nothing or less, which
    /// is a hard edge — and saying so is better than dividing by it.
    pub fn new(edge: Edge, band: Rational) -> Result<Self> {
        if !band.is_positive() {
            return Err(RenderStatus::DegenerateShape);
        }
        Ok(Self { edge, band })
    }

    /// The exact coverage of one pixel, with the ramp integrated over it.
    ///
    /// Not a sample of the ramp at the pixel's centre, and not an average of
    /// samples: the true integral. It is available exactly, and cheaply, for
    /// a reason worth stating because it is the whole reason this is not an
    /// approximation.
    ///
    /// **The integral of an affine function over a polygon is its area times
    /// its value at the polygon's centroid.** That is the definition of a
    /// centroid rather than a result about it. The ramp is affine, the region
    /// it runs over is the pixel square clipped by two parallel half-planes —
    /// convex, because the complement of a half-plane is a half-plane — and
    /// the polygon's area and centroid are rational. So the answer is exact
    /// with no case analysis at all.
    ///
    /// This module previously recorded a soft edge as "a far larger case
    /// analysis" and left it out on that basis. The claim was wrong; it is
    /// two clips and a moment.
    ///
    /// # Errors
    ///
    /// [`RenderStatus::Time`] wrapping an overflow.
    pub fn coverage(&self, x: i64, y: i64) -> Result<Rational> {
        let half = self
            .band
            .checked_div(Rational::from_integer(2))
            .map_err(RenderStatus::Time)?;
        let near = Edge {
            a: self.edge.a,
            b: self.edge.b,
            c: self.edge.c.checked_sub(half).map_err(RenderStatus::Time)?,
        };
        let far = Edge {
            a: self.edge.a,
            b: self.edge.b,
            c: self.edge.c.checked_add(half).map_err(RenderStatus::Time)?,
        };

        // Everything the near line already covers, at full weight.
        let whole = Shape::half_plane(near)?.coverage(x, y)?;

        // And the slab between the two, weighted by where in it each part
        // lies. Clipping by `far` and then by the *complement* of `near` is
        // what leaves exactly that slab, and leaves it convex.
        let mut band = clip(&square(x, y)?, far)?;
        if band.len() >= 3 {
            band = clip(&band, near.complement()?)?;
        }
        if band.len() < 3 {
            return Ok(whole);
        }
        let (area, moment_x, moment_y) = moments(&band)?;
        // The ramp is `(far.c - (a·x + b·y)) / band`, so integrating it over
        // the slab is `(far.c·area - a·Mx - b·My) / band` — where `Mx` and
        // `My` are the first moments, which are `area × centroid` and are
        // computed *without* dividing by the area. That keeps the arithmetic
        // one division shorter and has no degenerate case when the area is
        // nought.
        let weighted = far
            .c
            .checked_mul(area)
            .map_err(RenderStatus::Time)?
            .checked_sub(
                self.edge
                    .a
                    .checked_mul(moment_x)
                    .map_err(RenderStatus::Time)?,
            )
            .map_err(RenderStatus::Time)?
            .checked_sub(
                self.edge
                    .b
                    .checked_mul(moment_y)
                    .map_err(RenderStatus::Time)?,
            )
            .map_err(RenderStatus::Time)?
            .checked_div(self.band)
            .map_err(RenderStatus::Time)?;
        whole.checked_add(weighted).map_err(RenderStatus::Time)
    }
}

/// A polygon's area and its two first moments, all positive.
///
/// The moments are `area × centroid`, kept in that form because every use
/// multiplies them by the area again and the division would only be undone.
fn moments(polygon: &[(Rational, Rational)]) -> Result<(Rational, Rational, Rational)> {
    let mut twice_area = Rational::ZERO;
    let mut six_x = Rational::ZERO;
    let mut six_y = Rational::ZERO;
    for (index, current) in polygon.iter().enumerate() {
        let next = polygon[(index + 1) % polygon.len()];
        let cross = current
            .0
            .checked_mul(next.1)
            .map_err(RenderStatus::Time)?
            .checked_sub(next.0.checked_mul(current.1).map_err(RenderStatus::Time)?)
            .map_err(RenderStatus::Time)?;
        twice_area = twice_area.checked_add(cross).map_err(RenderStatus::Time)?;
        six_x = six_x
            .checked_add(
                current
                    .0
                    .checked_add(next.0)
                    .map_err(RenderStatus::Time)?
                    .checked_mul(cross)
                    .map_err(RenderStatus::Time)?,
            )
            .map_err(RenderStatus::Time)?;
        six_y = six_y
            .checked_add(
                current
                    .1
                    .checked_add(next.1)
                    .map_err(RenderStatus::Time)?
                    .checked_mul(cross)
                    .map_err(RenderStatus::Time)?,
            )
            .map_err(RenderStatus::Time)?;
    }
    // The sign says which way round the vertices run; an area does not have
    // one, and negating all three together is the same as reversing them.
    if !twice_area.is_positive() {
        twice_area = twice_area.checked_neg().map_err(RenderStatus::Time)?;
        six_x = six_x.checked_neg().map_err(RenderStatus::Time)?;
        six_y = six_y.checked_neg().map_err(RenderStatus::Time)?;
    }
    Ok((
        twice_area
            .checked_div(Rational::from_integer(2))
            .map_err(RenderStatus::Time)?,
        six_x
            .checked_div(Rational::from_integer(6))
            .map_err(RenderStatus::Time)?,
        six_y
            .checked_div(Rational::from_integer(6))
            .map_err(RenderStatus::Time)?,
    ))
}

/// A soft wipe's coverage over a whole picture, one byte per pixel.
///
/// `softness` is a fraction of the wipe's total travel, so it is unit-free and
/// means the same thing at every size and angle. Nought is a hard edge, and it
/// delegates rather than dividing by a band of nothing — which is also the
/// test that the two agree in the limit.
///
/// # Errors
///
/// As [`sweeping`], plus [`RenderStatus::OutsideDomain`] for a softness
/// outside nought to one.
pub fn feathered(
    across: Rational,
    down: Rational,
    fraction: Rational,
    softness: Rational,
    width: usize,
    height: usize,
) -> Result<Vec<u8>> {
    feathered_rows(across, down, fraction, softness, width, height, 0..height)
}

/// One row of a wipe's coverage, rasterised against the whole frame.
///
/// The sweep is placed against the **full** width and height, for the reason
/// [`masking_row`] gives: a wipe travels across the picture, and a wipe placed
/// against one row would travel across that row.
///
/// # Errors
///
/// As [`feathered`], and [`RenderStatus::OutsideDomain`] for a row outside the
/// frame.
pub fn feathered_row(
    across: Rational,
    down: Rational,
    fraction: Rational,
    softness: Rational,
    width: usize,
    height: usize,
    row: usize,
) -> Result<Vec<u8>> {
    if row >= height {
        return Err(RenderStatus::OutsideDomain);
    }
    feathered_rows(
        across,
        down,
        fraction,
        softness,
        width,
        height,
        row..row + 1,
    )
}

fn feathered_rows(
    across: Rational,
    down: Rational,
    fraction: Rational,
    softness: Rational,
    width: usize,
    height: usize,
    rows: core::ops::Range<usize>,
) -> Result<Vec<u8>> {
    let shape = sweeping(across, down, fraction, width, height)?;
    if softness.is_zero() {
        return plane_rows(&shape, width, height, rows);
    }
    if softness.numerator() < 0
        || softness
            .checked_sub(Rational::ONE)
            .map_err(RenderStatus::Time)?
            .is_positive()
    {
        return Err(RenderStatus::OutsideDomain);
    }
    let edge = shape.edges()[0];
    let band = travel(across, down, width, height)?
        .checked_mul(softness)
        .map_err(RenderStatus::Time)?;
    let feather = Feather::new(edge, band)?;
    let mut out = Vec::new();
    out.try_reserve(
        width
            .checked_mul(rows.len())
            .ok_or(RenderStatus::OutsideDomain)?,
    )
    .map_err(|_| RenderStatus::OutOfMemory)?;
    for y in rows {
        for x in 0..width {
            let row = i64::try_from(y).map_err(|_| RenderStatus::OutsideDomain)?;
            let column = i64::try_from(x).map_err(|_| RenderStatus::OutsideDomain)?;
            let held = feather.coverage(column, row)?;
            out.push(quantise(clamped(held)?)?);
        }
    }
    Ok(out)
}

/// A coverage brought back inside nought to one.
///
/// The integral cannot leave that range, and this is not there in case it
/// does: it is there because the *rounding* of two divisions can put a value
/// a vanishing distance outside, and `quantise` refuses rather than clamping,
/// which is right for it and wrong here.
fn clamped(value: Rational) -> Result<Rational> {
    if value.numerator() < 0 {
        return Ok(Rational::ZERO);
    }
    if value
        .checked_sub(Rational::ONE)
        .map_err(RenderStatus::Time)?
        .is_positive()
    {
        return Ok(Rational::ONE);
    }
    Ok(value)
}

/// A convex region from its corners, in pixel coordinates.
///
/// The winding is **not** the caller's problem. A polygon's edges point inward
/// on one winding and outward on the other, so the region they describe is
/// either the polygon or everything outside all of its edges — which is
/// nothing. This picks the winding by measuring, from the sign of the
/// polygon's own area, rather than insisting the caller supply one.
///
/// Two things need this and they arrive from opposite directions: a mask's
/// corners are dragged by a person, and a resampled pixel's preimage is
/// whatever an affine map made of a square — including, under a mirror, the
/// other winding. Neither can promise a direction, so neither is asked.
///
/// # Errors
///
/// [`RenderStatus::DegenerateShape`] for fewer than three corners or for
/// corners enclosing no area, [`RenderStatus::ShapeTooComplex`] past
/// [`MAX_EDGES`], and [`RenderStatus::Time`] wrapping an overflow.
pub fn convex(corners: &[(Rational, Rational)]) -> Result<Shape> {
    if corners.len() < 3 {
        return Err(RenderStatus::DegenerateShape);
    }
    if corners.len() > MAX_EDGES {
        return Err(RenderStatus::ShapeTooComplex);
    }
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
    if twice.is_zero() {
        return Err(RenderStatus::DegenerateShape);
    }
    let clockwise = twice.is_positive();

    let mut edges = Vec::new();
    edges
        .try_reserve(corners.len())
        .map_err(|_| RenderStatus::OutOfMemory)?;
    for (index, current) in corners.iter().enumerate() {
        let next = corners[(index + 1) % corners.len()];
        // The inward normal of this edge: its direction turned a quarter of a
        // turn, the way the winding says.
        let run = next.0.checked_sub(current.0).map_err(RenderStatus::Time)?;
        let rise = next.1.checked_sub(current.1).map_err(RenderStatus::Time)?;
        let normal = if clockwise {
            (rise, run.checked_neg().map_err(RenderStatus::Time)?)
        } else {
            (rise.checked_neg().map_err(RenderStatus::Time)?, run)
        };
        if normal.0.is_zero() && normal.1.is_zero() {
            // Two corners in the same place: no edge, and nothing to clip by.
            continue;
        }
        let offset = normal
            .0
            .checked_mul(current.0)
            .map_err(RenderStatus::Time)?
            .checked_add(
                normal
                    .1
                    .checked_mul(current.1)
                    .map_err(RenderStatus::Time)?,
            )
            .map_err(RenderStatus::Time)?;
        edges.push(Edge::new(normal.0, normal.1, offset)?);
    }
    Shape::new(edges)
}

/// A convex mask's coverage over a picture, one byte per pixel.
///
/// The corners are fractions of the frame, so this is where they become
/// pixels — exactly, because a fraction times an integer is a rational and
/// nothing here rounds until the byte at the very end.
///
/// The winding is not the caller's problem. A polygon's edges point inward on
/// one winding and outward on the other, so the shape they describe is either
/// the polygon or everything outside every one of its edges — which is
/// nothing. This picks the winding by measuring, from the sign of the
/// polygon's own area, rather than insisting the caller supply one.
///
/// # Errors
///
/// [`RenderStatus::DegenerateShape`] for corners enclosing no area,
/// [`RenderStatus::ShapeTooComplex`] past [`MAX_EDGES`],
/// [`RenderStatus::OutsideDomain`] for a picture with no pixels, and
/// [`RenderStatus::Time`] wrapping an overflow.
pub fn masking(
    corners: &[(Rational, Rational)],
    inverted: bool,
    width: usize,
    height: usize,
) -> Result<Vec<u8>> {
    masking_rows(corners, inverted, width, height, 0..height)
}

/// One row of a mask's coverage, rasterised against the whole frame.
///
/// The shape is placed against the **full** width and height and only the row
/// asked for is rasterised, which is what makes a row of a mask the same bytes
/// as that row of the whole mask. Placing it against a one-row frame instead
/// would stretch the shape over one row, which is a different picture — and is
/// the mistake this signature exists to make impossible.
///
/// # Errors
///
/// As [`masking`], and [`RenderStatus::OutsideDomain`] for a row outside the
/// frame.
pub fn masking_row(
    corners: &[(Rational, Rational)],
    inverted: bool,
    width: usize,
    height: usize,
    row: usize,
) -> Result<Vec<u8>> {
    if row >= height {
        return Err(RenderStatus::OutsideDomain);
    }
    masking_rows(corners, inverted, width, height, row..row + 1)
}

fn masking_rows(
    corners: &[(Rational, Rational)],
    inverted: bool,
    width: usize,
    height: usize,
    rows: core::ops::Range<usize>,
) -> Result<Vec<u8>> {
    if width == 0 || height == 0 || width > MAX_EXTENT || height > MAX_EXTENT {
        return Err(RenderStatus::OutsideDomain);
    }
    if corners.len() < 3 {
        return Err(RenderStatus::DegenerateShape);
    }
    if corners.len() > MAX_EDGES {
        return Err(RenderStatus::ShapeTooComplex);
    }
    let across =
        Rational::from_integer(i64::try_from(width).map_err(|_| RenderStatus::OutsideDomain)?);
    let downward =
        Rational::from_integer(i64::try_from(height).map_err(|_| RenderStatus::OutsideDomain)?);
    let mut placed = Vec::new();
    placed
        .try_reserve(corners.len())
        .map_err(|_| RenderStatus::OutOfMemory)?;
    for corner in corners {
        placed.push((
            corner.0.checked_mul(across).map_err(RenderStatus::Time)?,
            corner.1.checked_mul(downward).map_err(RenderStatus::Time)?,
        ));
    }

    let shape = convex(&placed)?;

    let mut out = Vec::new();
    out.try_reserve(
        width
            .checked_mul(rows.len())
            .ok_or(RenderStatus::OutsideDomain)?,
    )
    .map_err(|_| RenderStatus::OutOfMemory)?;
    for y in rows {
        for x in 0..width {
            let row = i64::try_from(y).map_err(|_| RenderStatus::OutsideDomain)?;
            let column = i64::try_from(x).map_err(|_| RenderStatus::OutsideDomain)?;
            let held = quantise(shape.coverage(column, row)?)?;
            // Inverting the *byte* rather than the shape, so that the two
            // sides of a mask always sum to exactly full coverage. Rasterising
            // the complement instead would quantise twice and could sum to two
            // hundred and fifty-six, which is the same trap the wipe records.
            out.push(if inverted { FULL - held } else { held });
        }
    }
    Ok(out)
}
