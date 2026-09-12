// SPDX-License-Identifier: GPL-3.0-only
//! Frames Media Editor draws for itself.
//!
//! Every fixture in this project is built by a deterministic tool rather than
//! downloaded, which is Phipia's practice and the only way a test's inputs can
//! be part of its evidence. These patterns are pure functions of their
//! description and their frame number: the same arguments produce the same
//! bytes on every machine, for ever, which is what makes a golden digest mean
//! something.

use alloc::vec::Vec;

use crate::colour::MatrixCoefficients;
use crate::format::PixelFormat;
use crate::frame::{Frame, FrameDescription};
use crate::geometry::Geometry;
use crate::status::{MediaStatus, Result};

/// The eight bars, in the order the standard puts them, as full-range red,
/// green, and blue.
const BARS: [[u8; 3]; 8] = [
    [255, 255, 255],
    [255, 255, 0],
    [0, 255, 255],
    [0, 255, 0],
    [255, 0, 255],
    [255, 0, 0],
    [0, 0, 255],
    [0, 0, 0],
];

/// What to draw.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum TestPattern {
    /// Eight vertical bars: white, yellow, cyan, green, magenta, red, blue,
    /// black.
    Bars,
    /// A horizontal ramp from black to white.
    Ramp,
    /// A checkerboard of a given square size, which makes scaling errors
    /// obvious at a glance.
    Checkerboard {
        /// How many pixels on a side each square is.
        square: u32,
    },
    /// A field of one value, which is the cheapest way to prove a pipeline
    /// changed nothing.
    Flat {
        /// The value every sample takes.
        value: u8,
    },
    /// Diagonal stripes, for a clip whose media is not there.
    ///
    /// Not a solid colour, and not black. Black is what an empty timeline
    /// shows and a solid colour is something a programme might legitimately
    /// contain, so either one lets "the drive is not mounted" look like
    /// footage. Diagonal stripes at an angle no camera produces cannot be
    /// mistaken for a picture, which is the only property this pattern needs
    /// and the reason it is a pattern rather than a colour.
    Offline,
}

impl TestPattern {
    /// Draw this pattern.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::PatternFormatUnsupported`] for a luma-chroma format:
    /// drawing one means choosing a matrix and a range and doing the
    /// conversion, and that is the colour pipeline's job rather than a
    /// fixture generator's (R-8.3).
    pub fn render(self, description: FrameDescription) -> Result<Frame> {
        let height = description.geometry().height();
        self.render_rows(description, 0, height, description)
    }

    /// Draw one row of this pattern, as a frame one row high.
    ///
    /// The pattern is placed against the **whole** picture and one row of it
    /// drawn — the same discipline a mask takes, and for the same reason: bars
    /// are eighths of the width and a checkerboard's squares are counted from
    /// the top, so a pattern drawn into a frame one row high would be a
    /// different picture rather than a row of this one.
    ///
    /// Exact rather than approximate, because [`TestPattern::colour_at`] is a
    /// function of position and nothing else. There is no state to carry from
    /// the row above.
    ///
    /// # Errors
    ///
    /// As [`TestPattern::render`], plus [`MediaStatus::GeometryTooLarge`] for
    /// a row past the bottom of the picture.
    pub fn render_row(self, description: FrameDescription, row: u32) -> Result<Frame> {
        let geometry = description.geometry();
        if row >= geometry.height() {
            return Err(MediaStatus::GeometryTooLarge);
        }
        let one = FrameDescription::new(
            Geometry::new(geometry.width(), 1)?,
            description.format(),
            description.colour(),
            description.siting(),
            description.alpha(),
            description.pixel_aspect(),
        )?;
        self.render_rows(description, row, row + 1, one)
    }

    /// Draw a run of rows, placed against the whole picture.
    ///
    /// One rasteriser for both forms, so a row and a frame cannot disagree
    /// about where a bar begins.
    fn render_rows(
        self,
        description: FrameDescription,
        from: u32,
        to: u32,
        out: FrameDescription,
    ) -> Result<Frame> {
        if description.colour().matrix != MatrixCoefficients::Identity {
            return Err(MediaStatus::PatternFormatUnsupported);
        }
        let format = description.format();
        let channels = match format {
            PixelFormat::Rgba8 => 4_usize,
            PixelFormat::Rgb8 => 3,
            PixelFormat::Gray8 => 1,
            PixelFormat::Yuv420p8 | PixelFormat::Yuv422p8 | PixelFormat::Yuv444p8 => {
                return Err(MediaStatus::PatternFormatUnsupported);
            }
        };

        let geometry = description.geometry();
        let total = out.packed_bytes()?;
        let mut samples = Vec::new();
        samples
            .try_reserve(total)
            .map_err(|_| MediaStatus::OutOfMemory)?;

        for y in from..to {
            for x in 0..geometry.width() {
                let rgb = self.colour_at(x, y, geometry.width(), geometry.height());
                match channels {
                    1 => samples.push(luma_of(rgb)),
                    3 => samples.extend_from_slice(&rgb),
                    _ => {
                        samples.extend_from_slice(&rgb);
                        samples.push(255);
                    }
                }
            }
        }
        Frame::from_owned(out, samples)
    }

    /// The colour of one pixel.
    fn colour_at(self, x: u32, y: u32, width: u32, height: u32) -> [u8; 3] {
        match self {
            Self::Bars => {
                // Integer arithmetic throughout: the boundary between bars must
                // fall on the same pixel on every machine (R-4.1), and a
                // floating-point width would not promise that.
                let bar = (u64::from(x) * 8) / u64::from(width);
                let index = usize::try_from(bar).unwrap_or(7).min(7);
                BARS[index]
            }
            Self::Ramp => {
                let value = if width == 1 {
                    0
                } else {
                    u8::try_from((u64::from(x) * 255) / u64::from(width - 1)).unwrap_or(255)
                };
                [value, value, value]
            }
            Self::Checkerboard { square } => {
                let side = u64::from(square.max(1));
                let dark = ((u64::from(x) / side) + (u64::from(y) / side)) % 2 == 0;
                if dark { [16, 16, 16] } else { [235, 235, 235] }
            }
            Self::Flat { value } => {
                let _ = (x, y, width, height);
                [value, value, value]
            }
            Self::Offline => {
                // A **fraction of the frame** rather than a pixel count. A
                // slate has to read as a slate at every size, and a fixed
                // sixteen-pixel period is a solid colour on anything smaller
                // than sixteen pixels -- which is precisely the case where
                // "the drive is not mounted" would look like footage. A test
                // at four pixels across is what found that.
                //
                // Two pixels at the floor, because one would alternate every
                // pixel and read as noise rather than as stripes.
                let _ = height;
                let period = (u64::from(width) / 32).max(2);
                let band = ((u64::from(x) + u64::from(y)) / period) % 2 == 0;
                if band { [180, 24, 24] } else { [48, 8, 8] }
            }
        }
    }
}

/// The BT.709 luma of a full-range colour, in integers.
///
/// The coefficients are 0.2126, 0.7152, and 0.0722, scaled by 65,536 and
/// rounded once, so that this is exact and reproducible rather than
/// approximately right in a way that differs by compiler.
fn luma_of(rgb: [u8; 3]) -> u8 {
    const RED: u32 = 13_933;
    const GREEN: u32 = 46_871;
    const BLUE: u32 = 4_732;
    let value = u32::from(rgb[0]) * RED + u32::from(rgb[1]) * GREEN + u32::from(rgb[2]) * BLUE;
    // Round to nearest rather than truncate, so white stays 255.
    u8::try_from((value + 32_768) >> 16).unwrap_or(255)
}
