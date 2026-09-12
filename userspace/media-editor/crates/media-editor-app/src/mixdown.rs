// SPDX-License-Identifier: GPL-3.0-only
//! The sound half of rendering a sequence.
//!
//! [`crate::timeline`] answers what a sequence looks like at an instant. This
//! answers what it *sounds* like over a span, and the difference in shape is
//! the whole of the difference between picture and sound: a frame is a moment,
//! a sound is a stretch.
//!
//! The interesting part is where the two meet. A frame at 24 into 48 kHz is
//! 2000 samples exactly, and at 30000/1001 — the commonest rate in television
//! — it is 1601.6, which is not a number of samples. No frame at 29.97 holds a
//! whole number of samples and none ever will.
//!
//! So a frame's samples are not counted, they are *bounded*: the samples of
//! frame `n` are those from [`media_editor_core::Instant::floor_into`] of `n` up
//! to the same of `n + 1`. Each frame therefore holds 1601 samples or 1602,
//! each frame's end is the next frame's beginning, and over any span the
//! counts sum to exactly the samples in that span. Nothing drifts, nothing is
//! dropped, and nothing is played twice — which is what the alternative,
//! rounding to nearest, cannot promise at every boundary.
//!
//! Each track mixes at its own fader. The fader is a property of the *project*
//! — it is set by an edit, it has an inverse, it undoes, and it is saved — so
//! this function reads it rather than taking it as a parameter. A mix level
//! that lived only in a function call would be a mix nobody could save.
//!
//! The model stores decibels and the mixer wants a factor, and the conversion
//! happens here because this is the only place that sees both. A muted track
//! contributes nothing at all, which is not the same as a track turned all the
//! way down: the logarithm of zero is not a point on the scale.

use alloc::vec::Vec;

use media_editor_audio::mix::Source;
use media_editor_audio::{AudioBuffer, Gain, MixReport, SampleRate};
use media_editor_core::{Instant, Rational, TimeRange};
use media_editor_model::{Lane, Project, SequenceId};

use crate::SlateStatus;

pub use media_editor_audio::SampleSource;

/// The mixer's gain for a fader position.
fn gain_of(fader: media_editor_model::Fader) -> Result<Gain, SlateStatus> {
    match fader.decibels() {
        None => Ok(Gain::SILENT),
        Some(decibels) => Gain::decibels(decibels).map_err(SlateStatus::Audio),
    }
}

/// Which sample a timeline instant falls in.
///
/// The timebase a rate counts in used to be a table here. It is
/// [`SampleRate::timebase`] now, because the reel format needs the same answer
/// to say how many samples a run of frames holds — and two tables that must
/// agree are one table waiting to disagree.
fn sample_of(instant: Instant, rate: SampleRate) -> Result<i64, SlateStatus> {
    Ok(instant.floor_into(rate.timebase())?.ticks())
}

/// Mix a sequence's sound tracks over a span.
///
/// The result holds exactly the samples the span covers, which is
/// `sample_of(end) - sample_of(start)` and is *not* the frame count times
/// anything.
///
/// ## Why this takes a project and not only a sequence
///
/// It did take only a sequence, and that was possible because the layer handed
/// a [`media_editor_model::MediaId`] straight to the source and nothing here ever
/// had to know what the media *was*. A [`SampleSource`] is asked for a content
/// digest now, for the reason that trait's own comment gives, and turning an
/// identifier into a digest means reading the project's table — which is what
/// [`crate::timeline::plan`] has always done on the picture side.
///
/// So the asymmetry is gone rather than papered over. Both halves of rendering
/// a sequence take the project, both name media by what it is, and both can
/// therefore read from a vault that several projects share.
///
/// # Errors
///
/// [`SlateStatus::Model`] if the range is not in the sequence's timebase or
/// the sequence is not in the project, [`SlateStatus::Audio`] if the sources
/// do not agree with each other or with what was asked for, and whatever a
/// source refuses.
pub fn mix(
    project: &Project,
    sequence: SequenceId,
    range: TimeRange,
    rate: SampleRate,
    channels: usize,
    source: &mut dyn SampleSource,
) -> Result<(AudioBuffer, MixReport), SlateStatus> {
    let held = project.sequence(sequence)?;
    let start = sample_of(range.start(), rate)?;
    let end = sample_of(
        range.end().map_err(media_editor_model::ModelStatus::Time)?,
        rate,
    )?;
    let total = usize::try_from(end - start)
        .map_err(|_| SlateStatus::Audio(media_editor_audio::AudioStatus::BufferTooLong))?;

    let mut written: Vec<Vec<i32>> = Vec::new();
    written
        .try_reserve(channels)
        .map_err(|_| SlateStatus::Audio(media_editor_audio::AudioStatus::OutOfMemory))?;
    for _ in 0..channels {
        let mut channel = Vec::new();
        channel
            .try_reserve(total)
            .map_err(|_| SlateStatus::Audio(media_editor_audio::AudioStatus::OutOfMemory))?;
        written.push(channel);
    }
    let mut report = MixReport::default();

    // Frame by frame, because that is the granularity at which the stack can
    // change: an item boundary is on a frame, so within one frame the set of
    // layers is fixed and their sample spans are contiguous.
    let mut frame = range.start();
    let end_instant = range.end().map_err(media_editor_model::ModelStatus::Time)?;
    while frame.ticks() < end_instant.ticks() {
        let next = Instant::new(
            frame.ticks().checked_add(1).ok_or(SlateStatus::Model(
                media_editor_model::ModelStatus::Time(media_editor_core::CoreStatus::Overflow),
            ))?,
            frame.timebase(),
        );
        let span = sample_of(next, rate)? - sample_of(frame, rate)?;
        let count = usize::try_from(span)
            .map_err(|_| SlateStatus::Audio(media_editor_audio::AudioStatus::BufferTooLong))?;

        let block = frame_block(
            project,
            held,
            Block {
                from: frame,
                to: next,
                rate,
                channels,
                count,
            },
            source,
            &mut report,
        )?;
        for (channel, samples) in written.iter_mut().enumerate() {
            let held = block.channel(channel).map_err(SlateStatus::Audio)?;
            samples
                .try_reserve(held.len())
                .map_err(|_| SlateStatus::Audio(media_editor_audio::AudioStatus::OutOfMemory))?;
            samples.extend_from_slice(held);
        }
        frame = next;
    }

    // The blocks are supposed to tile the span exactly — that is the whole
    // claim of the floor. Checking it here costs one comparison and turns a
    // silent disagreement into a named refusal: a buffer that is a sample
    // short is a click at the end of every export, and it would otherwise
    // arrive as a perfectly valid buffer of the wrong length.
    if written.first().map(Vec::len) != Some(total) {
        return Err(SlateStatus::Audio(
            media_editor_audio::AudioStatus::NotMixable,
        ));
    }

    Ok((
        AudioBuffer::new(rate, written).map_err(SlateStatus::Audio)?,
        report,
    ))
}

/// One frame of the mix: the span it covers and the buffer it must produce.
///
/// The two instants are this frame and the next, not this frame and a length.
/// A fader is read at both, and the second is by construction the *first*
/// instant of the following block — so a move that crosses a frame boundary
/// is handed to both blocks as the same number and they join without a step.
#[derive(Clone, Copy)]
struct Block {
    /// The instant this frame begins.
    from: Instant,
    /// The instant the next frame begins.
    to: Instant,
    /// What rate the samples are counted at.
    rate: SampleRate,
    /// How many channels to produce.
    channels: usize,
    /// How many samples this frame holds — 1601 or 1602 at 29.97, never both.
    count: usize,
}

/// One frame's worth of samples, with every layer on it summed.
fn frame_block(
    project: &Project,
    sequence: &media_editor_model::Sequence,
    block: Block,
    source: &mut dyn SampleSource,
    report: &mut MixReport,
) -> Result<AudioBuffer, SlateStatus> {
    let Block {
        from: frame,
        to: next,
        rate,
        channels,
        count,
    } = block;
    let stack = sequence.stack_at(Lane::Sound, frame)?;
    let silence = AudioBuffer::silence(rate, channels, count).map_err(SlateStatus::Audio)?;
    if stack.is_empty() {
        // Nothing on any track is silence, not a hole. A programme with no
        // sound at an instant is a programme with silence there, and an export
        // writes it.
        return Ok(silence);
    }

    let mut held = Vec::new();
    held.try_reserve(stack.len())
        .map_err(|_| SlateStatus::Audio(media_editor_audio::AudioStatus::OutOfMemory))?;
    let mut gains = Vec::new();
    gains
        .try_reserve(stack.len())
        .map_err(|_| SlateStatus::Audio(media_editor_audio::AudioStatus::OutOfMemory))?;
    for layer in &stack {
        let track = sequence.track(layer.track())?;
        // Both ends of the block, because a fader may be moving across it. The
        // second is the *next* frame's position, which is the first position
        // of the next block — so the ramps tile the move exactly, for the same
        // reason the sample counts tile the span.
        let gain = gain_of(track.fader_at(frame)?)?;
        let arriving = gain_of(track.fader_at(next)?)?;
        if gain.is_silent() {
            // A muted track is not decoded. There is nothing to fetch and
            // nothing a fetch could contribute, and reading media for a track
            // nobody will hear is the difference between a mix that keeps up
            // and one that does not.
            //
            // One end is enough, and only because mute cannot change within a
            // block: it is a switch on the track rather than a position a
            // curve can reach, so a track is muted for the whole mix or none
            // of it. If mute ever gets automation of its own — a separate lane
            // of its own, as a console has — this becomes a two-ended question
            // and skipping the fetch on the strength of the first sample would
            // put a hole at the start of every unmute.
            continue;
        }
        gains.push((gain, arriving));
        // Where the clip's own fade is at each end of the block. The fader is
        // a gain in decibels and this is a fraction of the material, and they
        // are two different things multiplied together -- so the fade scales
        // the samples and the fader scales the source, rather than one of them
        // being converted into the other's units.
        let fading = (layer.fade(), layer.fade_arriving());
        // The layer's source position is a timeline tick of the media, so it
        // goes through the same floor: a clip that starts on a frame boundary
        // starts on the sample that frame boundary falls in, and two clips of
        // the same material at the same offset ask for the same samples.
        let begins = sample_of(Instant::new(layer.source(), frame.timebase()), rate)?;
        // The identifier resolved into a digest here, where the project is,
        // rather than by whatever is on the other side of the trait. A source
        // reading from a shared vault has no table to consult.
        let digest = project.media().get(layer.media())?.digest();
        let block = source
            .samples(digest, begins, count)
            .map_err(SlateStatus::Audio)?;
        if block.rate() != rate || block.channel_count() != channels || block.len() != count {
            // A source that answers with a different span has answered a
            // different question, and resampling or padding it here would be a
            // decision made in the wrong place (R-1.3).
            return Err(SlateStatus::Audio(
                media_editor_audio::AudioStatus::NotMixable,
            ));
        }
        held.push(if fading.0 == Rational::ONE && fading.1 == Rational::ONE {
            // A clip nobody has faded is not copied. Scaling every sample by
            // one is exact and is still a pass over the whole block.
            block
        } else {
            block
                .faded(fading.0, fading.1)
                .map_err(SlateStatus::Audio)?
        });
    }

    if held.is_empty() {
        // Every track that reaches here was muted, which is silence — the same
        // answer as no tracks at all.
        return Ok(silence);
    }

    let sources: Vec<Source<'_>> = held
        .iter()
        .zip(gains.iter())
        .map(|(buffer, (gain, arriving))| {
            if gain == arriving {
                // A fader that is not moving stays a copy at unity and a
                // nought at silence, which the ramp cannot promise.
                Source::new(buffer, *gain)
            } else {
                Source::ramped(buffer, *gain, *arriving)
            }
        })
        .collect();
    let (mixed, block_report) = media_editor_audio::mix(&sources).map_err(SlateStatus::Audio)?;
    report.clipped = report.clipped.saturating_add(block_report.clipped);
    report.overshoot = report.overshoot.max(block_report.overshoot);
    Ok(mixed)
}
