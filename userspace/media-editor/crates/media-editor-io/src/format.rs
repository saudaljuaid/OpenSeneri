// SPDX-License-Identifier: GPL-3.0-only
//! The Media Editor project file.
//!
//! ```text
//! offset  size  field
//! 0       4     magic, "SPRJ"
//! 4       2     format version, little-endian
//! 6       2     reserved, must be zero
//! 8       8     payload length in bytes, little-endian
//! 16      32    SHA-256 of the payload
//! 48      N     payload
//! ```
//!
//! Versioned from its first byte, length-prefixed, and digested (R-9.3). The
//! digest is what makes a half-written file detectable rather than loadable:
//! any change to any payload byte, and any change to the length, is refused by
//! name before a single field reaches the model.
//!
//! The payload is written by hand rather than derived. That is deliberate: the
//! format is the long-term custody of the user's work, so every field in it is
//! a decision someone made and can read here, and the decoder's bounds are the
//! model's own capacities rather than whatever a derive happened to allow.
//!
//! History is not saved. Undo is a property of a session, not of a project; a
//! file that carried its own history would make "open the file" and "open the
//! file and undo twice" two different projects with one name.

use alloc::vec::Vec;

use media_editor_core::{Digest, Duration, Instant, Rational, Timebase};
use media_editor_model::curve::MAX_KEYFRAMES;
use media_editor_model::mask::{MAX_CORNERS, Mask};
use media_editor_model::media::{Location, MAX_LOCATION_BYTES};
use media_editor_model::project::{MAX_MEDIA, MAX_SEQUENCES};
use media_editor_model::sequence::MAX_TRACKS_PER_SEQUENCE;
use media_editor_model::title::{Alignment, Ink, MAX_TITLE_LINES, MAX_TITLE_TEXT, Title};
use media_editor_model::track::MAX_ITEMS_PER_TRACK;
use media_editor_model::transform::{Motion, Resampling, Transform};
use media_editor_model::{
    Clip, Curve, Edit, Fader, Interpolation, Item, Keyframe, MAX_MARKER_TEXT, MAX_MARKERS_PER_CLIP,
    MAX_MARKERS_PER_SEQUENCE, MediaAsset, MediaId, Playback, Project, TrackKind, Transition,
    TransitionKind, Wipe,
};

use crate::bytes::{Reader, Writer};
use crate::status::{IoStatus, Result};

/// The four bytes every project file begins with.
pub const MAGIC: [u8; 4] = *b"SPRJ";

/// The format this build writes.
/// Six. Version two added the fader, three the dissolves, four a picture
/// track's opacity over time, five a sound track's fader over time, and six a
/// clip's grade.
///
/// Version one had no fader. A project written by version one and read as
/// version two would have its faders read out of whatever followed them, which
/// is why the number moves for every change to what the bytes mean and not
/// only for changes that break (R-1.2). A clip's grade is written after its
/// length, so a version-five file read as version six would find a flag byte
/// in the next item's tag.
pub const FORMAT_VERSION: u16 = 28;

/// How long the fixed header is.
pub const HEADER_BYTES: usize = 48;

/// The largest payload this format accepts.
///
/// Sixteen mebibytes: a project is structure, not media, and a structural
/// description larger than this is a generated one rather than an edited one.
pub const MAX_PAYLOAD_BYTES: usize = 16 * 1024 * 1024;

/// How an item is tagged in the payload.
const TAG_CLIP: u8 = 0;
const TAG_GAP: u8 = 1;

/// How a track's kind is tagged in the payload.
const KIND_VIDEO: u8 = 0;
const KIND_AUDIO: u8 = 1;

/// A transition that cross-fades the whole frame.
const KIND_DISSOLVE: u8 = 0;

/// A transition that sweeps a straight edge across it.
const KIND_WIPE: u8 = 1;

/// Encode a project as a complete file.
///
/// # Errors
///
/// [`IoStatus::PayloadTooLarge`], [`IoStatus::OutOfMemory`], or a model
/// refusal if the project holds something the format cannot describe.
pub fn encode(project: &Project) -> Result<Vec<u8>> {
    let payload = encode_payload(project)?;
    let digest = Digest::of(&payload);

    let mut file = Writer::new(HEADER_BYTES + MAX_PAYLOAD_BYTES);
    file.bytes(&MAGIC)?;
    file.u16(FORMAT_VERSION)?;
    file.u16(0)?;
    file.u64(payload.len() as u64)?;
    file.bytes(digest.bytes())?;
    file.bytes(&payload)?;
    Ok(file.finish())
}

/// Write one item.
///
/// The decoder has had a `read_item` since the format's first version; this is
/// its other half, and having them face each other is what makes it possible
/// to read the two side by side and see that every field written is a field
/// read.
fn write_item(writer: &mut Writer, item: &Item, media: &[(MediaId, &MediaAsset)]) -> Result<()> {
    match item {
        Item::Clip(clip) => {
            writer.u8(TAG_CLIP)?;
            let index = media
                .iter()
                .position(|(id, _)| *id == clip.media())
                .ok_or(IoStatus::MediaIndexOutOfRange)?;
            writer.u32(u32::try_from(index).map_err(|_| IoStatus::TooMany)?)?;
            writer.i64(clip.source_start())?;
            writer.i64(clip.duration().ticks())?;
            // A flag rather than a fixed thirty-two bytes, because
            // most clips have no grade and an all-zero digest is a
            // real digest of something rather than a spare value
            // to spend on "none".
            match clip.transform() {
                None => writer.u8(0)?,
                Some(transform) => {
                    writer.u8(1)?;
                    writer.u8(match transform.resampling() {
                        Resampling::Area => 0,
                        Resampling::Bilinear => 1,
                    })?;
                    for held in transform.linear() {
                        write_rational(writer, held)?;
                    }
                    write_rational(writer, transform.offset().0)?;
                    write_rational(writer, transform.offset().1)?;
                    // The pivot, after the move it is not interchangeable
                    // with. Thirty-two bytes a transform rather than a flag
                    // and a default, because a transform is already the most
                    // expensive thing a clip carries and a fifth tag to
                    // reserve would buy less than it costs to explain.
                    write_rational(writer, transform.anchor().0)?;
                    write_rational(writer, transform.anchor().1)?;
                }
            }
            // After the transform it changes, and before the mask, because a
            // motion is a modification of the framing rather than a fourth
            // independent property of the clip -- and a reader that has just
            // built the transform is the reader in the best position to refuse
            // an animation with nothing to animate.
            match clip.motion() {
                None => writer.u8(MOTION_NONE)?,
                Some(motion) => {
                    writer.u8(MOTION_PRESENT)?;
                    for lane in media_editor_model::transform::lanes(motion) {
                        write_curve(writer, lane)?;
                    }
                }
            }
            // A count of nought when there is none, which is what
            // `write_curve` already writes for an absent lane -- four bytes,
            // the same as the motion's lanes cost, and no new tag to reserve.
            write_curve(writer, clip.opacity())?;
            // The mask's animation, written exactly as the framing's is and
            // read back the same way. Its tag comes *before* the mask itself
            // reads, which is deliberate: the decoder can then refuse an
            // animation with no shape at the moment it has both in hand.
            match clip.mask_motion() {
                None => writer.u8(MOTION_NONE)?,
                Some(motion) => {
                    writer.u8(MOTION_PRESENT)?;
                    for lane in media_editor_model::transform::lanes(motion) {
                        write_curve(writer, lane)?;
                    }
                }
            }
            match clip.mask() {
                None => writer.u8(MASK_NONE)?,
                Some(mask) => {
                    writer.u8(MASK_PRESENT)?;
                    writer.u8(u8::from(mask.is_inverted()))?;
                    writer
                        .u32(u32::try_from(mask.corners().len()).map_err(|_| IoStatus::TooMany)?)?;
                    for corner in mask.corners() {
                        write_rational(writer, corner.0)?;
                        write_rational(writer, corner.1)?;
                    }
                }
            }
            // A flag again, and for the same reason: real time is the common
            // case and a fraction of one over one is sixteen bytes of saying
            // so.
            match clip.playback() {
                Playback::At(speed) if *speed == media_editor_core::Rational::ONE => {
                    writer.u8(SPEED_REAL_TIME)?;
                }
                Playback::At(speed) => {
                    writer.u8(SPEED_PRESENT)?;
                    write_rational(writer, *speed)?;
                }
                // A tag and nothing after it. A freeze has no number: the
                // frame it holds is the clip's in point, which is already in
                // the file a few bytes above this.
                Playback::Frozen => writer.u8(SPEED_FROZEN)?,
                // And a ramp is a curve, written by the same helper every
                // other curve in this format goes through. Its count is never
                // nought -- a curve holds at least one keyframe -- so the tag
                // above it is what says whether there is one at all, and the
                // count is never asked to mean two things.
                Playback::Ramp(curve) => {
                    writer.u8(SPEED_RAMP)?;
                    write_curve(writer, Some(curve))?;
                }
            }
            // A flag, because a clip nobody has faded is the common one and
            // two lengths of nought are sixteen bytes of saying so.
            if clip.fade_in().is_zero() && clip.fade_out().is_zero() {
                writer.u8(FADES_NONE)?;
            } else {
                writer.u8(FADES_PRESENT)?;
                writer.i64(clip.fade_in().ticks())?;
                writer.i64(clip.fade_out().ticks())?;
            }
            match clip.grade() {
                None => writer.u8(GRADE_NONE)?,
                Some(grade) => {
                    writer.u8(GRADE_PRESENT)?;
                    writer.bytes(grade.bytes())?;
                }
            }
            // After the grade, matching the order the motion takes after the
            // transform. The first version of this comment went further and
            // said the reader is then "holding both when it has to refuse a
            // strength with no look" -- and the control for that swapped both
            // the write and the read and broke nothing, because the refusal
            // lives in `Clip::with_grade_strength` at the end of the builder
            // chain and cannot see what order the bytes arrived in. The order
            // is a convention, and saying so is better than leaving a reason
            // nothing checks.
            //
            // A count of nought when there is none, which is what `write_curve`
            // already writes for an absent lane -- four bytes, and no new tag
            // to reserve.
            write_curve(writer, clip.grade_strength())?;
            // And the notes on the shot, last, in the same shape a sequence's
            // markers take a few hundred lines below: a count, then an instant
            // and a length-prefixed run of bytes for each. The same shape
            // because they are the same type -- what differs is what the
            // instant is measured from, and the file does not have to know
            // that any more than a trim does.
            write_markers(writer, clip.markers())?;
        }
        Item::Gap(duration) => {
            writer.u8(TAG_GAP)?;
            writer.i64(duration.ticks())?;
        }
    }
    Ok(())
}

/// Decode a complete file into a project.
///
/// Nothing is published until every byte has been accounted for: the header,
/// the digest, the whole structure, and the absence of anything after it.
///
/// # Errors
///
/// Any [`IoStatus`]. On a refusal nothing is returned, so a caller cannot act
/// on a partly decoded project (R-1.4).
pub fn decode(file: &[u8]) -> Result<Project> {
    if file.len() < HEADER_BYTES {
        return Err(IoStatus::TruncatedHeader);
    }
    let mut header = Reader::new(&file[..HEADER_BYTES]);
    if header.take(4)? != MAGIC {
        return Err(IoStatus::NotAProjectFile);
    }
    let version = header.u16()?;
    if version != FORMAT_VERSION {
        return Err(IoStatus::UnsupportedVersion(version));
    }
    if header.u16()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let declared = header.u64()?;
    let expected = Digest::new(header.digest_bytes()?);

    let declared = usize::try_from(declared).map_err(|_| IoStatus::PayloadTooLarge)?;
    if declared > MAX_PAYLOAD_BYTES {
        return Err(IoStatus::PayloadTooLarge);
    }
    let available = file.len() - HEADER_BYTES;
    if declared > available {
        return Err(IoStatus::TruncatedPayload);
    }
    if declared < available {
        return Err(IoStatus::TrailingBytes);
    }

    let payload = &file[HEADER_BYTES..];
    if Digest::of(payload) != expected {
        return Err(IoStatus::DigestMismatch);
    }
    decode_payload(payload)
}

/// Encode just the payload.
fn encode_payload(project: &Project) -> Result<Vec<u8>> {
    let mut writer = Writer::new(MAX_PAYLOAD_BYTES);

    // Media are written in slot order and referred to afterwards by their
    // position in this list. Generational identifiers are a runtime idea: they
    // describe which occupancy of a slot a reference meant, and a file has no
    // slots and no occupancies.
    // The sequences' *headers* come first, before the media, and that is the
    // ordering nested sequences forced.
    //
    // A clip names its media by position in the media list, and a nested
    // asset names its sequence by position in the sequence list, so each table
    // refers to the other and neither can simply come first. Reading media and
    // then sequences leaves a nest with no sequence to point at; reading
    // sequences and then media leaves a clip with no asset.
    //
    // What breaks the knot is that only a sequence's *timebase* is needed to
    // create it. So the file states how many sequences there are and what each
    // is counted in; the media table can then name any of them; and the
    // sequence bodies -- tracks, items, transitions, markers -- come last,
    // matched to the headers by position, with every media identifier already
    // in hand.
    //
    // Three phases rather than two, for one field. The alternative was a
    // reader that builds half an asset and patches it later, and a
    // half-built value is a value some other code path can find.
    let sequences: Vec<_> = project.sequences().iter().collect();
    writer.u32(u32::try_from(sequences.len()).map_err(|_| IoStatus::TooMany)?)?;
    for (_, sequence) in &sequences {
        write_timebase(&mut writer, sequence.timebase())?;
    }

    let media = write_media(&mut writer, project, &sequences)?;

    // And now the bodies — but **not** in the order their headers were
    // written, and each one saying which header it belongs to.
    //
    // A reader builds a project by applying edits, and the model checks after
    // every one that no clip reads past the end of its media. A nest's length
    // is its sequence's length, so a body that places a nested clip while the
    // nested sequence is still empty places a clip reading past the end of
    // nothing — and is refused, correctly, halfway through loading a file that
    // is perfectly valid.
    //
    // So the bodies are written innermost first. A sequence's body comes after
    // the bodies of every sequence it nests, which is a topological order and
    // exists because the model refuses a cycle. The index before each body is
    // what lets the reader follow an order it could not have worked out for
    // itself: which sequences nest which is a fact about the *bodies*, and the
    // reader has not read them yet.
    for place in body_order(project, &sequences)? {
        let (_, sequence) = sequences
            .get(place)
            .ok_or(IoStatus::SequenceIndexOutOfRange)?;
        writer.u32(u32::try_from(place).map_err(|_| IoStatus::TooMany)?)?;
        writer.u32(u32::try_from(sequence.track_count()).map_err(|_| IoStatus::TooMany)?)?;
        for track in sequence.tracks() {
            writer.u8(match track.kind() {
                TrackKind::Video => KIND_VIDEO,
                TrackKind::Audio => KIND_AUDIO,
            })?;
            write_fader(&mut writer, track.fader())?;
            writer.u32(u32::try_from(track.len()).map_err(|_| IoStatus::TooMany)?)?;
            for item in track.items() {
                write_item(&mut writer, item, &media)?;
            }
            // Transitions come after the items rather than before, because
            // they name item indices and a reader cannot check one against a
            // track it has not read yet.
            let held = track.transitions();
            writer.u32(u32::try_from(held.len()).map_err(|_| IoStatus::TooMany)?)?;
            for transition in held {
                writer.u32(u32::try_from(transition.boundary()).map_err(|_| IoStatus::TooMany)?)?;
                writer.i64(transition.duration().ticks())?;
                // A tag before the parameters, so that a later kind adds a
                // tag rather than changing what the bytes after the duration
                // mean. Version six wrote no tag at all because there was one
                // kind, which is exactly the shape that makes adding a second
                // a format break rather than an extension.
                match transition.kind() {
                    TransitionKind::Dissolve => writer.u8(KIND_DISSOLVE)?,
                    TransitionKind::Wipe(wipe) => {
                        writer.u8(KIND_WIPE)?;
                        write_rational(&mut writer, wipe.across())?;
                        write_rational(&mut writer, wipe.down())?;
                        write_rational(&mut writer, wipe.softness())?;
                    }
                }
            }
            write_curve(&mut writer, track.opacity())?;
            write_curve(&mut writer, track.level())?;
        }
        // The markers come after every track, because a marker names an
        // instant in the *programme* rather than an index into anything -- so
        // unlike a transition, a reader needs nothing read first to check one.
        // Last rather than first only so that the file's order matches the
        // order somebody reading the two halves of this module would expect.
        write_markers(&mut writer, sequence.markers())?;
    }
    Ok(writer.finish())
}

/// Write a list of notes: a count, then an instant and its text for each.
///
/// One helper for both lists, because they are the same shape and the same
/// type -- a sequence's markers and a clip's differ in what their instants are
/// measured from, which is a fact about the model rather than about the bytes.
fn write_markers(writer: &mut Writer, notes: &[media_editor_model::Marker]) -> Result<()> {
    writer.u32(u32::try_from(notes.len()).map_err(|_| IoStatus::TooMany)?)?;
    for marker in notes {
        writer.i64(marker.at().ticks())?;
        let text = marker.text().as_bytes();
        writer.u32(u32::try_from(text.len()).map_err(|_| IoStatus::TooMany)?)?;
        writer.bytes(text)?;
    }
    Ok(())
}

/// Read a list of notes, bounded by whatever holds them.
///
/// The bound is passed rather than assumed: a sequence may hold thousands and
/// a clip may hold eight, and reading a clip's list against the sequence's
/// bound would let a file talk its way past the smaller one (R-11.2).
fn read_markers(
    reader: &mut Reader<'_>,
    timebase: Timebase,
    bound: usize,
) -> Result<Vec<media_editor_model::Marker>> {
    let count = bounded(reader.u32()?, bound)?;
    let mut notes = Vec::new();
    notes
        .try_reserve(count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for _ in 0..count {
        let at = Instant::new(reader.i64()?, timebase);
        let words = bounded(reader.u32()?, MAX_MARKER_TEXT)?;
        let text = alloc::string::String::from_utf8(reader.take(words)?.to_vec())
            .map_err(|_| IoStatus::MarkerNotText)?;
        notes.push(media_editor_model::Marker::new(at, text).map_err(IoStatus::Model)?);
    }
    Ok(notes)
}

/// Write the media table.
///
/// Out of [`encode_payload`] because that function outgrew the length lint the
/// moment the sequence headers moved above it, and because a table is a thing
/// with a name.
fn write_media<'a>(
    writer: &mut Writer,
    project: &'a Project,
    sequences: &[(
        media_editor_model::SequenceId,
        &media_editor_model::Sequence,
    )],
) -> Result<Vec<(MediaId, &'a MediaAsset)>> {
    let media: Vec<(MediaId, &MediaAsset)> = project.media().iter().collect();
    writer.u32(u32::try_from(media.len()).map_err(|_| IoStatus::TooMany)?)?;
    for (_, asset) in &media {
        writer.bytes(asset.digest().bytes())?;
        // The transcript, before the source tag. A count of nought is four
        // bytes on every asset that has none, which is what every other
        // optional lane in this format costs and for the same reason: a field
        // that is sometimes absent is a field a reader has to guess at.
        writer.u32(u32::try_from(asset.captions().len()).map_err(|_| IoStatus::TooMany)?)?;
        for caption in asset.captions() {
            writer.i64(caption.from())?;
            writer.i64(caption.to())?;
            writer.u8(caption.voice())?;
            let words = caption.text().as_bytes();
            writer.u32(u32::try_from(words.len()).map_err(|_| IoStatus::TooMany)?)?;
            writer.bytes(words)?;
        }
        write_timebase(writer, asset.timebase())?;
        writer.i64(asset.duration().ticks())?;
        // A length and then the bytes, uninterpreted. A path is whatever the
        // platform says it is, and this format does not know which platform
        // wrote it -- so it carries the hint and declines to read it.
        match asset.location() {
            None => writer.u32(0)?,
            Some(location) => {
                writer
                    .u32(u32::try_from(location.bytes().len()).map_err(|_| IoStatus::TooMany)?)?;
                writer.bytes(location.bytes())?;
            }
        }
        // Where its frames come from. A recording is a tag and nothing more --
        // the digest above already says what it is -- and a title is the
        // description the program draws.
        if let Some(nested) = asset.nested() {
            writer.u8(SOURCE_NESTED)?;
            let place = sequences
                .iter()
                .position(|(id, _)| *id == nested)
                .ok_or(IoStatus::SequenceIndexOutOfRange)?;
            writer.u32(u32::try_from(place).map_err(|_| IoStatus::TooMany)?)?;
        } else {
            match asset.title() {
                None => writer.u8(SOURCE_RECORDED)?,
                Some(title) => {
                    writer.u8(SOURCE_TITLE)?;
                    writer
                        .u32(u32::try_from(title.lines().len()).map_err(|_| IoStatus::TooMany)?)?;
                    for line in title.lines() {
                        let words = line.as_bytes();
                        writer.u32(u32::try_from(words.len()).map_err(|_| IoStatus::TooMany)?)?;
                        writer.bytes(words)?;
                    }
                    writer.u8(match title.alignment() {
                        Alignment::Left => ALIGN_LEFT,
                        Alignment::Centre => ALIGN_CENTRE,
                        Alignment::Right => ALIGN_RIGHT,
                    })?;
                    write_rational(writer, title.size())?;
                    write_rational(writer, title.across())?;
                    write_rational(writer, title.down())?;
                    // A tag before the three channels, so a card nobody coloured
                    // costs one byte to say so rather than forty-eight to say
                    // white three times.
                    if title.ink().is_white() {
                        writer.u8(INK_WHITE)?;
                    } else {
                        writer.u8(INK_PRESENT)?;
                        for channel in title.ink().channels() {
                            write_rational(writer, channel)?;
                        }
                    }
                }
            }
        }
    }
    Ok(media)
}

/// The order the sequence bodies are written in: innermost first.
///
/// Kahn's algorithm, with the graph read straight off the clips: a sequence
/// depends on every sequence it nests. A cycle would leave sequences the loop
/// can never emit, and the model refuses one long before a file can be
/// written — so this refuses rather than looping, which is the difference
/// between an invariant and a hope.
fn body_order(
    project: &Project,
    sequences: &[(
        media_editor_model::SequenceId,
        &media_editor_model::Sequence,
    )],
) -> Result<Vec<usize>> {
    let mut order = Vec::new();
    order
        .try_reserve(sequences.len())
        .map_err(|_| IoStatus::OutOfMemory)?;
    let mut done = alloc::vec![false; sequences.len()];
    for _ in 0..sequences.len() {
        let mut moved = false;
        for (place, (_, sequence)) in sequences.iter().enumerate() {
            if done[place] {
                continue;
            }
            let ready = nests_of(project, sequence)?.into_iter().all(|named| {
                sequences
                    .iter()
                    .position(|(id, _)| *id == named)
                    .is_none_or(|found| done[found])
            });
            if ready {
                order.push(place);
                done[place] = true;
                moved = true;
            }
        }
        if !moved {
            break;
        }
    }
    if order.len() != sequences.len() {
        // Only a cycle can leave one behind, and the model refuses those.
        return Err(IoStatus::SequenceIndexOutOfRange);
    }
    Ok(order)
}

/// Which sequences a sequence nests, in index order.
fn nests_of(
    project: &Project,
    sequence: &media_editor_model::Sequence,
) -> Result<Vec<media_editor_model::SequenceId>> {
    let mut named = Vec::new();
    for track in sequence.tracks() {
        for item in track.items() {
            let Item::Clip(clip) = item else {
                continue;
            };
            let Ok(asset) = project.media().get(clip.media()) else {
                continue;
            };
            if let Some(inner) = asset.nested() {
                named.try_reserve(1).map_err(|_| IoStatus::OutOfMemory)?;
                named.push(inner);
            }
        }
    }
    Ok(named)
}

/// Decode just the payload.
fn decode_payload(payload: &[u8]) -> Result<Project> {
    let mut reader = Reader::new(payload);
    let mut project = Project::new();

    // The sequences' headers first, and nothing but their timebases: enough to
    // create each one and hand back an identifier, and not enough to need a
    // single media asset. That is what lets the media table below name one.
    // See the writer for why the knot needs three phases rather than two.
    let sequence_count = bounded(reader.u32()?, MAX_SEQUENCES)?;
    let mut sequences = Vec::new();
    sequences
        .try_reserve(sequence_count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for _ in 0..sequence_count {
        let timebase = read_timebase(&mut reader)?;
        sequences.push(project.add_sequence(timebase)?);
    }

    let media_count = bounded(reader.u32()?, MAX_MEDIA)?;
    let mut media = Vec::new();
    media
        .try_reserve(media_count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for _ in 0..media_count {
        let digest = Digest::new(reader.digest_bytes()?);
        let captions = read_captions(&mut reader)?;
        let timebase = read_timebase(&mut reader)?;
        let ticks = reader.i64()?;
        let duration = Duration::new(ticks, timebase)?;
        let length = bounded(reader.u32()?, MAX_LOCATION_BYTES)?;
        let location = if length == 0 {
            None
        } else {
            Some(Location::new(reader.take(length)?).map_err(IoStatus::Model)?)
        };
        // One asset per digest, so a file that lists one twice is not a file
        // this encoder could have written. `add_media` would quietly hand back
        // the identifier it already had, and every clip indexing the second
        // record would then point at the first -- which is a different
        // programme, arrived at silently. Refused instead.
        if project.find_media(digest).is_some() {
            return Err(IoStatus::DuplicateMedia);
        }
        let asset = read_asset(
            &mut reader,
            digest,
            timebase,
            duration,
            location,
            &sequences,
        )?;
        media.push(project.add_media(asset.with_captions(captions)?)?);
    }

    // And now the bodies, each saying which header it belongs to. Innermost
    // first — see the writer for why the order is not the headers' own.
    let mut built = alloc::vec![false; sequences.len()];
    for _ in 0..sequences.len() {
        let place = usize::try_from(reader.u32()?).map_err(|_| IoStatus::TooMany)?;
        let id = *sequences
            .get(place)
            .ok_or(IoStatus::SequenceIndexOutOfRange)?;
        // Once each, and every one. A file naming a body twice would leave
        // another sequence empty while the first was built over the top of
        // itself, and both halves of that are a project no editor produced.
        let seen = built
            .get_mut(place)
            .ok_or(IoStatus::SequenceIndexOutOfRange)?;
        if *seen {
            return Err(IoStatus::SequenceBodyTwice);
        }
        *seen = true;
        read_body(&mut reader, &mut project, id, &media)?;
    }

    if !reader.is_finished() {
        return Err(IoStatus::TrailingBytes);
    }

    // Loading is not editing: a freshly opened project has nothing to undo.
    // The edits above are how the structure is built, not a history the user
    // performed, and offering to undo them would offer to undo the file.
    project.forget_history();
    Ok(project)
}

/// A clip's framing, if it has one.
///
/// Out of [`read_item`] with its two neighbours, because a clip's decorations
/// are each a tag and a body and reading them in line made one function long
/// enough for the length lint to say so. Each of them ends by handing what it
/// read to the model's own constructor, which is the property worth being able
/// to see at a glance.
fn read_transform(reader: &mut Reader<'_>) -> Result<Option<Transform>> {
    match reader.u8()? {
        0 => Ok(None),
        1 => {
            let resampling = match reader.u8()? {
                0 => Resampling::Area,
                1 => Resampling::Bilinear,
                other => return Err(IoStatus::UnknownTransformTag(other)),
            };
            let mut linear = [media_editor_core::Rational::ZERO; 4];
            for slot in &mut linear {
                *slot = read_rational(reader)?;
            }
            let across = read_rational(reader)?;
            let down = read_rational(reader)?;
            let pivot = (read_rational(reader)?, read_rational(reader)?);
            // Through the model's own constructor, so a file cannot hold a
            // transform the model would have refused -- one that flattens the
            // picture onto a line. The pivot goes on afterwards through
            // `with_anchor`, which is the same path an edit takes and which
            // refuses nothing: every point is a pivot, including points
            // outside the frame.
            Ok(Some(
                Transform::new(linear, (across, down), resampling)
                    .map_err(IoStatus::Model)?
                    .with_anchor(pivot),
            ))
        }
        other => Err(IoStatus::UnknownTransformTag(other)),
    }
}

/// A clip's animation of its framing, if it has one.
fn read_motion(reader: &mut Reader<'_>, timebase: Timebase) -> Result<Option<Motion>> {
    match reader.u8()? {
        MOTION_NONE => Ok(None),
        MOTION_PRESENT => {
            let scale = read_curve(reader, timebase)?;
            let across = read_curve(reader, timebase)?;
            let down = read_curve(reader, timebase)?;
            // The turn's lane last, matching `transform::lanes` -- which is
            // what the writer iterates, so the two orders are one list rather
            // than two that have to be kept agreeing.
            let turn = read_curve(reader, timebase)?;
            // Through the model's own constructor, so a file cannot hold a
            // motion the model would have refused -- one with no lanes at all,
            // or one that scales to nothing.
            Ok(Some(
                Motion::new(scale, across, down, turn).map_err(IoStatus::Model)?,
            ))
        }
        other => Err(IoStatus::UnknownMotionTag(other)),
    }
}

/// A clip's mask, if it has one.
fn read_mask(reader: &mut Reader<'_>) -> Result<Option<Mask>> {
    match reader.u8()? {
        MASK_NONE => Ok(None),
        MASK_PRESENT => {
            let inverted = match reader.u8()? {
                0 => false,
                1 => true,
                other => return Err(IoStatus::UnknownMaskTag(other)),
            };
            let count = bounded(reader.u32()?, MAX_CORNERS)?;
            let mut corners = Vec::new();
            for _ in 0..count {
                let x = read_rational(reader)?;
                let y = read_rational(reader)?;
                corners.push((x, y));
            }
            // Through the model's own constructor, like everything else, so a
            // file cannot hold a shape the model would have refused -- a
            // concave one, or one with no area.
            Ok(Some(
                Mask::new(corners)
                    .map_err(IoStatus::Model)?
                    .with_inversion(inverted),
            ))
        }
        other => Err(IoStatus::UnknownMaskTag(other)),
    }
}

/// Read one sequence's body: its tracks, their contents, and its markers.
///
/// Out of [`decode_payload`] because that function outgrew the length lint
/// when the bodies gained an index, and because a body is a thing with a name.
///
/// Everything here goes through [`Project::apply`], like everything else the
/// reader does, so a file cannot set a value the model itself would have
/// refused.
fn read_body(
    reader: &mut Reader<'_>,
    project: &mut Project,
    id: media_editor_model::SequenceId,
    media: &[MediaId],
) -> Result<()> {
    let timebase = project.sequence(id)?.timebase();
    let track_count = bounded(reader.u32()?, MAX_TRACKS_PER_SEQUENCE)?;
    for track in 0..track_count {
        let tag = reader.u8()?;
        let kind = match tag {
            KIND_VIDEO => TrackKind::Video,
            KIND_AUDIO => TrackKind::Audio,
            other => return Err(IoStatus::UnknownTrackKind(other)),
        };
        project.apply(id, Edit::AddTrack { index: track, kind })?;
        let fader = read_fader(reader)?;
        if fader != Fader::UNITY {
            // Applied as an edit like everything else, so that loading a
            // project uses the one way in and nothing can set a value the
            // model would have refused.
            project.apply(id, Edit::SetTrackFader { track, fader })?;
        }
        let item_count = bounded(reader.u32()?, MAX_ITEMS_PER_TRACK)?;
        for index in 0..item_count {
            let item = read_item(reader, timebase, media)?;
            project.apply(id, Edit::InsertItem { track, index, item })?;
        }
        let transition_count = bounded(reader.u32()?, MAX_ITEMS_PER_TRACK)?;
        for _ in 0..transition_count {
            let boundary = bounded(reader.u32()?, MAX_ITEMS_PER_TRACK)?;
            let duration = Duration::new(reader.i64()?, timebase).map_err(IoStatus::Time)?;
            let tag = reader.u8()?;
            let kind = match tag {
                KIND_DISSOLVE => TransitionKind::Dissolve,
                KIND_WIPE => {
                    let across = read_rational(reader)?;
                    let down = read_rational(reader)?;
                    let softness = read_rational(reader)?;
                    TransitionKind::Wipe(
                        Wipe::soft(across, down, softness).map_err(IoStatus::Model)?,
                    )
                }
                other => return Err(IoStatus::UnknownTransitionTag(other)),
            };
            let transition = Transition::of(boundary, duration, kind).map_err(IoStatus::Model)?;
            // Through the edit, like everything else, so a file cannot set
            // a dissolve the model itself would have refused — one on a
            // gap, one longer than its clips, or one whose incoming side
            // has no room for handles.
            project.apply(id, Edit::AddTransition { track, transition })?;
        }
        if let Some(opacity) = read_curve(reader, timebase)? {
            // Through the edit, like everything else, so a file cannot set
            // a curve the model itself would have refused — one on the
            // wrong kind of track, or one counted in another timebase.
            project.apply(
                id,
                Edit::SetTrackOpacity {
                    track,
                    opacity: Some(opacity),
                },
            )?;
        }
        if let Some(level) = read_curve(reader, timebase)? {
            project.apply(
                id,
                Edit::SetTrackLevel {
                    track,
                    level: Some(level),
                },
            )?;
        }
    }
    for marker in read_markers(reader, timebase, MAX_MARKERS_PER_SEQUENCE)? {
        // Through the edit, like everything else, so a file cannot hold a
        // marker the model would have refused -- two at one instant, one
        // before the programme starts, or one carrying more text than the
        // bound allows.
        project.apply(
            id,
            Edit::AddMarker {
                at: marker.at(),
                text: alloc::string::String::from(marker.text()),
            },
        )?;
    }
    Ok(())
}

/// One item.
fn read_item(reader: &mut Reader<'_>, timebase: Timebase, media: &[MediaId]) -> Result<Item> {
    match reader.u8()? {
        TAG_CLIP => {
            let index = usize::try_from(reader.u32()?).map_err(|_| IoStatus::TooMany)?;
            let id = *media.get(index).ok_or(IoStatus::MediaIndexOutOfRange)?;
            let source_start = reader.i64()?;
            let duration = Duration::new(reader.i64()?, timebase)?;
            let transform = read_transform(reader)?;
            let motion = read_motion(reader, timebase)?;
            if motion.is_some() && transform.is_none() {
                // The invariant the edit enforces, enforced again here. A
                // decoder that skipped it would let a file produce a project
                // no sequence of edits could have produced, and every later
                // reader of that project would be entitled to assume it could.
                return Err(IoStatus::Model(
                    media_editor_model::ModelStatus::NoTransformToAnimate,
                ));
            }
            let opacity = read_curve(reader, timebase)?;
            let mask_motion = read_motion(reader, timebase)?;
            let mask = read_mask(reader)?;
            let playback = match reader.u8()? {
                SPEED_REAL_TIME => None,
                SPEED_PRESENT => Some(Playback::At(read_rational(reader)?)),
                SPEED_FROZEN => Some(Playback::Frozen),
                SPEED_RAMP => Some(Playback::Ramp(
                    read_curve(reader, timebase)?.ok_or(IoStatus::RampWithoutKeyframes)?,
                )),
                other => return Err(IoStatus::UnknownSpeedTag(other)),
            };
            let fades = match reader.u8()? {
                FADES_NONE => None,
                FADES_PRESENT => {
                    let rising = Duration::new(reader.i64()?, timebase)?;
                    let falling = Duration::new(reader.i64()?, timebase)?;
                    Some((rising, falling))
                }
                other => return Err(IoStatus::UnknownFadeTag(other)),
            };
            let grade = match reader.u8()? {
                GRADE_NONE => None,
                GRADE_PRESENT => Some(Digest::new(reader.digest_bytes()?)),
                other => return Err(IoStatus::UnknownGradeTag(other)),
            };
            let grade_strength = read_curve(reader, timebase)?;
            let notes = read_markers(reader, timebase, MAX_MARKERS_PER_CLIP)?;
            let clip = Clip::new(id, source_start, duration)?
                .with_grade(grade)
                .with_mask(mask)
                .with_transform(transform)
                .with_motion(motion)
                // Through the model's own constructor, so a file cannot hold
                // an animation of a mask that is not there -- a project no
                // sequence of edits could have produced.
                .with_mask_motion(mask_motion)
                .map_err(IoStatus::Model)?
                // Through the model's own constructor, like everything else,
                // though here the refusal cannot actually arise: the curve is
                // read at the sequence's timebase and the clip's duration is
                // built from the same one, so the two agree by construction.
                // The `?` is plumbing rather than a guard, and saying so is
                // better than leaving a reader to look for the test that
                // exercises it.
                .with_opacity(opacity)
                .map_err(IoStatus::Model)?
                // Through the model's own constructor again, and here the
                // refusal is a real one rather than plumbing: a file holding a
                // strength for a clip with no grade would produce a project no
                // sequence of edits could, and every later reader of it would
                // be entitled to assume the strength meant something.
                .with_grade_strength(grade_strength)
                .map_err(IoStatus::Model)?;
            // Through the model's own door, one at a time, so a file cannot
            // hold two notes at one offset, a note before the clip begins, or
            // more of them than the bound allows.
            let mut clip = clip;
            for marker in notes {
                clip = clip.with_marker(marker).map_err(IoStatus::Model)?;
            }
            let clip = match fades {
                None => clip,
                // Through the model's own constructor, so a file cannot hold a
                // clip whose fades outlast it.
                Some((rising, falling)) => {
                    clip.with_fades(rising, falling).map_err(IoStatus::Model)?
                }
            };
            Ok(Item::Clip(match playback {
                None => clip,
                // Likewise: a file cannot hold a clip stopped at a speed of
                // nought, or one reversed so far it reads before its media
                // begins, or a ramp that turns around or eases. Through the
                // model's own doors, so the file cannot hold a clip no
                // sequence of edits could produce. A freeze goes through
                // `Clip::frozen`, which refuses nothing -- it shrinks what the
                // clip reads to one frame.
                Some(held) => held.applied_to(&clip).map_err(IoStatus::Model)?,
            }))
        }
        TAG_GAP => {
            let duration = Duration::new(reader.i64()?, timebase)?;
            Ok(Item::gap(duration)?)
        }
        other => Err(IoStatus::UnknownItemTag(other)),
    }
}

/// Read an asset's transcript.
///
/// Through [`media_editor_model::caption::Caption::new`] and the model's own
/// check, so a file cannot hold a caption no editor could have made: not one
/// that ends before it begins, not one longer than the bound, and not two of
/// one voice over the same tick. A hostile file must not be able to talk its
/// way past a bound (R-11.2), and the bound it must not pass is the model's.
fn read_captions(reader: &mut Reader<'_>) -> Result<Vec<media_editor_model::caption::Caption>> {
    let count = bounded(
        reader.u32()?,
        media_editor_model::caption::MAX_CAPTIONS_PER_ASSET,
    )?;
    let mut captions = Vec::new();
    captions
        .try_reserve(count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for _ in 0..count {
        let from = reader.i64()?;
        let to = reader.i64()?;
        let voice = reader.u8()?;
        let length = bounded(
            reader.u32()?,
            media_editor_model::caption::MAX_CAPTION_TEXT * 4,
        )?;
        let words =
            core::str::from_utf8(reader.take(length)?).map_err(|_| IoStatus::CaptionNotText)?;
        captions.push(
            media_editor_model::caption::Caption::new(from, to, voice, words)
                .map_err(IoStatus::Model)?,
        );
    }
    // There was a call to `caption::checked` here and its control could not be
    // made to fail: the caller hands these to `MediaAsset::with_captions`,
    // which performs exactly this check, so a file holding two captions of one
    // voice over one tick is refused either way. Sixth guard this project has
    // found that changes no answer, and the second in this milestone.
    Ok(captions)
}

/// Read one media asset's source, and build the asset.
///
/// Out of [`decode_payload`] because it reads before it decides, which is the
/// shape worth having a name — and because a title's own checks belong beside
/// the bytes they check rather than in the middle of a loop over sequences.
fn read_asset(
    reader: &mut Reader<'_>,
    digest: Digest,
    timebase: Timebase,
    duration: Duration,
    location: Option<Location>,
    sequences: &[media_editor_model::SequenceId],
) -> Result<MediaAsset> {
    match reader.u8()? {
        SOURCE_RECORDED => {
            Ok(MediaAsset::new(digest, timebase, duration)?.with_location(location)?)
        }
        SOURCE_NESTED => {
            let place = usize::try_from(reader.u32()?).map_err(|_| IoStatus::TooMany)?;
            let named = *sequences
                .get(place)
                .ok_or(IoStatus::SequenceIndexOutOfRange)?;
            // Through the model's own constructor, which computes the digest
            // from the identifier -- so a file that carried a *different*
            // digest for a nest is refused below rather than believed. A nest
            // is named by what it is, and what it is, is which sequence.
            let asset = MediaAsset::nesting(named, timebase, duration)?;
            if asset.digest() != digest {
                return Err(IoStatus::NestDigestMismatch);
            }
            if location.is_some() {
                // A nest has nowhere to be, for the reason a title has
                // nowhere to be: it is made rather than found, and a location
                // would invite somebody to relink it to a file.
                return Err(IoStatus::NestHasLocation);
            }
            Ok(asset)
        }
        SOURCE_TITLE => {
            let count = bounded(reader.u32()?, MAX_TITLE_LINES)?;
            let mut lines = Vec::new();
            lines
                .try_reserve(count)
                .map_err(|_| IoStatus::OutOfMemory)?;
            for _ in 0..count {
                let words = bounded(reader.u32()?, MAX_TITLE_TEXT)?;
                lines.push(
                    alloc::string::String::from_utf8(reader.take(words)?.to_vec())
                        .map_err(|_| IoStatus::TitleNotText)?,
                );
            }
            let alignment = match reader.u8()? {
                ALIGN_LEFT => Alignment::Left,
                ALIGN_CENTRE => Alignment::Centre,
                ALIGN_RIGHT => Alignment::Right,
                other => return Err(IoStatus::UnknownAlignmentTag(other)),
            };
            let size = read_rational(reader)?;
            let across = read_rational(reader)?;
            let down = read_rational(reader)?;
            let ink = match reader.u8()? {
                INK_WHITE => None,
                INK_PRESENT => {
                    let mut channels = [media_editor_core::Rational::ZERO; 3];
                    for slot in &mut channels {
                        *slot = read_rational(reader)?;
                    }
                    let [red, green, blue] = channels;
                    // Through the model's own constructor, so a file cannot
                    // hold an ink the model would have refused -- one brighter
                    // than white, or one that subtracts.
                    Some(Ink::new(red, green, blue).map_err(IoStatus::Model)?)
                }
                other => return Err(IoStatus::UnknownInkTag(other)),
            };
            // Through the model's own constructor, like everything else, so a
            // file cannot hold a title the model would have refused.
            let title =
                Title::new(lines, size, across, down, alignment).map_err(IoStatus::Model)?;
            let title = match ink {
                None => title,
                Some(held) => title.with_ink(held),
            };
            let asset = MediaAsset::titled(title, timebase, duration)?;
            if asset.digest() != digest {
                // A title is *named by* its description, so the two are one
                // fact written twice and a file where they disagree is a file
                // that has been edited. Recomputing and accepting whichever
                // came out would silently repoint every clip of the card;
                // refusing says what happened.
                return Err(IoStatus::TitleDigestMismatch);
            }
            if location.is_some() {
                // A title has nowhere to be, and the model refuses a hint on
                // one. A file carrying both would describe something no
                // sequence of edits could produce.
                return Err(IoStatus::Model(
                    media_editor_model::ModelStatus::NotRecordedMedia,
                ));
            }
            Ok(asset)
        }
        other => Err(IoStatus::UnknownMediaSourceTag(other)),
    }
}

/// How a title's alignment is tagged.
///
/// The numbers are part of the format and may never be reassigned: a tag that
/// changed meaning would silently re-set every saved card (R-1.2).
const ALIGN_LEFT: u8 = 0;
/// Every line centred on the block's middle.
const ALIGN_CENTRE: u8 = 1;
/// Every line ending at the block's right edge.
const ALIGN_RIGHT: u8 = 2;

/// An asset whose bytes are somewhere.
const SOURCE_RECORDED: u8 = 0;

/// An asset the program draws from a description that follows.
const SOURCE_TITLE: u8 = 1;

/// A media asset that is another sequence of this project.
const SOURCE_NESTED: u8 = 2;

/// A title nobody has coloured, which is white.
const INK_WHITE: u8 = 0;

/// A title whose three channels of light follow.
const INK_PRESENT: u8 = 1;

/// A clip playing its media in real time and forwards.
const SPEED_REAL_TIME: u8 = 0;

/// A clip whose speed follows as an exact fraction.
const SPEED_PRESENT: u8 = 1;

/// A clip held on the one frame at its in point.
const SPEED_FROZEN: u8 = 2;

/// A clip whose speed is a curve, which follows.
const SPEED_RAMP: u8 = 3;

/// A clip nobody has faded.
const FADES_NONE: u8 = 0;

/// A clip whose two fade lengths follow.
const FADES_PRESENT: u8 = 1;

/// A clip whose framing is held still.
const MOTION_NONE: u8 = 0;

/// A clip whose animation follows as three curves: scale, across, down.
const MOTION_PRESENT: u8 = 1;

/// A clip with no shape on it.
const MASK_NONE: u8 = 0;

/// A clip whose shape follows.
const MASK_PRESENT: u8 = 1;

/// A clip with no look on it.
const GRADE_NONE: u8 = 0;
/// A clip whose look follows as thirty-two bytes.
const GRADE_PRESENT: u8 = 1;

/// How the interpolations are tagged.
///
/// The numbers are part of the format and may never be reassigned: a tag that
/// changed meaning would turn every saved ease into a hold, silently (R-1.2).
const HOW_HOLD: u8 = 1;
/// A straight run to the next keyframe.
const HOW_LINEAR: u8 = 2;
/// A cubic Bézier, followed by its four handle components.
const HOW_EASE: u8 = 3;

/// Write one of a track's automation curves, or the absence of one.
///
/// A count of nought means no automation, which is not the same as a curve
/// holding one: a track with no automation has none to read, and the model
/// keeps them apart so automation can be switched off and back on without
/// losing the shape somebody drew.
fn write_curve(writer: &mut Writer, curve: Option<&Curve>) -> Result<()> {
    let Some(curve) = curve else {
        return writer.u32(0);
    };
    let keyframes = curve.keyframes();
    writer.u32(u32::try_from(keyframes.len()).map_err(|_| IoStatus::TooMany)?)?;
    for keyframe in keyframes {
        writer.i64(keyframe.at().ticks())?;
        write_rational(writer, keyframe.value())?;
        match keyframe.interpolation() {
            Interpolation::Hold => writer.u8(HOW_HOLD)?,
            Interpolation::Linear => writer.u8(HOW_LINEAR)?,
            Interpolation::Ease {
                out_x,
                out_y,
                in_x,
                in_y,
            } => {
                writer.u8(HOW_EASE)?;
                for handle in [out_x, out_y, in_x, in_y] {
                    write_rational(writer, handle)?;
                }
            }
        }
    }
    Ok(())
}

/// Read one of a track's automation curves, if it has one.
fn read_curve(reader: &mut Reader<'_>, timebase: Timebase) -> Result<Option<Curve>> {
    let count = bounded(reader.u32()?, MAX_KEYFRAMES)?;
    if count == 0 {
        return Ok(None);
    }
    let mut keyframes = Vec::new();
    keyframes
        .try_reserve(count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for _ in 0..count {
        let at = Instant::new(reader.i64()?, timebase);
        let value = read_rational(reader)?;
        let tag = reader.u8()?;
        let how = match tag {
            HOW_HOLD => Interpolation::Hold,
            HOW_LINEAR => Interpolation::Linear,
            HOW_EASE => Interpolation::Ease {
                out_x: read_rational(reader)?,
                out_y: read_rational(reader)?,
                in_x: read_rational(reader)?,
                in_y: read_rational(reader)?,
            },
            other => return Err(IoStatus::UnknownInterpolationTag(other)),
        };
        // Through the model's own constructor, so a file cannot carry a handle
        // that folds the curve back on itself.
        keyframes.push(Keyframe::new(at, value, how).map_err(IoStatus::Model)?);
    }
    Ok(Some(Curve::new(keyframes).map_err(IoStatus::Model)?))
}

/// Write a rational as its two halves.
///
/// Both, rather than a single scaled integer, because the whole point of the
/// type is that a third is a third: writing it as a decimal would be the one
/// place in the project file where an exact number stopped being one.
fn write_rational(writer: &mut Writer, value: Rational) -> Result<()> {
    writer.i64(value.numerator())?;
    writer.i64(value.denominator())
}

/// Read a rational, refusing one the core type would not build.
fn read_rational(reader: &mut Reader<'_>) -> Result<Rational> {
    let numerator = reader.i64()?;
    let denominator = reader.i64()?;
    Rational::new(numerator, denominator).map_err(IoStatus::Time)
}

/// How a muted fader is tagged, as against one at a level.
const FADER_MUTED: u8 = 0;

/// How a fader at a level is tagged.
const FADER_LEVEL: u8 = 1;

/// Write a track's fader.
///
/// A tag then a ratio, because "off" is not a point on the decibel scale and
/// encoding it as a very small number would make it one — a reader could then
/// not tell a muted track from a track turned all the way down, and unmuting
/// would not restore the level.
fn write_fader(writer: &mut Writer, fader: Fader) -> Result<()> {
    match fader.decibels() {
        None => {
            writer.u8(FADER_MUTED)?;
            writer.i64(0)?;
            writer.i64(1)
        }
        Some(decibels) => {
            writer.u8(FADER_LEVEL)?;
            writer.i64(decibels.numerator())?;
            writer.i64(decibels.denominator())
        }
    }
}

/// Read a track's fader, refusing a level the model would not accept.
fn read_fader(reader: &mut Reader<'_>) -> Result<Fader> {
    let tag = reader.u8()?;
    let numerator = reader.i64()?;
    let denominator = reader.i64()?;
    match tag {
        FADER_MUTED => Ok(Fader::MUTED),
        FADER_LEVEL => {
            let level = Rational::new(numerator, denominator).map_err(IoStatus::Time)?;
            Ok(Fader::at(level)?)
        }
        other => Err(IoStatus::UnknownFaderTag(other)),
    }
}

fn write_timebase(writer: &mut Writer, timebase: Timebase) -> Result<()> {
    writer.i64(timebase.rate().numerator())?;
    writer.i64(timebase.rate().denominator())
}

/// Read a timebase, refusing anything that is not a rate.
fn read_timebase(reader: &mut Reader<'_>) -> Result<Timebase> {
    let numerator = reader.i64()?;
    let denominator = reader.i64()?;
    Ok(Timebase::new(Rational::new(numerator, denominator)?)?)
}

/// Refuse a declared count the model could not hold anyway.
///
/// This is the check that keeps a hostile file from asking for four billion
/// tracks and being refused only after the allocator has tried (R-11.2).
fn bounded(declared: u32, limit: usize) -> Result<usize> {
    let count = usize::try_from(declared).map_err(|_| IoStatus::TooMany)?;
    if count > limit {
        return Err(IoStatus::TooMany);
    }
    Ok(count)
}
