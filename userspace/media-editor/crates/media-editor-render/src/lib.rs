// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![allow(
    clippy::doc_markdown,
    reason = "Media Editor and Phipia are product names, not identifiers"
)]
//! The colour pipeline, computed exactly.
//!
//! A colour standard defines a gamut with twelve exact decimal numbers, and
//! everything an editor needs — the RGB to XYZ matrix, the luma coefficients,
//! the matrix that takes one gamut to another — is derived from them by linear
//! algebra. Most implementations do that derivation once in floating point,
//! round it to four places, and ship the rounding.
//!
//! This one does it in exact rationals, every time. The matrix is the real
//! one, it is identical on every machine, and it stays exact through as many
//! conversions as are chained together (R-4.1). Nothing here uses a floating
//! point number, which is also what makes it work on Phipia, where there is no
//! guarantee a Ring 3 program may execute one.

extern crate alloc;

pub mod chromaticity;
pub mod composite;
pub mod convert;
pub mod font;
pub mod graph;
pub mod look;
pub mod lut;
pub mod matrix;
pub mod resample;
pub mod scope;
pub mod shape;
pub mod status;
pub mod transfer;
pub mod vector;

pub use chromaticity::{Chromaticity, Gamut, gamut_of};
pub use composite::{checked_premultiplied, faded, over, premultiply, unpremultiply};
pub use convert::convert;
pub use graph::{Graph, Library, Node, NodeId, row_description};
pub use look::Look;
pub use lut::{Colour, Interpolation, Lut3D};
pub use matrix::{Matrix3, Vector3};
/// Re-exported so that `media_editor_render::Fixed` still names the one
/// fixed-point type. It lives in the core crate because a decibel needs the
/// same integer power function a transfer curve does, and sound must not
/// depend on colour to get at it.
pub use media_editor_core::Fixed;
pub use scope::{Histogram, LumaWeights, Waveform, histogram, waveform};
pub use status::{RenderStatus, Result};
pub use transfer::{decode, encode};
pub use vector::{MAX_BINS, Vectorscope, chroma_of, vectorscope};
