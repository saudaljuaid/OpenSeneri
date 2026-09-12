// SPDX-License-Identifier: GPL-3.0-only
//! A caption sidecar: WebVTT written beside the reel.
//!
//! The reel carries its own transcript (`SPRW` version five), and that is the
//! right place for it: one file, one digest, one thing to hand somebody. A
//! **sidecar** is the other thing an edit room wants — the same words as a
//! separate file, in a format that is not ours, so that captions stay
//! switchable, a player that has never heard of `SPRW` can still show them,
//! and a caption editor can open them without a converter.
//!
//! ## Why WebVTT and not SCC
//!
//! SCC is the broadcast format and it is the wrong first one. It is
//! byte-oriented, tied to 29.97 and drop-frame, and carries its text as
//! CEA-608 control codes — a format about a *decoder*, not about words. WebVTT
//! is text, its cues are written in order with nothing to seek back to, and
//! its timestamps are wall-clock. All three suit a writer that streams and a
//! machine that cannot hold the file.
//!
//! ## The timestamps are wall-clock, and that is the whole arithmetic
//!
//! [`media_editor_core::Timecode`] is a *label* on a frame. WebVTT wants the
//! moment the frame is shown. At 30000/1001 those are not the same number and
//! the difference compounds: non-drop timecode labels frame *n* as `n/30`
//! seconds while the frame is at `n·1001/30000`, which is one part in a
//! thousand — **3.6 seconds of drift an hour**, enough to put a caption over
//! the wrong line of dialogue by the end of a reel.
//!
//! So a cue's time comes from [`media_editor_core::Instant::seconds`], which is
//! exact in rationals, and is rounded to the millisecond WebVTT writes in.
//! Rounding is where the care goes; [`milliseconds`] says what it preserves.

use media_editor_core::{Instant, Rational, Timebase};
use media_editor_model::caption::Caption;

use crate::bytes::{Sink, Writer};
use crate::status::{IoStatus, Result};

/// What a WebVTT file must begin with.
///
/// The blank line is part of it: a WebVTT file is a signature block followed
/// by cue blocks, and blocks are separated by blank lines.
pub const MAGIC: &[u8] = b"WEBVTT\n\n";

/// The most bytes one cue can occupy, which is what its buffer is bounded at.
///
/// Derived rather than guessed, worst case by worst case:
///
/// - the identifier line is a cue number and a newline. Twenty digits is every
///   `u64` there is: **21**;
/// - the timing line is two timestamps, ` --> ` and a newline. A timestamp is
///   `HH:MM:SS.mmm` with the hours unbounded above, so twenty digits of hours
///   plus `:MM:SS.mmm` is 30: **2 × 30 + 5 + 1 = 66**;
/// - the voice tag is `<v Voice N>` with one digit of voice, since
///   [`media_editor_model::caption::MAX_VOICES`] is eight: **11**;
/// - the text is [`media_editor_model::caption::MAX_CAPTION_TEXT`] **characters**. A character is at most
///   four bytes of UTF-8, and the only characters that grow are `&`, `<` and
///   `>` — one byte each, becoming at most five (`&amp;`). No character is
///   both, so the worst is five bytes each: **128 × 5 = 640**;
/// - `</v>`, the newline after it, and the blank line that ends the block:
///   **6**.
///
/// That is 744, and this is 768.
pub const MAX_CUE_BYTES: usize = 768;

/// How many milliseconds are in one second, one minute and one hour.
const SECOND: u64 = 1_000;
const MINUTE: u64 = 60 * SECOND;
const HOUR: u64 = 60 * MINUTE;

/// The millisecond a position falls on, rounded to nearest.
///
/// Exact until the last step. [`Instant::seconds`] is a rational, the
/// multiplication by a thousand is exact, and only the floor at the end loses
/// anything — by design, because WebVTT has no finer unit to lose it in.
///
/// **Ties round up**, which is the ordinary convention and is not the
/// interesting part. The interesting part is what rounding preserves, because
/// a caption file that drifts is a nuisance and one that reorders itself is
/// wrong:
///
/// - it is **monotone**. Rounding a monotone function is monotone, so two
///   positions in order stay in order, and two captions that were disjoint in
///   ticks are disjoint in milliseconds. Nothing here has to check that; it is
///   a property of flooring, and [`Spotter::cue`]'s ordering refusal is
///   therefore about a caller handing cues in the wrong order rather than
///   about this arithmetic.
/// - it is **not injective**, and that is the one thing it costs. Two ticks
///   under a millisecond apart become one number, so a cue shorter than half a
///   millisecond vanishes. That is refused rather than written, because a cue
///   with no duration is a cue nobody sees and a file nobody can tell is
///   wrong.
///
/// # Errors
///
/// [`IoStatus::Time`] wrapping an overflow.
pub fn milliseconds(at: Instant) -> Result<i64> {
    at.seconds()
        .and_then(|seconds| seconds.scale(1_000))
        .and_then(|millis| millis.checked_add(Rational::HALF))
        .and_then(Rational::floor)
        .map_err(IoStatus::Time)
}

/// A sidecar written one cue at a time.
///
/// The same shape as [`crate::sprw::Winder`] and for the same reason: a
/// machine mapped at seventy-six kibibytes cannot hold a file to write it. One
/// buffer of [`MAX_CUE_BYTES`] is allocated here and refilled for every cue,
/// so a sidecar of sixteen thousand cues allocates once.
///
/// ## There is no `finish`
///
/// A reel ends with a trailer, so an interrupted reel is a file that refuses
/// to open. A WebVTT file ends whenever it ends: an interrupted sidecar is a
/// *shorter valid sidecar*, and nothing in the format can tell. That is a
/// property of WebVTT and not something to fix here — what makes it safe is
/// where the bytes go, not what they are. A sidecar is assembled in the
/// scratch slot and committed atomically like everything else (R-9.4), so a
/// truncated one never becomes the committed one.
pub struct Spotter {
    held: Writer,
    count: usize,
    /// The last cue's start, in milliseconds. Nought before the first, which
    /// is what makes a cue before the beginning of the file refuse as being
    /// out of order — it is.
    last: i64,
}

impl Spotter {
    /// Begin a sidecar, writing the signature.
    ///
    /// # Errors
    ///
    /// [`IoStatus::SinkNotEmpty`] for a sink that already holds something — a
    /// signature belongs at offset nought — and whatever the sink refuses.
    pub fn begin(sink: &mut dyn Sink) -> Result<Self> {
        if sink.written() != 0 {
            return Err(IoStatus::SinkNotEmpty);
        }
        sink.append(MAGIC)?;
        Ok(Self {
            held: Writer::new(MAX_CUE_BYTES),
            count: 0,
            last: 0,
        })
    }

    /// How many cues have been written.
    #[must_use]
    pub const fn count(&self) -> usize {
        self.count
    }

    /// Write one cue.
    ///
    /// The caption's in and out points are ticks of `timebase`, counted from
    /// the sidecar's own nought — which is the programme's start, because a
    /// sidecar describes the file it sits beside rather than the timeline the
    /// file was cut from.
    ///
    /// # Errors
    ///
    /// [`IoStatus::CueOutOfOrder`] for a cue starting before the one before it
    /// or before the file does, [`IoStatus::CueVanishes`] for one that rounds
    /// to no duration, [`IoStatus::EmptyCue`] for one with no words,
    /// [`IoStatus::CueTextNotOneBlock`] for text holding a blank line,
    /// [`IoStatus::CueTextLooksLikeATiming`] for text holding `-->`,
    /// [`IoStatus::Time`] wrapping an overflow, and whatever the sink refuses.
    pub fn cue(
        &mut self,
        sink: &mut dyn Sink,
        caption: &Caption,
        timebase: Timebase,
    ) -> Result<()> {
        let from = milliseconds(Instant::new(caption.from(), timebase))?;
        let to = milliseconds(Instant::new(caption.to(), timebase))?;
        if from < self.last {
            return Err(IoStatus::CueOutOfOrder);
        }
        if to <= from {
            return Err(IoStatus::CueVanishes);
        }
        checked_text(caption.text())?;

        self.held.clear();
        let number = u64::try_from(self.count).map_err(|_| IoStatus::TooMany)?;
        self.held.decimal(number.saturating_add(1), 1)?;
        self.held.u8(b'\n')?;
        stamp(&mut self.held, from)?;
        self.held.bytes(b" --> ")?;
        stamp(&mut self.held, to)?;
        self.held.bytes(b"\n<v Voice ")?;
        self.held.decimal(u64::from(caption.voice()), 1)?;
        self.held.u8(b'>')?;
        escaped(&mut self.held, caption.text())?;
        self.held.bytes(b"</v>\n\n")?;

        sink.append(self.held.as_slice())?;
        self.count = self.count.checked_add(1).ok_or(IoStatus::TooMany)?;
        self.last = from;
        Ok(())
    }
}

/// What a cue's text may not be.
///
/// Two of these are about the *file* rather than about the words, which is why
/// they are refusals and not escapes: WebVTT has no spelling for either. A
/// blank line ends a cue block, so text containing one would silently become
/// two cues, the second of them malformed. `-->` is what a timing line is made
/// of, so text containing one would silently become a timing line.
///
/// A carriage return counts as a line ending in WebVTT, so `\r\n\r\n` and
/// `\r\r` are blank lines too, and a lone `\r` is a line break this writer has
/// no reason to emit. All of them are refused together.
///
/// A single newline is *allowed*, and deliberately: two lines is what a
/// caption normally is.
fn checked_text(text: &str) -> Result<()> {
    if text.is_empty() {
        return Err(IoStatus::EmptyCue);
    }
    if text.contains('\r') || text.contains("\n\n") {
        return Err(IoStatus::CueTextNotOneBlock);
    }
    if text.contains("-->") {
        return Err(IoStatus::CueTextLooksLikeATiming);
    }
    Ok(())
}

/// One timestamp, `HH:MM:SS.mmm`.
///
/// Hours are written to at least two digits and are not bounded above: a
/// sidecar for a long programme says `100:00:00.000` rather than wrapping,
/// which is what WebVTT asks for and is the only answer that stays ordered.
fn stamp(out: &mut Writer, millis: i64) -> Result<()> {
    // A negative millisecond is a cue before the file begins, which `cue`
    // has already turned away as being out of order -- `last` starts at
    // nought. This is the same refusal reached from the other side, so that
    // a caller of `stamp` cannot get a wrapped timestamp out of it.
    let unsigned = u64::try_from(millis).map_err(|_| IoStatus::CueOutOfOrder)?;
    out.decimal(unsigned / HOUR, 2)?;
    out.u8(b':')?;
    out.decimal((unsigned / MINUTE) % 60, 2)?;
    out.u8(b':')?;
    out.decimal((unsigned / SECOND) % 60, 2)?;
    out.u8(b'.')?;
    out.decimal(unsigned % SECOND, 3)
}

/// A cue's words, with the three characters that are markup spelled out.
///
/// Escaped rather than refused, unlike the two conditions in [`checked_text`],
/// and the difference is that WebVTT *has* a spelling for these. "R&D" and
/// "5 < 6" are things people say, and a caption format that refused them would
/// be a caption format nobody could use.
fn escaped(out: &mut Writer, text: &str) -> Result<()> {
    let mut plain = 0_usize;
    for (at, character) in text.char_indices() {
        let spelled: &[u8] = match character {
            '&' => b"&amp;",
            '<' => b"&lt;",
            '>' => b"&gt;",
            _ => continue,
        };
        out.bytes(&text.as_bytes()[plain..at])?;
        out.bytes(spelled)?;
        plain = at + character.len_utf8();
    }
    out.bytes(&text.as_bytes()[plain..])
}

/// How many bytes a sidecar of this many captions can be.
///
/// The signature plus the worst cue, that many times. A caller that has to
/// reserve a slot before it writes asks this rather than guessing.
///
/// # Errors
///
/// [`IoStatus::TooMany`] if the answer does not fit.
pub fn sidecar_bytes(count: usize) -> Result<usize> {
    count
        .checked_mul(MAX_CUE_BYTES)
        .and_then(|cues| cues.checked_add(MAGIC.len()))
        .ok_or(IoStatus::TooMany)
}
