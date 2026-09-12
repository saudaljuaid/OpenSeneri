// SPDX-License-Identifier: GPL-3.0-only
//! How big a picture is.

use crate::status::{MediaStatus, Result};

/// The largest dimension a frame may have.
///
/// Sixteen thousand three hundred and eighty-four: past 16K in either
/// direction, a frame is not a picture this application was designed around,
/// and the bound exists so that the arithmetic below cannot be asked to
/// overflow rather than so that someone is stopped from trying (R-1.1).
pub const MAX_DIMENSION: u32 = 16_384;

/// How many pixels a frame may hold.
///
/// Sixteen 4K frames' worth. The real limit on a freestanding target is the
/// heap, and that is a far smaller number today; this one only has to keep the
/// products below inside a `usize`.
pub const MAX_PIXELS: u64 = 16_384 * 16_384;

/// A picture's dimensions in whole pixels.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Geometry {
    width: u32,
    height: u32,
}

impl Geometry {
    /// A geometry.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::EmptyGeometry`] for a zero dimension, and
    /// [`MediaStatus::GeometryTooLarge`] past the bounds above.
    pub fn new(width: u32, height: u32) -> Result<Self> {
        if width == 0 || height == 0 {
            return Err(MediaStatus::EmptyGeometry);
        }
        if width > MAX_DIMENSION || height > MAX_DIMENSION {
            return Err(MediaStatus::GeometryTooLarge);
        }
        if u64::from(width) * u64::from(height) > MAX_PIXELS {
            return Err(MediaStatus::GeometryTooLarge);
        }
        Ok(Self { width, height })
    }

    /// How wide.
    #[must_use]
    pub const fn width(self) -> u32 {
        self.width
    }

    /// How tall.
    #[must_use]
    pub const fn height(self) -> u32 {
        self.height
    }

    /// How many pixels.
    #[must_use]
    pub const fn pixels(self) -> u64 {
        self.width as u64 * self.height as u64
    }
}
