// SPDX-License-Identifier: GPL-3.0-only
//! Where samples of media come from.
//!
//! The sound counterpart of [`media_editor_render::Library`], and it took until
//! the media vault could serve sound for the two to be shaped alike.
//!
//! ## Named by content, not by position
//!
//! This trait was in the mixer and took a `MediaId` — an index into *one
//! project's* table of assets. That was serviceable while every source was a
//! test double, and it is wrong for the same reason the render library says a
//! source node's identity is a digest: a name that means "the fourth asset in
//! this file" is a name that means something different in the next file.
//!
//! A vault is **shared**. It is one of Phipia's files holding material that
//! several projects refer to, keyed by what the material *is*. A sound source
//! reading from one cannot be asked for "media four" — there is no four — and
//! an adapter that translated per project would be a translation layer whose
//! only job is to undo a mistake in this signature.
//!
//! So a source is asked for a digest, which is what a clip actually refers to,
//! and the mixer resolves the one into the other exactly where the picture
//! planner already does.

use media_editor_core::Digest;

use crate::buffer::AudioBuffer;
use crate::status::Result;

/// Where samples of media come from.
pub trait SampleSource {
    /// `count` samples of the media with this content digest, beginning at
    /// sample `start` of it.
    ///
    /// The buffer must have the rate and channel count that was asked for, and
    /// exactly `count` samples. A source that returns a different length has
    /// answered a different question.
    ///
    /// # Errors
    ///
    /// Whatever the source cannot do.
    fn samples(&mut self, media: Digest, start: i64, count: usize) -> Result<AudioBuffer>;
}
