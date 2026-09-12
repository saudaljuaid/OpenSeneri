// SPDX-License-Identifier: GPL-3.0-only
//! Writing a sequence out, without ever holding a frame.
//!
//! This is the far end of a chain four milestones long, and it is worth naming
//! the links because each one exists only to make this function possible:
//! storage reads at an offset and extends at the end; a catalogue holds a
//! count rather than a vault; a material is one entry's bytes; a spool holds a
//! description rather than a reel; the render graph evaluates a row; a scan
//! plans an instant once and renders its rows; a winder writes a reel
//! forwards. Nothing on the path below holds a project file, a vault, a reel,
//! or a frame.
//!
//! ## The number
//!
//! A 1920×1080 eight-bit RGBA frame is 8,294,400 bytes. Ten seconds of it at
//! 24 frames a second is a reel of 1,990,656,000 — and a Phipia program is
//! mapped **76 KiB**. Writing that reel with [`media_editor_io::sprw::encode`]
//! would need twenty-five thousand times the program's whole address space.
//! Writing it with this needs one row at a time: 7,680 bytes, which is a tenth
//! of what there is.
//!
//! ## The protocol is the one `save` uses, performed streaming
//!
//! R-9.4 asks that an interrupted save leave the previous file exactly where
//! it was. [`media_editor_io::save::save`] does that by encoding into memory,
//! writing, reading back and comparing, and only then committing. An export
//! cannot hold what it wrote, so it compares differently and not less:
//!
//! 1. wind the reel onto the **scratch** slot, a row at a time, keeping a
//!    running digest of everything handed over;
//! 2. open what landed as a reel — which checks its structure and its length
//!    against the header, in sixty-four bytes;
//! 3. read the trailer and require it to be the digest the winder computed,
//!    so that the file *states* what was written;
//! 4. walk what landed in windows and require it to hash to the same thing,
//!    so that what is stored *is* what the file states;
//! 5. only then commit.
//!
//! Three and four are two claims and both are needed. Three alone would pass
//! for a store that wrote the trailer and dropped a row; four alone would pass
//! for a store that corrupted a row and recomputed the trailer to match.
//! Together they say the bytes on the disk hash to the digest computed from
//! the rows the renderer produced, and step five is the seam's one indivisible
//! operation.

use media_editor_audio::{MixReport, SampleRate};
use media_editor_core::{Digest, Instant, TimeRange};
use media_editor_media::{Frame, FrameDescription};
use media_editor_model::{Project, SequenceId};
use media_editor_render::Library;

use media_editor_abi::seam::{Slot, Storage};
use media_editor_io::bytes::{Extent, Sink};
use media_editor_io::save::{Scratch, Staged};
use media_editor_io::sprw::{Sound, Spoken, Spool, TRAILER_BYTES, Winder, sound_bounds};
use media_editor_io::status::IoStatus;
use media_editor_io::vtt::Spotter;

use crate::SlateStatus;
use media_editor_model::caption::{Caption, Transcript, captions_over};

use crate::mixdown::{self, SampleSource};
use crate::timeline::Scan;

/// What to export, and where to put it.
///
/// A structure rather than five arguments, because an export has a lot of
/// inputs and naming them at the call site reads better than counting commas
/// — and because the sound half took the function past what one signature
/// should carry.
pub struct Job<'a> {
    /// The project the sequence belongs to.
    pub project: &'a Project,
    /// Which sequence.
    pub sequence: SequenceId,
    /// Which stretch of it, in the sequence's own timebase.
    pub span: TimeRange,
    /// What every frame is rendered as.
    pub description: FrameDescription,
    /// Which committed slot the finished reel goes into.
    pub into: Slot,
}

/// How many rows an export computes at once.
///
/// [`media_editor_render::resample::MAX_TILE_ROWS`], because a band of rows is
/// held while it is drawn and that constant is where the trade between memory
/// and re-reading is written down. The export takes the largest tile the
/// renderer admits, which is the right default for the one caller that walks a
/// whole picture in order: it pays the memory once and gets every row of the
/// saving.
const TILE: usize = media_editor_render::resample::MAX_TILE_ROWS;

/// The sound to lay against the picture, and where its samples come from.
///
/// The two travel together because they are one decision. A rate and a channel
/// count with no source would be an export that declares sound it cannot
/// produce, and a source with no rate would be one that has samples and
/// nowhere to put them — so the type makes both unrepresentable rather than
/// growing two refusals for a caller's slip.
pub struct Dub<'a> {
    /// What the mix is produced at.
    pub rate: SampleRate,
    /// How many channels it is produced in.
    pub channels: usize,
    /// Where the material's samples come from.
    pub source: &'a mut dyn SampleSource,
}

/// What an export produced, and what happened while it was produced.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Report {
    /// The digest of the reel that was committed.
    pub digest: Digest,
    /// What the mix did, summed over every block.
    ///
    /// **Carried rather than swallowed**, because a mix that clipped is the
    /// one thing about an export that a person has to be told and that no
    /// later reader can recover: the samples in the file are the clipped ones,
    /// and they look exactly like samples somebody meant.
    pub mix: MixReport,
}

/// How much of a staged reel is read at once when it is checked.
///
/// One page. A window is a straight trade — a larger one is fewer reads and
/// more memory — and the machine this is for maps nineteen pages in total, so
/// spending one on the buffer that proves a save is sound is the largest
/// number that is obviously affordable.
pub const WINDOW_BYTES: usize = 4096;

/// Render a span of a sequence into the committed slot named, as a reel.
///
/// The frame count goes into the reel's header before a single sample is
/// written, so the span is asked for up front and every frame of it must
/// render: an export that gave up halfway would leave a header that lies, and
/// [`Winder::finish`] refuses rather than truncating.
///
/// ## Two passes, because the file has two sections
///
/// Pictures are written first and sound after them, which is where they live —
/// see [`media_editor_io::sprw`] for why they are not interleaved. So the span is
/// walked twice: once opening a [`Scan`] per instant and pumping its rows, and
/// once mixing one frame at a time. Neither pass holds more than it is
/// producing: a row of picture, or a frame of sound.
///
/// A frame of sound is 1601 samples or 1602 at 30000/1001 — the mixer's own
/// arithmetic, unchanged and unhelped — and mixing frame by frame gives
/// exactly the samples that mixing the whole span at once would, because that
/// is what [`crate::mixdown::mix`] already does inside itself. Ten seconds of
/// 48 kHz stereo is 3,840,000 bytes; one frame of it is 12,816.
///
/// # Errors
///
/// [`SlateStatus::Render`] if any instant of the span cannot be scanned — a
/// clip with a framing on it, a title, an offline slate — which is answered at
/// the *first* such instant rather than after the ones before it have been
/// written; [`SlateStatus::Audio`] or [`SlateStatus::Model`] if the sound
/// cannot be mixed; [`SlateStatus::File`] for anything the format or the seam
/// refuses, including [`IoStatus::WriteNotVerified`] if what was stored is not
/// what was written; and whatever the library refuses. In every case the
/// destination slot is unchanged.
pub fn export(
    job: &Job<'_>,
    library: &mut dyn Library,
    dub: Option<Dub<'_>>,
    transcript: Option<&mut dyn Transcript>,
    storage: &mut dyn Storage,
) -> Result<Report, SlateStatus> {
    let sequence = job.project.sequence(job.sequence)?;
    let timebase = sequence.timebase();
    let count = usize::try_from(job.span.duration().ticks()).map_err(|_| IoStatus::TooMany)?;
    let sound = declared(job, count, timebase, dub.as_ref())?;
    // The words the programme shows over this span, projected now and rebased
    // to the reel's own nought -- because an exported reel *is* the programme,
    // so its source time is the span's timeline time.
    //
    // Projected before anything is written, because the transcript's count and
    // length go in the header. That means holding them, bounded by
    // `MAX_CAPTIONS_SHOWN`; it is the one thing on this path that is not
    // streamed, and it is bounded rather than unbounded because the query
    // refuses past that count rather than truncating.
    let words = spoken_over(job, timebase, count, transcript)?;
    let said = Spoken::of(&words)?;

    let mut dub = dub;
    let mut mix = MixReport::default();
    let written = {
        let mut sink = Scratch::new(storage);
        // Emptied rather than appended to. The scratch slot is where the last
        // save was assembled and may still hold it, and a reel's header
        // belongs at offset nought.
        sink.clear()?;
        let mut winder = Winder::begin(&mut sink, job.description, timebase, count, sound, said)?;
        for index in 0..count {
            let instant = Scan::open(
                job.project,
                job.sequence,
                at(job, index, timebase)?,
                job.description,
                library,
            )?;
            // A band at a time rather than a row at a time, and the rows
            // are handed to the winder out of it one by one. A framing
            // resamples, and consecutive rows read overlapping bands of the
            // source -- so a band of `TILE` fetches the union once instead of
            // each row's share separately, which on a turn is more than a
            // tenfold difference in rows read.
            //
            // The winder still takes rows, and that is the point: what
            // changed is how many are computed at once, not how many are
            // held for writing.
            let mut row = 0;
            while row < instant.height() {
                let last = (row + TILE).min(instant.height());
                let band = instant.rows(row, last, library)?;
                for (offset, line) in rows_of(&band, last - row)?.into_iter().enumerate() {
                    winder.row(&mut sink, index, row + offset, &line)?;
                }
                row = last;
            }
        }
        if let Some(dub) = dub.as_mut() {
            for index in 0..count {
                let from = at(job, index, timebase)?;
                let one = TimeRange::new(from, media_editor_core::Duration::new(1, timebase)?)
                    .map_err(media_editor_model::ModelStatus::Time)?;
                let (block, block_report) = mixdown::mix(
                    job.project,
                    job.sequence,
                    one,
                    dub.rate,
                    dub.channels,
                    dub.source,
                )?;
                // Summed rather than replaced: a clip in frame four hundred
                // and a clip in frame nine are two clips, and the loudest
                // overshoot is the one worth reporting.
                mix.clipped = mix.clipped.saturating_add(block_report.clipped);
                mix.overshoot = mix.overshoot.max(block_report.overshoot);
                winder.sound_block(&mut sink, index, &block)?;
            }
        }
        for (index, caption) in words.iter().enumerate() {
            winder.caption(&mut sink, index, caption)?;
        }
        winder.finish(&mut sink)?
    };

    verified(storage, written)?;
    storage.commit(job.into).map_err(IoStatus::Seam)?;
    Ok(Report {
        digest: written,
        mix,
    })
}

/// Write a caption sidecar for the same span an export would write.
///
/// The **same words** the reel carries, in the same order, counted from the
/// same nought — because it is the same call to [`spoken_over`] that produces
/// them. A sidecar that projected the captions a second time would be a
/// sidecar that could disagree with its reel, and two files that disagree
/// about what was said are worse than one file with no captions at all.
///
/// It takes a [`Sink`] rather than a [`Slot`], and that is deliberate: the
/// storage seam has three slots and a sidecar is not one of them (R-1.2 — a
/// widening is a new contract, not a convenience). Where a sidecar goes is the
/// caller's decision, and a caller that wants it committed atomically wraps it
/// in the same scratch-and-commit protocol everything else uses.
///
/// # Errors
///
/// Whatever the projection refuses, whatever [`Spotter`] refuses about a cue,
/// and whatever the sink refuses.
pub fn spot(
    job: &Job<'_>,
    transcript: Option<&mut dyn Transcript>,
    sink: &mut dyn Sink,
) -> Result<usize, SlateStatus> {
    let sequence = job.project.sequence(job.sequence)?;
    let timebase = sequence.timebase();
    let count = usize::try_from(job.span.duration().ticks()).map_err(|_| IoStatus::TooMany)?;
    let words = spoken_over(job, timebase, count, transcript)?;
    let mut spotter = Spotter::begin(sink)?;
    for caption in &words {
        spotter.cue(sink, caption, timebase)?;
    }
    Ok(spotter.count())
}

/// One frame per row of a band, in order.
///
/// The band is the frame the scan produced; these are its rows, which is what
/// the winder takes. Each is built from the band's own bytes rather than
/// copied out of a re-packed whole (M8.45), so a band of sixteen rows is one
/// allocation a row and none of them is the band again.
///
/// # Errors
///
/// [`IoStatus::Media`] wrapping whatever the frame types refuse.
fn rows_of(band: &Frame, count: usize) -> Result<alloc::vec::Vec<Frame>, SlateStatus> {
    let described = *band.description();
    let one = media_editor_render::row_description(described, 0)?;
    let line = one.packed_bytes().map_err(IoStatus::Media)?;
    let packed = band.packed().map_err(IoStatus::Media)?;
    let mut found = alloc::vec::Vec::new();
    found
        .try_reserve(count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for index in 0..count {
        let mut held = alloc::vec::Vec::new();
        held.try_reserve(line).map_err(|_| IoStatus::OutOfMemory)?;
        held.extend_from_slice(
            packed
                .get(index * line..(index + 1) * line)
                .ok_or(IoStatus::TruncatedPayload)?,
        );
        found.push(Frame::from_owned(one, held).map_err(IoStatus::Media)?);
    }
    Ok(found)
}

/// The programme's own words over the span, rebased to the reel's nought.
///
/// A caption in the reel is a caption of *the reel*, so its in and out points
/// are counted from the first frame exported rather than from wherever the
/// span happened to sit on the timeline. That is the same rebasing a cut
/// through a ramp performs, for the same reason: a thing extracted from a
/// programme is measured from its own start.
fn spoken_over(
    job: &Job<'_>,
    timebase: media_editor_core::Timebase,
    count: usize,
    transcript: Option<&mut dyn Transcript>,
) -> Result<alloc::vec::Vec<Caption>, SlateStatus> {
    let Some(transcript) = transcript else {
        return Ok(alloc::vec::Vec::new());
    };
    let span = TimeRange::new(
        job.span.start(),
        media_editor_core::Duration::new(
            i64::try_from(count).map_err(|_| IoStatus::TooMany)?,
            timebase,
        )?,
    )
    .map_err(media_editor_model::ModelStatus::Time)?;
    let shown = captions_over(job.project, job.sequence, span, transcript)?;
    let first = job.span.start().ticks();
    let mut words = alloc::vec::Vec::new();
    words
        .try_reserve(shown.len())
        .map_err(|_| IoStatus::OutOfMemory)?;
    for held in shown {
        words.push(Caption::new(
            held.from.ticks() - first,
            held.to.ticks() - first,
            held.caption.voice(),
            held.caption.text(),
        )?);
    }
    Ok(words)
}

/// The instant frame `index` of the span begins at.
fn at(
    job: &Job<'_>,
    index: usize,
    timebase: media_editor_core::Timebase,
) -> Result<Instant, SlateStatus> {
    let tick = job
        .span
        .start()
        .ticks()
        .checked_add(i64::try_from(index).map_err(|_| IoStatus::TooMany)?)
        .ok_or(IoStatus::TooMany)?;
    Ok(Instant::new(tick, timebase))
}

/// What the reel's header will say about its sound.
///
/// The sample count is worked out here, before anything is written, from the
/// span's own boundaries — `⌊(s+n)·r⌋ − ⌊s·r⌋`, which is what the mixer will
/// produce and is *not* in general `⌊n·r⌋`. See [`media_editor_io::sprw`]: which
/// of the two counts a run of frames holds depends on where in a timeline it
/// was cut from, and this is the one place in the program that knows.
fn declared(
    job: &Job<'_>,
    count: usize,
    timebase: media_editor_core::Timebase,
    dub: Option<&Dub<'_>>,
) -> Result<Option<Sound>, SlateStatus> {
    let Some(dub) = dub else {
        return Ok(None);
    };
    let start = usize::try_from(
        job.span
            .start()
            .floor_into(dub.rate.timebase())?
            .ticks()
            .max(0),
    )
    .map_err(|_| IoStatus::TooMany)?;
    let end = usize::try_from(
        at(job, count, timebase)?
            .floor_into(dub.rate.timebase())?
            .ticks()
            .max(0),
    )
    .map_err(|_| IoStatus::TooMany)?;
    let samples = end.checked_sub(start).ok_or(IoStatus::TooMany)?;
    // And it must be a length this many frames could hold, which is the
    // format's own check applied before a header is written rather than after.
    let (least, most) = sound_bounds(count, timebase, dub.rate)?;
    if samples < least || samples > most {
        return Err(IoStatus::SoundRunsPastPicture.into());
    }
    Ok(Some(Sound::new(dub.rate, dub.channels, samples)?))
}

/// Check that what is staged is the reel that was written, and nothing else.
///
/// Split out because it is the half of the protocol that has nothing to do
/// with rendering, and because a test that wants to prove the check works has
/// to be able to reach it without a project.
///
/// # Errors
///
/// [`SlateStatus::File`] with [`IoStatus::WriteNotVerified`] if the trailer is
/// not the digest that was computed, or whatever opening and walking the reel
/// refuses.
fn verified(storage: &dyn Storage, written: Digest) -> Result<(), SlateStatus> {
    let staged = Staged::new(storage)?;
    // Structure first: the magic, the version, every tag, and a length that
    // agrees with the count in the header. Sixty-four bytes, and it is what
    // catches a recording that stopped between the last sample and the
    // trailer.
    let spool = Spool::open(&staged)?;
    // Then the trailer, which must be what the winder computed. A store that
    // wrote every sample and then dropped the last row would pass the walk
    // below, because it would have hashed the file it actually holds.
    let mut tail = [0_u8; TRAILER_BYTES];
    let at = staged
        .length()
        .checked_sub(TRAILER_BYTES as u64)
        .ok_or(IoStatus::TruncatedTrailer)?;
    if staged.read_at(at, &mut tail)? != TRAILER_BYTES {
        return Err(IoStatus::TruncatedTrailer.into());
    }
    if Digest::new(tail) != written {
        return Err(IoStatus::WriteNotVerified.into());
    }
    // And then the walk, in windows, which is the expensive half and the one
    // that says the samples on the disk are the samples that were sent.
    spool.verify(&staged, WINDOW_BYTES)?;
    Ok(())
}
