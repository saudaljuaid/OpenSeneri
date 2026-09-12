// SPDX-License-Identifier: GPL-3.0-only
//! What a track is made of.
//!
//! An item is a clip or a gap. Both have a length; only a clip has a source.
//! A gap is an item rather than a hole, because arithmetic over holes is where
//! timelines go wrong: with explicit gaps, a track's contents always tile its
//! whole length with no overlap and no missing region, by construction.

use media_editor_core::{Duration, Instant, Rational, Timebase};

use crate::media::MediaId;
use crate::status::{ModelStatus, Result};

/// A reference to a range of a media asset, placed on a track.
///
/// The source position is counted in the track's timebase. For this milestone
/// a clip's media must share the sequence's timebase; mixed-rate cutting is a
/// later contract with its own conversion rules, not a widening of this one
/// (R-1.2).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Clip {
    media: MediaId,
    source_start: i64,
    duration: Duration,
    grade: Option<crate::media::Digest>,
    mask: Option<crate::mask::Mask>,
    transform: Option<crate::transform::Transform>,
    motion: Option<crate::transform::Motion>,
    /// In ticks of the clip's own timebase.
    ///
    /// Ticks rather than [`Duration`]s, and that is not only sixteen bytes an
    /// item. A `Duration` carries a timebase, and a fade's timebase is the
    /// clip's — so two of them would be the same fact written three times, and
    /// three facts that have to be kept agreeing. The accessors hand back
    /// `Duration`s counted the clip's way, which is what a caller wants.
    fade_in: i64,
    fade_out: i64,
    playback: Playback,
    /// An animation of the mask, if there is a mask to animate.
    ///
    /// Separate from [`Clip::motion`], which animates the *framing*, and
    /// deliberately: a mask glued to the picture is what tracking wants and
    /// exactly wrong for a vignette, which should stay where it was put while
    /// the shot pushes in. Two animations, because they are two questions.
    mask_motion: Option<crate::transform::Motion>,
    /// An opacity that changes over the clip's own length.
    ///
    /// Measured from the clip's start, like [`crate::Motion`] and for the same
    /// reason: a keyframe at tick twelve is twelve ticks into *this clip*, so
    /// sliding a shot down the timeline slides its animation with it and a
    /// ripple that renumbers every item after it renumbers nothing here.
    opacity: Option<crate::curve::Curve>,
    /// How much of the grade is on, over the clip's own length.
    ///
    /// Not *which* look — a digest is not a quantity and two tables have
    /// nothing between them to interpolate. What animates is how far the
    /// picture has travelled from ungraded towards graded, which is the last
    /// place in this model a parameter was a value where it could be a curve.
    ///
    /// Measured from the clip's own start, like the three lanes above it and
    /// for the same reason.
    grade_strength: Option<crate::curve::Curve>,
    /// Notes on this shot, measured from its own start.
    ///
    /// The other half of the pair M8.28 opened. A sequence's markers name
    /// positions in the *programme* and stay where they are put; these name
    /// positions in the *shot* and travel with it — through a move, a trim, a
    /// lift and a cut.
    ///
    /// Kept in time order for the reason the sequence's are: a list of notes
    /// that is not the timeline is a list somebody has to sort before showing.
    markers: alloc::vec::Vec<crate::marker::Marker>,
}

/// How a clip consumes its media.
///
/// Two cases rather than one, and the reason is arithmetic rather than
/// taxonomy. M8.18 refused a speed of nought with a note that it "would show
/// one frame forever and consume no media — a freeze, which is a different
/// edit with a different name", and the second half of that sentence is the
/// tell: a freeze does *not* consume no media. It consumes exactly one frame,
/// and `floor(offset x 0)` cannot say so — it puts [`Clip::source_end`] at the
/// in point, claiming a clip that shows a frame reads none of it. A join, a
/// library check and a reel would all believe it.
///
/// So the freeze is its own case, where `source_end` is the in point plus one
/// and the span is a single tick, and a speed is a speed.
///
/// The third case is a speed that *changes*, and it is the one that costs this
/// enum its [`Copy`]: a ramp is a curve, a curve owns a `Vec`, and R-5.2
/// forbids the boxing that would hide it. That is a real cost paid for a real
/// thing — see [`Playback::Ramp`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Playback {
    /// At a speed, as an exact fraction of real time. Never nought.
    At(Rational),
    /// Held on one frame of media, however long the clip runs.
    Frozen,
    /// At a speed that changes over the clip: a ramp.
    ///
    /// The curve's values are speeds and its instants are measured from the
    /// clip's own start, like the four lanes of animation a clip already
    /// carries. What makes it a different kind of thing from all four is where
    /// the answer comes from: an opacity at an offset is the curve's *value*
    /// there, and a source position at an offset is the curve's **area** up to
    /// there. A ramp integrates.
    ///
    /// That is why [`crate::curve::Curve::area_to`] exists and why it refuses
    /// an ease: the area under a Bézier is exactly rational over a whole
    /// segment and not at the ticks inside one, and a clip is asked at every
    /// tick. So a ramp's keyframes hold or run straight, and the ramp is exact
    /// at every tick of every clip that carries one.
    ///
    /// Its speed may not change sign. A ramp that turned around would read
    /// part of its media twice and would put [`Clip::source_span`]'s extremes
    /// somewhere other than at its ends -- which is a different feature, with
    /// a different span check, and it would have to say so.
    Ramp(crate::curve::Curve),
}

impl Playback {
    /// This clip, playing the way this says.
    ///
    /// Here rather than at the two call sites, and that is the same lesson the
    /// source bound taught one commit earlier: `Edit::apply` and
    /// `Project::validate` both need the clip an edit *would* produce, and two
    /// matches building it is one construction written twice with nothing to
    /// say the day they stop agreeing.
    ///
    /// # Errors
    ///
    /// Whatever [`Clip::with_speed`] or [`Clip::with_ramp`] refuses. A freeze
    /// refuses nothing: it shrinks what a clip reads to the one frame it
    /// already began at.
    pub fn applied_to(&self, clip: &Clip) -> Result<Clip> {
        match self {
            Self::At(speed) => clip.with_speed(*speed),
            Self::Frozen => Ok(clip.frozen()),
            Self::Ramp(curve) => clip.with_ramp(curve.clone()),
        }
    }
}

impl Clip {
    /// Refer to `duration` of `media`, beginning `source_start` ticks in.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length, and
    /// [`ModelStatus::SourceBeforeStart`] for a negative source position.
    pub fn new(media: MediaId, source_start: i64, duration: Duration) -> Result<Self> {
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        if source_start < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        Ok(Self {
            media,
            source_start,
            duration,
            grade: None,
            mask: None,
            transform: None,
            motion: None,
            fade_in: 0,
            fade_out: 0,
            playback: Playback::At(Rational::ONE),
            mask_motion: None,
            opacity: None,
            grade_strength: None,
            markers: alloc::vec::Vec::new(),
        })
    }

    /// The media this cuts from.
    #[must_use]
    pub const fn media(&self) -> MediaId {
        self.media
    }

    /// The first tick of the media this uses.
    #[must_use]
    pub const fn source_start(&self) -> i64 {
        self.source_start
    }

    /// How much of the media this uses.
    #[must_use]
    pub const fn duration(&self) -> Duration {
        self.duration
    }

    /// How this clip consumes its media: at a speed, on a ramp, or held on
    /// one frame.
    ///
    /// By reference, because a ramp carries a curve and R-5.2 has opinions
    /// about copies nobody asked for.
    #[must_use]
    pub const fn playback(&self) -> &Playback {
        &self.playback
    }

    /// How fast this clip plays its media, or nothing if it is frozen.
    ///
    /// One is real time. A half is slow motion — two ticks of the timeline for
    /// one of the media — and two is fast. A **negative** speed runs the media
    /// backwards from the clip's in point.
    ///
    /// An exact rational, and that is not decoration: a clip at 24/25 is the
    /// standard PAL pull-down and a clip at 0.96 is a rounding of it that
    /// drifts a frame every twenty-five seconds. A speed nobody can write down
    /// exactly is a speed two builds can disagree about.
    ///
    /// [`None`] for a freeze, which is not a slow speed. A speed however small
    /// still moves through the media eventually; a freeze never does, and the
    /// two answer [`Clip::source_end`] differently.
    ///
    /// [`None`] for a **ramp** too, and for the plainer reason: a clip whose
    /// speed changes has no one speed to hand back. Ask
    /// [`Clip::playback`] for the curve, or [`Clip::source_at`] for where it
    /// has got to.
    #[must_use]
    pub const fn speed(&self) -> Option<Rational> {
        match self.playback {
            Playback::At(speed) => Some(speed),
            Playback::Frozen | Playback::Ramp(_) => None,
        }
    }

    /// Whether this clip plays its media in real time and forwards.
    ///
    /// Worth asking for the reason [`crate::Transform::is_still`] is: a clip
    /// nobody has retimed must map its ticks by *addition* rather than by an
    /// exact multiply that happens to be the identity.
    #[must_use]
    pub fn is_real_time(&self) -> bool {
        self.playback == Playback::At(Rational::ONE)
    }

    /// Whether this clip is held on one frame.
    #[must_use]
    pub fn is_frozen(&self) -> bool {
        self.playback == Playback::Frozen
    }

    /// The animation of this clip's mask, if there is one.
    #[must_use]
    pub const fn mask_motion(&self) -> Option<&crate::transform::Motion> {
        self.mask_motion.as_ref()
    }

    /// The same clip with its mask animated, or held still.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NoMaskToAnimate`] for an animation on a clip with no
    /// mask. The same invariant a motion has against a transform, and for the
    /// same reason: an animation of nothing is a value the model would be
    /// carrying that no sequence of edits could give meaning to, and every
    /// later reader would be entitled to assume it meant something.
    pub fn with_mask_motion(&self, motion: Option<crate::transform::Motion>) -> Result<Self> {
        if motion.is_some() && self.mask.is_none() {
            return Err(ModelStatus::NoMaskToAnimate);
        }
        Ok(Self {
            mask_motion: motion,
            ..self.clone()
        })
    }

    /// This clip's mask as it stands `offset` ticks in.
    ///
    /// Resolved here rather than handed out as a shape and an animation
    /// beside it, exactly as the framing is: by the time a layer describes a
    /// frame, an animation has already become the shape it reads at that
    /// moment, and nothing below has to be told there was a curve.
    ///
    /// # Errors
    ///
    /// Whatever the curve or the move refuses.
    pub fn mask_at(&self, offset: i64) -> Result<Option<crate::mask::Mask>> {
        let Some(shape) = &self.mask else {
            return Ok(None);
        };
        let Some(motion) = &self.mask_motion else {
            return Ok(Some(shape.clone()));
        };
        let (scale, across, down, turn) = motion.at(media_editor_core::Instant::new(
            offset,
            self.duration.timebase(),
        ))?;
        Ok(Some(shape.moved_by(scale, across, down, turn)?))
    }

    /// The curve driving this clip's opacity, if there is one.
    #[must_use]
    pub const fn opacity(&self) -> Option<&crate::curve::Curve> {
        self.opacity.as_ref()
    }

    /// The same clip with its opacity animated, or held still.
    ///
    /// [`None`] is not the same as a curve holding one: it is a clip with no
    /// animation at all, which is what lets an animation be switched off and
    /// back on without losing the shape somebody drew. The same distinction a
    /// track's automation makes.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::WrongTimebase`] for a curve counted another way than the
    /// clip is. A curve and a clip that disagree about what a tick is would
    /// read the animation at the wrong frames, silently.
    pub fn with_opacity(&self, opacity: Option<crate::curve::Curve>) -> Result<Self> {
        if let Some(curve) = &opacity {
            if curve.timebase() != self.duration.timebase() {
                return Err(ModelStatus::WrongTimebase);
            }
        }
        Ok(Self {
            opacity,
            ..self.clone()
        })
    }

    /// What this clip's own opacity reads, `offset` ticks into it.
    ///
    /// One when there is no curve, which is the neutral value for something
    /// that multiplies. **Clamped** to nought and one, exactly as a track's
    /// automation is: an ease between two legal keyframes can overshoot on the
    /// way, deliberately, and a layer at more than full coverage is a frame
    /// the compositor refuses. One rule for both, rather than two that have to
    /// agree.
    ///
    /// # Errors
    ///
    /// Whatever the curve refuses.
    pub fn opacity_at(&self, offset: i64) -> Result<Rational> {
        let Some(curve) = &self.opacity else {
            return Ok(Rational::ONE);
        };
        let held = curve.value_at(media_editor_core::Instant::new(
            offset,
            self.duration.timebase(),
        ))?;
        Ok(held.clamp(Rational::ZERO, Rational::ONE))
    }

    /// The curve bringing this clip's grade on, if there is one.
    #[must_use]
    pub const fn grade_strength(&self) -> Option<&crate::curve::Curve> {
        self.grade_strength.as_ref()
    }

    /// The same clip with its grade brought on over time, or applied flat.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NoGradeToAnimate`] for a curve on a clip with no grade.
    /// The same invariant a motion has against a transform and a mask
    /// animation has against a mask, for the same reason: a strength with no
    /// look to be the strength *of* is a value no sequence of edits could give
    /// meaning to, and every later reader would be entitled to assume it meant
    /// something.
    ///
    /// [`ModelStatus::WrongTimebase`] for a curve counted another way than the
    /// clip is, which would read the animation at the wrong frames silently.
    pub fn with_grade_strength(&self, strength: Option<crate::curve::Curve>) -> Result<Self> {
        if let Some(curve) = &strength {
            if self.grade.is_none() {
                return Err(ModelStatus::NoGradeToAnimate);
            }
            if curve.timebase() != self.duration.timebase() {
                return Err(ModelStatus::WrongTimebase);
            }
        }
        Ok(Self {
            grade_strength: strength,
            ..self.clone()
        })
    }

    /// How far this clip has travelled from ungraded to graded, `offset` ticks
    /// in.
    ///
    /// **One** when there is no curve, which is the neutral value here and is
    /// worth saying why: neutral for something that multiplies is one, and
    /// neutral for a grade is *fully applied*, because that is what a graded
    /// clip has done since grades existed. A default of nought would have
    /// turned off every look in every project written before this milestone.
    ///
    /// **Clamped** to nought and one, exactly as [`Clip::opacity_at`] is and
    /// for a reason of the same shape: an ease between two legal keyframes can
    /// overshoot on the way, deliberately, and a strength past one asks the
    /// picture to continue *beyond* the look — code values on the far side of
    /// what the table was sampled at, which is arithmetic rather than a grade.
    /// One rule for both, rather than two that have to agree.
    ///
    /// # Errors
    ///
    /// Whatever the curve refuses.
    pub fn grade_strength_at(&self, offset: i64) -> Result<Rational> {
        let Some(curve) = &self.grade_strength else {
            return Ok(Rational::ONE);
        };
        let held = curve.value_at(media_editor_core::Instant::new(
            offset,
            self.duration.timebase(),
        ))?;
        Ok(held.clamp(Rational::ZERO, Rational::ONE))
    }

    /// The same clip held on the frame at its in point.
    ///
    /// Infallible, and that is a property rather than an oversight: a freeze
    /// *shrinks* what a clip reads to the single frame it already began at, so
    /// there is no bound it can cross that the clip was not already inside.
    /// Lengthening a frozen clip reads no more media either, which is what
    /// makes a still hold as long as somebody wants.
    #[must_use]
    pub fn frozen(&self) -> Self {
        Self {
            playback: Playback::Frozen,
            ..self.clone()
        }
    }

    /// The same clip played at a different speed.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::SpeedNotUsable`] for a speed of nought, which would show
    /// one frame forever and consume no media — a freeze, which is a different
    /// edit with a different name. [`ModelStatus::SourceBeforeStart`] if the
    /// clip would have to read before its media begins, which a negative speed
    /// can ask for. [`ModelStatus::Time`] wrapping an overflow.
    pub fn with_speed(&self, speed: Rational) -> Result<Self> {
        if speed.is_zero() {
            return Err(ModelStatus::SpeedNotUsable);
        }
        let retimed = Self {
            playback: Playback::At(speed),
            ..self.clone()
        };
        // Checked here rather than at the frame that reads it. A clip whose
        // last frame is before its media began is a clip the library will
        // refuse, and the editor who set the speed is the one who can do
        // something about it.
        //
        // Only the *lower* end, because that is the one this can answer alone:
        // media begins at nought for everybody, and where it ends is a fact
        // about the asset, which a clip does not carry. The upper end is the
        // library's, in `Project::validate`, where the asset is at hand.
        if retimed.source_span()?.0 < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        Ok(retimed)
    }

    /// The same clip played on a ramp: a speed that changes over its length.
    ///
    /// The curve's values are speeds and its instants are measured from this
    /// clip's own start, held at both ends -- so a two-keyframe curve in the
    /// middle of a clip is a plateau, a ramp, and a plateau, which is the
    /// gesture an editor actually makes.
    ///
    /// What it refuses is what makes it exact, and each refusal is a decision:
    ///
    /// - a keyframe at nought, because a speed of nought is a freeze and the
    ///   ramp would stop consuming media without saying so;
    /// - a **sign change**, because a ramp that turns around reads part of its
    ///   media twice and puts [`Clip::source_span`]'s extremes somewhere other
    ///   than at its ends. Checking the keyframes is enough to know: between
    ///   two of one sign a hold stays at the first and a straight run stays
    ///   between the two, so neither shape can cross nought in the middle --
    ///   which is another thing the ease would have broken, since a handle
    ///   past one is an overshoot and an overshoot can;
    /// - an **ease**, for the reason [`crate::curve::Curve::area_to`] gives.
    ///   Refused over the *whole* curve rather than only where the clip walks,
    ///   which is the half of it a control had to be written twice to hold: a
    ///   ramp whose ease sits past the clip's last tick is never integrated
    ///   through, so nothing downstream would notice — until somebody
    ///   lengthened the clip, and a trim made a clip unreadable at a distance.
    ///   The door is where a refusal belongs (R-1.3).
    ///
    /// There was a timebase check here too, and its negative control could not
    /// be made to fail. `source_span` below asks `area_to` for the area at a
    /// tick counted in the *clip's* timebase, and a curve counted another way
    /// refuses that — with the same [`ModelStatus::WrongTimebase`], from four
    /// lines further down. A guard whose absence changes no answer is a guard
    /// no test can hold, which is the same finding
    /// [`Clip::source_at`] records about its own missing real-time arm. It
    /// went; the test that pins the refusal stayed.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::WrongTimebase`] for a curve counted another way,
    /// [`ModelStatus::SpeedNotUsable`], [`ModelStatus::SpeedRampChangesDirection`],
    /// [`ModelStatus::EaseHasNoExactArea`], [`ModelStatus::SourceBeforeStart`]
    /// if the ramp would read below where its media begins, and
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn with_ramp(&self, curve: crate::curve::Curve) -> Result<Self> {
        let leading = curve.keyframes()[0].value();
        if leading.is_zero() {
            return Err(ModelStatus::SpeedNotUsable);
        }
        for keyframe in curve.keyframes() {
            if keyframe.value().is_zero() {
                return Err(ModelStatus::SpeedNotUsable);
            }
            if keyframe.value().is_positive() != leading.is_positive() {
                return Err(ModelStatus::SpeedRampChangesDirection);
            }
            if let crate::curve::Interpolation::Ease { .. } = keyframe.interpolation() {
                return Err(ModelStatus::EaseHasNoExactArea);
            }
        }
        let ramped = Self {
            playback: Playback::Ramp(curve),
            ..self.clone()
        };
        // The same lower-end check `with_speed` makes, for the same reason and
        // at the same place: a ramp running backwards can ask to read below
        // where its media begins, and the editor who drew it is the one who
        // can do something about that.
        if ramped.source_span()?.0 < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        Ok(ramped)
    }

    /// The notes on this shot, in time order.
    #[must_use]
    pub fn markers(&self) -> &[crate::marker::Marker] {
        &self.markers
    }

    /// The note at an offset from this clip's start, if there is one.
    #[must_use]
    pub fn marker_at(&self, at: media_editor_core::Instant) -> Option<&crate::marker::Marker> {
        self.markers.iter().find(|held| held.at() == at)
    }

    /// This clip with a note on it.
    ///
    /// There is deliberately **no upper bound** on where a note may sit. A
    /// note past the clip's last tick is carried and not shown, which is what
    /// makes a trim a trim rather than a delete: shorten a shot and the notes
    /// on the part you hid are still there when you pull it back out. A curve
    /// on a clip is treated the same way — held past its end rather than
    /// refused for reaching past it.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::WrongTimebase`] for a note counted another way,
    /// [`ModelStatus::MarkerBeforeStart`] for one before the clip begins,
    /// [`ModelStatus::MarkerExists`] for one where there is already a note,
    /// and [`ModelStatus::CapacityExhausted`] past
    /// [`crate::marker::MAX_MARKERS_PER_CLIP`].
    pub fn with_marker(&self, marker: crate::marker::Marker) -> Result<Self> {
        if marker.at().timebase() != self.duration.timebase() {
            return Err(ModelStatus::WrongTimebase);
        }
        if marker.at().ticks() < 0 {
            return Err(ModelStatus::MarkerBeforeStart);
        }
        let mut place = self.markers.len();
        for (index, held) in self.markers.iter().enumerate() {
            if held.at() == marker.at() {
                return Err(ModelStatus::MarkerExists);
            }
            if held.at().ticks() > marker.at().ticks() {
                place = index;
                break;
            }
        }
        let mut noted = self.clone();
        crate::bounded::insert_bounded(
            &mut noted.markers,
            place,
            marker,
            crate::marker::MAX_MARKERS_PER_CLIP,
        )?;
        Ok(noted)
    }

    /// This clip without the note at an offset, and the note that came off.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NoSuchMarker`] if there is no note there.
    pub fn without_marker(
        &self,
        at: media_editor_core::Instant,
    ) -> Result<(Self, crate::marker::Marker)> {
        let place = self
            .markers
            .iter()
            .position(|held| held.at() == at)
            .ok_or(ModelStatus::NoSuchMarker)?;
        let mut bare = self.clone();
        let taken = bare.markers.remove(place);
        Ok((bare, taken))
    }

    /// This clip keeping only the notes below `offset`.
    fn noted_before(&self, offset: i64) -> Self {
        let mut kept = self.clone();
        kept.markers.retain(|held| held.at().ticks() < offset);
        kept
    }

    /// The notes at or past `offset`, re-based onto it.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow, and
    /// [`ModelStatus::OutOfMemory`] if the list cannot be reserved.
    fn noted_from(&self, offset: i64) -> Result<alloc::vec::Vec<crate::marker::Marker>> {
        let mut moved = alloc::vec::Vec::new();
        moved
            .try_reserve(self.markers.len())
            .map_err(|_| ModelStatus::OutOfMemory)?;
        for held in &self.markers {
            if held.at().ticks() < offset {
                continue;
            }
            let ticks = held
                .at()
                .ticks()
                .checked_sub(offset)
                .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?;
            moved.push(held.moved_to(media_editor_core::Instant::new(
                ticks,
                self.duration.timebase(),
            ))?);
        }
        Ok(moved)
    }

    /// This clip with `later`'s notes added, each moved on by `by` ticks.
    ///
    /// # Errors
    ///
    /// Whatever [`Clip::with_marker`] refuses — in practice only
    /// [`ModelStatus::CapacityExhausted`], since a join's two halves cannot
    /// collide: everything in the first is below the length being added and
    /// everything arriving from the second is at or above it.
    fn noted_with(&self, later: &Self, by: i64) -> Result<Self> {
        let mut joined = self.clone();
        for held in &later.markers {
            let ticks = held
                .at()
                .ticks()
                .checked_add(by)
                .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?;
            joined = joined.with_marker(held.moved_to(media_editor_core::Instant::new(
                ticks,
                self.duration.timebase(),
            ))?)?;
        }
        Ok(joined)
    }

    /// Which tick of the media this clip shows, `offset` ticks into it.
    ///
    /// The **size** of the travel says how much media passes and the **sign**
    /// says which way: `floor(|travel|)` ticks of media, added to the in point
    /// or subtracted from it. At a fixed speed the travel is `offset x speed`;
    /// on a ramp it is the **area** under the speed curve up to `offset`,
    /// which is the whole of what a ramp is.
    ///
    /// There is no separate arm for real time. There was one, briefly, on the
    /// grounds that a clip nobody has retimed should be added to rather than
    /// multiplied — and its negative control could not be made to fail,
    /// because a speed of one takes `floor(offset x 1) = offset` and the two
    /// arms agree on every input. A guard whose absence changes no answer is
    /// a guard no test can hold, so it went.
    ///
    /// Taking the size before the floor rather than flooring a signed travel
    /// is a decision, and it is the one that makes a reversed clip show
    /// exactly the frames its forward twin shows. Flooring a negative travel
    /// rounds the other way, so a clip reversed at half speed would give
    /// `100, 99, 99, 98` — the in point once and everything after it twice —
    /// against the forward `100, 100, 101, 101`. Same speed, different frames,
    /// for no reason anybody could point at.
    ///
    /// The floor itself is the same floor a sample position takes, and for the
    /// same reason: a tick names a frame rather than a moment, so a position
    /// part way through one is that frame and not the next.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn source_at(&self, offset: i64) -> Result<i64> {
        let travel = match &self.playback {
            // A freeze shows the frame at the in point, at every offset. Not
            // the general mapping with a speed of nought: that would agree
            // here and disagree about `source_end`, which is the whole reason
            // the two are separate cases.
            Playback::Frozen => return Ok(self.source_start),
            Playback::At(speed) => speed
                .checked_mul(Rational::new(offset, 1)?)
                .map_err(ModelStatus::from)?,
            Playback::Ramp(curve) => curve.area_to(media_editor_core::Instant::new(
                offset,
                self.duration.timebase(),
            ))?,
        };
        let forwards = travel.is_positive() || travel.is_zero();
        let size = if forwards {
            travel
        } else {
            Rational::ZERO
                .checked_sub(travel)
                .map_err(ModelStatus::from)?
        };
        let along = floor_of(size);
        if forwards {
            self.source_start.checked_add(along)
        } else {
            self.source_start.checked_sub(along)
        }
        .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))
    }

    /// The offsets of this clip that show a half-open range of the media.
    ///
    /// **The inverse of [`Clip::source_at`]**, and the reason it is a search
    /// rather than a formula is worth the paragraph.
    ///
    /// # Why there is no formula
    ///
    /// At a fixed speed the map is `source = start + floor(offset x speed)`,
    /// and inverting that is a division. On a **ramp** the map is the integral
    /// of the speed curve, and the segments of that curve are held or linear —
    /// so the integral is a rectangle or a **trapezium**, and a trapezium's
    /// area is *quadratic* in how far along the segment you are. Inverting a
    /// quadratic needs a square root, and the square root of a rational is not
    /// a rational.
    ///
    /// That is the same wall [`crate::Interpolation::Ease`] hit from the other
    /// side: a cubic Bézier cannot be evaluated at a *time* without a cube
    /// root, so `area_to` refuses to integrate one at all. Here the shape a
    /// ramp is *allowed* to be is exactly the shape whose forward map is exact
    /// and whose inverse is not.
    ///
    /// # Why a search is not a compromise
    ///
    /// A caption does not need a sub-frame answer. It needs to know **which
    /// frame** the word lands on, and the frames of a clip are an integer
    /// sequence: `source_at(0), source_at(1), …, source_at(duration)`. That
    /// sequence is monotone — a ramp may not change sign and a speed may not
    /// be nought — so "the first offset at which the clip has reached this
    /// tick" is a lower bound over a sorted sequence, found by halving, with
    /// every comparison exact.
    ///
    /// So the answer is not an approximation of the right offset. It is the
    /// right offset, obtained without ever asking a question arithmetic cannot
    /// answer. Twenty comparisons cover a clip a million frames long.
    ///
    /// # A frame **covers** a stretch of the recording; it does not sample one
    ///
    /// The first version of this asked which offsets *land on* a source tick,
    /// and a test caught it in one line. At triple speed the frames show ticks
    /// 100, 103, 106 — so a caption over ticks 104 and 105 lands on no frame
    /// at all and would have vanished. Speed a clip up and two thirds of the
    /// transcript would drop out, silently, chosen by where the sampling
    /// happened to fall.
    ///
    /// So frame `k` shows the half-open source range given by
    /// [`Clip::shows`]: from its own tick to the next frame's, and **at least
    /// one tick wide** where the next frame has not moved on. Every source
    /// tick the clip reads is covered by at least one frame, and no caption
    /// can fall between two.
    ///
    /// That one rule is also what makes the three awkward shapes stop being
    /// special cases:
    ///
    /// **Slow motion** repeats. At half speed the frames show 100, 100, 101,
    /// 101, so ticks 2 and 3 both cover `[101, 102)` and a word held twice as
    /// long is on screen twice as long.
    ///
    /// **A reversed clip** reads downwards, so the ranges descend and the
    /// words come out backwards — which is what a reversed clip does to
    /// everything it shows, and is not a defect to correct here.
    ///
    /// **A freeze** shows one tick throughout, so every frame covers the same
    /// one tick and a caption over it is on screen for the whole clip. There
    /// was a special case for this and it went: the general rule already gives
    /// that answer, and a guard whose absence changes no answer is a guard no
    /// test can hold.
    ///
    /// # Errors
    ///
    /// Whatever [`Clip::source_at`] refuses, which on a ramp includes
    /// [`ModelStatus::EaseHasNoExactArea`].
    pub fn offsets_showing(&self, from: i64, to: i64) -> Result<Option<(i64, i64)>> {
        let length = self.duration.ticks();
        if to <= from || length <= 0 {
            return Ok(None);
        }
        // Which way it reads, asked of the clip rather than of the playback: a
        // fixed speed says so in its sign and a ramp says so in its area, and
        // both answer the same question here.
        let backwards = self.source_at(length)? < self.source_start;
        // Both ends of the shown range move the same way as the clip reads, so
        // each of the two comparisons below is false then true along it —
        // which is what makes halving correct.
        let (start, end) = if backwards {
            (
                self.first_offset(|offset| Ok(self.shows(offset)?.0 < to))?,
                self.first_offset(|offset| Ok(self.shows(offset)?.1 <= from))?,
            )
        } else {
            (
                self.first_offset(|offset| Ok(self.shows(offset)?.1 > from))?,
                self.first_offset(|offset| Ok(self.shows(offset)?.0 >= to))?,
            )
        };
        Ok(if start < end {
            Some((start, end))
        } else {
            None
        })
    }

    /// The half-open range of the media one frame of this clip shows.
    ///
    /// From this frame's tick to the next frame's, in whichever order they
    /// come — and **one tick wide** where the two are the same, which is what
    /// a frame held by slow motion or by a freeze shows.
    ///
    /// # Errors
    ///
    /// Whatever [`Clip::source_at`] refuses.
    pub fn shows(&self, offset: i64) -> Result<(i64, i64)> {
        let here = self.source_at(offset)?;
        let next = self.source_at(
            offset
                .checked_add(1)
                .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?,
        )?;
        Ok(match next.cmp(&here) {
            core::cmp::Ordering::Greater => (here, next),
            core::cmp::Ordering::Less => (next + 1, here + 1),
            core::cmp::Ordering::Equal => (here, here + 1),
        })
    }

    /// The first offset satisfying a monotone predicate, or the clip's length
    /// if none does.
    ///
    /// The predicate must be false then true along the clip. Every predicate
    /// above is a comparison against a monotone sequence, so every one of them
    /// is.
    fn first_offset(&self, holds: impl Fn(i64) -> Result<bool>) -> Result<i64> {
        let (mut low, mut high) = (0_i64, self.duration.ticks());
        while low < high {
            let middle = low + (high - low) / 2;
            if holds(middle)? {
                high = middle;
            } else {
                low = middle + 1;
            }
        }
        Ok(low)
    }

    /// The first tick of the media past this clip.
    ///
    /// At a speed other than one this is past the end of what the clip
    /// *consumes* rather than past its length on the timeline — which is what
    /// makes a join between two retimed clips ask the right question.
    ///
    /// Past its end in the direction it is *playing*: a reversed clip runs
    /// backwards, so its end is below its in point. For the two ticks that
    /// bound what it reads, whichever way it runs, ask [`Clip::source_span`].
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn source_end(&self) -> Result<i64> {
        if self.is_frozen() {
            // One frame, however long the clip runs. `source_at` would answer
            // the in point at every offset including this one, which would say
            // a clip that shows a frame reads none of it.
            return self
                .source_start
                .checked_add(1)
                .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow));
        }
        self.source_at(self.duration.ticks())
    }

    /// The lowest and highest ticks of media this clip reads, both included.
    ///
    /// The question the media library has to ask, and the one `source_start`
    /// and a length could not answer once a clip could be retimed: a clip at
    /// double speed reads twice its length, and a reversed one reads *below*
    /// its in point. Both ends, in order, so a caller checking a range against
    /// an asset does not have to know which way the clip runs.
    ///
    /// The extremes are at the first and last frames because the mapping is
    /// monotone in the offset — it is a floor of a multiply by a fixed sign —
    /// so there is nothing between them to look at.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn source_span(&self) -> Result<(i64, i64)> {
        // A clip of no length is not representable: `Clip::new` refuses one as
        // `EmptyItem`, so there is always a last frame to ask about.
        let last = self.source_at(self.duration.ticks().saturating_sub(1))?;
        Ok(if last < self.source_start {
            (last, self.source_start)
        } else {
            (self.source_start, last)
        })
    }

    /// The source position as an instant in the track's timebase.
    #[must_use]
    pub const fn source_instant(&self, timebase: Timebase) -> Instant {
        Instant::new(self.source_start, timebase)
    }

    /// The look applied to this clip, if it has one.
    ///
    /// Named by the digest of the table rather than by an index into anything,
    /// for exactly the reason media is: the same grade in two projects is the
    /// same grade, a project-local handle would cache it twice, and a file
    /// swapped underneath a handle is a different look wearing the same name.
    ///
    /// The model holds the digest and not the table. A cube is 35,937 triples
    /// and the model is structure — and `media-editor-render`, where a table
    /// lives, sits above the model rather than beside it, so holding one here
    /// would invert the layering as well as the size.
    #[must_use]
    pub const fn grade(&self) -> Option<crate::media::Digest> {
        self.grade
    }

    /// The mask on this clip, if it has one.
    #[must_use]
    pub const fn mask(&self) -> Option<&crate::mask::Mask> {
        self.mask.as_ref()
    }

    /// Where this clip sits in the frame, if it has been moved.
    #[must_use]
    pub const fn transform(&self) -> Option<crate::transform::Transform> {
        self.transform
    }

    /// How long this clip takes to come up from nothing.
    #[must_use]
    pub fn fade_in(&self) -> Duration {
        self.counted(self.fade_in)
    }

    /// And to go back down to it.
    #[must_use]
    pub fn fade_out(&self) -> Duration {
        self.counted(self.fade_out)
    }

    /// A count of the clip's own ticks, as a length.
    ///
    /// The construction cannot refuse: a fade is only ever set from a
    /// `Duration`, which is already non-negative, and the timebase is this
    /// clip's own. The fallback is written out rather than unwrapped so that
    /// the day one of those stops being true, the answer is a length of
    /// nothing rather than a panic.
    fn counted(&self, ticks: i64) -> Duration {
        let timebase = self.duration.timebase();
        Duration::new(ticks, timebase).unwrap_or_else(|_| Duration::zero(timebase))
    }

    /// The same clip fading up and down over the given lengths.
    ///
    /// A fade **on the clip**, which is a different thing from a transition at
    /// a cut and is the thing a cut cannot do. A dissolve needs two clips: the
    /// first item of a programme has nothing before it, so before this there
    /// was no way to bring a programme up from black at all.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a timebase mismatch, and
    /// [`ModelStatus::FadesLongerThanClip`] if the two together outlast the
    /// clip. Clamping them instead would make a trim silently re-time
    /// somebody's fade, and a fade that is not where it was put is worse than
    /// a refusal.
    pub fn with_fades(&self, fade_in: Duration, fade_out: Duration) -> Result<Self> {
        let timebase = self.duration.timebase();
        if fade_in.timebase() != timebase || fade_out.timebase() != timebase {
            return Err(media_editor_core::CoreStatus::TimebaseMismatch.into());
        }
        if fade_in.checked_add(fade_out)?.compare(self.duration)? == core::cmp::Ordering::Greater {
            return Err(ModelStatus::FadesLongerThanClip);
        }
        Ok(Self {
            fade_in: fade_in.ticks(),
            fade_out: fade_out.ticks(),
            ..self.clone()
        })
    }

    /// How much of this clip is up, `offset` ticks into it.
    ///
    /// One at every instant of a clip nobody has faded. Otherwise it rises
    /// from **nought** on the clip's first frame to one after `fade_in`, and
    /// falls back to nought on its last — which is what a fade from black
    /// means, and is a different question from a dissolve's fraction. A
    /// dissolve never reaches nought or one because a frame at either end
    /// would repeat a neighbour; a fade from black *is* the black.
    ///
    /// Outside the clip — which happens inside a dissolve's handles, where the
    /// offset runs past both ends — it is nought. Material before a clip's own
    /// start is material before its fade began.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn fade_at(&self, offset: i64) -> Result<Rational> {
        let ticks = self.duration.ticks();
        let rising = ramp(offset, self.fade_in)?;
        let falling = ramp(
            ticks
                .checked_sub(1)
                .and_then(|last| last.checked_sub(offset))
                .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?,
            self.fade_out,
        )?;
        Ok(if rising < falling { rising } else { falling })
    }

    /// How this clip's framing changes over its own length, if it does.
    ///
    /// Measured from the clip's start rather than from the programme's, which
    /// is what makes sliding a shot down the timeline slide its push-in with
    /// it instead of re-timing it.
    #[must_use]
    pub const fn motion(&self) -> Option<&crate::transform::Motion> {
        self.motion.as_ref()
    }

    /// The same clip animated, or held still.
    ///
    /// Nothing here requires a base transform. The refusal lives in the edit
    /// that sets one, where a caller can be told; a builder that could refuse
    /// would make the order the two are set in matter, and "animate it, then
    /// frame it" is a perfectly ordinary order to work in.
    #[must_use]
    pub fn with_motion(&self, motion: Option<crate::transform::Motion>) -> Self {
        Self {
            motion,
            ..self.clone()
        }
    }

    /// The same clip moved, or left where it was.
    #[must_use]
    pub fn with_transform(&self, transform: Option<crate::transform::Transform>) -> Self {
        Self {
            transform,
            ..self.clone()
        }
    }

    /// The same clip with a mask, or with none.
    #[must_use]
    pub fn with_mask(&self, mask: Option<crate::mask::Mask>) -> Self {
        Self {
            mask,
            ..self.clone()
        }
    }

    /// The same clip with a look on it, or with none.
    #[must_use]
    pub fn with_grade(&self, grade: Option<crate::media::Digest>) -> Self {
        Self {
            grade,
            ..self.clone()
        }
    }

    /// The same clip reading from a different point of its media.
    ///
    /// This exists so that a slip is written as a change to *one* field rather
    /// than as a rebuild from three of them. A rebuild through
    /// [`Clip::new`] starts from nothing, which is right for a clip that is
    /// new and wrong for a clip that already existed — and when the grade was
    /// added, that difference silently dropped the look off every slipped
    /// clip. A test caught it at the third such site after two were found by
    /// reading, which is two more than should have needed finding.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::SourceBeforeStart`] for a negative source position.
    pub fn with_source(&self, source_start: i64) -> Result<Self> {
        if source_start < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        Ok(Self {
            source_start,
            ..self.clone()
        })
    }

    /// The same clip cut to a different length, keeping everything else.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length.
    pub fn with_duration(&self, duration: Duration) -> Result<Self> {
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        if self
            .fade_in
            .checked_add(self.fade_out)
            .is_none_or(|together| together > duration.ticks())
        {
            // A trim shorter than the fades on it. Clamping them would silently
            // re-time somebody's fade; dropping them would silently remove it.
            // Neither is what a trim was asked to do.
            return Err(ModelStatus::FadesLongerThanClip);
        }
        Ok(Self {
            duration,
            ..self.clone()
        })
    }
}

/// The whole number at or below a fraction.
///
/// Rust's integer division truncates towards zero, which is not the floor for
/// a negative value — and a negative value is exactly what a reversed clip
/// produces at every offset. `div_euclid` is the floor.
/// Whether `second` picks up in the media where `first` left off.
///
/// The ordinary answer is that the first clip's source end is the second's in
/// point. A **freeze** is the exception, and it has to be: a freeze consumes
/// one frame, so its end is the in point plus one, and two frozen clips of the
/// same still both begin at that in point. Asking the ordinary question of
/// them would refuse to join a freeze that had been cut in two — and join is
/// the exact inverse of split, which is the property that says this is right
/// rather than convenient.
/// The second half of a clip cut at `offset`, running for `length`.
///
/// Out of [`Item::split`] because the number of things that have to be
/// re-based grew past what one function should hold, and because every one of
/// them is the same decision made again: anything measured from the clip's own
/// start moves back by the length of the head.
fn tail_of(clip: &Clip, offset: i64, length: Duration) -> Result<Clip> {
    // Through the clip's own mapping rather than by adding the offset: at a
    // speed other than one the tail begins where the *head left off in the
    // media*, which is `offset x speed` in and not `offset` in -- and on a
    // ramp it is the area under the curve, which nothing but the mapping
    // knows. Adding would have re-timed every cut through every retimed clip,
    // and would have looked like a frame of drift rather than like a bug.
    let source_start = clip.source_at(offset)?;
    // The motion is measured from its clip's start, and the tail's start is
    // `offset` ticks later than the original's, so it is re-based by that
    // much. Carried unchanged it would restart the move at the cut, which is
    // a silent fault and a hard one to see: the tail would still animate, just
    // from the wrong place.
    let rebased = match &clip.motion {
        None => None,
        Some(motion) => Some(motion.shifted(-offset)?),
    };
    // And the opacity curve is measured the same way, so it is re-based the
    // same way. Two lanes of animation on one clip, one of them re-based and
    // one not, would be a fade that restarted at every cut while the push-in
    // did not.
    let faded = match &clip.opacity {
        None => None,
        Some(curve) => Some(curve.shifted(-offset)?),
    };
    // And the mask's animation, which is measured the same way as the
    // framing's. A cut that re-based two of them would be worse than one that
    // re-based none: the shape would drift out of step with the push-in.
    let reshaped = match &clip.mask_motion {
        None => None,
        Some(motion) => Some(motion.shifted(-offset)?),
    };
    // And the grade's strength, which is the fourth lane measured from the
    // clip's own start. The argument has not changed since the first: a cut
    // that re-based three of them and carried one would put the look's arrival
    // out of step with the push-in it was drawn against.
    let brought = match &clip.grade_strength {
        None => None,
        Some(curve) => Some(curve.shifted(-offset)?),
    };
    // The grade travels with the clip, and `Clip::new` would give the tail
    // none -- writing this the obvious way made a split lose the look off half
    // of what it cut, and left join no longer the exact inverse of split,
    // which is the property the two are built around.
    let mut cut = clip
        .with_source(source_start)?
        .with_duration(length)?
        .with_motion(rebased)
        .with_mask_motion(reshaped)?
        .with_opacity(faded)?
        .with_grade_strength(brought)?;
    // And the notes at or past the cut, re-based like everything else that is
    // measured from the clip's own start. **At** the cut goes to the tail: a
    // note at tick `k` names the frame at `k`, the head's frames are the ones
    // below `k`, and that frame is now the tail's first -- the same half-open
    // convention every span in this model uses.
    cut.markers = clip.noted_from(offset)?;
    // And the ramp, which is the fifth. It is re-based like the four above and
    // then put on through the same door an edit uses, so the tail's own source
    // bound is checked rather than assumed -- a half of a cut is a clip like
    // any other and has to be a legal one.
    match &clip.playback {
        Playback::Ramp(curve) => Playback::Ramp(curve.shifted(-offset)?).applied_to(&cut),
        Playback::At(_) | Playback::Frozen => Ok(cut),
    }
}

fn continues_source(first: &Clip, second: &Clip) -> bool {
    if first.is_frozen() {
        return first.source_start == second.source_start;
    }
    first
        .source_end()
        .is_ok_and(|end| end == second.source_start)
}

fn floor_of(value: Rational) -> i64 {
    value.numerator().div_euclid(value.denominator())
}

/// A fraction of the way through a ramp of `over` ticks, held at both ends.
///
/// `along` may be negative — a dissolve's handles reach outside the clip — and
/// the answer there is nought rather than an extrapolation, for the same
/// reason a curve is held rather than continued past its last keyframe.
fn ramp(along: i64, over: i64) -> Result<Rational> {
    if over == 0 {
        return Ok(Rational::ONE);
    }
    if along <= 0 {
        return Ok(Rational::ZERO);
    }
    if along >= over {
        return Ok(Rational::ONE);
    }
    Rational::new(along, over).map_err(ModelStatus::from)
}

/// Whether `second`'s animation is `first`'s, re-based onto `second`'s start.
///
/// The condition that keeps join the exact inverse of split. Split re-bases
/// the tail's motion by the head's length, so join must only accept a pair
/// standing in exactly that relation — and then keeping the head's motion,
/// which already describes the whole of the joined length, loses nothing.
///
/// Anything else is refused rather than reconciled. Two differently animated
/// clips of one piece of media, adjacent in its source, are two shots that
/// move differently; joining them would keep the first's move and discard the
/// second's without saying so.
fn continues_motion(first: &Clip, second: &Clip) -> bool {
    match (&first.motion, &second.motion) {
        (None, None) => true,
        (Some(held), Some(later)) => held
            .shifted(-first.duration.ticks())
            .is_ok_and(|rebased| &rebased == later),
        _ => false,
    }
}

/// Whether two clips' mask animations line up across a cut.
fn continues_mask_motion(first: &Clip, second: &Clip) -> bool {
    match (&first.mask_motion, &second.mask_motion) {
        (None, None) => true,
        (Some(held), Some(later)) => held
            .shifted(-first.duration.ticks())
            .is_ok_and(|rebased| &rebased == later),
        _ => false,
    }
}

/// Whether two clips' grade strengths line up across a cut.
///
/// Asked in addition to whether the two grades are the same digest, and the
/// two questions are not one: two clips of one shot carrying the same look,
/// one of it bringing it on and one not, are two shots and joining them would
/// discard one of the answers silently.
fn continues_grade_strength(first: &Clip, second: &Clip) -> bool {
    match (&first.grade_strength, &second.grade_strength) {
        (None, None) => true,
        (Some(held), Some(later)) => held
            .shifted(-first.duration.ticks())
            .is_ok_and(|rebased| &rebased == later),
        _ => false,
    }
}

/// Whether two clips play their media the same way across a cut.
///
/// Plain equality for a fixed speed and for a freeze: both describe the whole
/// of a clip and a cut does not move them. A **ramp** is the exception, and it
/// is the exception for the reason the four animation lanes are: a ramp is
/// measured from its clip's own start, so a split re-bases the tail's by the
/// head's length and a join must accept exactly that pair and nothing else.
///
/// Which makes a ramp the fifth lane, and the first one that is not optional
/// decoration -- it decides which frames the clip shows.
fn continues_playback(first: &Clip, second: &Clip) -> bool {
    match (&first.playback, &second.playback) {
        (Playback::Ramp(held), Playback::Ramp(later)) => held
            .shifted(-first.duration.ticks())
            .is_ok_and(|rebased| &rebased == later),
        (held, later) => held == later,
    }
}

/// Whether two clips' opacity animations line up across a cut.
///
/// The same question `continues_motion` asks and the same answer, because the
/// two lanes are measured the same way: the second's curve must be the first's
/// re-based by the first's length, which is exactly what a split produces.
fn continues_opacity(first: &Clip, second: &Clip) -> bool {
    match (&first.opacity, &second.opacity) {
        (None, None) => true,
        (Some(held), Some(later)) => held
            .shifted(-first.duration.ticks())
            .is_ok_and(|rebased| &rebased == later),
        _ => false,
    }
}

/// One entry on a track.
///
/// ## Why the clip variant is allowed to dwarf the gap
///
/// A clip is 520 bytes and a gap is 24, and clippy is right that the
/// difference is large. Its remedy is not available here: boxing the clip
/// means an infallible allocation, and this crate allows no allocation that is
/// not both bounded by a named policy constant and fallible (R-5.1, R-5.2).
/// `Box::try_new` does not exist yet, and routing every field access through a
/// one-element `Vec` to get `try_reserve` would be indirection wearing a
/// disguise.
///
/// The cost is also the opposite way round from what the lint assumes. It
/// warns because a `Vec` of the *small* variant pays for the large one, and a
/// track is mostly clips: the waste is 496 bytes per gap, on the minority
/// variant, against 520 bytes per clip that has to live somewhere regardless.
/// A track at [`crate::track::MAX_ITEMS_PER_TRACK`] costs about thirty-two
/// megabytes whether or not this enum is tightened.
///
/// It is an `expect` rather than an `allow` so that the day the difference
/// falls back under the threshold — because a field moved, or because
/// fallible boxing arrived — the build says so instead of carrying a stale
/// exemption. And `tests/size.rs` pins the number, because a size nobody
/// measures is a size that grows.
#[expect(
    clippy::large_enum_variant,
    reason = "the remedy is boxing, and R-5.2 forbids an infallible allocation"
)]
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Item {
    /// Material.
    Clip(Clip),
    /// Deliberate absence of material, with a length.
    Gap(Duration),
}

impl Item {
    /// A gap of a given length.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length.
    pub fn gap(duration: Duration) -> Result<Self> {
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        Ok(Self::Gap(duration))
    }

    /// How long this item occupies its track.
    #[must_use]
    pub const fn duration(&self) -> Duration {
        match self {
            Self::Clip(clip) => clip.duration(),
            Self::Gap(duration) => *duration,
        }
    }

    /// The timebase this item is counted in.
    #[must_use]
    pub const fn timebase(&self) -> Timebase {
        self.duration().timebase()
    }

    /// This item with a different length, keeping its source position.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyItem`] for a zero length, or a timebase mismatch.
    pub fn with_duration(&self, duration: Duration) -> Result<Self> {
        if duration.timebase() != self.timebase() {
            return Err(media_editor_core::CoreStatus::TimebaseMismatch.into());
        }
        if duration.is_zero() {
            return Err(ModelStatus::EmptyItem);
        }
        Ok(match self {
            // The grade travels with the clip. A trim is a change of length,
            // and a clip that lost its look because somebody shortened it
            // would be the kind of fault nobody thinks to look for.
            Self::Clip(clip) => Self::Clip(clip.with_duration(duration)?),
            Self::Gap(_) => Self::Gap(duration),
        })
    }

    /// This item cut in two at `offset` ticks from its start.
    ///
    /// The two pieces are contiguous in their source, so joining them is the
    /// exact inverse of this operation.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::SplitOutsideItem`] if the offset would leave either
    /// piece empty.
    pub fn split(&self, offset: i64) -> Result<(Self, Self)> {
        if offset <= 0 || offset >= self.duration().ticks() {
            return Err(ModelStatus::SplitOutsideItem);
        }
        let timebase = self.timebase();
        let head_length = Duration::new(offset, timebase).map_err(ModelStatus::from)?;
        let tail_length = self.duration().checked_sub(head_length)?;
        // The head keeps the notes below the cut and hands the rest over.
        // A note is not deleted by a cut -- it is on the other half, which is
        // what makes join put it back.
        let head = match self.with_duration(head_length)? {
            Self::Clip(clip) => Self::Clip(clip.noted_before(offset)),
            Self::Gap(length) => Self::Gap(length),
        };
        let tail = match self {
            Self::Clip(clip) => Self::Clip(tail_of(clip, offset, tail_length)?),
            Self::Gap(_) => Self::Gap(tail_length),
        };
        Ok((head, tail))
    }

    /// Whether `later` continues this item without a break in its source.
    #[must_use]
    pub fn continues_into(&self, later: &Self) -> bool {
        match (self, later) {
            (Self::Gap(_), Self::Gap(_)) => self.timebase() == later.timebase(),
            (Self::Clip(first), Self::Clip(second)) => {
                first.media == second.media
                    && first.duration.timebase() == second.duration.timebase()
                    // The same look, too. Two clips of one piece of media,
                    // adjacent in its source and graded differently, are not
                    // one item — they are two shots with two looks, and
                    // joining them would keep the first's and discard the
                    // second's without saying so. Refusing keeps join the
                    // exact inverse of split, which only cuts one look in two.
                    && first.grade == second.grade
                    && continues_playback(first, second)
                    && continues_motion(first, second)
                    && continues_mask_motion(first, second)
                    && continues_opacity(first, second)
                    && continues_grade_strength(first, second)
                    && continues_source(first, second)
            }
            _ => false,
        }
    }

    /// This item and the one that continues it, as one item.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::ItemsNotContiguous`] if the two are not adjacent in
    /// their source, which is what makes join the exact inverse of split.
    pub fn join(&self, later: &Self) -> Result<Self> {
        if !self.continues_into(later) {
            return Err(ModelStatus::ItemsNotContiguous);
        }
        let duration = self.duration().checked_add(later.duration())?;
        // The notes are the one thing a join has to *merge* rather than keep.
        // The five re-based lanes are all still the head's, which already
        // describe the whole joined length -- but notes were divided by the
        // cut, so half of them are on the other item and joining without them
        // would delete them.
        match (self.with_duration(duration)?, later) {
            (Self::Clip(first), Self::Clip(second)) => Ok(Self::Clip(
                first.noted_with(second, self.duration().ticks())?,
            )),
            (Self::Gap(length), _) => Ok(Self::Gap(length)),
            (joined @ Self::Clip(_), Self::Gap(_)) => Ok(joined),
        }
    }
}
