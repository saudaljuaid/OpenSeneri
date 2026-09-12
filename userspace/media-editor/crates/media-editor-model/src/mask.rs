// SPDX-License-Identifier: GPL-3.0-only
//! Convex clip masks in frame-relative coordinates.
//!
//! Corners are fractions of frame width and height, so masks are independent
//! of output resolution. Coordinates may extend beyond the frame. Convexity is
//! required by the exact-area rasteriser; concave shapes can be represented as
//! multiple convex masks at a higher layer.

use alloc::vec::Vec;

use media_editor_core::Rational;

use crate::status::{ModelStatus, Result};

/// The most corners one mask may have.
///
/// Thirty-two: an ellipse drawn as a polygon is convincing at sixteen and
/// past arguing at thirty-two, and it is a bound a hostile project file
/// cannot talk its way past (R-11.2).
pub const MAX_CORNERS: usize = 32;

/// The fewest.
pub const MIN_CORNERS: usize = 3;

/// A convex region of the frame, in fractions of it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Mask {
    corners: Vec<(Rational, Rational)>,
    inverted: bool,
}

impl Mask {
    /// A mask from its corners, in order around the shape.
    ///
    /// Clockwise and counter-clockwise winding are both accepted.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::MaskTooSimple`] for fewer than [`MIN_CORNERS`] corners,
    /// which enclose no area; [`ModelStatus::CapacityExhausted`] past
    /// [`MAX_CORNERS`]; and [`ModelStatus::MaskNotConvex`] for corners that
    /// turn both ways, which is a shape this build cannot compute an exact
    /// area for.
    pub fn new(corners: Vec<(Rational, Rational)>) -> Result<Self> {
        if corners.len() < MIN_CORNERS {
            return Err(ModelStatus::MaskTooSimple);
        }
        if corners.len() > MAX_CORNERS {
            return Err(ModelStatus::CapacityExhausted);
        }
        check_convex(&corners)?;
        Ok(Self {
            corners,
            inverted: false,
        })
    }

    /// An axis-aligned rectangle, from its left, top, right and bottom.
    ///
    /// # Errors
    ///
    /// As [`Mask::new`], and [`ModelStatus::MaskTooSimple`] for a rectangle
    /// enclosing nothing.
    pub fn rectangle(
        left: Rational,
        top: Rational,
        right: Rational,
        bottom: Rational,
    ) -> Result<Self> {
        if !right.checked_sub(left)?.is_positive() || !bottom.checked_sub(top)?.is_positive() {
            return Err(ModelStatus::MaskTooSimple);
        }
        Self::new(alloc::vec![
            (left, top),
            (right, top),
            (right, bottom),
            (left, bottom),
        ])
    }

    /// The corners, in the order they were given.
    #[must_use]
    pub fn corners(&self) -> &[(Rational, Rational)] {
        &self.corners
    }

    /// Whether what is kept is what lies *outside* the shape.
    ///
    /// A separate flag rather than a second kind of mask, because inverting is
    /// something an editor does to a shape they have already drawn and expects
    /// to be able to undo without redrawing it.
    #[must_use]
    pub const fn is_inverted(&self) -> bool {
        self.inverted
    }

    /// The same mask, keeping the other side.
    #[must_use]
    pub fn inverted(&self) -> Self {
        Self {
            corners: self.corners.clone(),
            inverted: !self.inverted,
        }
    }

    /// The same mask with its inversion set.
    #[must_use]
    pub fn with_inversion(&self, inverted: bool) -> Self {
        Self {
            corners: self.corners.clone(),
            inverted,
        }
    }

    /// The point this shape balances on, exactly.
    ///
    /// The area-weighted centroid rather than the mean of the corners, which
    /// are the same point only for a shape whose corners are evenly spread —
    /// and an ellipse drawn as sixteen points is not, nor is a rectangle with
    /// a corner cut off. Scaling about the wrong one would drift a shape
    /// sideways while it grew, which reads as a bug in the animation rather
    /// than as a choice about which point is the middle.
    ///
    /// All of it rational, so a mask that opens to a half and back is the same
    /// mask it started as, exactly.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow. The division cannot fail:
    /// [`Mask::new`] refuses a polygon with no area, so the denominator here
    /// is never nought.
    pub fn centroid(&self) -> Result<(Rational, Rational)> {
        let mut twice_area = Rational::ZERO;
        let mut across = Rational::ZERO;
        let mut down = Rational::ZERO;
        for index in 0..self.corners.len() {
            let (x0, y0) = self.corners[index];
            let (x1, y1) = self.corners[(index + 1) % self.corners.len()];
            let cross = x0.checked_mul(y1)?.checked_sub(x1.checked_mul(y0)?)?;
            twice_area = twice_area.checked_add(cross)?;
            across = across.checked_add(x0.checked_add(x1)?.checked_mul(cross)?)?;
            down = down.checked_add(y0.checked_add(y1)?.checked_mul(cross)?)?;
        }
        // Six times the area, which is twice the area times three -- and the
        // sign cancels, so a mask wound either way gives the same point.
        let scale = twice_area.checked_mul(Rational::new(3, 1)?)?;
        Ok((across.checked_div(scale)?, down.checked_div(scale)?))
    }

    /// Scale and rotate this mask about its centroid, then translate it.
    ///
    /// Positive scale and rotation preserve convexity and winding. Operations
    /// use the mask's frame-relative coordinate space; pixel-space rotation
    /// would require the delivery aspect ratio, which the model does not own.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::ScaleNotPositive`] for a non-positive scale, and
    /// [`ModelStatus::Time`] for arithmetic overflow.
    pub fn moved_by(
        &self,
        scale: Rational,
        across: Rational,
        down: Rational,
        turn: crate::transform::Turn,
    ) -> Result<Self> {
        if !scale.is_positive() {
            return Err(ModelStatus::ScaleNotPositive);
        }
        let (middle_x, middle_y) = self.centroid()?;
        let mut moved = Vec::new();
        moved
            .try_reserve(self.corners.len())
            .map_err(|_| ModelStatus::OutOfMemory)?;
        for (x, y) in &self.corners {
            // Taken to the centroid's frame, scaled and turned there, and put
            // back. Written as one journey rather than as a scale followed by
            // a turn so that a corner crosses the centroid exactly once and
            // there is one rounding point -- which, over exact rationals, is
            // no rounding at all, and is the habit that stops being free the
            // day any of this is computed in fixed point.
            let turned = turn.applied_to((
                x.checked_sub(middle_x)?.checked_mul(scale)?,
                y.checked_sub(middle_y)?.checked_mul(scale)?,
            ))?;
            moved.push((
                middle_x.checked_add(turned.0)?.checked_add(across)?,
                middle_y.checked_add(turned.1)?.checked_add(down)?,
            ));
        }
        Ok(Self {
            corners: moved,
            inverted: self.inverted,
        })
    }
}

/// Whether every turn goes the same way.
///
/// The cross product of consecutive edges is positive for one direction of
/// turn and negative for the other, so a shape that produces both turns both
/// ways and is not convex. A zero is a corner that does not turn at all —
/// three points in a line — which is permitted: it describes the same region
/// as the shape without it, and refusing it would refuse a rectangle somebody
/// built by dragging a fifth point onto an edge.
fn check_convex(corners: &[(Rational, Rational)]) -> Result<()> {
    let mut sign = 0_i8;
    for index in 0..corners.len() {
        let a = corners[index];
        let b = corners[(index + 1) % corners.len()];
        let c = corners[(index + 2) % corners.len()];
        let turn =
            b.0.checked_sub(a.0)?
                .checked_mul(c.1.checked_sub(b.1)?)?
                .checked_sub(b.1.checked_sub(a.1)?.checked_mul(c.0.checked_sub(b.0)?)?)?;
        if turn.is_zero() {
            continue;
        }
        let here = if turn.is_positive() { 1 } else { -1 };
        if sign == 0 {
            sign = here;
        } else if sign != here {
            return Err(ModelStatus::MaskNotConvex);
        }
    }
    if sign == 0 {
        // Every corner in a line: a polygon with no area at all.
        return Err(ModelStatus::MaskTooSimple);
    }
    Ok(())
}
