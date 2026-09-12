// SPDX-License-Identifier: GPL-3.0-only
//! Read and write CMX 3600 edit decision lists.
//!
//! ```text
//! TITLE:   REEL ONE
//! FCM: NON-DROP FRAME
//! 001  AX       V     C        00:00:11:18 00:01:09:28 00:00:00:00 00:00:58:10
//! 002  AX       V     C        00:02:46:24 00:03:16:26 00:00:58:10 00:01:28:12
//! ```
//!
//! Source out points are exclusive. `FCM` state applies until replaced, and
//! drop-frame punctuation must agree with it when both are present. Reel names
//! are limited to eight characters, so the writer also emits a `FROM CLIP NAME`
//! comment with the fuller source identity.

use alloc::string::String;
use alloc::vec::Vec;

use media_editor_core::{CoreStatus, Timecode};

use crate::status::{IoStatus, Result};

/// The most events one list may hold.
///
/// Ten thousand: a feature's conformed cut list is a few thousand, and this is
/// a bound a hostile file cannot talk its way past (R-11.2).
pub const MAX_EVENTS: usize = 10_000;

/// How many characters a reel name may have.
///
/// Eight, because that is what the format allows. It is not a limit this
/// implementation chose and it is not one it can lift.
pub const REEL_CHARACTERS: usize = 8;

/// The longest line this parser will look at.
pub const MAX_LINE_BYTES: usize = 256;

/// Which track an event lands on.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Channel {
    /// Picture.
    Video,
    /// Sound, on the numbered track. `A` is 1, `A2` is 2.
    Audio(u8),
    /// Picture and the first two sound tracks together, written `B`.
    VideoAndAudio,
}

/// How an event begins.
///
/// A dissolve and a wipe carry a duration in frames; a cut does not, because a
/// cut has no duration. Representing that as an `Option` on every transition
/// would let a cut carry one.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Transition {
    /// A straight cut.
    Cut,
    /// A dissolve over this many frames.
    Dissolve(u32),
    /// A wipe of this pattern number over this many frames.
    Wipe {
        /// Which of the machine's wipe patterns, by its number.
        pattern: u32,
        /// How many frames it takes.
        frames: u32,
    },
}

/// One event: a piece of source, and where it lands on the record.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Event {
    number: u32,
    reel: String,
    channel: Channel,
    transition: Transition,
    source_in: Timecode,
    source_out: Timecode,
    record_in: Timecode,
    record_out: Timecode,
    from_clip_name: Option<String>,
}

impl Event {
    /// The event number as the file gave it.
    #[must_use]
    pub const fn number(&self) -> u32 {
        self.number
    }

    /// The eight-character reel name.
    #[must_use]
    pub fn reel(&self) -> &str {
        &self.reel
    }

    /// Which track this lands on.
    #[must_use]
    pub const fn channel(&self) -> Channel {
        self.channel
    }

    /// How it begins.
    #[must_use]
    pub const fn transition(&self) -> Transition {
        self.transition
    }

    /// The first frame of source used.
    #[must_use]
    pub const fn source_in(&self) -> Timecode {
        self.source_in
    }

    /// The first frame of source **not** used.
    #[must_use]
    pub const fn source_out(&self) -> Timecode {
        self.source_out
    }

    /// Where it starts on the record.
    #[must_use]
    pub const fn record_in(&self) -> Timecode {
        self.record_in
    }

    /// The first frame on the record this event does not occupy.
    #[must_use]
    pub const fn record_out(&self) -> Timecode {
        self.record_out
    }

    /// The full source name, if the file carried the comment that holds it.
    #[must_use]
    pub fn from_clip_name(&self) -> Option<&str> {
        self.from_clip_name.as_deref()
    }

    /// How many frames of source this event uses.
    ///
    /// Out minus in, because out is exclusive. This is the arithmetic that an
    /// off-by-one importer gets wrong.
    ///
    /// # Errors
    ///
    /// [`IoStatus::Time`] wrapping an arithmetic refusal, or
    /// [`IoStatus::EdlNegativeDuration`] if out is not after in.
    pub fn source_frames(&self) -> Result<i64> {
        span(self.source_in, self.source_out)
    }

    /// How many frames of the record this event occupies.
    ///
    /// # Errors
    ///
    /// As [`Event::source_frames`].
    pub fn record_frames(&self) -> Result<i64> {
        span(self.record_in, self.record_out)
    }
}

/// The frames between two timecodes, with out exclusive.
fn span(from: Timecode, to: Timecode) -> Result<i64> {
    if from.nominal_rate() != to.nominal_rate() || from.is_drop_frame() != to.is_drop_frame() {
        return Err(IoStatus::Time(CoreStatus::TimebaseMismatch));
    }
    let start = from.to_frame_number().map_err(IoStatus::Time)?;
    let end = to.to_frame_number().map_err(IoStatus::Time)?;
    if end <= start {
        return Err(IoStatus::EdlNegativeDuration);
    }
    Ok(end - start)
}

/// A parsed edit decision list.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EditDecisionList {
    title: String,
    events: Vec<Event>,
}

impl EditDecisionList {
    /// The list's title.
    #[must_use]
    pub fn title(&self) -> &str {
        &self.title
    }

    /// The events, in file order.
    #[must_use]
    pub fn events(&self) -> &[Event] {
        &self.events
    }

    /// Whether one channel's record side is an unbroken run with no gaps.
    ///
    /// A conformed cut list is contiguous: each event begins where the last one
    /// ended. A list that is not has black between its events, and an importer
    /// that assumed otherwise would silently close the gaps and shorten the
    /// programme.
    ///
    /// Asked one channel at a time, because a list is not one run. Picture and
    /// sound both start at the top of the programme, so a whole-list version of
    /// this question would report every ordinary two-channel list as full of
    /// holes — an answer that is true of the file and false of the cut.
    ///
    /// # Errors
    ///
    /// [`IoStatus::Time`] wrapping an arithmetic refusal.
    pub fn is_contiguous(&self, channel: Channel) -> Result<bool> {
        let mut previous: Option<Timecode> = None;
        for event in self.events.iter().filter(|event| event.channel == channel) {
            if let Some(end) = previous
                && (end.to_frame_number().map_err(IoStatus::Time)?
                    != event.record_in.to_frame_number().map_err(IoStatus::Time)?)
            {
                return Ok(false);
            }
            previous = Some(event.record_out);
        }
        Ok(true)
    }

    /// Every channel the list puts an event on, in the order they first appear.
    #[must_use]
    pub fn channels(&self) -> Vec<Channel> {
        let mut seen = Vec::new();
        for event in &self.events {
            if !seen.contains(&event.channel) {
                seen.push(event.channel);
            }
        }
        seen
    }
}

/// Read an edit decision list.
///
/// # Errors
///
/// One of the `Edl` variants of [`IoStatus`] naming what the file said that it
/// could not mean, or [`IoStatus::Time`] for a timecode that names no frame.
pub fn parse(text: &str) -> Result<EditDecisionList> {
    let mut title = String::new();
    let mut events: Vec<Event> = Vec::new();
    let mut drop_frame: Option<bool> = None;

    for line in text.lines() {
        if line.len() > MAX_LINE_BYTES {
            return Err(IoStatus::EdlLineTooLong);
        }
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }
        if let Some(rest) = strip_prefix_ignoring_case(trimmed, "TITLE:") {
            title.clear();
            title.push_str(rest.trim());
            continue;
        }
        if let Some(rest) = strip_prefix_ignoring_case(trimmed, "FCM:") {
            drop_frame = Some(read_frame_code_mode(rest.trim())?);
            continue;
        }
        if let Some(rest) = trimmed.strip_prefix('*') {
            // A comment, and one of them carries the source name the eight
            // character reel field could not.
            if let Some(name) = strip_prefix_ignoring_case(rest.trim(), "FROM CLIP NAME:") {
                let last = events.last_mut().ok_or(IoStatus::EdlCommentBeforeEvent)?;
                let mut held = String::new();
                held.push_str(name.trim());
                last.from_clip_name = Some(held);
            }
            continue;
        }
        if events.len() >= MAX_EVENTS {
            return Err(IoStatus::TooMany);
        }
        let event = read_event(trimmed, drop_frame)?;
        events.push(event);
    }

    if events.is_empty() {
        return Err(IoStatus::EdlNoEvents);
    }
    Ok(EditDecisionList { title, events })
}

/// Read one event line.
fn read_event(line: &str, declared_drop_frame: Option<bool>) -> Result<Event> {
    let mut fields = line.split_whitespace();
    let number = fields
        .next()
        .ok_or(IoStatus::EdlMalformedEvent)?
        .parse::<u32>()
        .map_err(|_| IoStatus::EdlMalformedEvent)?;

    let reel = fields.next().ok_or(IoStatus::EdlMalformedEvent)?;
    if reel.chars().count() > REEL_CHARACTERS {
        return Err(IoStatus::EdlReelTooLong);
    }
    if !reel.chars().all(|character| character.is_ascii_graphic()) {
        return Err(IoStatus::EdlMalformedEvent);
    }

    let channel = read_channel(fields.next().ok_or(IoStatus::EdlMalformedEvent)?)?;
    let code = fields.next().ok_or(IoStatus::EdlMalformedEvent)?;
    let transition = read_transition(code, &mut fields)?;

    let mut timecodes = [None; 4];
    for slot in &mut timecodes {
        *slot = Some(fields.next().ok_or(IoStatus::EdlMalformedEvent)?);
    }
    if fields.next().is_some() {
        return Err(IoStatus::EdlMalformedEvent);
    }

    let mut parsed = [None; 4];
    for (slot, text) in parsed.iter_mut().zip(timecodes.iter()) {
        let text = text.ok_or(IoStatus::EdlMalformedEvent)?;
        *slot = Some(read_timecode(text, declared_drop_frame)?);
    }

    let mut held = String::new();
    held.push_str(reel);
    let event = Event {
        number,
        reel: held,
        channel,
        transition,
        source_in: parsed[0].ok_or(IoStatus::EdlMalformedEvent)?,
        source_out: parsed[1].ok_or(IoStatus::EdlMalformedEvent)?,
        record_in: parsed[2].ok_or(IoStatus::EdlMalformedEvent)?,
        record_out: parsed[3].ok_or(IoStatus::EdlMalformedEvent)?,
        from_clip_name: None,
    };
    // An event whose out is not after its in names no frames. It is not a
    // zero-length event; it is a file that does not describe an edit.
    event.source_frames()?;
    event.record_frames()?;
    Ok(event)
}

/// Read the channel field.
fn read_channel(field: &str) -> Result<Channel> {
    match field {
        "V" => Ok(Channel::Video),
        "B" => Ok(Channel::VideoAndAudio),
        "A" | "AA" => Ok(Channel::Audio(1)),
        other => {
            let rest = other
                .strip_prefix('A')
                .ok_or(IoStatus::EdlUnknownChannel)?
                .parse::<u8>()
                .map_err(|_| IoStatus::EdlUnknownChannel)?;
            if rest == 0 {
                return Err(IoStatus::EdlUnknownChannel);
            }
            Ok(Channel::Audio(rest))
        }
    }
}

/// Read the transition field, taking its duration from the following fields.
fn read_transition<'a>(
    code: &str,
    fields: &mut impl Iterator<Item = &'a str>,
) -> Result<Transition> {
    if code == "C" {
        return Ok(Transition::Cut);
    }
    if code == "D" {
        let frames = fields
            .next()
            .ok_or(IoStatus::EdlMalformedEvent)?
            .parse::<u32>()
            .map_err(|_| IoStatus::EdlMalformedEvent)?;
        if frames == 0 {
            // A dissolve of no frames is a cut, and saying it the long way
            // round is a file describing something it does not mean.
            return Err(IoStatus::EdlMalformedEvent);
        }
        return Ok(Transition::Dissolve(frames));
    }
    let pattern = code
        .strip_prefix('W')
        .ok_or(IoStatus::EdlUnknownTransition)?
        .parse::<u32>()
        .map_err(|_| IoStatus::EdlUnknownTransition)?;
    let frames = fields
        .next()
        .ok_or(IoStatus::EdlMalformedEvent)?
        .parse::<u32>()
        .map_err(|_| IoStatus::EdlMalformedEvent)?;
    if frames == 0 {
        return Err(IoStatus::EdlMalformedEvent);
    }
    Ok(Transition::Wipe { pattern, frames })
}

/// Read `NON-DROP FRAME` or `DROP FRAME`.
fn read_frame_code_mode(text: &str) -> Result<bool> {
    // Checked in this order because "DROP FRAME" is a suffix of the other one,
    // and getting the order wrong would read every non-drop file as drop.
    if equals_ignoring_case(text, "NON-DROP FRAME") || equals_ignoring_case(text, "NON DROP FRAME")
    {
        return Ok(false);
    }
    if equals_ignoring_case(text, "DROP FRAME") {
        return Ok(true);
    }
    Err(IoStatus::EdlUnknownFrameCodeMode)
}

/// Read one timecode, and check its punctuation against the declared mode.
///
/// The separator before the frames field states drop-frame on its own: a
/// semicolon means yes, a colon means no. When a file states it twice and the
/// statements disagree, this refuses. Picking one would be guessing, and the
/// two answers are more than an hour apart over a day.
fn read_timecode(text: &str, declared: Option<bool>) -> Result<Timecode> {
    let bytes = text.as_bytes();
    if bytes.len() != 11 {
        return Err(IoStatus::EdlMalformedTimecode);
    }
    let punctuated = match bytes[8] {
        b';' | b'.' => true,
        b':' => false,
        _ => return Err(IoStatus::EdlMalformedTimecode),
    };
    if bytes[2] != b':' || bytes[5] != b':' {
        return Err(IoStatus::EdlMalformedTimecode);
    }
    if let Some(declared) = declared
        && declared != punctuated
    {
        return Err(IoStatus::EdlFrameCodeModeConflict);
    }

    let field = |start: usize| -> Result<u8> {
        text.get(start..start + 2)
            .ok_or(IoStatus::EdlMalformedTimecode)?
            .parse::<u8>()
            .map_err(|_| IoStatus::EdlMalformedTimecode)
    };
    let rate = if punctuated { 30 } else { EDL_RATE };
    Timecode::new(field(0)?, field(3)?, field(6)?, field(9)?, rate, punctuated)
        .map_err(IoStatus::Time)
}

/// The rate a non-drop CMX 3600 timecode counts in.
///
/// Thirty, and this is the format's deepest limitation rather than a choice:
/// a CMX 3600 timecode has two digits for frames and no field anywhere saying
/// what rate it counts in. A 24-frame film cut and a 25-frame PAL cut are
/// written identically, and only the human handing over the file knows which
/// it is. Anything that needs the real rate must be told it; this parser will
/// not invent it.
const EDL_RATE: u32 = 30;

/// Compare without allocating, for the format's keywords.
fn equals_ignoring_case(text: &str, expected: &str) -> bool {
    text.len() == expected.len()
        && text
            .chars()
            .zip(expected.chars())
            .all(|(one, other)| one.eq_ignore_ascii_case(&other))
}

/// Strip a keyword prefix, whatever case the file wrote it in.
fn strip_prefix_ignoring_case<'a>(text: &'a str, prefix: &str) -> Option<&'a str> {
    let head = text.get(..prefix.len())?;
    if equals_ignoring_case(head, prefix) {
        text.get(prefix.len()..)
    } else {
        None
    }
}

/// Write an edit decision list.
///
/// The output is the format's fixed layout, and it is deterministic: the same
/// list writes the same bytes on every machine (R-4.1). One `FCM` line is
/// emitted before the first event and again wherever the mode changes, so a
/// reader that watches for it and one that reads only the first agree
/// wherever they can.
///
/// # Errors
///
/// [`IoStatus::OutOfMemory`] if the text cannot be held.
pub fn write(list: &EditDecisionList) -> Result<String> {
    use core::fmt::Write as _;

    let mut out = String::new();
    let _ = writeln!(out, "TITLE:   {}", list.title);

    let mut mode: Option<bool> = None;
    for event in &list.events {
        let drop_frame = event.record_in.is_drop_frame();
        if mode != Some(drop_frame) {
            let _ = writeln!(
                out,
                "FCM: {}",
                if drop_frame {
                    "DROP FRAME"
                } else {
                    "NON-DROP FRAME"
                }
            );
            mode = Some(drop_frame);
        }
        let _ = write!(
            out,
            "{:03}  {:<8} {:<5} {:<8} ",
            event.number,
            event.reel,
            channel_text(event.channel),
            transition_text(event.transition)
        );
        if let Transition::Dissolve(frames) | Transition::Wipe { frames, .. } = event.transition {
            let _ = write!(out, "{frames:03} ");
        }
        let _ = writeln!(
            out,
            "{} {} {} {}",
            stamp(event.source_in),
            stamp(event.source_out),
            stamp(event.record_in),
            stamp(event.record_out)
        );
        if let Some(name) = &event.from_clip_name {
            let _ = writeln!(out, "* FROM CLIP NAME: {name}");
        }
    }
    Ok(out)
}

/// One timecode in the format's own punctuation.
fn stamp(timecode: Timecode) -> String {
    use core::fmt::Write as _;

    let mut out = String::new();
    let separator = if timecode.is_drop_frame() { ';' } else { ':' };
    let _ = write!(
        out,
        "{:02}:{:02}:{:02}{}{:02}",
        timecode.hours(),
        timecode.minutes(),
        timecode.seconds(),
        separator,
        timecode.frames()
    );
    out
}

/// The channel field's text.
fn channel_text(channel: Channel) -> String {
    use core::fmt::Write as _;

    let mut out = String::new();
    match channel {
        Channel::Video => out.push('V'),
        Channel::VideoAndAudio => out.push('B'),
        Channel::Audio(1) => out.push('A'),
        Channel::Audio(track) => {
            let _ = write!(out, "A{track}");
        }
    }
    out
}

/// The transition field's text.
fn transition_text(transition: Transition) -> String {
    use core::fmt::Write as _;

    let mut out = String::new();
    match transition {
        Transition::Cut => out.push('C'),
        Transition::Dissolve(_) => out.push('D'),
        Transition::Wipe { pattern, .. } => {
            let _ = write!(out, "W{pattern:03}");
        }
    }
    out
}

/// Build a list from events, for writing one that was not read.
///
/// # Errors
///
/// [`IoStatus::EdlNoEvents`] for an empty list, [`IoStatus::TooMany`] past
/// [`MAX_EVENTS`], and [`IoStatus::EdlReelTooLong`] for a reel name the format
/// cannot carry.
pub fn list(title: &str, events: Vec<Event>) -> Result<EditDecisionList> {
    if events.is_empty() {
        return Err(IoStatus::EdlNoEvents);
    }
    if events.len() > MAX_EVENTS {
        return Err(IoStatus::TooMany);
    }
    for event in &events {
        if event.reel.chars().count() > REEL_CHARACTERS {
            return Err(IoStatus::EdlReelTooLong);
        }
    }
    let mut held = String::new();
    held.push_str(title);
    Ok(EditDecisionList {
        title: held,
        events,
    })
}

/// Build one event.
///
/// # Errors
///
/// [`IoStatus::EdlReelTooLong`] for a reel name past [`REEL_CHARACTERS`],
/// [`IoStatus::EdlNegativeDuration`] if either out is not after its in, and
/// [`IoStatus::Time`] for timecodes that cannot be compared.
pub fn event(
    number: u32,
    reel: &str,
    channel: Channel,
    transition: Transition,
    source: (Timecode, Timecode),
    record: (Timecode, Timecode),
) -> Result<Event> {
    if reel.chars().count() > REEL_CHARACTERS {
        return Err(IoStatus::EdlReelTooLong);
    }
    let mut held = String::new();
    held.push_str(reel);
    let event = Event {
        number,
        reel: held,
        channel,
        transition,
        source_in: source.0,
        source_out: source.1,
        record_in: record.0,
        record_out: record.1,
        from_clip_name: None,
    };
    event.source_frames()?;
    event.record_frames()?;
    Ok(event)
}

/// The same event with the comment that carries its full source name.
///
/// The reel field holds eight characters, so this is where the rest of a name
/// goes. It is a comment: a reader is entitled to ignore it, and one that does
/// still gets a correct cut.
#[must_use]
pub fn named(mut event: Event, name: &str) -> Event {
    let mut held = String::new();
    held.push_str(name);
    event.from_clip_name = Some(held);
    event
}
