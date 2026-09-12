// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![allow(
    clippy::doc_markdown,
    reason = "Media Editor and Phipia are product names, not identifiers"
)]
//! Frames, and everything true about them.
//!
//! A frame here is immutable, fully described, and identified by a digest over
//! both. There is no way to build one whose colour is unstated: the types have
//! no `Unknown`, no `Unspecified`, and no `Default`, so the untagged frame that
//! causes every washed-out export in this industry is not a value this crate
//! can produce.
//!
//! The pool that holds frames is bounded in frames and in bytes, evicts by
//! use with deterministic tie-breaking, and keys entries by a digest over
//! their inputs, their parameters, and a code version — so a cached frame
//! cannot outlive a change to the code that made it.

extern crate alloc;

pub mod colour;
pub mod format;
pub mod frame;
pub mod geometry;
pub mod pattern;
pub mod pool;
pub mod status;

pub use colour::{
    AlphaState, ChromaSiting, ColourDescription, MatrixCoefficients, Primaries, Range,
    TransferFunction,
};
pub use format::PixelFormat;
pub use frame::{Frame, FrameDescription, MAX_FRAME_BYTES, Plane};
pub use geometry::Geometry;
pub use pattern::TestPattern;
pub use pool::{CODE_VERSION, CacheKey, FramePool, PoolCensus};
pub use status::{MediaStatus, Result};
