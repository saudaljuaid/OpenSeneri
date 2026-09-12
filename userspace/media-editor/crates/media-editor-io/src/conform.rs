// SPDX-License-Identifier: GPL-3.0-only
//! Convert between Media Editor sequences and edit decision lists.
//!
//! [`crate::edl`] handles the text format. This module maps tracks, clips, and
//! transitions to its records. A lossless export must survive
//! export/write/parse/import with full [`Sequence`] equality.
//!
//! [`LeftBehind`] reports decorations that EDL cannot represent, such as
//! grades and automation. Structural changes, including extra picture tracks,
//! are rejected because they would describe a different edit.
//!
//! Reel names contain the first eight characters of the source digest, while
//! `FROM CLIP NAME` carries the complete digest. Import requires both values to
//! agree and resolves the digest through the project media library.
//!
//! CMX does not encode a frame rate or record origin. Callers supply both, and
//! import relabels parsed timecodes before converting them to frame numbers.

use alloc::string::String;
use alloc::vec::Vec;

use media_editor_core::{Duration, Timebase, Timecode};
use media_editor_model::media::Digest;
use media_editor_model::track::Track;
use media_editor_model::track::{Fader, Transition as Dissolve, TransitionKind};
use media_editor_model::{Clip, Edit, Item, MediaId, Project, Sequence, SequenceId, TrackKind};

use crate::edl::{self, Channel, EditDecisionList, Event, Transition};
use crate::status::{IoStatus, Result};

/// How many characters of a source digest a reel name carries.
///
/// Eight, because [`crate::edl::REEL_CHARACTERS`] is eight. It is restated
/// here as a hexadecimal character count rather than imported as a byte count,
/// because four bytes would be eight characters and three and a half would
/// not, and the arithmetic that connects them is exactly the kind that goes
/// wrong silently.
pub const REEL_DIGEST_CHARACTERS: usize = edl::REEL_CHARACTERS;

/// How many characters a full digest is written as.
const DIGEST_CHARACTERS: usize = 64;

/// What a cut carried that the format cannot.
///
/// Every field counts a thing that leaves the frames in the right order at the
/// right times and only changes how they look. Anything that would have moved
/// a frame is refused instead, so this is never a list of ways the cut is
/// wrong — it is a list of ways it is bare.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LeftBehind {
    grades: u32,
    automation: u32,
    faders: u32,
    silent_tracks: u32,
    trailing_gaps: u32,
    wipes: u32,
}

impl LeftBehind {
    /// Clips whose grade the list cannot name.
    #[must_use]
    pub const fn grades(self) -> u32 {
        self.grades
    }

    /// Tracks carrying an opacity or level curve.
    #[must_use]
    pub const fn automation(self) -> u32 {
        self.automation
    }

    /// Tracks whose fader is not at unity, muted included.
    #[must_use]
    pub const fn faders(self) -> u32 {
        self.faders
    }

    /// Tracks with no clip on them at all, which therefore write no event and
    /// cannot be said to exist.
    #[must_use]
    pub const fn silent_tracks(self) -> u32 {
        self.silent_tracks
    }

    /// Tracks ending in a gap. A list ends at its last event, so black after
    /// the last picture is not something it can state.
    #[must_use]
    pub const fn trailing_gaps(self) -> u32 {
        self.trailing_gaps
    }

    /// Wipes written as dissolves.
    ///
    /// The format has a wipe event, and it names the shape by a *pattern
    /// number* — which is a convention of whichever machine wrote the list
    /// rather than anything the format defines. Inventing one would produce a
    /// file that says a shape this application cannot read back, so a wipe is
    /// written as the dissolve it is timed like: every frame lands where it
    /// belongs and the edge is gone.
    #[must_use]
    pub const fn wipes(self) -> u32 {
        self.wipes
    }

    /// Whether the list carries the whole sequence.
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.grades == 0
            && self.automation == 0
            && self.faders == 0
            && self.silent_tracks == 0
            && self.trailing_gaps == 0
            && self.wipes == 0
    }
}

/// A list, and what writing it cost.
///
/// The two are one value because they are one answer. A caller that took the
/// list and dropped the accounting would be claiming the cut is complete
/// without having looked.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Conformed {
    list: EditDecisionList,
    left_behind: LeftBehind,
}

impl Conformed {
    /// The list.
    #[must_use]
    pub const fn list(&self) -> &EditDecisionList {
        &self.list
    }

    /// What the sequence carried that the list does not.
    #[must_use]
    pub const fn left_behind(&self) -> LeftBehind {
        self.left_behind
    }
}

/// Turn a sequence into an edit decision list.
///
/// `record_start` is where the programme sits on the record, which the format
/// has no way to imply — see the module note.
///
/// # Errors
///
/// [`IoStatus::ConformManyVideoTracks`] or [`IoStatus::ConformTracksNotOrdered`]
/// for a track layout one list cannot describe,
/// [`IoStatus::ConformReelCollision`] for two sources whose digests agree in
/// the eight characters a reel name holds, [`IoStatus::Model`] for a sequence
/// or media identifier the project does not hold, [`IoStatus::Time`] for a
/// position outside the day's timecode or a rate with no label form, and
/// [`IoStatus::EdlNoEvents`] for a sequence with no clip anywhere on it.
pub fn export(
    project: &Project,
    sequence: SequenceId,
    record_start: Timecode,
) -> Result<Conformed> {
    let held = project.sequence(sequence).map_err(IoStatus::Model)?;
    let timebase = held.timebase();
    // Refuses a record start counting at a rate the sequence does not, which
    // is the one way the two arguments can contradict each other.
    let origin = record_start
        .to_instant(timebase)
        .map_err(IoStatus::Time)?
        .ticks();
    let labelling = Labelling {
        nominal: timebase.nominal_rate().map_err(IoStatus::Time)?,
        drop_frame: record_start.is_drop_frame(),
        origin,
    };

    let mut left_behind = LeftBehind::default();
    let mut events: Vec<Event> = Vec::new();
    let mut reels: Vec<(String, Digest)> = Vec::new();

    for (track, channel) in held.tracks().iter().zip(channels_of(held)?) {
        if track.fader() != Fader::UNITY {
            left_behind.faders += 1;
        }
        if track.opacity().is_some() || track.level().is_some() {
            left_behind.automation += 1;
        }
        if !track
            .items()
            .iter()
            .any(|item| matches!(item, Item::Clip(_)))
        {
            // Nothing to write an event about, so the channel does not appear
            // and the track's existence is not something the list can state.
            left_behind.silent_tracks += 1;
            continue;
        }
        if matches!(track.items().last(), Some(Item::Gap(_))) {
            left_behind.trailing_gaps += 1;
        }

        for (index, item) in track.items().iter().enumerate() {
            let Item::Clip(clip) = item else {
                // A gap is a hole in the record timecode. It is written by not
                // being written.
                continue;
            };
            if clip.grade().is_some() {
                left_behind.grades += 1;
            }
            if matches!(
                track.transition_at(index).map(|held| held.kind()),
                Some(TransitionKind::Wipe(_))
            ) {
                left_behind.wipes += 1;
            }
            let digest = project
                .media()
                .get(clip.media())
                .map_err(IoStatus::Model)?
                .digest();
            remember(&mut reels, &reel_of(digest), digest)?;
            let number = u32::try_from(events.len() + 1).map_err(|_| IoStatus::TooMany)?;
            events.push(one_event(
                track, index, clip, digest, channel, number, labelling,
            )?);
        }
    }

    Ok(Conformed {
        list: edl::list("MEDIAEDTO", events)?,
        left_behind,
    })
}

/// The three facts every timecode this export writes is made from.
#[derive(Clone, Copy)]
struct Labelling {
    nominal: u32,
    drop_frame: bool,
    origin: i64,
}

/// Which channel each of a sequence's tracks writes to.
///
/// A list names channels and not an order, so an import always rebuilds
/// picture first and then sound by number. A sequence not already in that
/// order does not come back, and a sequence with two picture tracks does not
/// go out — one video channel cannot say which of two pictures is on top.
fn channels_of(sequence: &Sequence) -> Result<Vec<Channel>> {
    let mut out = Vec::new();
    let mut audio = 0_u8;
    let mut seen_audio = false;
    let mut seen_video = false;
    for track in sequence.tracks() {
        out.push(match track.kind() {
            TrackKind::Video => {
                if seen_video {
                    return Err(IoStatus::ConformManyVideoTracks);
                }
                if seen_audio {
                    return Err(IoStatus::ConformTracksNotOrdered);
                }
                seen_video = true;
                Channel::Video
            }
            TrackKind::Audio => {
                seen_audio = true;
                audio = audio
                    .checked_add(1)
                    .ok_or(IoStatus::ConformTracksNotOrdered)?;
                Channel::Audio(audio)
            }
        });
    }
    Ok(out)
}

/// One clip's event.
///
/// A dissolve is centred on its cut and opens half its length — rounded down —
/// before it, which is the model's own rule. The one at this clip's head pulls
/// its record in earlier and its source in back into handles; the one at its
/// tail pulls its record out earlier by the same arithmetic on the next cut,
/// and the frames between are stated by the dissolve rather than by either
/// event.
fn one_event(
    track: &Track,
    index: usize,
    clip: &Clip,
    digest: Digest,
    channel: Channel,
    number: u32,
    labelling: Labelling,
) -> Result<Event> {
    let opening_in = opening(track.transition_at(index));
    let opening_out = opening(track.transition_at(index + 1));
    let start = track.item_start(index).map_err(IoStatus::Model)?.ticks();
    let length = clip.duration().ticks();

    let transition = match track.transition_at(index) {
        None => Transition::Cut,
        Some(dissolve) => Transition::Dissolve(
            u32::try_from(dissolve.duration().ticks()).map_err(|_| IoStatus::TooMany)?,
        ),
    };
    let stamp = |frame: i64| {
        Timecode::from_frame_number(frame, labelling.nominal, labelling.drop_frame)
            .map_err(IoStatus::Time)
    };
    let event = edl::event(
        number,
        &reel_of(digest),
        channel,
        transition,
        (
            stamp(clip.source_start() - opening_in)?,
            stamp(clip.source_start() + length - opening_out)?,
        ),
        (
            stamp(labelling.origin + start - opening_in)?,
            stamp(labelling.origin + start + length - opening_out)?,
        ),
    )?;
    Ok(edl::named(event, &hex(digest)))
}

/// Build a sequence in a project from an edit decision list.
///
/// `timebase` and `record_start` are the two facts the file does not carry.
/// Every source the list names must already be in the project's media library:
/// conforming is not importing media, and a sequence that pointed at an asset
/// nobody had opened would be a project referring to something that is not
/// there.
///
/// # Errors
///
/// [`IoStatus::ConformNoSourceDigest`], [`IoStatus::ConformUnknownSource`] or
/// [`IoStatus::ConformReelDisagreesWithSource`] for an event whose source
/// cannot be established, [`IoStatus::ConformChannelNotSeparate`] or
/// [`IoStatus::ConformWipeUnsupported`] for an event this model has no shape
/// for, [`IoStatus::ConformEventsOverlap`],
/// [`IoStatus::ConformDissolveFromNothing`] or
/// [`IoStatus::ConformDissolveTooLong`] for events that do not lay out on a
/// track, [`IoStatus::Time`] for a timecode counting at another rate, and
/// [`IoStatus::Model`] for anything the model itself refuses to hold.
pub fn import(
    project: &mut Project,
    list: &EditDecisionList,
    timebase: Timebase,
    record_start: Timecode,
) -> Result<SequenceId> {
    let origin = record_start
        .to_instant(timebase)
        .map_err(IoStatus::Time)?
        .ticks();
    let nominal = timebase.nominal_rate().map_err(IoStatus::Time)?;
    let sources = resolve(project, list)?;

    // The channels present, in the order a sequence lays them out: picture
    // first, then sound by its number. An `A3` with no `A2` still makes three
    // sound tracks, because the number *is* the channel's meaning and closing
    // the hole would silently move a track.
    let mut has_video = false;
    let mut audio_tracks = 0_u8;
    for event in list.events() {
        match event.channel() {
            Channel::Video => has_video = true,
            Channel::Audio(number) => audio_tracks = audio_tracks.max(number),
            Channel::VideoAndAudio => return Err(IoStatus::ConformChannelNotSeparate),
        }
    }

    let sequence = project.add_sequence(timebase).map_err(IoStatus::Model)?;
    let mut channels: Vec<Channel> = Vec::new();
    if has_video {
        channels.push(Channel::Video);
    }
    for number in 1..=audio_tracks {
        channels.push(Channel::Audio(number));
    }
    for (index, channel) in channels.iter().enumerate() {
        let kind = match channel {
            Channel::Video => TrackKind::Video,
            _ => TrackKind::Audio,
        };
        project
            .apply(sequence, Edit::AddTrack { index, kind })
            .map_err(IoStatus::Model)?;
        lay_out(
            project, sequence, index, list, *channel, &sources, timebase, origin, nominal,
        )?;
    }

    // Conforming is not editing. The edits above are how the cut is built, and
    // offering to undo them would offer to undo the file.
    project.forget_history();
    Ok(sequence)
}

/// Place one channel's events on one track.
#[expect(
    clippy::too_many_arguments,
    reason = "every argument is a distinct fact this needs and none has a \
              default; bundling them into a struct would name the bundle \
              after this function and explain nothing"
)]
fn lay_out(
    project: &mut Project,
    sequence: SequenceId,
    track: usize,
    list: &EditDecisionList,
    channel: Channel,
    sources: &[MediaId],
    timebase: Timebase,
    origin: i64,
    nominal: u32,
) -> Result<()> {
    // Sorted by where they land, not by their numbers. An event number is a
    // label a machine operator typed; the record timecode is the statement
    // about when the frame is on screen, and only one of those two can be
    // wrong without the file being wrong.
    //
    // By the frame it counts rather than by the label, because a list may
    // change counting style halfway through and `01:00:00;02` is not after
    // `01:00:00:02` — they are two ways of writing positions two frames apart
    // in a count that skips labels.
    let mut ordered: Vec<(i64, usize, &Event)> = Vec::new();
    for (source, event) in list.events().iter().enumerate() {
        if event.channel() != channel {
            continue;
        }
        let at = recount(event.record_in(), nominal)?;
        ordered.push((at, source, event));
    }
    ordered.sort_by_key(|(at, _, _)| *at);

    let mut position = origin;
    let mut index = 0_usize;
    for (record_in, source, event) in ordered {
        let opening = match event.transition() {
            Transition::Cut => 0,
            Transition::Dissolve(frames) => i64::from(frames) / 2,
            Transition::Wipe { .. } => return Err(IoStatus::ConformWipeUnsupported),
        };
        let record_out = recount(event.record_out(), nominal)?;
        let source_in = recount(event.source_in(), nominal)?;
        let cut = record_in + opening;

        if opening > 0 {
            // The outgoing clip gave up half the dissolve from its tail when
            // this was written. Give it back before the incoming one lands, so
            // the two meet at the cut the dissolve is centred on.
            if index == 0 || position != record_in {
                return Err(IoStatus::ConformDissolveFromNothing);
            }
            if record_out <= cut {
                return Err(IoStatus::ConformDissolveTooLong);
            }
            let previous = project
                .sequence(sequence)
                .map_err(IoStatus::Model)?
                .track(track)
                .map_err(IoStatus::Model)?
                .item(index - 1)
                .map_err(IoStatus::Model)?
                .duration()
                .ticks();
            let duration = Duration::new(previous + opening, timebase).map_err(IoStatus::Time)?;
            project
                .apply(
                    sequence,
                    Edit::SetItemDuration {
                        track,
                        index: index - 1,
                        duration,
                    },
                )
                .map_err(IoStatus::Model)?;
        } else if cut < position {
            return Err(IoStatus::ConformEventsOverlap);
        } else if cut > position {
            let gap = Item::gap(Duration::new(cut - position, timebase).map_err(IoStatus::Time)?)
                .map_err(IoStatus::Model)?;
            project
                .apply(
                    sequence,
                    Edit::InsertItem {
                        track,
                        index,
                        item: gap,
                    },
                )
                .map_err(IoStatus::Model)?;
            index += 1;
        }

        let length = Duration::new(record_out - cut, timebase).map_err(IoStatus::Time)?;
        let clip =
            Clip::new(sources[source], source_in + opening, length).map_err(IoStatus::Model)?;
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track,
                    index,
                    item: Item::Clip(clip),
                },
            )
            .map_err(IoStatus::Model)?;
        if let Transition::Dissolve(frames) = event.transition() {
            let duration = Duration::new(i64::from(frames), timebase).map_err(IoStatus::Time)?;
            // Through the edit, like everything else, so a file cannot set a
            // dissolve the model itself would have refused.
            let transition = Dissolve::new(index, duration).map_err(IoStatus::Model)?;
            project
                .apply(sequence, Edit::AddTransition { track, transition })
                .map_err(IoStatus::Model)?;
        }
        index += 1;
        position = record_out;
    }
    Ok(())
}

/// Which media each event names, by position in the list.
fn resolve(project: &Project, list: &EditDecisionList) -> Result<Vec<MediaId>> {
    let mut out: Vec<MediaId> = Vec::new();
    for event in list.events() {
        let name = event
            .from_clip_name()
            .ok_or(IoStatus::ConformNoSourceDigest)?;
        let digest = unhex(name).ok_or(IoStatus::ConformNoSourceDigest)?;
        if event.reel() != reel_of(digest) {
            return Err(IoStatus::ConformReelDisagreesWithSource);
        }
        let found = project
            .media()
            .iter()
            .find(|(_, asset)| asset.digest() == digest)
            .map(|(id, _)| id)
            .ok_or(IoStatus::ConformUnknownSource)?;
        out.push(found);
    }
    Ok(out)
}

/// Convert a parsed label using the frame rate supplied for this import.
///
/// Relabeling also validates that the frame field fits the supplied rate.
fn recount(label: Timecode, nominal: u32) -> Result<i64> {
    Timecode::new(
        label.hours(),
        label.minutes(),
        label.seconds(),
        label.frames(),
        nominal,
        // Drop-frame mode is encoded by the file and remains unchanged.
        label.is_drop_frame(),
    )
    .map_err(IoStatus::Time)?
    .to_frame_number()
    .map_err(IoStatus::Time)
}

/// How far before its cut a dissolve opens: half its length, rounded down.
fn opening(dissolve: Option<Dissolve>) -> i64 {
    dissolve.map_or(0, |held| held.duration().ticks() / 2)
}

/// A digest written out in full.
fn hex(digest: Digest) -> String {
    use core::fmt::Write as _;

    let mut out = String::new();
    for byte in digest.bytes() {
        let _ = write!(out, "{byte:02X}");
    }
    out
}

/// The reel name for a source: the first eight characters of its digest.
fn reel_of(digest: Digest) -> String {
    let mut out = hex(digest);
    out.truncate(REEL_DIGEST_CHARACTERS);
    out
}

/// Read a digest back out of a comment, or nothing if it is not one.
fn unhex(text: &str) -> Option<Digest> {
    if text.len() != DIGEST_CHARACTERS {
        return None;
    }
    let mut bytes = [0_u8; 32];
    let raw = text.as_bytes();
    for (index, byte) in bytes.iter_mut().enumerate() {
        let high = nibble(raw[index * 2])?;
        let low = nibble(raw[index * 2 + 1])?;
        *byte = (high << 4) | low;
    }
    Some(Digest::new(bytes))
}

/// One hexadecimal character, upper case only.
///
/// Lower case is not accepted, because a digest this application wrote is
/// upper case and a comment in another case was written by something else —
/// which is the case the reel check exists to catch.
fn nibble(character: u8) -> Option<u8> {
    match character {
        b'0'..=b'9' => Some(character - b'0'),
        b'A'..=b'F' => Some(character - b'A' + 10),
        _ => None,
    }
}

/// Record a reel name, refusing a second source that would answer to it.
fn remember(reels: &mut Vec<(String, Digest)>, reel: &str, digest: Digest) -> Result<()> {
    for (held, known) in reels.iter() {
        if held == reel {
            return if *known == digest {
                Ok(())
            } else {
                Err(IoStatus::ConformReelCollision)
            };
        }
    }
    let mut held = String::new();
    held.push_str(reel);
    reels.push((held, digest));
    Ok(())
}
