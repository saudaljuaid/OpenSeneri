// SPDX-License-Identifier: GPL-3.0-only
//! Resolve a sequence into layers at one instant.
//!
//! Layers are returned bottom-first, with higher tracks on top. Gaps and tracks
//! that have ended contribute no layer. An empty stack remains empty; the
//! renderer decides whether that means black or transparency.

use alloc::vec::Vec;

use media_editor_core::{Instant, Rational};

use crate::bounded::push_bounded;
use crate::media::MediaId;
use crate::sequence::Sequence;
use crate::status::{ModelStatus, Result};
use crate::track::{Track, TrackKind, Transition, TransitionKind, Wipe};

/// One piece of material, at one instant, on one track.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Layer {
    track: usize,
    media: MediaId,
    source: i64,
    opacity: Rational,
    grade: Option<Graded>,
    mask: Option<crate::mask::Mask>,
    transform: Option<crate::transform::Transform>,
    fade: (Rational, Rational),
    wipe: Option<Revealed>,
}

/// A look on a layer, and how far the picture has travelled towards it.
///
/// Two fields rather than a digest and a rational beside it, and the reason is
/// the same one [`Revealed`] is a pair: a strength means nothing without a
/// look to be the strength *of*, so a layer with no grade has no strength
/// either, rather than a neutral one that a reader has to know to ignore.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Graded {
    look: crate::media::Digest,
    strength: Rational,
}

impl Graded {
    /// Which look.
    #[must_use]
    pub const fn look(self) -> crate::media::Digest {
        self.look
    }

    /// How far from ungraded to graded, from none of the look to all of it.
    ///
    /// One for a clip nobody has animated, which is a grade applied the way a
    /// grade has always been applied. Resolved here rather than handed out as
    /// a curve, exactly as the framing and the shape are: by the time a layer
    /// describes a frame, an animation has already become the number it reads
    /// at that moment, and a renderer that had to be told about curves would
    /// need a clock — and a node that depends on a clock is a node whose cache
    /// key is a lie.
    #[must_use]
    pub const fn strength(self) -> Rational {
        self.strength
    }
}

/// The incoming side of a wipe, and how far its edge has travelled.
///
/// Only the incoming clip of a wipe carries one. The outgoing clip is whole
/// underneath it, exactly as it is under a dissolve — which is what lets a
/// wipe share the dissolve's whole structure and differ only in what the
/// renderer does with the number.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Revealed {
    wipe: Wipe,
    fraction: Rational,
}

impl Revealed {
    /// Which way the edge sweeps.
    #[must_use]
    pub const fn wipe(self) -> Wipe {
        self.wipe
    }

    /// How far along its travel the edge is, from none to all.
    ///
    /// The same exact fraction a dissolve would have faded to at this instant.
    /// A wipe and a dissolve of the same length are the same transition timed
    /// the same way; only what the fraction *means* differs, and that is the
    /// renderer's business rather than the model's.
    #[must_use]
    pub const fn fraction(self) -> Rational {
        self.fraction
    }
}

impl Layer {
    /// Which track this came from.
    #[must_use]
    pub const fn track(&self) -> usize {
        self.track
    }

    /// Which media to read.
    #[must_use]
    pub const fn media(&self) -> MediaId {
        self.media
    }

    /// The look on the clip this came from, if it has one, and how much of it.
    ///
    /// Carried up from the clip rather than looked up here, because the stack
    /// answers what is on each track and a table is not on a track — it is
    /// something a renderer will have to fetch, by the same digest, from the
    /// same place it fetches frames.
    #[must_use]
    pub const fn grade(&self) -> Option<Graded> {
        self.grade
    }

    /// How much of this layer is showing, from none to all of it.
    ///
    /// One everywhere except inside a dissolve, where the outgoing clip fades
    /// from one to nothing and the incoming one rises to meet it. It is an
    /// exact rational — `n` frames into an `N`-frame dissolve is `n/N`, not a
    /// decimal near it — so a dissolve is the same dissolve on every machine
    /// and the two halves always sum to exactly one.
    #[must_use]
    pub const fn opacity(&self) -> Rational {
        self.opacity
    }

    /// The mask on the clip this came from, if it has one.
    ///
    /// Carried up rather than applied here for the same reason a grade is:
    /// the stack answers what is on each track, and a shape is not a fact
    /// about the track — it is something a renderer turns into coverage.
    #[must_use]
    pub const fn mask(&self) -> Option<&crate::mask::Mask> {
        self.mask.as_ref()
    }

    /// Where the clip this came from sits in the frame, if it was moved.
    #[must_use]
    pub const fn transform(&self) -> Option<crate::transform::Transform> {
        self.transform
    }

    /// How much of this layer its *own* fade is letting through.
    ///
    /// Separate from [`Layer::opacity`], and deliberately. Opacity is what the
    /// track and any transition at a cut are doing; this is what the clip
    /// itself is doing, and the two multiply. Keeping them apart is what lets
    /// a test tell a clip that fades up from black apart from one halfway
    /// through a dissolve — and what lets the sound side use this without
    /// having to know what a picture track's opacity lane is.
    #[must_use]
    pub const fn fade(&self) -> Rational {
        self.fade.0
    }

    /// Where that fade has got to by the *next* frame.
    ///
    /// A frame of picture is a moment and one number answers it; a frame of
    /// sound is two thousand samples and a fade may be somewhere different at
    /// each. So the pair is the same half-open shape the fader's ramp uses —
    /// this frame's value, and the value the next frame's first sample gets —
    /// and consecutive blocks tile a fade rather than repeating one value at
    /// every seam.
    ///
    /// It is the **clip's own** fade one tick on, not whatever the timeline
    /// shows there. A clip that simply ends is not a clip fading out, and
    /// reading the next frame off the track would have made every unfaded clip
    /// duck to silence over its last block. It did, until a test said so.
    #[must_use]
    pub const fn fade_arriving(&self) -> Rational {
        self.fade.1
    }

    /// The wipe revealing this layer, if it is the incoming side of one.
    ///
    /// `None` for everything else, including the outgoing side of a wipe and
    /// both sides of a dissolve — a dissolve says everything it needs to
    /// through [`Layer::opacity`].
    #[must_use]
    pub const fn wipe(&self) -> Option<Revealed> {
        self.wipe
    }

    /// Which tick of that media, counted from its own start.
    ///
    /// This is the clip's source start plus how far into the clip the instant
    /// falls — the arithmetic that decides *which frame* a playhead shows, and
    /// the one an off-by-one makes wrong for the whole clip rather than for
    /// one frame of it.
    #[must_use]
    pub const fn source(&self) -> i64 {
        self.source
    }
}

/// Which track this stack is of.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Lane {
    /// The picture tracks.
    Picture,
    /// The sound tracks.
    Sound,
}

impl Lane {
    /// The kind of track this lane holds.
    const fn kind(self) -> TrackKind {
        match self {
            Self::Picture => TrackKind::Video,
            Self::Sound => TrackKind::Audio,
        }
    }
}

impl Sequence {
    /// What is on each track of one lane at an instant, bottom first.
    ///
    /// Tracks with a gap there, and tracks that do not reach that far,
    /// contribute nothing — so an empty result means the sequence shows
    /// nothing at that instant, which is a real answer.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a timebase mismatch if the instant is
    /// not counted in the sequence's own timebase, [`ModelStatus::UnknownItem`]
    /// if a track's contents disagree with its own index, or
    /// [`ModelStatus::OutOfMemory`].
    pub fn stack_at(&self, lane: Lane, instant: Instant) -> Result<Vec<Layer>> {
        if instant.timebase() != self.timebase() {
            return Err(media_editor_core::CoreStatus::TimebaseMismatch.into());
        }
        let mut stack = Vec::new();
        for (index, track) in self.tracks().iter().enumerate() {
            if track.kind() != lane.kind() {
                continue;
            }
            // A track's own opacity multiplies whatever the items on it are
            // doing. It is read once for the track rather than once per layer,
            // because a dissolve's two layers are on the same track at the
            // same instant and reading it twice would be asking one question
            // twice and hoping for the same answer.
            let animated = track.opacity_at(instant)?;
            if let Some(transition) = dissolve_at(track, instant)? {
                // Both sides of the cut are on screen. The outgoing one is at
                // full opacity underneath and the incoming one fades up over
                // it, which is a cross-fade written as an `over` — the mix is
                // `in x t + out x (1 - t)` either way, and this way the
                // compositor needs no second operator.
                //
                // A wipe is the same structure with the fraction meaning
                // something else: the incoming clip is *whole* rather than
                // faded, and the fraction says how far its edge has swept. So
                // both sides stay at full opacity and the fraction travels
                // with the layer instead of being spent on it.
                let travelled = fraction(track, transition, instant)?;
                let sweeping = match transition.kind() {
                    TransitionKind::Dissolve => None,
                    TransitionKind::Wipe(wipe) => Some(Revealed {
                        wipe,
                        fraction: travelled,
                    }),
                };
                let rising = if sweeping.is_some() {
                    Rational::ONE
                } else {
                    travelled
                };
                for (item, opacity, revealed) in [
                    (transition.boundary() - 1, Rational::ONE, None),
                    (transition.boundary(), rising, sweeping),
                ] {
                    let crate::Item::Clip(clip) = track.item(item)? else {
                        return Err(ModelStatus::NotAClip);
                    };
                    // The offset is measured from the item's own start, so it
                    // runs past the outgoing clip's end and before the
                    // incoming clip's beginning — which is exactly what
                    // handles are.
                    let offset = instant
                        .ticks()
                        .checked_sub(track.item_start(item)?.ticks())
                        .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?;
                    let source = clip.source_at(offset)?;
                    push_bounded(
                        &mut stack,
                        Layer {
                            track: index,
                            media: clip.media(),
                            source,
                            opacity: opacity
                                .checked_mul(animated)?
                                .checked_mul(clip.opacity_at(offset)?)?,
                            grade: graded(clip, offset)?,
                            mask: clip.mask_at(offset)?,
                            transform: framing(clip, offset)?,
                            fade: (
                                clip.fade_at(offset)?,
                                clip.fade_at(offset.saturating_add(1))?,
                            ),
                            wipe: revealed,
                        },
                        MAX_LAYERS,
                    )?;
                }
                continue;
            }
            let Some((item, offset)) = track.item_at(instant)? else {
                // The track has stopped. Not black past its end — stopped.
                continue;
            };
            let crate::Item::Clip(clip) = track.item(item)? else {
                // A gap is transparent, so whatever is beneath shows through.
                continue;
            };
            let source = clip.source_at(offset)?;
            push_bounded(
                &mut stack,
                Layer {
                    track: index,
                    media: clip.media(),
                    source,
                    // The track's automation and the clip's own, multiplied.
                    // Three things can decide what is on screen at once -- a
                    // track fading, a clip fading, a dissolve at the cut --
                    // and any one of them replacing the others would throw
                    // away a decision somebody made.
                    opacity: animated.checked_mul(clip.opacity_at(offset)?)?,
                    grade: graded(clip, offset)?,
                    mask: clip.mask_at(offset)?,
                    transform: framing(clip, offset)?,
                    fade: (
                        clip.fade_at(offset)?,
                        clip.fade_at(offset.saturating_add(1))?,
                    ),
                    wipe: None,
                },
                MAX_LAYERS,
            )?;
        }
        Ok(stack)
    }
}

/// The look on a clip at an instant `offset` ticks into it, and how far on.
///
/// The strength is read even when the curve is absent, because the absent
/// answer is one rather than nothing: a graded clip nobody animated is a
/// clip with all of its look on it.
fn graded(clip: &crate::Clip, offset: i64) -> Result<Option<Graded>> {
    let Some(look) = clip.grade() else {
        return Ok(None);
    };
    Ok(Some(Graded {
        look,
        strength: clip.grade_strength_at(offset)?,
    }))
}

/// Where a clip sits at an instant `offset` ticks into it.
///
/// The stack hands out a *resolved* transform rather than a base and an
/// animation beside it, and that is the whole reason animating a clip needed
/// no change to the renderer at all: by the time a frame is described, a
/// motion has already become the framing it reads at that moment. A renderer
/// that had to be told about curves would need a clock, and a node that
/// depends on a clock is a node whose cache key is a lie.
///
/// The offset may be negative here, in a transition's handles, and that is
/// meant: a curve holds its first value before its first keyframe, so a clip
/// dissolving in reads the framing it was going to start at rather than an
/// extrapolation of where it came from.
fn framing(clip: &crate::Clip, offset: i64) -> Result<Option<crate::transform::Transform>> {
    let Some(base) = clip.transform() else {
        return Ok(None);
    };
    let Some(motion) = clip.motion() else {
        return Ok(Some(base));
    };
    let (scale, across, down, turn) =
        motion.at(Instant::new(offset, clip.duration().timebase()))?;
    Ok(Some(base.moved_by(scale, across, down, turn)?))
}

/// Where a dissolve begins, relative to the cut it sits on.
///
/// Half its length, rounded down — so an even dissolve is centred exactly and
/// an odd one puts its extra frame after the cut. That is a choice rather than
/// a fact, and it is written here so that it is one place rather than several.
fn opening(transition: &Transition) -> i64 {
    transition.duration().ticks() / 2
}

/// The dissolve covering an instant on a track, if there is one.
fn dissolve_at(track: &Track, instant: Instant) -> Result<Option<Transition>> {
    for transition in track.transitions() {
        let cut = track.item_start(transition.boundary())?;
        let start = cut
            .ticks()
            .checked_sub(opening(transition))
            .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?;
        let end = start
            .checked_add(transition.duration().ticks())
            .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?;
        if instant.ticks() >= start && instant.ticks() < end {
            return Ok(Some(*transition));
        }
    }
    Ok(None)
}

/// How much of the incoming clip is showing, as an exact fraction.
///
/// An `N`-frame dissolve runs from `1/(N+1)` to `N/(N+1)`, never touching
/// nought or one. That is deliberate: a dissolve whose first frame is entirely
/// the outgoing clip has wasted a frame showing what the frame before it
/// already showed, and the same at the other end. Every frame of a dissolve is
/// a real mix.
fn fraction(track: &Track, transition: Transition, instant: Instant) -> Result<Rational> {
    let cut = track.item_start(transition.boundary())?;
    let start = cut
        .ticks()
        .checked_sub(opening(&transition))
        .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?;
    let elapsed = instant
        .ticks()
        .checked_sub(start)
        .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?;
    let length = transition.duration().ticks();
    Ok(Rational::new(elapsed + 1, length + 1)?)
}

/// The most layers one instant may stack.
///
/// Twice a sequence's track bound, because a track inside a dissolve names
/// *two* layers — both sides of the cut are on screen at once. Bounded by
/// something the caller already agreed to (R-11.2).
const MAX_LAYERS: usize = 2 * crate::sequence::MAX_TRACKS_PER_SEQUENCE;
