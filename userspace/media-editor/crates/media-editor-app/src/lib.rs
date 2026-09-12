// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![allow(
    clippy::doc_markdown,
    reason = "Media Editor and Phipia are product names, not identifiers"
)]
//! The application.
//!
//! This is the M1 slate: it builds a real sequence with the real model, cuts
//! it, undoes the whole session, redoes it, and reports every step as exact
//! drop-frame timecode. There is no picture yet, because `PHIP-06` does not
//! exist yet; what there is instead is proof that the model, the time
//! arithmetic, and the heap all work on the freestanding target, written so
//! that its entire output is one deterministic string a test can pin.
//!
//! [`slate`] takes a [`Console`] rather than reaching for one, which is what
//! lets the host suite run the same code against a buffer and compare it to a
//! golden transcript (R-14.1's supporting evidence, not its acceptance).

extern crate alloc;

pub mod export;
pub mod mixdown;
pub mod timeline;

use core::fmt::Write;

use media_editor_abi::seam::{Console, ConsoleWriter, SeamStatus};
use media_editor_core::{CoreStatus, Duration, Instant, Timebase, Timecode};
use media_editor_io::sprw::{self, Reel};
use media_editor_io::{IoStatus, MemoryStorage};
use media_editor_media::{
    CacheKey, ColourDescription, FrameDescription, FramePool, Geometry, MediaStatus, PixelFormat,
    TestPattern,
};
use media_editor_model::curve::{Curve, Interpolation, Keyframe};
use media_editor_model::media::Digest;
use media_editor_model::{Clip, Edit, Item, MediaAsset, ModelStatus, Project, TrackKind};

/// What the slate can refuse.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SlateStatus {
    /// The model refused something the slate asked for.
    Model(ModelStatus),
    /// The console refused output.
    Console(SeamStatus),
    /// Saving or loading the project refused.
    File(IoStatus),
    /// The media types refused.
    Media(MediaStatus),
    /// The colour pipeline refused.
    Render(media_editor_render::RenderStatus),
    /// Compositing was asked to build a stack on a base that is not
    /// premultiplied, and `over` is only correct on values that are.
    BaseNotPremultiplied,
    /// A nested plan came back for a layer that is not there.
    NestWithoutLayer,
    /// Sequences nested deeper than this planner walks.
    NestingTooDeep,
    /// The sound side refused.
    Audio(media_editor_audio::AudioStatus),
}

impl From<media_editor_audio::AudioStatus> for SlateStatus {
    fn from(status: media_editor_audio::AudioStatus) -> Self {
        Self::Audio(status)
    }
}

impl From<media_editor_render::RenderStatus> for SlateStatus {
    fn from(status: media_editor_render::RenderStatus) -> Self {
        Self::Render(status)
    }
}

impl From<MediaStatus> for SlateStatus {
    fn from(status: MediaStatus) -> Self {
        Self::Media(status)
    }
}

impl From<IoStatus> for SlateStatus {
    fn from(status: IoStatus) -> Self {
        Self::File(status)
    }
}

impl From<ModelStatus> for SlateStatus {
    fn from(status: ModelStatus) -> Self {
        Self::Model(status)
    }
}

impl From<CoreStatus> for SlateStatus {
    fn from(status: CoreStatus) -> Self {
        Self::Model(ModelStatus::Time(status))
    }
}

impl From<SeamStatus> for SlateStatus {
    fn from(status: SeamStatus) -> Self {
        Self::Console(status)
    }
}

/// The rate the slate works at: NTSC video, which is where timecode is hard.
const RATE: Timebase = Timebase::NTSC_30;

/// Ten minutes of media, which at 30000/1001 is exactly 17,982 frames.
const MEDIA_FRAMES: i64 = 17_982;

/// How much room the slate's in-memory storage offers a project file.
const FILE_CAPACITY: usize = 8 * 1024;

/// The status a completed run reports to the kernel.
pub const EXIT_SUCCESS: i32 = 0;

/// The status a refused run reports to the kernel.
pub const EXIT_FAILURE: i32 = 1;

/// Run the slate, writing its report to a console.
///
/// Returns the status to exit with, so that the caller does the exiting: the
/// application decides what happened, the runtime decides how to say so to the
/// kernel.
pub fn run(console: &mut dyn Console) -> i32 {
    match slate(console) {
        Ok(()) => EXIT_SUCCESS,
        Err(failure) => {
            let mut writer = ConsoleWriter::new(console);
            let _ = writeln!(writer, "media-editor: refused: {failure:?}");
            EXIT_FAILURE
        }
    }
}

/// Build a sequence, cut it, undo it, redo it, and report every step.
///
/// # Errors
///
/// [`SlateStatus`] for anything the model or the console refuses. A refusal
/// stops the run; nothing here repairs or retries.
pub fn slate(console: &mut dyn Console) -> Result<(), SlateStatus> {
    let mut project = Project::new();
    let asset = MediaAsset::new(Digest::new([0x5A; 32]), RATE, frames(MEDIA_FRAMES)?)?;
    let media = project.add_media(asset)?;
    let sequence = project.add_sequence(RATE)?;

    line(console, "Media Editor slate")?;
    line(console, "")?;
    say(console, "timebase       ", RATE.rate())?;
    say(console, "media length   ", timecode(MEDIA_FRAMES)?)?;
    line(console, "")?;

    project.apply(
        sequence,
        Edit::AddTrack {
            index: 0,
            kind: TrackKind::Video,
        },
    )?;
    project.apply(
        sequence,
        Edit::AddTrack {
            index: 1,
            kind: TrackKind::Audio,
        },
    )?;

    // An hour of drop-frame counting is 107,892 frames, so ten seconds in is
    // frame 300 minus nothing: the first minute is whole. These are ordinary
    // in and out points, chosen so the report shows a label that skips.
    insert_clip(&mut project, sequence, media, 0, 0, 300, 1_798)?;
    insert_clip(&mut project, sequence, media, 0, 1, 5_000, 900)?;
    insert_clip(&mut project, sequence, media, 1, 0, 300, 2_998)?;

    // Cut the first clip and take out the head, which is what trimming an
    // opening actually is.
    project.apply(
        sequence,
        Edit::SplitItem {
            track: 0,
            index: 0,
            offset: 48,
        },
    )?;
    project.apply(sequence, Edit::RemoveItem { track: 0, index: 0 })?;

    report(console, &project, sequence, "after the cut")?;

    let edited_duration = project.sequence(sequence)?.duration()?;

    // Undo the whole session.
    let mut undone = 0_u32;
    while project.undo(sequence).is_ok() {
        undone += 1;
    }
    let mut writer = ConsoleWriter::new(console);
    let _ = writeln!(writer, "undone         {undone} edits");
    writer.finish()?;
    say(
        console,
        "tracks now     ",
        project.sequence(sequence)?.track_count(),
    )?;

    // And put it back.
    let mut redone = 0_u32;
    while project.redo(sequence).is_ok() {
        redone += 1;
    }
    let mut writer = ConsoleWriter::new(console);
    let _ = writeln!(writer, "redone         {redone} edits");
    writer.finish()?;

    let restored = project.sequence(sequence)?.duration()?;
    say(console, "restored       ", restored == edited_duration)?;
    line(console, "")?;

    // Through a file and back. The storage is in memory because Phipia has no
    // writable one yet (PHIP-08), but the format, the digest, the read-back
    // verification and the commit are the real ones, running here on the
    // freestanding target rather than only in the host suite.
    let mut storage = MemoryStorage::new(FILE_CAPACITY);
    let digest = media_editor_io::save(&project, &mut storage)?;
    let bytes = storage.committed().map_or(0, <[u8]>::len);
    let reloaded = media_editor_io::load(&storage)?;
    // Saving what was loaded must produce the same file. That is the whole
    // claim a project format can make: the round trip is lossless, and it is
    // checked by digest rather than by inspection. The reloaded project is
    // not compared to the one in hand, because it deliberately differs in one
    // way - it has no undo history, because a file does not carry one.
    let again = media_editor_io::save(&reloaded, &mut storage)?;
    say(console, "saved          ", bytes)?;
    say(console, "digest         ", digest)?;
    say(console, "round trip     ", again == digest)?;

    line(console, "")?;
    reel(console)?;

    line(console, "")?;
    picture(console)?;

    line(console, "")?;
    line(console, "slate complete")?;
    Ok(())
}

/// Render frames, put them through the mezzanine, and cache them.
///
/// Sixteen by nine pixels, which is not a picture. It is the whole media path
/// — describe, draw, encode, digest, decode, cache, evict — running on the
/// freestanding target with the heap it actually has. `PHIP-03` is what turns
/// this into a frame.
fn reel(console: &mut dyn Console) -> Result<(), SlateStatus> {
    let described = FrameDescription::square(
        Geometry::new(16, 9)?,
        PixelFormat::Gray8,
        ColourDescription::srgb_full(),
        None,
        None,
    )?;
    let frames = [
        TestPattern::Bars.render(described)?,
        TestPattern::Ramp.render(described)?,
        TestPattern::Flat { value: 77 }.render(described)?,
    ];

    let mut pool = FramePool::new(2, 4096);
    for (index, frame) in frames.iter().enumerate() {
        let ordinal = u32::try_from(index).unwrap_or(0);
        let key = CacheKey::current(&[frame.digest()], &ordinal.to_le_bytes());
        pool.insert(key, frame.clone())?;
    }
    let census = pool.census();

    let mut held = alloc::vec::Vec::new();
    held.extend_from_slice(&frames);
    let reel = Reel::new(RATE, held)?;
    let encoded = sprw::encode(&reel)?;
    let decoded = sprw::decode(&encoded)?;

    say(console, "reel frames    ", decoded.len())?;
    say(console, "reel bytes     ", encoded.len())?;
    say(
        console,
        "reel digest    ",
        media_editor_core::Digest::of(&encoded),
    )?;
    say(console, "reel matches   ", decoded == reel)?;
    say(console, "pool frames    ", census.frames)?;
    say(console, "pool bytes     ", census.bytes)?;
    say(console, "pool evictions ", census.evictions)?;
    Ok(())
}

/// Where the picture below reads its frames from.
///
/// Two flat colours, keyed by content digest, which is how the graph names
/// media. A real one reads a file; `PHIP-08` is what turns this into that.
struct Held {
    frames: alloc::vec::Vec<(Digest, media_editor_media::Frame)>,
}

impl media_editor_render::Library for Held {
    fn frame(
        &mut self,
        media: Digest,
        _tick: i64,
        description: FrameDescription,
    ) -> Result<media_editor_media::Frame, media_editor_render::RenderStatus> {
        let held = self
            .frames
            .iter()
            .find(|(digest, _)| *digest == media)
            .map(|(_, frame)| frame.clone())
            .ok_or(media_editor_render::RenderStatus::UnknownNode)?;
        // No check that the description matches what was asked for. The graph
        // makes exactly that check on the way back and refuses by name, so a
        // second one here is a branch nothing can reach — a control removed it
        // and no test noticed, which is what an unreachable guard looks like.
        let _ = description;
        Ok(held)
    }

    fn look(
        &mut self,
        _look: Digest,
    ) -> Result<media_editor_render::Look, media_editor_render::RenderStatus> {
        // Nothing here is graded. A request for a look is a fault rather than
        // a case to handle, and refusing says so.
        Err(media_editor_render::RenderStatus::UnknownNode)
    }
}

/// Render one instant of a sequence, and say what came out.
///
/// This is the half of the program the freestanding image did not touch until
/// now — the layer stack, the plan, the graph, the compositor and the pool,
/// running on the target with the heap it actually has. Everything above it
/// tested the model and the formats; the picture is what the rest of the
/// project is made of.
///
/// Sixteen by nine pixels, which is not a picture. `PHIP-03` is what turns it
/// into one.
///
/// The digest at the end is a golden render hash, which
/// [`docs/VERIFICATION.md`] asks every render to carry and which nothing here
/// had. It names the project, the instant and the description that produced
/// it: change any of the three, or the arithmetic behind them, and it moves.
fn picture(console: &mut dyn Console) -> Result<(), SlateStatus> {
    let described = FrameDescription::square(
        Geometry::new(16, 9)?,
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(media_editor_media::AlphaState::Premultiplied),
    )?;

    // Two flat layers: an opaque one underneath and a half-covered one over
    // it, so the composite is a real `over` rather than a copy.
    let mut library = Held {
        frames: alloc::vec::Vec::new(),
    };
    let mut project = Project::new();
    let mut placed = alloc::vec::Vec::new();
    for (track, pixel) in [(0_usize, [40_u8, 40, 40, 255]), (1, [128, 0, 0, 128])] {
        let frame = flat(described, pixel)?;
        let digest = frame.digest();
        library.frames.push((digest, frame));
        let asset = MediaAsset::new(digest, RATE, frames(240)?)?;
        placed.push((track, project.add_media(asset)?));
    }

    let sequence = project.add_sequence(RATE)?;
    for (track, media) in placed {
        project.apply(
            sequence,
            Edit::AddTrack {
                index: track,
                kind: TrackKind::Video,
            },
        )?;
        insert_clip(&mut project, sequence, media, track, 0, 0, 48)?;
    }

    // A fade on the upper track, so the picture depends on *when* it is
    // rendered. Without it both clips run the whole span, every instant looks
    // the same, and the golden hash below says nothing about time — which a
    // control proved by moving the playhead a frame and breaking nothing.
    //
    // It also puts the curve arithmetic in the image: exact rationals, an
    // interpolation, and a fixed-point opacity reaching the compositor, all on
    // the target rather than only on the host.
    let rise = Curve::new({
        let mut keyframes = alloc::vec::Vec::new();
        keyframes
            .try_reserve(2)
            .map_err(|_| ModelStatus::OutOfMemory)?;
        keyframes.push(Keyframe::new(
            Instant::new(0, RATE),
            media_editor_core::Rational::new(0, 1)?,
            Interpolation::Linear,
        )?);
        keyframes.push(Keyframe::new(
            Instant::new(24, RATE),
            media_editor_core::Rational::new(1, 1)?,
            Interpolation::Hold,
        )?);
        keyframes
    })?;
    project.apply(
        sequence,
        Edit::SetTrackOpacity {
            track: 1,
            opacity: Some(rise),
        },
    )?;

    let mut pool = FramePool::new(8, 16 * 1024);
    let rendered = crate::timeline::render(
        &project,
        sequence,
        Instant::new(12, RATE),
        described,
        &mut pool,
        &mut library,
    )?;

    say(console, "picture size   ", rendered.bytes())?;
    say(console, "picture digest ", rendered.digest())?;
    // Worked out rather than read off. At frame 12 of a 24-frame rise the
    // track is at half opacity. Fading a premultiplied layer scales its
    // **colour in light** and its **coverage in its stored value**, because
    // only one of the two is light — so the top's red 128 becomes light
    // 0.2159 × ½ = 0.1080, and its coverage 128 becomes 64.
    //
    // Over the dark grey, in linear light: 40 decodes to 0.0212, and
    // `0.1080 + 0.0212·(1 − 64/255)` is 0.1238, which encodes back to 98.
    //
    // This number has been wrong twice and each time for a different reason,
    // which is why it is derived here in full rather than recorded.
    //
    // It read 148 once, from adding code values — the exact mistake `over`
    // exists to prevent. The arithmetic was right and the comment was wrong.
    //
    // It read 73 until M8.17, and that time the *arithmetic* was wrong:
    // `faded` scaled the colour in code values too, so the top arrived as red
    // 64 (light 0.0513) instead of light 0.1080. Every test the project had
    // faded a layer that was black, where nought times anything is nought and
    // the two arithmetics agree — so nothing caught it until a dissolve
    // between two *identical* pictures was asked to be that picture, and
    // sagged by twenty-eight code values in the middle instead.
    //
    // The frame is opaque because the programme is: an empty instant is black
    // leader rather than a hole.
    let packed = rendered.packed()?;
    say(console, "picture red    ", packed[0])?;
    say(console, "picture alpha  ", packed[3])?;
    Ok(())
}

/// A frame of one repeated pixel.
fn flat(
    description: FrameDescription,
    pixel: [u8; 4],
) -> Result<media_editor_media::Frame, SlateStatus> {
    let wanted = description.packed_bytes()?;
    let mut bytes = alloc::vec::Vec::new();
    bytes
        .try_reserve(wanted)
        .map_err(|_| MediaStatus::OutOfMemory)?;
    while bytes.len() < wanted {
        bytes.extend_from_slice(&pixel);
    }
    Ok(media_editor_media::Frame::from_owned(description, bytes)?)
}

/// A length in frames of the slate's timebase.
fn frames(count: i64) -> Result<Duration, SlateStatus> {
    Ok(Duration::new(count, RATE)?)
}

/// The drop-frame label of a frame number.
fn timecode(frame: i64) -> Result<Timecode, SlateStatus> {
    Ok(Timecode::from_instant(Instant::new(frame, RATE))?)
}

/// Place a clip on a track.
fn insert_clip(
    project: &mut Project,
    sequence: media_editor_model::SequenceId,
    media: media_editor_model::MediaId,
    track: usize,
    index: usize,
    source_start: i64,
    length: i64,
) -> Result<(), SlateStatus> {
    let clip = Clip::new(media, source_start, frames(length)?)?;
    project.apply(
        sequence,
        Edit::InsertItem {
            track,
            index,
            item: Item::Clip(clip),
        },
    )?;
    Ok(())
}

/// Write one line.
fn line(console: &mut dyn Console, text: &str) -> Result<(), SlateStatus> {
    console.write_line(text)?;
    Ok(())
}

/// Write a labelled value.
fn say<T: core::fmt::Display>(
    console: &mut dyn Console,
    label: &str,
    value: T,
) -> Result<(), SlateStatus> {
    let mut writer = ConsoleWriter::new(console);
    let _ = writeln!(writer, "{label}{value}");
    writer.finish()?;
    Ok(())
}

/// Print every track, every item, and the sequence's length.
fn report(
    console: &mut dyn Console,
    project: &Project,
    id: media_editor_model::SequenceId,
    caption: &str,
) -> Result<(), SlateStatus> {
    let sequence = project.sequence(id)?;
    line(console, caption)?;
    // Tracks are numbered within their kind, the way an edit suite names them:
    // the first audio track is A1 whatever its position in the track list.
    let mut video_number = 0_u32;
    let mut audio_number = 0_u32;
    for track in sequence.tracks() {
        let (kind, number) = match track.kind() {
            TrackKind::Video => {
                video_number += 1;
                ("V", video_number)
            }
            TrackKind::Audio => {
                audio_number += 1;
                ("A", audio_number)
            }
        };
        for index in 0..track.len() {
            let range = track.item_range(index)?;
            let start = timecode(range.start().ticks())?;
            let end = timecode(range.end()?.ticks())?;
            let mut writer = ConsoleWriter::new(console);
            let _ = match track.item(index)? {
                Item::Clip(clip) => {
                    let source = timecode(clip.source_start())?;
                    writeln!(
                        writer,
                        "  {kind}{number}  {start}  {end}  clip  src {source}"
                    )
                }
                Item::Gap(_) => {
                    writeln!(writer, "  {kind}{number}  {start}  {end}  gap")
                }
            };
            writer.finish()?;
        }
    }
    say(
        console,
        "duration       ",
        timecode(sequence.duration()?.ticks())?,
    )?;
    line(console, "")?;
    Ok(())
}
