// SPDX-License-Identifier: GPL-3.0-only
//! SMPTE timecode.
//!
//! Timecode is a *rendering* of an [`Instant`], never a storage form. It
//! labels frames; it does not count them. Drop-frame is the clearest case:
//! at 30000/1001 the labels `;00` and `;01` are skipped at the start of every
//! minute except every tenth, so that the label tracks wall-clock time. No
//! frame is dropped. Nothing is lost. Only the name changes.
//!
//! This is the arithmetic editors get wrong most often, so every path here is
//! exact integer arithmetic and every impossible label is refused by name.

use crate::status::{CoreStatus, Result};
use crate::time::Instant;
use crate::timebase::Timebase;

/// The number of frame labels skipped at each dropping minute.
///
/// Two at a nominal thirty, four at a nominal sixty: one label per whole
/// frame per second of the 0.1% the NTSC rate falls short.
const fn dropped_labels(nominal_rate: u32) -> u32 {
    nominal_rate / 15
}

/// A frame label: hours, minutes, seconds, frames, and how it counts.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Timecode {
    hours: u8,
    minutes: u8,
    seconds: u8,
    frames: u8,
    nominal_rate: u32,
    drop_frame: bool,
}

impl Timecode {
    /// Build a timecode from its fields.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::UnsupportedTimecodeRate`] for a rate this crate does not
    /// label, [`CoreStatus::DropFrameUnavailable`] when drop-frame is asked
    /// for at a rate that does not define it, and
    /// [`CoreStatus::TimecodeMalformed`] for fields that name no real frame —
    /// including a dropped label such as `00:01:00;00` at 30000/1001.
    pub fn new(
        hours: u8,
        minutes: u8,
        seconds: u8,
        frames: u8,
        nominal_rate: u32,
        drop_frame: bool,
    ) -> Result<Self> {
        if !matches!(nominal_rate, 24 | 25 | 30 | 48 | 50 | 60) {
            return Err(CoreStatus::UnsupportedTimecodeRate);
        }
        if drop_frame && !matches!(nominal_rate, 30 | 60) {
            return Err(CoreStatus::DropFrameUnavailable);
        }
        if hours >= 24 || minutes >= 60 || seconds >= 60 {
            return Err(CoreStatus::TimecodeMalformed);
        }
        if u32::from(frames) >= nominal_rate {
            return Err(CoreStatus::TimecodeMalformed);
        }
        if drop_frame
            && seconds == 0
            && minutes % 10 != 0
            && u32::from(frames) < dropped_labels(nominal_rate)
        {
            // This label is one of the ones the counting skips, so no frame
            // carries it. Refuse it rather than silently returning a
            // neighbouring frame (R-1.3).
            return Err(CoreStatus::TimecodeMalformed);
        }
        Ok(Self {
            hours,
            minutes,
            seconds,
            frames,
            nominal_rate,
            drop_frame,
        })
    }

    /// The hours field.
    #[must_use]
    pub const fn hours(self) -> u8 {
        self.hours
    }

    /// The minutes field.
    #[must_use]
    pub const fn minutes(self) -> u8 {
        self.minutes
    }

    /// The seconds field.
    #[must_use]
    pub const fn seconds(self) -> u8 {
        self.seconds
    }

    /// The frames field.
    #[must_use]
    pub const fn frames(self) -> u8 {
        self.frames
    }

    /// The nominal whole-number rate this label counts at.
    #[must_use]
    pub const fn nominal_rate(self) -> u32 {
        self.nominal_rate
    }

    /// Whether this label uses drop-frame counting.
    #[must_use]
    pub const fn is_drop_frame(self) -> bool {
        self.drop_frame
    }

    /// Label a frame number, counted from zero at midnight.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::UnsupportedTimecodeRate`],
    /// [`CoreStatus::DropFrameUnavailable`], or
    /// [`CoreStatus::TimecodeOutOfRange`] if the frame is negative or falls
    /// outside the twenty-four hour count.
    pub fn from_frame_number(frame: i64, nominal_rate: u32, drop_frame: bool) -> Result<Self> {
        let counting = Counting::new(nominal_rate, drop_frame)?;
        if frame < 0 || frame >= counting.frames_per_day() {
            return Err(CoreStatus::TimecodeOutOfRange);
        }
        let labelled = counting.frame_to_label_position(frame);
        let rate = i64::from(nominal_rate);
        let frames = labelled % rate;
        let total_seconds = labelled / rate;
        let seconds = total_seconds % 60;
        let total_minutes = total_seconds / 60;
        let minutes = total_minutes % 60;
        let hours = total_minutes / 60;
        // Every value here is bounded by the day check above, so each cast is
        // exact; the fallible conversion states that rather than assuming it.
        Self::new(
            u8::try_from(hours).map_err(|_| CoreStatus::TimecodeOutOfRange)?,
            u8::try_from(minutes).map_err(|_| CoreStatus::TimecodeOutOfRange)?,
            u8::try_from(seconds).map_err(|_| CoreStatus::TimecodeOutOfRange)?,
            u8::try_from(frames).map_err(|_| CoreStatus::TimecodeOutOfRange)?,
            nominal_rate,
            drop_frame,
        )
    }

    /// The frame number this label names, counted from zero at midnight.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::UnsupportedTimecodeRate`] or
    /// [`CoreStatus::DropFrameUnavailable`].
    pub fn to_frame_number(self) -> Result<i64> {
        let counting = Counting::new(self.nominal_rate, self.drop_frame)?;
        let rate = i64::from(self.nominal_rate);
        let total_minutes = i64::from(self.hours) * 60 + i64::from(self.minutes);
        let label_position =
            ((total_minutes * 60) + i64::from(self.seconds)) * rate + i64::from(self.frames);
        Ok(label_position - counting.labels_skipped_by(total_minutes))
    }

    /// Label the instant at this position.
    ///
    /// The counting style follows the timebase: 30000/1001 and 60000/1001 are
    /// labelled drop-frame, every other supported rate non-drop. Use
    /// [`Timecode::from_frame_number`] to label an NTSC rate non-drop.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::UnsupportedTimecodeRate`] for a timebase with no label
    /// form, or [`CoreStatus::TimecodeOutOfRange`].
    pub fn from_instant(instant: Instant) -> Result<Self> {
        let timebase = instant.timebase();
        let nominal_rate = timebase.nominal_rate()?;
        let drop_frame = timebase == Timebase::NTSC_30 || timebase == Timebase::NTSC_60;
        Self::from_frame_number(instant.ticks(), nominal_rate, drop_frame)
    }

    /// The instant this label names, in a timebase.
    ///
    /// # Errors
    ///
    /// [`CoreStatus::UnsupportedTimecodeRate`] if the timebase's nominal rate
    /// is not the one this label counts at.
    pub fn to_instant(self, timebase: Timebase) -> Result<Instant> {
        if timebase.nominal_rate()? != self.nominal_rate {
            return Err(CoreStatus::UnsupportedTimecodeRate);
        }
        Ok(Instant::new(self.to_frame_number()?, timebase))
    }
}

impl core::fmt::Display for Timecode {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        // A semicolon before the frames field is how drop-frame counting has
        // always announced itself, and reading it back is how a paste from a
        // spreadsheet keeps its meaning.
        let separator = if self.drop_frame { ';' } else { ':' };
        write!(
            formatter,
            "{:02}:{:02}:{:02}{}{:02}",
            self.hours, self.minutes, self.seconds, separator, self.frames
        )
    }
}

/// The counting rule for one rate and style.
#[derive(Clone, Copy)]
struct Counting {
    rate: i64,
    dropped: i64,
}

impl Counting {
    fn new(nominal_rate: u32, drop_frame: bool) -> Result<Self> {
        if !matches!(nominal_rate, 24 | 25 | 30 | 48 | 50 | 60) {
            return Err(CoreStatus::UnsupportedTimecodeRate);
        }
        if drop_frame && !matches!(nominal_rate, 30 | 60) {
            return Err(CoreStatus::DropFrameUnavailable);
        }
        Ok(Self {
            rate: i64::from(nominal_rate),
            dropped: if drop_frame {
                i64::from(dropped_labels(nominal_rate))
            } else {
                0
            },
        })
    }

    /// Frames in one minute of counting: one short minute, or a whole one.
    const fn frames_per_minute(self) -> i64 {
        self.rate * 60 - self.dropped
    }

    /// Frames in ten minutes: nine short minutes and one whole one.
    const fn frames_per_ten_minutes(self) -> i64 {
        self.rate * 600 - 9 * self.dropped
    }

    const fn frames_per_day(self) -> i64 {
        self.frames_per_ten_minutes() * 6 * 24
    }

    /// How many labels the counting has skipped by a given minute.
    const fn labels_skipped_by(self, total_minutes: i64) -> i64 {
        self.dropped * (total_minutes - total_minutes / 10)
    }

    /// Turn a frame number into the position it occupies in the label
    /// sequence, which is the frame number plus every label skipped before it.
    const fn frame_to_label_position(self, frame: i64) -> i64 {
        if self.dropped == 0 {
            return frame;
        }
        let ten_minute_blocks = frame / self.frames_per_ten_minutes();
        let within_block = frame % self.frames_per_ten_minutes();
        // The first minute of each ten-minute block is whole; the nine after
        // it are short, so each contributes one more skipped pair of labels.
        let skipped_within = if within_block >= self.dropped {
            self.dropped * ((within_block - self.dropped) / self.frames_per_minute())
        } else {
            0
        };
        frame + 9 * self.dropped * ten_minute_blocks + skipped_within
    }
}
