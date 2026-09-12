// SPDX-License-Identifier: GPL-3.0-only
//! A note somebody left on the timeline.
//!
//! `ARCHITECTURE.md` has listed markers as planned since its first version,
//! beside nested sequences, "so the shape is decided before the pressure to
//! compromise it arrives". This is the shape.
//!
//! A marker is **not** an item and does not live on a track. It names an
//! instant in the programme and carries text, and that is the whole of it:
//! nothing renders it, nothing composites it, and no clip is affected by one.
//! It is the one thing in this model that exists purely for the person editing.
//!
//! ## At an instant, and absolutely
//!
//! A marker's instant is a position in the *programme*, not an offset into
//! anything. It does not move when an item ripples, and that is a decision
//! rather than an omission: a marker says "look at this moment", and the
//! moment is a moment of the finished piece. A note reading "fix the sync
//! here" is about a place on the timeline; moving it because an unrelated
//! shot got longer would move it away from the thing it is about.
//!
//! The opposite decision — a marker that belongs to a **clip** and travels
//! with it — is the other half of the pair, and it is now here too. Same type,
//! same text bound, same one-per-instant rule; what differs is what the
//! instant *means*. On a sequence it is a position in the programme. On a clip
//! it is an offset from the clip's own start, like every curve a clip carries,
//! so it moves when the shot moves and survives being cut in two.
//!
//! Which of the two an editor wants is decided by what the note is *about*. A
//! note reading "the sync drifts here" is about a place in the programme. A
//! note reading "his eyeline goes off here" is about the shot, and it should
//! still be on the shot after somebody moves it a minute later.
//!
//! ## One per instant
//!
//! Two markers at one instant is the same nothing as none: neither can be
//! named, moved, or removed without saying which, and "which" is exactly what
//! an instant was going to answer. So a collision is refused, the same
//! decision [`crate::Curve`] makes about keyframes and for the same reason.

use alloc::string::String;

use media_editor_core::Instant;

use crate::status::{ModelStatus, Result};

/// How many characters a marker may carry.
///
/// The same bound a title's line has, and not by coincidence: both are text a
/// person types into a box, both are bounded because a hostile file must not
/// be able to talk its way past a bound (R-11.2), and having one number for
/// "a line of text somebody typed" is better than having two that drift.
pub const MAX_MARKER_TEXT: usize = 128;

/// How many markers one sequence may hold.
///
/// A policy bound. A feature-length programme carries tens of notes; a
/// sequence that reaches this has been generated rather than edited.
pub const MAX_MARKERS_PER_SEQUENCE: usize = 4096;

/// How many markers one **clip** may hold.
///
/// Eight, which is three orders of magnitude below the sequence's bound, and
/// the reason is arithmetic rather than taste. A marker is 48 bytes plus up to
/// [`MAX_MARKER_TEXT`] characters, which is 512 bytes of UTF-8 in the worst
/// case — so eight of them is **4,480 bytes**, or a little over one of the
/// nineteen pages a Phipia program is given.
///
/// Eight notes on one shot is already more than anybody writes. Refusing the
/// ninth is a bound a person can hit; refusing the ninth *thousandth* would be
/// a bound only a generator can, which is a bound that is not doing anything.
///
/// The sequence's four thousand is affordable because there is one sequence
/// being edited. A clip's bound is paid **per clip**, and a track may hold
/// many of them: at sixty-four notes each, one track of a thousand clips
/// could ask for nine megabytes of notes on a
/// machine with seventy-six kilobytes — `MAX_ITEMS_PER_TRACK` is 65,536, so
/// the true worst case is far past that. This is the same measured-budget
/// argument M8.27 used to refuse a column lift, reached from the other side.
pub const MAX_MARKERS_PER_CLIP: usize = 8;

/// A note at an instant.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Marker {
    at: Instant,
    text: String,
}

impl Marker {
    /// A marker at an instant, saying something.
    ///
    /// Empty text is allowed, and deliberately: a marker with nothing written
    /// on it is the commonest kind there is — somebody pressed the key to mark
    /// a spot and will come back to it. Refusing that would refuse the gesture
    /// the feature exists for.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::MarkerTextTooLong`] past [`MAX_MARKER_TEXT`] characters,
    /// counted in characters rather than bytes for the reason a title's are:
    /// a bound in bytes is a bound that means something different in every
    /// language.
    pub fn new(at: Instant, text: String) -> Result<Self> {
        if text.chars().count() > MAX_MARKER_TEXT {
            return Err(ModelStatus::MarkerTextTooLong);
        }
        Ok(Self { at, text })
    }

    /// Where it is.
    #[must_use]
    pub const fn at(&self) -> Instant {
        self.at
    }

    /// What it says.
    #[must_use]
    pub fn text(&self) -> &str {
        &self.text
    }

    /// The same marker, moved.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a timebase mismatch.
    pub fn moved_to(&self, at: Instant) -> Result<Self> {
        if at.timebase() != self.at.timebase() {
            return Err(media_editor_core::CoreStatus::TimebaseMismatch.into());
        }
        Ok(Self {
            at,
            text: self.text.clone(),
        })
    }
}
