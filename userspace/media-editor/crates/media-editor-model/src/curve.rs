// SPDX-License-Identifier: GPL-3.0-only
//! A parameter that changes over time.
//!
//! Opacity that fades, a scale that pushes in, a volume that ducks under
//! dialogue. Each is one number with a different value at different instants,
//! and a *curve* is the shape it takes between the moments somebody actually
//! set it.
//!
//! # Held, not extrapolated
//!
//! Before the first keyframe a curve holds the first value; after the last it
//! holds the last. It does not continue the slope. That is not conservatism:
//! extrapolating an animation past its last key is how a parameter set to
//! reach 100% at the end of a shot arrives at 340% two shots later, and the
//! editor who set two keyframes did not describe anything outside them.
//!
//! # Hold, linear, and ease
//!
//! [`Interpolation::Hold`] keeps the outgoing value until the next keyframe
//! and then jumps. [`Interpolation::Linear`] runs straight between them. Both
//! are exact: the value at any instant is a rational function of rationals,
//! computed with [`Rational`] and no rounding anywhere.
//!
//! [`Interpolation::Ease`] is the cubic Bézier every editor draws as two
//! handles, and it is the one with an arithmetic problem worth stating
//! plainly. A Bézier is parameterised by `t`, but a curve is asked for a value
//! at a *time*, and getting from one to the other means solving `x(t) = time`
//! — a cubic. Cardano's formula needs a cube root, which is not rational, so
//! there is no exact answer to find.
//!
//! What is done instead: `t` is found by bisection to [`EASE_BITS`] halvings,
//! and the value is then computed *exactly* at that `t`. So the number handed
//! back is not an approximation of the curve — it is the curve's exact value
//! at a dyadic parameter within `2^-32` of the right one. The approximation is
//! entirely in which point of the curve was chosen, never in the arithmetic
//! that evaluated it, and the same instant gives the same answer on every
//! machine because integer bisection has no rounding mode to disagree about.
//!
//! Bisection needs `x(t)` to be monotone, which is why the handles' horizontal
//! positions are held inside the span (R-1.3): a handle outside it makes a
//! curve that goes back in time, where "the value at this instant" has more
//! than one answer and no amount of arithmetic will pick between them.

use alloc::vec::Vec;

use media_editor_core::{Instant, Rational};

use crate::bounded::{insert_bounded, push_bounded};
use crate::status::{ModelStatus, Result};

/// The most keyframes one curve may hold.
///
/// A thousand is more than any hand-drawn animation and far fewer than a
/// tracker's output, which is a different thing and arrives as its own kind of
/// data rather than as somebody's keyframes (R-11.2).
pub const MAX_KEYFRAMES: usize = 1_000;

/// How many halvings the ease inversion performs, and the precision of the
/// fraction it hands back.
///
/// Twenty, which is one part in a million — far finer than any pixel, any
/// sample, or any parameter an editor sets. It is not larger because exact
/// arithmetic on a cubic *cubes* the denominator: a parameter written as a
/// fraction over two to the thirty-second would make `t³` a fraction over two
/// to the ninety-sixth, which no 64-bit rational holds. The inversion
/// therefore runs in 128-bit integers and rounds once at the end, and both the
/// bit count and the rounding are stated rather than discovered.
///
/// It is fixed rather than adaptive because a loop that stops when it is
/// "close enough" stops in a different place depending on where it started,
/// and two machines would then disagree about a value neither had rounded.
pub const EASE_BITS: u32 = 20;

/// The denominator the ease works in: two to the [`EASE_BITS`].
const EASE_SCALE: i128 = 1 << EASE_BITS;

/// How a value gets from one keyframe to the next.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Interpolation {
    /// Keep this value until the next keyframe, then jump.
    ///
    /// What a parameter with discrete settings needs — a blend mode, a
    /// two-state switch — and what an editor reaches for when a smooth ramp
    /// would be a mistake.
    Hold,
    /// Run straight from this value to the next.
    Linear,
    /// A cubic Bézier, given as the two handles every editor draws.
    ///
    /// The horizontals are fractions of the span between the two keyframes and
    /// must lie within it. The verticals are fractions of the value change and
    /// may lie outside it — a handle past one is an overshoot, which is a
    /// deliberate and useful thing for a curve to do.
    Ease {
        /// Where the outgoing handle sits along the span, from nought to one.
        out_x: Rational,
        /// How far the outgoing handle rises, as a fraction of the change.
        out_y: Rational,
        /// Where the incoming handle sits along the span, from nought to one.
        in_x: Rational,
        /// How far the incoming handle rises, as a fraction of the change.
        in_y: Rational,
    },
}

impl Interpolation {
    /// The ease every editor offers as its default: slow out, slow in.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] only, and never in practice: the constants are
    /// literal.
    pub fn ease_in_out() -> Result<Self> {
        Ok(Self::Ease {
            out_x: Rational::new(1, 3)?,
            out_y: Rational::new(0, 1)?,
            in_x: Rational::new(2, 3)?,
            in_y: Rational::new(1, 1)?,
        })
    }

    /// Whether the handles describe a curve a value can be read off.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::HandleOutOfSpan`] for a horizontal outside nought to
    /// one.
    pub fn check(self) -> Result<()> {
        let Self::Ease { out_x, in_x, .. } = self else {
            return Ok(());
        };
        let one = Rational::from_integer(1);
        let zero = Rational::from_integer(0);
        if out_x < zero || out_x > one || in_x < zero || in_x > one {
            // A handle outside the span makes x(t) fold back on itself, and
            // "the value at this instant" stops having one answer. Refusing is
            // the only honest option: clamping would silently draw a different
            // curve from the one the editor dragged.
            return Err(ModelStatus::HandleOutOfSpan);
        }
        Ok(())
    }
}

/// A value somebody set, at a moment, and how it leaves.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Keyframe {
    /// When the value is exactly this.
    at: Instant,
    /// What it is.
    value: Rational,
    /// How it reaches the next keyframe.
    interpolation: Interpolation,
}

impl Keyframe {
    /// Set a value at an instant.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::HandleOutOfSpan`] for an ease whose handles fold.
    pub fn new(at: Instant, value: Rational, interpolation: Interpolation) -> Result<Self> {
        interpolation.check()?;
        Ok(Self {
            at,
            value,
            interpolation,
        })
    }

    /// When.
    #[must_use]
    pub const fn at(self) -> Instant {
        self.at
    }

    /// What.
    #[must_use]
    pub const fn value(self) -> Rational {
        self.value
    }

    /// How it leaves.
    #[must_use]
    pub const fn interpolation(self) -> Interpolation {
        self.interpolation
    }
}

/// Which of a track's parameters a curve belongs to.
///
/// A track has two automation lanes and they are not interchangeable: one
/// holds an opacity from nought to one and only a picture track has it, the
/// other holds decibels and only a sound track has it. Naming the lane rather
/// than inferring it from the track's kind means an edit written for the wrong
/// lane is refused rather than quietly applied to the only one available.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Automation {
    /// How see-through a picture track is.
    Opacity,
    /// Where a sound track's fader sits, in decibels.
    Level,
}

/// One change to one keyframe.
///
/// A nested operation rather than four more [`crate::Edit`] variants, and the
/// reason is [`crate::Edit::apply`]: a dispatch with one arm per operation
/// grows with the number of operations, and splitting that dispatch would need
/// an arm for every variant the split does not handle — a branch nothing
/// reaches and no test can cover. Nesting gives one arm at the top and a match
/// underneath that is exhaustive over exactly these four.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeyframeEdit {
    /// Put a keyframe in. On a lane with no automation this starts one.
    Add(Keyframe),
    /// Take the keyframe at an instant out. Taking the last one out turns the
    /// automation off, which is what makes it the exact inverse of adding the
    /// first.
    Remove(Instant),
    /// Move a keyframe in time, keeping what it holds.
    Move {
        /// Where it is.
        from: Instant,
        /// Where it goes.
        to: Instant,
    },
    /// Change what a keyframe holds, keeping when it is.
    Set {
        /// Which keyframe.
        at: Instant,
        /// Its new value.
        value: Rational,
        /// How it now reaches the next one.
        interpolation: Interpolation,
    },
}

/// One parameter's value over time.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Curve {
    /// In time order, strictly increasing, never empty.
    keyframes: Vec<Keyframe>,
}

impl Curve {
    /// A curve that holds one value forever.
    ///
    /// The shape a parameter has before anybody animates it, and the reason
    /// there is no such thing as an empty curve: a parameter always has a
    /// value, and a curve with no keyframes could not say what it is.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::OutOfMemory`].
    pub fn constant(at: Instant, value: Rational) -> Result<Self> {
        let mut keyframes = Vec::new();
        push_bounded(
            &mut keyframes,
            Keyframe::new(at, value, Interpolation::Hold)?,
            MAX_KEYFRAMES,
        )?;
        Ok(Self { keyframes })
    }

    /// Gather keyframes into a curve.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyCurve`], [`ModelStatus::CapacityExhausted`] past
    /// [`MAX_KEYFRAMES`], [`ModelStatus::KeyframesOutOfOrder`] if they are not
    /// strictly increasing in time — which covers two at one instant, because
    /// a parameter with two values at one moment has none — and
    /// [`ModelStatus::MixedTimebases`] if they are not all counted the same
    /// way.
    pub fn new(keyframes: Vec<Keyframe>) -> Result<Self> {
        if keyframes.is_empty() {
            return Err(ModelStatus::EmptyCurve);
        }
        if keyframes.len() > MAX_KEYFRAMES {
            return Err(ModelStatus::CapacityExhausted);
        }
        for pair in keyframes.windows(2) {
            if pair[0].at.timebase() != pair[1].at.timebase() {
                return Err(ModelStatus::MixedTimebases);
            }
            if pair[1].at.ticks() <= pair[0].at.ticks() {
                // Sorting them here would be deciding that the caller meant
                // something other than what it said, and two keyframes at one
                // instant have no order to sort into anyway (R-1.3).
                return Err(ModelStatus::KeyframesOutOfOrder);
            }
        }
        for keyframe in &keyframes {
            keyframe.interpolation.check()?;
        }
        Ok(Self { keyframes })
    }

    /// Put a keyframe in, keeping the order.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::KeyframeExists`] if one is already at that instant — a
    /// parameter with two values at one moment has none —
    /// [`ModelStatus::WrongTimebase`] for one counted another way,
    /// [`ModelStatus::CapacityExhausted`] past [`MAX_KEYFRAMES`], and
    /// [`ModelStatus::HandleOutOfSpan`] for an ease that folds.
    pub fn insert(&mut self, keyframe: Keyframe) -> Result<()> {
        if keyframe.at.timebase() != self.timebase() {
            return Err(ModelStatus::WrongTimebase);
        }
        keyframe.interpolation.check()?;
        let ticks = keyframe.at.ticks();
        let Err(at) = self.find(ticks) else {
            return Err(ModelStatus::KeyframeExists);
        };
        insert_bounded(&mut self.keyframes, at, keyframe, MAX_KEYFRAMES)
    }

    /// Take a keyframe out.
    ///
    /// The last one cannot be taken out here: a curve with no keyframes could
    /// not say what the parameter is, so removing the last is a different
    /// operation — turning the automation off — and belongs to whatever owns
    /// the curve rather than to the curve.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NoSuchKeyframe`] and [`ModelStatus::LastKeyframe`].
    pub fn remove(&mut self, at: Instant) -> Result<Keyframe> {
        if at.timebase() != self.timebase() {
            return Err(ModelStatus::WrongTimebase);
        }
        if self.keyframes.len() == 1 {
            return Err(ModelStatus::LastKeyframe);
        }
        let index = self
            .find(at.ticks())
            .map_err(|_| ModelStatus::NoSuchKeyframe)?;
        Ok(self.keyframes.remove(index))
    }

    /// Move a keyframe in time, keeping its value and its interpolation.
    ///
    /// A keyframe may move past its neighbours, which reorders the curve and
    /// is a thing an editor does on purpose. It may not land on one: two at an
    /// instant is the same nothing as none.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NoSuchKeyframe`] if nothing is at `from`,
    /// [`ModelStatus::KeyframeExists`] if something is at `to`, and
    /// [`ModelStatus::WrongTimebase`].
    pub fn move_keyframe(&mut self, from: Instant, to: Instant) -> Result<()> {
        if from.timebase() != self.timebase() || to.timebase() != self.timebase() {
            return Err(ModelStatus::WrongTimebase);
        }
        if from.ticks() == to.ticks() {
            return Ok(());
        }
        let index = self
            .find(from.ticks())
            .map_err(|_| ModelStatus::NoSuchKeyframe)?;
        if self.find(to.ticks()).is_ok() {
            return Err(ModelStatus::KeyframeExists);
        }
        let mut moved = self.keyframes.remove(index);
        moved.at = to;
        // Searched again after the removal rather than adjusted, because the
        // index the first search gave is relative to a list this one is no
        // longer in — and an off-by-one here silently reorders a curve.
        // Nothing is at `to`, checked above, so the search cannot find one.
        let Err(at) = self.find(to.ticks()) else {
            return Err(ModelStatus::KeyframeExists);
        };
        insert_bounded(&mut self.keyframes, at, moved, MAX_KEYFRAMES)
    }

    /// Change what a keyframe holds, keeping when it is.
    ///
    /// Separate from [`Curve::move_keyframe`] because they are different
    /// gestures with different risks: this one cannot reorder anything, and
    /// that one cannot change a value. Hands back what was there, so applying
    /// and inverting are one operation.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NoSuchKeyframe`], [`ModelStatus::WrongTimebase`], and
    /// [`ModelStatus::HandleOutOfSpan`].
    pub fn set(
        &mut self,
        at: Instant,
        value: Rational,
        interpolation: Interpolation,
    ) -> Result<(Rational, Interpolation)> {
        if at.timebase() != self.timebase() {
            return Err(ModelStatus::WrongTimebase);
        }
        interpolation.check()?;
        let index = self
            .find(at.ticks())
            .map_err(|_| ModelStatus::NoSuchKeyframe)?;
        let held = &mut self.keyframes[index];
        let previous = (held.value, held.interpolation);
        held.value = value;
        held.interpolation = interpolation;
        Ok(previous)
    }

    /// Where a tick is, or where it would go.
    ///
    /// `Ok(index)` for a keyframe exactly there, `Err(index)` for the place it
    /// would be inserted. The same shape as the standard library's binary
    /// search, because a caller that wants one of those answers almost always
    /// wants to know which it got.
    fn find(&self, ticks: i64) -> core::result::Result<usize, usize> {
        self.keyframes
            .binary_search_by_key(&ticks, |keyframe| keyframe.at.ticks())
    }

    /// The same curve with every keyframe moved by `by` ticks.
    ///
    /// Which is what a cut through an animated clip needs. A curve on a clip
    /// is measured from that clip's own start, so a split that gives the tail
    /// a new start must move the tail's keyframes back by however far into the
    /// original the cut fell.
    ///
    /// The keyframes that end up before nought stay. Dropping them would be
    /// the obvious tidy and it would change what the tail reads: a curve holds
    /// its first value before its first keyframe, so a pair straddling the cut
    /// is exactly the pair that says what the tail's opening frames do, and
    /// discarding the earlier half of it would flatten a move already underway
    /// into a hold. A keyframe at a negative instant is not a keyframe before
    /// the programme began — it is one before *this clip* began, which is an
    /// ordinary thing for a cut to produce.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn shifted(&self, by: i64) -> Result<Self> {
        let timebase = self.timebase();
        let mut keyframes = Vec::new();
        for keyframe in &self.keyframes {
            let ticks = keyframe
                .at
                .ticks()
                .checked_add(by)
                .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?;
            push_bounded(
                &mut keyframes,
                Keyframe {
                    at: Instant::new(ticks, timebase),
                    ..*keyframe
                },
                MAX_KEYFRAMES,
            )?;
        }
        // Shifting every keyframe by the same amount preserves the order and
        // the count, so the invariants `Curve::new` checks are already held
        // and re-checking them would only be able to disagree with itself.
        Ok(Self { keyframes })
    }

    /// The keyframes, in time order.
    #[must_use]
    pub fn keyframes(&self) -> &[Keyframe] {
        &self.keyframes
    }

    /// How the parameter is counted.
    #[must_use]
    pub fn timebase(&self) -> media_editor_core::Timebase {
        self.keyframes[0].at.timebase()
    }

    /// The value at an instant.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::WrongTimebase`] for an instant counted differently from
    /// the curve, and [`ModelStatus::Time`] on arithmetic that will not fit.
    pub fn value_at(&self, instant: Instant) -> Result<Rational> {
        if instant.timebase() != self.timebase() {
            return Err(ModelStatus::WrongTimebase);
        }
        let ticks = instant.ticks();

        // Held at both ends. A curve describes what happens between the
        // moments somebody set, and nothing at all about what happens outside
        // them.
        let first = self.keyframes[0];
        if ticks <= first.at.ticks() {
            return Ok(first.value);
        }
        let last = self.keyframes[self.keyframes.len() - 1];
        if ticks >= last.at.ticks() {
            return Ok(last.value);
        }

        let index = self.segment(ticks);
        let from = self.keyframes[index];
        let to = self.keyframes[index + 1];
        let span = to.at.ticks() - from.at.ticks();
        let elapsed = ticks - from.at.ticks();

        match from.interpolation {
            Interpolation::Hold => Ok(from.value),
            Interpolation::Linear => {
                let fraction = Rational::new(elapsed, span)?;
                between(from.value, to.value, fraction)
            }
            Interpolation::Ease {
                out_x,
                out_y,
                in_x,
                in_y,
            } => {
                let parameter = invert(out_x, in_x, elapsed, span)?;
                let fraction = ease_fraction(out_y, in_y, parameter)?;
                between(from.value, to.value, fraction)
            }
        }
    }

    /// The exact area under the curve, from tick nought to `instant`.
    ///
    /// The integral, and the reason it exists: a clip whose *speed* is a curve
    /// reads media at a position that is the area under that curve rather than
    /// a multiple of it. Nothing else in this model has needed one, and
    /// nothing else in it may: the answer is a rational only because the
    /// shapes below are the ones it can be a rational for.
    ///
    /// **Held at both ends, like [`Curve::value_at`]**, so the area before the
    /// first keyframe is a rectangle of its value and the area past the last
    /// is a rectangle of that one. That is not a convenience — it is what lets
    /// a ramp be written as two keyframes in the middle of a clip and still
    /// answer at every tick of it.
    ///
    /// From tick *nought* rather than from the first keyframe, because every
    /// curve in this model is measured from the start of the thing it belongs
    /// to.
    ///
    /// Computed as the difference of two walks from the first keyframe rather
    /// than as one walk from nought, and that is not a style: a curve
    /// **re-based by a cut** has keyframes below nought, and a walk that
    /// started at nought would have to charge the held first value for a
    /// stretch the curve was not held over. It came out as six and a quarter
    /// ticks of media on the first tail that was asked, which is a frame and a
    /// half of drift at the first cut through a ramp.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::WrongTimebase`] for an instant counted differently,
    /// [`ModelStatus::EaseHasNoExactArea`] if either walk **reaches** an ease,
    /// and [`ModelStatus::Time`] on arithmetic that will not fit.
    pub fn area_to(&self, instant: Instant) -> Result<Rational> {
        if instant.timebase() != self.timebase() {
            return Err(ModelStatus::WrongTimebase);
        }
        Ok(self
            .walked_from_first(instant.ticks())?
            .checked_sub(self.walked_from_first(0)?)?)
    }

    /// The area from the first keyframe to `ticks`, which may be either side.
    ///
    /// One reference point for both ends of the subtraction above, which is
    /// what makes the answer independent of where the curve happens to sit.
    fn walked_from_first(&self, ticks: i64) -> Result<Rational> {
        let first = self.keyframes[0];
        if ticks <= first.at.ticks() {
            // Held before the first keyframe, and the area is negative when
            // the instant is below it -- which is the direction a re-based
            // lane asks in.
            return Ok(first
                .value
                .checked_mul(Rational::from_integer(ticks - first.at.ticks()))?);
        }
        let mut total = Rational::ZERO;
        for pair in self.keyframes.windows(2) {
            let (from, to) = (pair[0], pair[1]);
            if ticks <= from.at.ticks() {
                // Everything after this is past the instant. Leaving the loop
                // here is also what lets a curve whose ease lies beyond the
                // instant answer at all -- the refusal is about an ease the
                // walk goes *through*, not one it stops short of.
                break;
            }
            let span = to.at.ticks() - from.at.ticks();
            let elapsed = (ticks - from.at.ticks()).min(span);
            total = total.checked_add(area_of(from, to, elapsed, span)?)?;
        }
        let last = self.keyframes[self.keyframes.len() - 1];
        if ticks > last.at.ticks() {
            total = total.checked_add(
                last.value
                    .checked_mul(Rational::from_integer(ticks - last.at.ticks()))?,
            )?;
        }
        Ok(total)
    }

    /// Which pair of keyframes an instant falls between.
    ///
    /// Only ever called for an instant strictly inside the curve, so there is
    /// always such a pair.
    fn segment(&self, ticks: i64) -> usize {
        let mut low = 0;
        let mut high = self.keyframes.len() - 1;
        while high - low > 1 {
            let middle = low + (high - low) / 2;
            if self.keyframes[middle].at.ticks() <= ticks {
                low = middle;
            } else {
                high = middle;
            }
        }
        low
    }
}

/// The area under one segment, `elapsed` ticks into a span of `span`.
///
/// Two shapes, and each is exact for a reason worth having written down.
///
/// **Hold** is a rectangle: `v x E`.
///
/// **Linear** is a trapezium, and the arithmetic is
/// `integral of a + (b-a)e/span de = a x E + (b-a)/2 x (E/span) x E`. Written
/// with the fraction `E/span` taken *first* rather than as `E^2/(2 x span)`,
/// because the fraction is never above one and so the product is never larger
/// than the area itself. Squaring the tick count first would overflow a
/// sixty-four bit numerator on a ramp of a few thousand frames, and would do
/// it while computing a number that fits.
///
/// **Ease** is refused, and the refusal is the interesting one. Over a *whole*
/// segment the area under a cubic Bézier is exactly rational — the integral of
/// `y ds/dx dx` is a polynomial in the parameter, and at the ends the
/// parameter is nought and one. Part way through a segment it is not: finding
/// the parameter at a given tick means solving a cubic, which is the cube root
/// [`EASE_BITS`] exists to approximate. A clip's source position is asked for
/// at *every* tick, so the case that would be exact is never the case that is
/// asked, and answering the approximation would put a frame of drift into
/// something whose whole claim is that it does not drift.
fn area_of(from: Keyframe, to: Keyframe, elapsed: i64, span: i64) -> Result<Rational> {
    let along = Rational::from_integer(elapsed);
    match from.interpolation {
        Interpolation::Hold => Ok(from.value.checked_mul(along)?),
        Interpolation::Linear => {
            let fraction = Rational::new(elapsed, span)?;
            let half = to
                .value
                .checked_sub(from.value)?
                .checked_mul(Rational::new(1, 2)?)?;
            Ok(from
                .value
                .checked_mul(along)?
                .checked_add(half.checked_mul(fraction)?.checked_mul(along)?)?)
        }
        Interpolation::Ease { .. } => Err(ModelStatus::EaseHasNoExactArea),
    }
}

/// A value a fraction of the way from one to another.
///
/// Written as `from + (to - from) * fraction` rather than
/// `from * (1 - fraction) + to * fraction`, because the first form gives
/// exactly `from` at nought and exactly `to` at one whatever the values are,
/// and the second only does so when the arithmetic happens to be exact. It is
/// exact here — these are rationals — but the habit is what matters, since the
/// same expression written in fixed point would not be.
fn between(from: Rational, to: Rational, fraction: Rational) -> Result<Rational> {
    let change = to.checked_sub(from)?;
    Ok(from.checked_add(change.checked_mul(fraction)?)?)
}

/// A cubic Bézier's value at `t`, as a numerator over a returned denominator.
///
/// `B(t) = 3(1-t)²t·a + 3(1-t)t²·b + t³`, which is the standard cubic with its
/// first control at nought and its last at one — the two the editor never
/// drags, because a curve that did not begin where the keyframe is would not
/// be a curve through the keyframe.
///
/// `t` arrives as a numerator over [`EASE_SCALE`]. Everything is done in
/// 128-bit integers over a common denominator rather than in [`Rational`],
/// because a rational cubic cubes its denominator and a fraction over two to
/// the twentieth would become one over two to the sixtieth before the handles
/// were even multiplied in.
///
/// The denominator comes back with the numerator rather than being assumed,
/// so that the one place that divides is the one place that decides how to
/// round.
fn bezier(a: Rational, b: Rational, t: i128) -> Result<(i128, i128)> {
    let (an, ad) = (i128::from(a.numerator()), i128::from(a.denominator()));
    let (bn, bd) = (i128::from(b.numerator()), i128::from(b.denominator()));
    let rest = EASE_SCALE - t;

    // Over 2^(3·EASE_BITS)·ad·bd, which is the common denominator of all three
    // terms: each is a cubic in a fraction over EASE_SCALE, and each carries
    // one handle's denominator.
    let first = mul(mul(mul(mul(3, rest)?, rest)?, t)?, mul(an, bd)?)?;
    let second = mul(mul(mul(mul(3, rest)?, t)?, t)?, mul(bn, ad)?)?;
    let third = mul(mul(mul(t, t)?, t)?, mul(ad, bd)?)?;
    let numerator = add(add(first, second)?, third)?;
    let denominator = mul(mul(mul(EASE_SCALE, EASE_SCALE)?, EASE_SCALE)?, mul(ad, bd)?)?;
    Ok((numerator, denominator))
}

/// The `t` at which the horizontal Bézier reaches `elapsed / span`.
///
/// Bisection, [`EASE_BITS`] times, on a dyadic parameter. The answer is a
/// halving of the unit interval rather than a solution of the cubic, because
/// the cubic's solution needs a cube root and is not rational — so the choice
/// is between an approximation whose size is stated and one that is not.
///
/// `x(t)` is non-decreasing whenever the handles are inside the span, which
/// [`Interpolation::check`] guarantees, so the bisection converges on the one
/// answer there is.
///
/// The comparison is a cross-multiplication rather than a division, so no step
/// of the search rounds and the answer does not depend on where the search
/// started.
fn invert(a: Rational, b: Rational, elapsed: i64, span: i64) -> Result<i128> {
    let elapsed = i128::from(elapsed);
    let span = i128::from(span);
    let mut low = 0_i128;
    let mut high = EASE_SCALE;

    // A fixed number of passes rather than a loop that stops when the interval
    // is small, so the work does not depend on the input and neither does the
    // answer. `high - low` starts at EASE_SCALE and halves each pass, so the
    // two ends end up adjacent dyadics with the answer between them.
    while high - low > 1 {
        let middle = low + (high - low) / 2;
        let (numerator, denominator) = bezier(a, b, middle)?;
        if mul(numerator, span)? <= mul(elapsed, denominator)? {
            low = middle;
        } else {
            high = middle;
        }
    }
    Ok(low)
}

/// The vertical Bézier at a parameter, rounded to [`EASE_BITS`] places.
///
/// The one division in the whole thing, and it rounds half away from zero —
/// the same rule the compositor uses, so that a fade drawn by the curve and a
/// fade drawn by the compositor round the same way at the same point.
///
/// A rounding here rather than an exact rational because the exact value has
/// two to the sixtieth in its denominator, times both handles', and the
/// numbers this hands to `between` have to leave room for the parameter's own
/// denominator afterwards.
fn ease_fraction(a: Rational, b: Rational, t: i128) -> Result<Rational> {
    let (numerator, denominator) = bezier(a, b, t)?;
    let scaled = mul(numerator, EASE_SCALE)?;
    let half = denominator / 2;
    let rounded = if scaled >= 0 {
        (scaled + half) / denominator
    } else {
        (scaled - half) / denominator
    };
    let numerator = i64::try_from(rounded).map_err(|_| overflow())?;
    let denominator = i64::try_from(EASE_SCALE).map_err(|_| overflow())?;
    Rational::new(numerator, denominator).map_err(ModelStatus::Time)
}

/// Multiply, or say it does not fit.
///
/// The ease works in 128-bit integers and still has a ceiling: a handle
/// written as a fraction with a very large denominator, over a very long span,
/// runs past it. That is a refusal rather than a wrap, because a curve that
/// wrapped would draw a fade that leapt backwards at one frame and nowhere
/// else — the hardest kind of fault to find by looking.
fn mul(left: i128, right: i128) -> Result<i128> {
    left.checked_mul(right).ok_or_else(overflow)
}

/// Add, or say it does not fit.
fn add(left: i128, right: i128) -> Result<i128> {
    left.checked_add(right).ok_or_else(overflow)
}

/// The refusal for arithmetic that will not fit.
fn overflow() -> ModelStatus {
    ModelStatus::Time(media_editor_core::CoreStatus::Overflow)
}
