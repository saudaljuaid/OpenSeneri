// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![allow(
    clippy::doc_markdown,
    reason = "Media Editor and Phipia are product names, not identifiers"
)]
//! The Media Editor project model.
//!
//! A project is media, sequences, and history. A sequence is tracks; a track
//! is items; an item is a clip or a gap. Positions are not stored: an item's
//! place on its track is the sum of the lengths before it, so overlapping
//! items and unaccounted holes are not representable rather than merely
//! rejected.
//!
//! An [`Edit`] is a value that carries its own inverse, which is what makes
//! [`EditJournal`] a list of pairs instead of a special mode, and what makes
//! "undo everything reproduces the project exactly" a property a test can
//! generate thousands of cases for.
//!
//! There is no floating point here, no `unsafe`, and no allocation that is not
//! both bounded by a named policy constant and fallible (R-5.1, R-5.2).

extern crate alloc;

mod bounded;

pub mod caption;
pub mod curve;
pub mod edit;
pub mod item;
pub mod journal;
pub mod marker;
pub mod mask;
pub mod media;
pub mod project;
pub mod sequence;
pub mod stack;
pub mod status;
pub mod store;
pub mod title;
pub mod track;
pub mod transform;

pub use curve::{Automation, Curve, Interpolation, Keyframe, KeyframeEdit, MAX_KEYFRAMES};
pub use edit::Edit;
pub use item::{Clip, Item, Playback};
pub use journal::EditJournal;
/// How deeply sequences may nest inside one another.
///
/// Eight. A policy bound, and a small one on purpose: nesting is a way of
/// naming a piece of work so it can be reused, and a chain eight deep is
/// already a structure nobody can hold in their head. It is also what bounds
/// the renderer, which walks the chain with an explicit stack — R-5.5 names
/// nested sequences among the places recursion is forbidden.
///
/// `media-editor-render` carries the same number, because the two crates are
/// siblings and neither may depend on the other. `the_two_nesting_bounds_agree`
/// in the application's tests asserts they are one number, exactly as the
/// fader's two bounds are — a duplication that cannot drift without something
/// failing.
pub const MAX_NESTING_DEPTH: usize = 8;

pub use marker::{MAX_MARKER_TEXT, MAX_MARKERS_PER_CLIP, MAX_MARKERS_PER_SEQUENCE, Marker};
pub use mask::{MAX_CORNERS, Mask};
pub use media::{Digest, Location, MAX_LOCATION_BYTES, MediaAsset, MediaId, MediaSource};
pub use project::Project;
pub use sequence::{MAX_TRACKS_PER_SEQUENCE, Sequence, SequenceId, TrackSet};
pub use stack::{Graded, Lane, Layer, Revealed};
pub use status::{ModelStatus, Result};
pub use store::Store;
pub use title::{Alignment, Ink, MAX_TITLE_LINES, MAX_TITLE_TEXT, Title};
pub use track::{
    Fader, MAXIMUM_DECIBELS, MINIMUM_DECIBELS, Track, TrackKind, Transition, TransitionKind, Wipe,
};
pub use transform::{Motion, Resampling, Transform, Turn};
