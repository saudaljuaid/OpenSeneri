// SPDX-License-Identifier: GPL-3.0-only
//! How a frame's samples are laid out.
//!
//! The plane arithmetic in this file is where picture bugs live: an off-by-one
//! in a chroma plane's height is a green band at the bottom of every frame in
//! an export, and it is the kind of mistake that survives review because the
//! expression looks right. So every size here is computed by one function,
//! that function refuses what it cannot compute exactly, and the tests check
//! it against every dimension in a range rather than against three examples.

use crate::geometry::Geometry;
use crate::status::{MediaStatus, Result};

/// How many planes any format in this set uses.
pub const MAX_PLANES: usize = 3;

/// The layouts Media Editor holds a frame in.
///
/// Eight bits per sample throughout, because that is what the first mezzanine
/// format carries. Deeper samples are a new contract with new arithmetic, not
/// a variant quietly added here (R-1.2).
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum PixelFormat {
    /// Red, green, blue, alpha, interleaved, one plane.
    Rgba8,
    /// Red, green, blue, interleaved, one plane.
    Rgb8,
    /// One sample per pixel, one plane.
    Gray8,
    /// Planar luma and half-resolution chroma in both directions.
    Yuv420p8,
    /// Planar luma and half-resolution chroma horizontally.
    Yuv422p8,
    /// Planar luma and full-resolution chroma.
    Yuv444p8,
}

impl PixelFormat {
    /// How many planes this format uses.
    #[must_use]
    pub const fn plane_count(self) -> usize {
        match self {
            Self::Rgba8 | Self::Rgb8 | Self::Gray8 => 1,
            Self::Yuv420p8 | Self::Yuv422p8 | Self::Yuv444p8 => 3,
        }
    }

    /// How many bytes one pixel of a packed format occupies.
    ///
    /// Planar formats have no such number, which is why this returns one only
    /// for the packed ones.
    #[must_use]
    pub const fn packed_bytes_per_pixel(self) -> Option<u32> {
        match self {
            Self::Rgba8 => Some(4),
            Self::Rgb8 => Some(3),
            Self::Gray8 => Some(1),
            Self::Yuv420p8 | Self::Yuv422p8 | Self::Yuv444p8 => None,
        }
    }

    /// Whether the samples are already red, green, and blue.
    #[must_use]
    pub const fn is_rgb(self) -> bool {
        matches!(self, Self::Rgba8 | Self::Rgb8 | Self::Gray8)
    }

    /// Whether this format carries an alpha channel.
    #[must_use]
    pub const fn has_alpha(self) -> bool {
        matches!(self, Self::Rgba8)
    }

    /// Whether chroma is stored at a lower resolution than luma.
    #[must_use]
    pub const fn is_subsampled(self) -> bool {
        matches!(self, Self::Yuv420p8 | Self::Yuv422p8)
    }

    /// How much this format divides a plane's width and height.
    ///
    /// Plane zero is never divided; the others are divided by these.
    const fn chroma_division(self) -> (u32, u32) {
        match self {
            Self::Yuv420p8 => (2, 2),
            Self::Yuv422p8 => (2, 1),
            _ => (1, 1),
        }
    }

    /// The dimensions of one plane of a frame of this format.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::PlaneCountMismatch`] if the index names no plane, and
    /// [`MediaStatus::OddDimension`] if a dimension this format halves is odd.
    /// An odd dimension is refused rather than rounded, because rounding it
    /// either way makes a chroma plane that does not describe the luma one.
    pub fn plane_geometry(self, geometry: Geometry, plane: usize) -> Result<Geometry> {
        if plane >= self.plane_count() {
            return Err(MediaStatus::PlaneCountMismatch);
        }
        if plane == 0 {
            return Ok(geometry);
        }
        let (horizontal, vertical) = self.chroma_division();
        if geometry.width() % horizontal != 0 || geometry.height() % vertical != 0 {
            return Err(MediaStatus::OddDimension);
        }
        Geometry::new(geometry.width() / horizontal, geometry.height() / vertical)
    }

    /// How many bytes one row of a plane occupies with no padding.
    ///
    /// # Errors
    ///
    /// As [`PixelFormat::plane_geometry`].
    pub fn plane_row_bytes(self, geometry: Geometry, plane: usize) -> Result<usize> {
        let plane_geometry = self.plane_geometry(geometry, plane)?;
        let samples =
            usize::try_from(plane_geometry.width()).map_err(|_| MediaStatus::GeometryTooLarge)?;
        let per_sample = if plane == 0 {
            usize::try_from(self.packed_bytes_per_pixel().unwrap_or(1))
                .map_err(|_| MediaStatus::GeometryTooLarge)?
        } else {
            1
        };
        samples
            .checked_mul(per_sample)
            .ok_or(MediaStatus::GeometryTooLarge)
    }

    /// How many bytes a whole frame of this format occupies with no padding.
    ///
    /// # Errors
    ///
    /// As [`PixelFormat::plane_geometry`], or [`MediaStatus::GeometryTooLarge`].
    pub fn packed_frame_bytes(self, geometry: Geometry) -> Result<usize> {
        let mut total = 0_usize;
        for plane in 0..self.plane_count() {
            let rows = usize::try_from(self.plane_geometry(geometry, plane)?.height())
                .map_err(|_| MediaStatus::GeometryTooLarge)?;
            let row = self.plane_row_bytes(geometry, plane)?;
            let plane_bytes = row.checked_mul(rows).ok_or(MediaStatus::GeometryTooLarge)?;
            total = total
                .checked_add(plane_bytes)
                .ok_or(MediaStatus::GeometryTooLarge)?;
        }
        Ok(total)
    }

    /// Whether a geometry can be expressed in this format at all.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::OddDimension`].
    pub fn accepts(self, geometry: Geometry) -> Result<()> {
        for plane in 0..self.plane_count() {
            self.plane_geometry(geometry, plane)?;
        }
        Ok(())
    }
}
