// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![allow(
    clippy::doc_markdown,
    reason = "Media Editor and Phipia are product names, not identifiers"
)]
//! Sound, computed with integers.
//!
//! Everything a mixer needs before it needs a sound device: blocks of samples
//! that know their own rate, gain in the decibels a fader is actually labelled
//! in, a pan law that keeps the energy constant across the image, and a sum
//! that reports what full scale cost rather than reaching it quietly.
//!
//! There is no floating point in this crate, and that is not a stylistic
//! choice twice over. Phipia enables neither `CR4.OSFXSR` nor `CR4.OSXSAVE`
//! and preserves no floating-point state anywhere, so a Ring 3 program has no
//! guarantee it may execute a single such instruction (`PHIP-04`). And even on
//! a machine that could, `pow` and `log` are not specified bit-for-bit by IEEE
//! 754 — so two machines mixing the same session would bounce different files,
//! which is exactly the class of difference an editor must not have (R-4.1).
//!
//! A decibel is a logarithm and a constant-power pan is a square root, so both
//! come from [`media_editor_core::Fixed`], which computes them with integers and
//! no libm.
//!
//! What is deliberately **not** here: resampling, dither, and any filter at
//! all. Each is a decision with a name and a contract of its own, and a mixer
//! that quietly performed one would be answering a question nobody asked
//! (R-1.3). `PHIP-13` is the sound device these buffers eventually reach.

extern crate alloc;

pub mod buffer;
pub mod gain;
pub mod loudness;
pub mod mix;
pub mod overview;
pub mod pan;
pub mod source;
pub mod status;

pub use buffer::{
    AudioBuffer, FULL_SCALE, MAX_CHANNELS, MAX_SAMPLES, NEGATIVE_FULL_SCALE, SampleRate,
};
pub use gain::{Gain, MAXIMUM_DECIBELS, MINIMUM_DECIBELS};
pub use loudness::{integrated, momentary};
pub use mix::{MixReport, Source, mix, pan_to_stereo};
pub use overview::{Bucket, Overview};
pub use pan::Pan;
pub use source::SampleSource;
pub use status::{AudioStatus, Result};
