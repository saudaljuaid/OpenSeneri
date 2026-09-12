// SPDX-License-Identifier: GPL-3.0-only
//! Clip geometry in dimensionless frame coordinates.
//!
//! The linear transform uses exact rationals and translation uses fractions of
//! the frame, so framing is resolution-independent. Transforms act about a
//! named anchor, with the frame centre as the default. [`Turn`] stores a
//! rational point on the unit circle. It can be built from `t = tan(θ/2)`:
//! `cos θ = (1 - t²)/(1 + t²)` and `sin θ = 2t/(1 + t²)`.

use alloc::vec::Vec;

use media_editor_core::{Instant, Rational};

use crate::curve::Curve;
use crate::status::{ModelStatus, Result};

/// How to weigh the source under a destination pixel.
///
/// Stored in the model so the selected filter is part of the project.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Resampling {
    /// The exact area-weighted mean of the source a destination pixel covers.
    /// Right for reduction.
    Area,
    /// The four samples around where a destination pixel's centre lands.
    /// Right for enlargement.
    Bilinear,
}

/// A rotation, held as an exact point on the unit circle.
///
/// Not an angle, and not a matrix either. An angle in degrees needs a sine and
/// a cosine to become geometry, and neither is exact; a general matrix can
/// shear, and a shear is a different feature. A point `(cos θ, sin θ)` with
/// `cos² + sin² = 1` is exactly a rotation and exactly nothing else, and the
/// rational ones are dense — see this module's header for the parametrisation
/// that reaches every one of them.
///
/// The determinant is one by construction, so a turn preserves area, preserves
/// convexity, and preserves winding. That is why [`Mask::moved_by`] can return
/// a mask after applying a positive scale and turn.
///
/// [`Mask::moved_by`]: crate::mask::Mask::moved_by
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Turn {
    cosine: Rational,
    sine: Rational,
}

impl Turn {
    /// No rotation at all.
    pub const NONE: Self = Self {
        cosine: Rational::ONE,
        sine: Rational::ZERO,
    };

    /// A turn from a point on the unit circle.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NotATurn`] if the point is not *on* the circle. Anything
    /// else is a scale wearing a rotation's name — and a caller who wanted a
    /// scale has [`Motion`]'s own lane for it, where it is checked for being
    /// positive rather than being smuggled in through a matrix.
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn new(cosine: Rational, sine: Rational) -> Result<Self> {
        if cosine
            .checked_mul(cosine)?
            .checked_add(sine.checked_mul(sine)?)?
            != Rational::ONE
        {
            return Err(ModelStatus::NotATurn);
        }
        Ok(Self { cosine, sine })
    }

    /// Build the turn whose half-angle tangent is `parameter`.
    ///
    /// This evaluates `cos = (1 - t²)/(1 + t²)` and
    /// `sin = 2t/(1 + t²)` exactly. Zero is no turn and one is a quarter turn.
    /// A finite parameter cannot represent the half-turn point `(-1, 0)`, so
    /// the parameter is a constructor input rather than the stored form.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn from_half_angle(parameter: Rational) -> Result<Self> {
        let square = parameter.checked_mul(parameter)?;
        let denominator = Rational::ONE.checked_add(square)?;
        // `1 + t²` is positive for every rational `t`.
        Ok(Self {
            cosine: Rational::ONE
                .checked_sub(square)?
                .checked_div(denominator)?,
            sine: parameter
                .checked_mul(Rational::new(2, 1)?)?
                .checked_div(denominator)?,
        })
    }

    /// The cosine of the angle turned through.
    #[must_use]
    pub const fn cosine(self) -> Rational {
        self.cosine
    }

    /// And its sine.
    #[must_use]
    pub const fn sine(self) -> Rational {
        self.sine
    }

    /// Whether this is the identity turn.
    #[must_use]
    pub fn is_still(self) -> bool {
        self.cosine == Rational::ONE && self.sine.is_zero()
    }

    /// Compose this turn with another using exact angle-addition formulas.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow — which is the real bound
    /// here, because the denominators multiply.
    pub fn composed_with(self, other: Self) -> Result<Self> {
        Ok(Self {
            cosine: self
                .cosine
                .checked_mul(other.cosine)?
                .checked_sub(self.sine.checked_mul(other.sine)?)?,
            sine: self
                .sine
                .checked_mul(other.cosine)?
                .checked_add(self.cosine.checked_mul(other.sine)?)?,
        })
    }

    /// This point, turned.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub(crate) fn applied_to(self, (x, y): (Rational, Rational)) -> Result<(Rational, Rational)> {
        Ok((
            x.checked_mul(self.cosine)?
                .checked_sub(y.checked_mul(self.sine)?)?,
            x.checked_mul(self.sine)?
                .checked_add(y.checked_mul(self.cosine)?)?,
        ))
    }
}

/// Where a clip sits in the frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Transform {
    linear: [Rational; 4],
    offset: (Rational, Rational),
    resampling: Resampling,
    /// Pivot of the linear transform, in frame fractions.
    anchor: (Rational, Rational),
}

/// Default transform pivot at the center of the frame.
const CENTRE: (Rational, Rational) = (Rational::HALF, Rational::HALF);

impl Transform {
    /// A scale about the frame's centre, and a move in fractions of it.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::TransformNotInvertible`] for a scale of nought on either
    /// axis, which flattens the picture onto a line and has no way back.
    pub fn scaled(
        across: Rational,
        down: Rational,
        offset: (Rational, Rational),
        resampling: Resampling,
    ) -> Result<Self> {
        Self::new(
            [across, Rational::ZERO, Rational::ZERO, down],
            offset,
            resampling,
        )
    }

    /// A transform from its linear part, row-major, and its move.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::TransformNotInvertible`] if the linear part has no
    /// inverse, and [`ModelStatus::Time`] wrapping an overflow.
    pub fn new(
        linear: [Rational; 4],
        offset: (Rational, Rational),
        resampling: Resampling,
    ) -> Result<Self> {
        let [a, b, c, d] = linear;
        let determinant = a.checked_mul(d)?.checked_sub(b.checked_mul(c)?)?;
        if determinant.is_zero() {
            return Err(ModelStatus::TransformNotInvertible);
        }
        Ok(Self {
            linear,
            offset,
            resampling,
            anchor: CENTRE,
        })
    }

    /// The point this acts about, in fractions of the frame.
    #[must_use]
    pub const fn anchor(&self) -> (Rational, Rational) {
        self.anchor
    }

    /// Return this transform with another pivot.
    ///
    /// Pivots may lie outside the frame.
    #[must_use]
    pub fn with_anchor(&self, anchor: (Rational, Rational)) -> Self {
        Self { anchor, ..*self }
    }

    /// The linear part, row-major and dimensionless.
    #[must_use]
    pub const fn linear(&self) -> [Rational; 4] {
        self.linear
    }

    /// The move, in fractions of the frame.
    #[must_use]
    pub const fn offset(&self) -> (Rational, Rational) {
        self.offset
    }

    /// Which filter this asks for.
    #[must_use]
    pub const fn resampling(&self) -> Resampling {
        self.resampling
    }

    /// Whether the linear transform and offset are both identities.
    ///
    /// The pivot does not affect an identity transform.
    #[must_use]
    pub fn is_still(&self) -> bool {
        self.linear == [Rational::ONE, Rational::ZERO, Rational::ZERO, Rational::ONE]
            && self.offset.0.is_zero()
            && self.offset.1.is_zero()
    }
}

/// Clip-local scale, translation, and rotation animation.
///
/// The turn lane stores a half-angle parameter so every curve value maps to a
/// valid rotation through [`Turn::from_half_angle`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Motion {
    scale: Option<Curve>,
    across: Option<Curve>,
    down: Option<Curve>,
    turn: Option<Curve>,
}

impl Motion {
    /// A motion from its lanes, at least one of which must be present.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NoAutomation`] if every lane is absent, or
    /// [`ModelStatus::ScaleNotPositive`] if a scale keyframe is not positive.
    pub fn new(
        scale: Option<Curve>,
        across: Option<Curve>,
        down: Option<Curve>,
        turn: Option<Curve>,
    ) -> Result<Self> {
        if scale.is_none() && across.is_none() && down.is_none() && turn.is_none() {
            return Err(ModelStatus::NoAutomation);
        }
        if let Some(held) = &scale {
            for keyframe in held.keyframes() {
                if !keyframe.value().is_positive() {
                    return Err(ModelStatus::ScaleNotPositive);
                }
            }
        }
        Ok(Self {
            scale,
            across,
            down,
            turn,
        })
    }

    /// The scale lane, if there is one.
    #[must_use]
    pub const fn scale(&self) -> Option<&Curve> {
        self.scale.as_ref()
    }

    /// The lane moving the picture across.
    #[must_use]
    pub const fn across(&self) -> Option<&Curve> {
        self.across.as_ref()
    }

    /// The lane moving it down.
    #[must_use]
    pub const fn down(&self) -> Option<&Curve> {
        self.down.as_ref()
    }

    /// The lane turning it, in half-angle parameter.
    ///
    /// Nought is no turn and one is a quarter turn. Not degrees, and not a
    /// cosine: see [`Turn::from_half_angle`] for why the parameter is what a
    /// curve is allowed to hold.
    #[must_use]
    pub const fn turn(&self) -> Option<&Curve> {
        self.turn.as_ref()
    }

    /// The same motion measured from a start `by` ticks further along.
    ///
    /// A split hands the tail a new start, and the tail's animation must be
    /// re-based onto it or the move restarts at the cut.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn shifted(&self, by: i64) -> Result<Self> {
        let shift = |lane: Option<&Curve>| -> Result<Option<Curve>> {
            match lane {
                None => Ok(None),
                Some(curve) => Ok(Some(curve.shifted(by)?)),
            }
        };
        Ok(Self {
            scale: shift(self.scale.as_ref())?,
            across: shift(self.across.as_ref())?,
            down: shift(self.down.as_ref())?,
            turn: shift(self.turn.as_ref())?,
        })
    }

    /// What the four lanes read at an instant measured from the clip's start.
    ///
    /// A lane with no curve reads its neutral value: one for the scale, which
    /// multiplies, nought for the moves, which add, and nought for the turn's
    /// parameter, which is [`Turn::NONE`].
    ///
    /// The turn comes back **resolved**, as a point on the circle rather than
    /// as the parameter it was stored as. That is the same decision the layer
    /// stack makes about everything else it hands downwards: a consumer should
    /// receive geometry, not a recipe for it.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a timebase mismatch, or whatever the
    /// curve refuses.
    pub fn at(&self, into: Instant) -> Result<(Rational, Rational, Rational, Turn)> {
        let read = |lane: Option<&Curve>, neutral: Rational| -> Result<Rational> {
            match lane {
                None => Ok(neutral),
                Some(curve) => curve.value_at(into),
            }
        };
        Ok((
            read(self.scale.as_ref(), Rational::ONE)?,
            read(self.across.as_ref(), Rational::ZERO)?,
            read(self.down.as_ref(), Rational::ZERO)?,
            Turn::from_half_angle(read(self.turn.as_ref(), Rational::ZERO)?)?,
        ))
    }
}

impl Transform {
    /// Apply motion scale, rotation, and translation to this transform.
    ///
    /// Scale multiplies the linear part, rotation is applied on the left, and
    /// translation adds to the offset. Left application makes rotation operate
    /// in viewer space and preserves the expected behavior of mirrored clips.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::ScaleNotPositive`] if easing produces a non-positive
    /// scale, and [`ModelStatus::Time`] for arithmetic overflow.
    pub fn moved_by(
        &self,
        scale: Rational,
        across: Rational,
        down: Rational,
        turn: Turn,
    ) -> Result<Self> {
        if !scale.is_positive() {
            return Err(ModelStatus::ScaleNotPositive);
        }
        let mut linear = self.linear;
        for held in &mut linear {
            *held = held.checked_mul(scale)?;
        }
        // `R·M`, written out: the turn acts on each column of the scaled
        // linear part, which is the same as acting on the two vectors the
        // matrix sends the axes to.
        let (a, c) = turn.applied_to((linear[0], linear[2]))?;
        let (b, d) = turn.applied_to((linear[1], linear[3]))?;
        Ok(Self::new(
            [a, b, c, d],
            (
                self.offset.0.checked_add(across)?,
                self.offset.1.checked_add(down)?,
            ),
            self.resampling,
        )?
        // `new` starts from nothing, which is right there and wrong here: an
        // animated clip pivots where its base transform pivots, and rebuilding
        // through the constructor would quietly move every animated clip's
        // pivot back to the centre. This is the third field to find that trap,
        // after the grade and the motion.
        .with_anchor(self.anchor))
    }
}

/// The four lanes of a motion, in the order the format writes them.
///
/// The turn is written last, after the three that were there before it, so
/// that the file's lane order and this function's order stay the same list —
/// which is what a reader comparing the two halves of the format checks.
#[must_use]
pub fn lanes(motion: &Motion) -> Vec<Option<&Curve>> {
    alloc::vec![
        motion.scale(),
        motion.across(),
        motion.down(),
        motion.turn()
    ]
}
