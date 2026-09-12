// SPDX-License-Identifier: GPL-3.0-only
//! `SPRW`: Media Editor's uncompressed mezzanine.
//!
//! ```text
//! offset  size  field
//! 0       4     magic, "SPRW"
//! 4       2     format version, little-endian
//! 6       2     reserved, must be zero
//! 8       4     width
//! 12      4     height
//! 16      1     pixel format
//! 17      1     colour primaries
//! 18      1     transfer function
//! 19      1     matrix coefficients
//! 20      1     range
//! 21      1     chroma siting, or 0 for none
//! 22      1     alpha association, or 0 for a format with no alpha
//! 23      1     reserved, must be zero
//! 24      8     pixel aspect numerator
//! 32      8     pixel aspect denominator
//! 40      8     timebase numerator
//! 48      8     timebase denominator
//! 56      8     frame count
//! 64      1     sample rate tag, or 0 for a reel with no sound
//! 65      1     channel count, or 0 for a reel with no sound
//! 66      6     reserved, must be zero
//! 72      8     sample count, per channel
//! 80      4     caption count
//! 84      4     how many bytes the captions occupy
//! 88      N     the frames, tightly packed, in order
//! 88+N    M     the samples, interleaved across channels, 32-bit little-endian
//! 88+N+M  C     the captions, in order: in point, out point, voice, text
//! 88+N+M+C 32   SHA-256 of bytes 0..88 followed by the payload
//! ```
//!
//! The digest covers the header as well as the samples. It has to: a flipped
//! bit in the transfer function tag would otherwise turn every frame in the
//! file into a different picture, silently, and a digest that only covers the
//! samples would report the file as sound. The single-byte sweep in the tests
//! is what found that, which is what a sweep is for.
//!
//! ## Why the digest is at the end
//!
//! It was at offset 64, in the header, until version three. That is the
//! obvious place and it has one fatal property: **the first byte of the file
//! cannot be written until the last byte of the payload is known.** Version
//! two's own `encode` says so — it built the whole payload, hashed it, and
//! only then assembled a file — which is a strange thing for a format whose
//! entire purpose is to be what a recorder writes.
//!
//! With the digest at the end, a reel is written in one pass, forwards, and
//! the writer needs nothing from storage but the ability to *extend* a file.
//! That is a strictly weaker capability than writing at an offset, and a
//! writer that cannot seek backwards is a writer that cannot corrupt what it
//! has already written. It is why every streaming container ever designed puts
//! its checksum in a trailer, and this one had it in the wrong place.
//!
//! It also makes a torn write **structurally** detectable. With the digest in
//! the header, a reel cut short is a valid-looking file that only a full
//! rehash — five hundred and twelve mebibytes of it, at the limit — proves
//! wrong. With the digest at the end, the file is the wrong length and
//! [`Spool::open`] refuses it after reading sixty-four bytes.
//!
//! Nothing changed about what the digest *means*: it is the same hash over the
//! same bytes in the same order. Only where the thirty-two bytes live. A file
//! written by version two and one written by version three are the same
//! length, and differ by those thirty-two bytes being in a different place.
//!
//! No entropy coding at all. That is the point: the whole pipeline — read,
//! describe, cache, present, render, write — can be proved correct before a
//! single codec exists, and when one does exist it is measured against this
//! rather than against itself.
//!
//! Every frame in one file shares one description. A file whose frames change
//! shape halfway through is a sequence of files, and pretending otherwise is
//! how a decoder ends up guessing.
//!
//! ## Sound comes after the pictures, and is not interleaved with them
//!
//! Every container built for *delivery* interleaves: a frame, then the sound
//! that goes with it, so a player reading forwards has both. This one does
//! not, and the reason is the property everything here rests on — frame `k`
//! lives at `HEADER_BYTES + k × packed_bytes`, which is arithmetic rather than
//! a search.
//!
//! Interleaving kills it. A frame at 30000/1001 into 48 kHz covers **1601.6**
//! samples, so no frame holds a whole number of them and the sound between two
//! pictures is 1601 samples or 1602. The offset of picture `k` would then
//! depend on how many samples happened to precede it, and finding a frame
//! would mean walking the file. Segregating the two keeps both offsets exact:
//! picture `k` where it always was, and the samples in one run after them.
//!
//! That makes this a working file read by seeking rather than a stream played
//! forwards, which is what a mezzanine is for. It also costs nothing to write:
//! both halves are still written in one pass, forwards.
//!
//! The samples are **interleaved across channels** — left, right, left, right
//! — where the pictures are one plane. The two look like opposite decisions
//! and are the same one: a streaming writer needs its bytes in the order they
//! are produced. Rows arrive one plane at a time, so a reel is one plane;
//! sound arrives a block at a time across every channel at once, so a reel is
//! interleaved. A planar sound section would need the whole take held before
//! the first byte, which is exactly what [`Winder`] refuses planar pictures
//! for.
//!
//! ## The transcript is scanned, not indexed
//!
//! A caption is a variable-length record — an in point, an out point, a voice
//! and text — so there is no stride and no `k`-th caption at a computed
//! offset. That is the opposite of everything else in this format, and it is
//! the right trade here: a caption is 21 bytes plus its text, so a thousand of
//! them is about thirty kilobytes, read in windows, holding **only the ones
//! that overlap** the stretch asked for.
//!
//! An index would cost eight bytes a caption and buy nothing, because a
//! projection never wants caption `k` — it wants every caption over a range,
//! and finding those means looking at all of them whatever the layout. The
//! header carries the byte count so that [`Spool::open`] can still check the
//! file's length by arithmetic without reading a word of it.
//!
//! ## How long a reel's sound is, and why the format cannot say exactly
//!
//! A reel of `n` frames holds `⌊n·r⌋` or `⌈n·r⌉` samples, where `r` is samples
//! per frame — and **which of the two depends on where in a timeline the take
//! was cut from**, because the samples of a span are
//! `⌊(s+n)·r⌋ − ⌊s·r⌋` and that varies with `s`. A reel does not record where
//! it came from, so it records the count and the reader checks the *bound*.
//!
//! The bound is exact and it is not a fudge: three frames at 30000/1001 into
//! 48 kHz hold 4804 samples or 4805, and never 4803 or 4806. A take whose
//! sound is a second short of its pictures is refused, which is the failure
//! that matters; a take that is one sample either side of the average is the
//! arithmetic being correct.

use alloc::vec::Vec;

use media_editor_audio::{AudioBuffer, SampleRate};
use media_editor_core::{Digest, Rational, Timebase};
use media_editor_media::colour::{
    AlphaState, ChromaSiting, ColourDescription, MatrixCoefficients, Primaries, Range,
    TransferFunction,
};
use media_editor_media::{Frame, FrameDescription, Geometry, PixelFormat};
use media_editor_model::caption::Caption;

use crate::bytes::{Extent, Reader, Sink, Writer};
use crate::status::{IoStatus, Result};

/// The four bytes every reel begins with.
pub const MAGIC: [u8; 4] = *b"SPRW";

/// The format this build writes.
///
/// Two, because version one had no field for the alpha association, and a
/// frame that does not say whether its colour has been multiplied by its alpha
/// is exactly the untagged frame this project refuses elsewhere.
///
/// Three, because the digest moved to a trailer so that a reel can be written
/// forwards — see the module comment for the argument.
///
/// Four, because a reel carries sound.
///
/// Five, because a reel carries the words that were said. Nothing was ever
/// released that wrote any earlier version, so nothing is being broken; the
/// number moves anyway, because changing what a version means is the one thing
/// a versioned format may never do (R-1.2). An earlier file is refused by name
/// rather than read: its bytes 80..88 are a picture where this version expects
/// a transcript, and a reader that guessed would be guessing about somebody's
/// material.
pub const FORMAT_VERSION: u16 = 5;

/// How long the fixed header is.
///
/// Eighty-eight. It was ninety-six while the digest lived in it, sixty-four
/// once that moved to a trailer, eighty when sixteen bytes came to describe
/// the sound, and eighty-eight now that eight more describe the transcript: a
/// count, and how many bytes it occupies. There
/// was a second constant here — `DESCRIBED_BYTES`, "how much of the header the
/// digest covers" — and it went with the digest: the header is described in
/// full, and one number does what two did.
pub const HEADER_BYTES: usize = 88;

/// How long the trailer is: one digest, and nothing else.
pub const TRAILER_BYTES: usize = 32;

/// The digest a reel carries: its description and its samples together.
fn digest_of(head: &[u8], payload: &[u8]) -> Digest {
    let mut hasher = media_editor_core::Sha256::new();
    hasher.update(head);
    hasher.update(payload);
    hasher.finish()
}

/// The most frames one reel may hold.
///
/// Twenty-four thousand: about sixteen minutes at 24 frames a second, which is
/// a generous single take and a small fraction of what a compressed format
/// would hold. A reel is a working file, not an archive.
pub const MAX_FRAMES: usize = 24_000;

/// The most bytes one reel may occupy.
pub const MAX_REEL_BYTES: usize = 512 * 1024 * 1024;

/// How many bytes one sample of one channel occupies.
pub const SAMPLE_BYTES: usize = 4;

/// What a reel's sound is: a rate, a channel count, and a length.
///
/// A length as well as a shape, because the length is not derivable. See the
/// module comment: a reel of `n` frames holds `⌊n·r⌋` or `⌈n·r⌉` samples and
/// which one depends on where the take was cut from, which a reel does not
/// record. So it is written down, and [`sound_bounds`] is what checks it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Sound {
    rate: SampleRate,
    channels: usize,
    samples: usize,
}

impl Sound {
    /// Describe a reel's sound.
    ///
    /// # Errors
    ///
    /// [`IoStatus::Sound`] for no channels or more than
    /// [`media_editor_audio::MAX_CHANNELS`], or for more than
    /// [`media_editor_audio::MAX_SAMPLES`].
    pub fn new(rate: SampleRate, channels: usize, samples: usize) -> Result<Self> {
        if channels == 0 || channels > media_editor_audio::MAX_CHANNELS {
            return Err(IoStatus::Sound(
                media_editor_audio::AudioStatus::ChannelCountUnsupported,
            ));
        }
        if samples > media_editor_audio::MAX_SAMPLES {
            return Err(IoStatus::Sound(
                media_editor_audio::AudioStatus::BufferTooLong,
            ));
        }
        Ok(Self {
            rate,
            channels,
            samples,
        })
    }

    /// The rate the samples are counted at.
    #[must_use]
    pub const fn rate(self) -> SampleRate {
        self.rate
    }

    /// How many channels.
    #[must_use]
    pub const fn channels(self) -> usize {
        self.channels
    }

    /// How many samples, per channel.
    #[must_use]
    pub const fn samples(self) -> usize {
        self.samples
    }

    /// How many bytes the sound section occupies.
    ///
    /// # Errors
    ///
    /// [`IoStatus::PayloadTooLarge`] if the product does not fit.
    pub fn bytes(self) -> Result<usize> {
        self.samples
            .checked_mul(self.channels)
            .and_then(|total| total.checked_mul(SAMPLE_BYTES))
            .ok_or(IoStatus::PayloadTooLarge)
    }

    /// Whether this many samples is a length `frames` frames could hold.
    ///
    /// # Errors
    ///
    /// As [`sound_bounds`].
    pub fn fits(self, frames: usize, timebase: Timebase) -> Result<bool> {
        let (least, most) = sound_bounds(frames, timebase, self.rate)?;
        Ok(self.samples >= least && self.samples <= most)
    }
}

/// The two sample counts a run of frames can hold.
///
/// `⌊n·r⌋` and `⌈n·r⌉`, computed as integers from the definition — `n` frames
/// is `n × den / num` seconds at a rate of `num/den` frames a second, which is
/// `n × den × hertz / num` samples.
///
/// The floor of it is what [`media_editor_core::Instant::floor_into`] gives for
/// frame `n` counted from nought, and a test requires the two to agree: this
/// arithmetic and the mixer's must never differ, because the mixer is what
/// produces the samples this bound is checked against.
///
/// # Errors
///
/// [`IoStatus::TooMany`] if the arithmetic does not fit, or if a timebase's
/// rate is not positive — which no timebase's is.
pub fn sound_bounds(frames: usize, timebase: Timebase, rate: SampleRate) -> Result<(usize, usize)> {
    let frames = i64::try_from(frames).map_err(|_| IoStatus::TooMany)?;
    let per_second = timebase.rate();
    let numerator = per_second.numerator();
    let denominator = per_second.denominator();
    if numerator <= 0 {
        return Err(IoStatus::TooMany);
    }
    let scaled = frames
        .checked_mul(denominator)
        .and_then(|value| value.checked_mul(i64::from(rate.hertz())))
        .ok_or(IoStatus::TooMany)?;
    let least = scaled / numerator;
    // Ceiling by adding one less than the divisor, which is exact for
    // non-negative numerators and is why the guard above exists.
    let most = scaled.checked_add(numerator - 1).ok_or(IoStatus::TooMany)? / numerator;
    Ok((
        usize::try_from(least).map_err(|_| IoStatus::TooMany)?,
        usize::try_from(most).map_err(|_| IoStatus::TooMany)?,
    ))
}

/// How many captions one reel may carry.
///
/// Sixteen thousand, against sixty-four in a project's own media table — and
/// the difference between those two numbers is the whole reason this section
/// exists. A project file is read in one piece, so sixty-four captions of an
/// asset is already 45% of what a Phipia program is mapped. A reel is read a
/// window at a time, so the count is bounded by what a take could plausibly
/// hold rather than by what fits in memory: sixteen thousand captions is about
/// twenty hours of speech, and a reel is bounded at sixteen minutes.
pub const MAX_CAPTIONS: usize = 16_384;

/// How many bytes one reel's captions may occupy.
///
/// A caption is 21 bytes plus its text and the text is bounded at 128
/// characters, so 512 bytes of UTF-8 in the worst case — 533 each, and
/// [`MAX_CAPTIONS`] of those is 8,732,672. Bounded separately from the count
/// because a reader has to know how far the section reaches before it reads a
/// word of it, and because a file claiming sixteen thousand short captions and
/// a section of eight megabytes is a file that disagrees with itself.
pub const MAX_CAPTION_BYTES: usize = 8_732_672;

/// What a reel's transcript is: how many captions, and how many bytes.
///
/// A pair rather than the captions themselves, because a [`Winder`] declares
/// this in the header before it writes a word and then takes the captions one
/// at a time. Sixteen thousand captions is 8.7 megabytes against the
/// seventy-six kilobytes a Phipia program is mapped — **114 times** what there
/// is — so a writer that held the transcript to write it would be the thing
/// this whole file exists to avoid, in a third place.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Spoken {
    count: usize,
    bytes: usize,
}

impl Spoken {
    /// Describe a transcript.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TooMany`] past [`MAX_CAPTIONS`],
    /// [`IoStatus::PayloadTooLarge`] past [`MAX_CAPTION_BYTES`], and
    /// [`IoStatus::TranscriptNotDeclared`] for a count and a length that
    /// cannot both be true.
    pub fn new(count: usize, bytes: usize) -> Result<Self> {
        if count > MAX_CAPTIONS {
            return Err(IoStatus::TooMany);
        }
        if bytes > MAX_CAPTION_BYTES {
            return Err(IoStatus::PayloadTooLarge);
        }
        if (count == 0) != (bytes == 0)
            || bytes
                < count
                    .checked_mul(CAPTION_FIXED)
                    .ok_or(IoStatus::PayloadTooLarge)?
        {
            return Err(IoStatus::TranscriptNotDeclared);
        }
        Ok(Self { count, bytes })
    }

    /// What a set of captions comes to.
    ///
    /// # Errors
    ///
    /// As [`Spoken::new`].
    pub fn of(captions: &[Caption]) -> Result<Self> {
        Self::new(captions.len(), caption_bytes(captions)?)
    }

    /// How many captions.
    #[must_use]
    pub const fn count(self) -> usize {
        self.count
    }

    /// How many bytes they occupy.
    #[must_use]
    pub const fn bytes(self) -> usize {
        self.bytes
    }
}

/// A run of frames that share one description and one rate.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Reel {
    description: FrameDescription,
    timebase: Timebase,
    frames: Vec<Frame>,
    sound: Option<AudioBuffer>,
    captions: Vec<Caption>,
}

impl Reel {
    /// Gather frames into a reel.
    ///
    /// # Errors
    ///
    /// [`IoStatus::ReelDescriptionMismatch`] if any frame is described
    /// differently from the first, or [`IoStatus::TooMany`].
    pub fn new(timebase: Timebase, frames: Vec<Frame>) -> Result<Self> {
        let first = frames.first().ok_or(IoStatus::EmptyReel)?;
        let description = *first.description();
        for frame in &frames {
            if frame.description() != &description {
                return Err(IoStatus::ReelDescriptionMismatch);
            }
        }
        if frames.len() > MAX_FRAMES {
            return Err(IoStatus::TooMany);
        }
        Ok(Self {
            description,
            timebase,
            frames,
            sound: None,
            captions: Vec::new(),
        })
    }

    /// The same reel, carrying sound.
    ///
    /// # Errors
    ///
    /// [`IoStatus::SoundRunsPastPicture`] if the buffer is not a length this
    /// many frames could hold — see [`sound_bounds`] — or
    /// [`IoStatus::Sound`] if the buffer is one the format cannot describe.
    pub fn with_sound(mut self, buffer: AudioBuffer) -> Result<Self> {
        let sound = Sound::new(buffer.rate(), buffer.channel_count(), buffer.len())?;
        if !sound.fits(self.frames.len(), self.timebase)? {
            return Err(IoStatus::SoundRunsPastPicture);
        }
        self.sound = Some(buffer);
        Ok(self)
    }

    /// The same reel, carrying a transcript.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TooMany`] past [`MAX_CAPTIONS`],
    /// [`IoStatus::PayloadTooLarge`] past [`MAX_CAPTION_BYTES`], and whatever
    /// the model refuses about the set — which is two captions of one voice
    /// over one tick.
    pub fn with_captions(mut self, captions: Vec<Caption>) -> Result<Self> {
        if captions.len() > MAX_CAPTIONS {
            return Err(IoStatus::TooMany);
        }
        if caption_bytes(&captions)? > MAX_CAPTION_BYTES {
            return Err(IoStatus::PayloadTooLarge);
        }
        media_editor_model::caption::checked(&captions).map_err(IoStatus::Model)?;
        self.captions = captions;
        Ok(self)
    }

    /// The words said in this reel, in order.
    #[must_use]
    pub fn captions(&self) -> &[Caption] {
        &self.captions
    }

    /// The reel's sound, if it has any.
    #[must_use]
    pub const fn sound(&self) -> Option<&AudioBuffer> {
        self.sound.as_ref()
    }

    /// What this reel's sound is, if it has any.
    ///
    /// # Errors
    ///
    /// As [`Sound::new`].
    pub fn sound_description(&self) -> Result<Option<Sound>> {
        self.sound.as_ref().map_or(Ok(None), |buffer| {
            Sound::new(buffer.rate(), buffer.channel_count(), buffer.len()).map(Some)
        })
    }

    /// What every frame in this reel is.
    #[must_use]
    pub const fn description(&self) -> &FrameDescription {
        &self.description
    }

    /// The rate the frames are counted at.
    #[must_use]
    pub const fn timebase(&self) -> Timebase {
        self.timebase
    }

    /// The frames, in order.
    #[must_use]
    pub fn frames(&self) -> &[Frame] {
        &self.frames
    }

    /// How many frames.
    #[must_use]
    pub fn len(&self) -> usize {
        self.frames.len()
    }

    /// Whether the reel holds nothing. It never does; a reel with no frames
    /// cannot be built.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.frames.is_empty()
    }
}

/// Encode a reel.
///
/// # Errors
///
/// [`IoStatus::PayloadTooLarge`] or [`IoStatus::OutOfMemory`].
pub fn encode(reel: &Reel) -> Result<Vec<u8>> {
    let mut payload = Writer::new(MAX_REEL_BYTES);
    for frame in reel.frames() {
        payload.bytes(&frame.packed()?)?;
    }
    // The sound after every picture, which is where it lives -- see the module
    // comment for why it is not between them.
    if let Some(buffer) = reel.sound() {
        payload.bytes(&packed_sound(buffer)?)?;
    }
    // And the words last, after every picture and every sample -- the same
    // reason the sound is after the pictures: a section whose length varies
    // may not sit in front of one whose offsets are arithmetic.
    payload.bytes(&packed_captions(reel.captions())?)?;
    let payload = payload.finish();

    let head = header(
        *reel.description(),
        reel.timebase(),
        reel.len(),
        reel.sound_description()?,
        Spoken::of(reel.captions())?,
    )?;

    // Header, payload, digest -- in that order, which is now also the order
    // the bytes appear in the file. Version two wrote the digest second and
    // could not have done otherwise, since it lived in the header; that is the
    // whole reason the format moved it.
    let mut file = Writer::new(HEADER_BYTES + MAX_REEL_BYTES + TRAILER_BYTES);
    file.bytes(&head)?;
    file.bytes(&payload)?;
    file.bytes(digest_of(&head, &payload).bytes())?;
    Ok(file.finish())
}

/// The sixty-four bytes a reel begins with.
///
/// One door, on the writing side, to match the one [`read_header`] is on the
/// reading side. [`encode`] and [`Winder::begin`] both come through here, so a
/// reel wound a row at a time and a reel written whole cannot disagree about
/// what they claim to be — and a test requires them to be byte for byte the
/// same file.
///
/// # Errors
///
/// [`IoStatus::PayloadTooLarge`] if the fields do not fit, which they always
/// do.
fn header(
    description: FrameDescription,
    timebase: Timebase,
    count: usize,
    sound: Option<Sound>,
    spoken: Spoken,
) -> Result<Vec<u8>> {
    let geometry = description.geometry();
    let colour = description.colour();
    let mut head = Writer::new(HEADER_BYTES);
    head.bytes(&MAGIC)?;
    head.u16(FORMAT_VERSION)?;
    head.u16(0)?;
    head.u32(geometry.width())?;
    head.u32(geometry.height())?;
    head.u8(format_tag(description.format()))?;
    head.u8(primaries_tag(colour.primaries))?;
    head.u8(transfer_tag(colour.transfer))?;
    head.u8(matrix_tag(colour.matrix))?;
    head.u8(range_tag(colour.range))?;
    head.u8(siting_tag(description.siting()))?;
    head.u8(alpha_tag(description.alpha()))?;
    head.u8(0)?;
    head.i64(description.pixel_aspect().numerator())?;
    head.i64(description.pixel_aspect().denominator())?;
    head.i64(timebase.rate().numerator())?;
    head.i64(timebase.rate().denominator())?;
    head.u64(count as u64)?;
    // Nought for both tags where there is no sound, rather than a rate that
    // describes nothing. A reel with a rate and no channels would be a reel
    // that half means it.
    head.u8(sound.map_or(0, |sound| sound.rate().tag()))?;
    head.u8(u8::try_from(sound.map_or(0, Sound::channels)).map_err(|_| IoStatus::TooMany)?)?;
    for _ in 0..6 {
        head.u8(0)?;
    }
    head.u64(sound.map_or(0, Sound::samples) as u64)?;
    head.u32(u32::try_from(spoken.count()).map_err(|_| IoStatus::TooMany)?)?;
    head.u32(u32::try_from(spoken.bytes()).map_err(|_| IoStatus::TooMany)?)?;
    Ok(head.finish())
}

/// How many bytes a transcript occupies on disk.
///
/// The one place this is worked out, so the header, the length check and the
/// writer cannot disagree about it.
fn caption_bytes(captions: &[Caption]) -> Result<usize> {
    let mut total = 0_usize;
    for caption in captions {
        total = total
            .checked_add(CAPTION_FIXED)
            .and_then(|held| held.checked_add(caption.text().len()))
            .ok_or(IoStatus::PayloadTooLarge)?;
    }
    Ok(total)
}

/// What a caption costs before its text: two instants, a voice and a length.
const CAPTION_FIXED: usize = 8 + 8 + 1 + 4;

/// The transcript of a reel, as bytes.
fn packed_captions(captions: &[Caption]) -> Result<Vec<u8>> {
    let mut out = Writer::new(MAX_CAPTION_BYTES);
    for caption in captions {
        out.i64(caption.from())?;
        out.i64(caption.to())?;
        out.u8(caption.voice())?;
        let words = caption.text().as_bytes();
        out.u32(u32::try_from(words.len()).map_err(|_| IoStatus::TooMany)?)?;
        out.bytes(words)?;
    }
    Ok(out.finish())
}

/// How much of a transcript is read at once.
///
/// One page, which is the same window the export's verification uses and for
/// the same reason: the machine this is for maps nineteen pages in total.
pub const CAPTION_WINDOW: usize = 4096;

/// The largest one caption can be: its fixed fields and the longest text.
///
/// The bound on the *carry* between windows, and therefore on what this costs
/// beyond the window itself. A record cannot straddle more than one boundary
/// because it cannot be longer than this.
pub const CAPTION_LARGEST: usize =
    CAPTION_FIXED + media_editor_model::caption::MAX_CAPTION_TEXT * 4;

/// Take every whole caption from the front of a buffer.
///
/// Hands back how many **bytes** were consumed and how many captions were
/// read, so the caller can carry the remainder into the next window. Stops at
/// the first record the buffer does not hold entire, which is the whole point:
/// a partial caption is not a caption, and guessing at one would be reading a
/// length as a picture.
fn scan_captions(
    held: &[u8],
    remaining: usize,
    from: i64,
    to: i64,
    into: &mut Vec<Caption>,
) -> Result<(usize, usize)> {
    let mut taken = 0_usize;
    let mut matched = 0_usize;
    while matched < remaining {
        let rest = held.get(taken..).unwrap_or(&[]);
        if rest.len() < CAPTION_FIXED {
            break;
        }
        let mut reader = Reader::new(rest);
        let start = reader.i64()?;
        let end = reader.i64()?;
        let voice = reader.u8()?;
        let length = usize::try_from(reader.u32()?).map_err(|_| IoStatus::TooMany)?;
        if rest.len() < CAPTION_FIXED + length {
            break;
        }
        let words =
            core::str::from_utf8(reader.take(length)?).map_err(|_| IoStatus::CaptionNotText)?;
        if start < to && from < end {
            into.try_reserve(1).map_err(|_| IoStatus::OutOfMemory)?;
            into.push(Caption::new(start, end, voice, words).map_err(IoStatus::Model)?);
        }
        taken += CAPTION_FIXED + length;
        matched += 1;
    }
    Ok((taken, matched))
}

/// Read a transcript, keeping only what overlaps `[from, to)`.
///
/// **Scanned rather than indexed**, which is what the section is laid out for:
/// every record is looked at and only the matches are held, so a reel of a
/// thousand captions costs a thousand short reads and a handful of allocations
/// rather than a transcript in memory.
fn unpacked_captions(bytes: &[u8], count: usize, from: i64, to: i64) -> Result<Vec<Caption>> {
    let mut reader = Reader::new(bytes);
    let mut found = Vec::new();
    for _ in 0..count {
        let start = reader.i64()?;
        let end = reader.i64()?;
        let voice = reader.u8()?;
        let length = usize::try_from(reader.u32()?).map_err(|_| IoStatus::TooMany)?;
        let words =
            core::str::from_utf8(reader.take(length)?).map_err(|_| IoStatus::CaptionNotText)?;
        if start < to && from < end {
            found.try_reserve(1).map_err(|_| IoStatus::OutOfMemory)?;
            found.push(Caption::new(start, end, voice, words).map_err(IoStatus::Model)?);
        }
    }
    Ok(found)
}

/// The samples of a reel, interleaved, as bytes.
fn packed_sound(buffer: &AudioBuffer) -> Result<Vec<u8>> {
    let channels = buffer.channel_count();
    let mut out = Vec::new();
    out.try_reserve(
        buffer
            .len()
            .checked_mul(channels)
            .and_then(|total| total.checked_mul(SAMPLE_BYTES))
            .ok_or(IoStatus::PayloadTooLarge)?,
    )
    .map_err(|_| IoStatus::OutOfMemory)?;
    for index in 0..buffer.len() {
        for channel in 0..channels {
            let samples = buffer.channel(channel).map_err(IoStatus::Sound)?;
            let sample = samples.get(index).ok_or(IoStatus::TruncatedPayload)?;
            out.extend_from_slice(&sample.to_le_bytes());
        }
    }
    Ok(out)
}

/// The samples of a reel, from interleaved bytes.
fn unpacked_sound(bytes: &[u8], sound: Sound) -> Result<AudioBuffer> {
    if bytes.len() != sound.bytes()? {
        return Err(IoStatus::TruncatedPayload);
    }
    let mut channels: Vec<Vec<i32>> = Vec::new();
    channels
        .try_reserve(sound.channels())
        .map_err(|_| IoStatus::OutOfMemory)?;
    for _ in 0..sound.channels() {
        let mut channel = Vec::new();
        channel
            .try_reserve(sound.samples())
            .map_err(|_| IoStatus::OutOfMemory)?;
        channels.push(channel);
    }
    let mut reader = Reader::new(bytes);
    for _ in 0..sound.samples() {
        for channel in &mut channels {
            channel.push(reader.i32()?);
        }
    }
    AudioBuffer::new(sound.rate(), channels).map_err(IoStatus::Sound)
}

/// What a header says about a reel's sound.
fn read_sound(rate: u8, channels: u8, samples: u64) -> Result<Option<Sound>> {
    // Both nought or neither: a reel that named a rate and no channels, or
    // channels at no rate, is a reel whose header disagrees with itself.
    if rate == 0 && channels == 0 {
        if samples != 0 {
            return Err(IoStatus::SoundNotDeclared);
        }
        return Ok(None);
    }
    if rate == 0 || channels == 0 {
        return Err(IoStatus::SoundNotDeclared);
    }
    let rate = SampleRate::from_tag(rate).map_err(IoStatus::Sound)?;
    let samples = usize::try_from(samples).map_err(|_| IoStatus::TooMany)?;
    Sound::new(rate, usize::from(channels), samples).map(Some)
}

/// Decode a reel.
///
/// # Errors
///
/// Any [`IoStatus`]. Nothing is returned on a refusal.
pub fn decode(file: &[u8]) -> Result<Reel> {
    if file.len() < HEADER_BYTES {
        return Err(IoStatus::TruncatedHeader);
    }
    // One door. `Spool::open` reads the same header through the same function,
    // so a streaming reader cannot admit a file this one refuses -- which
    // would be a second, weaker way into one format.
    let bytes = <&[u8; HEADER_BYTES]>::try_from(&file[..HEADER_BYTES])
        .map_err(|_| IoStatus::TruncatedHeader)?;
    let (description, timebase, count, sound, spoken) = read_header(bytes)?;

    let frame_bytes = description.packed_bytes()?;
    let pictures = frame_bytes
        .checked_mul(count)
        .ok_or(IoStatus::PayloadTooLarge)?;
    let sounded = pictures
        .checked_add(sound.map_or(Ok(0), Sound::bytes)?)
        .ok_or(IoStatus::PayloadTooLarge)?;
    let required = sounded
        .checked_add(spoken.bytes())
        .ok_or(IoStatus::PayloadTooLarge)?;
    // The trailer first, because its absence is the cheap answer. A reel cut
    // short during a write ends inside its payload and has no trailer at all,
    // and saying so costs a subtraction rather than a rehash of everything
    // that did arrive.
    let after = required
        .checked_add(TRAILER_BYTES)
        .ok_or(IoStatus::PayloadTooLarge)?;
    let body = file.len() - HEADER_BYTES;
    if body < after {
        return Err(IoStatus::TruncatedPayload);
    }
    if body > after {
        return Err(IoStatus::TrailingBytes);
    }
    let payload = &file[HEADER_BYTES..HEADER_BYTES + required];
    let (sounded_part, words) = payload.split_at(sounded);
    let (pictures, samples) = sounded_part.split_at(pictures);
    let expected = Digest::new(
        file.get(HEADER_BYTES + required..)
            .and_then(|field| <[u8; 32]>::try_from(field).ok())
            .ok_or(IoStatus::TruncatedTrailer)?,
    );
    if digest_of(&file[..HEADER_BYTES], payload) != expected {
        return Err(IoStatus::DigestMismatch);
    }

    let mut frames = Vec::new();
    frames
        .try_reserve(count)
        .map_err(|_| IoStatus::OutOfMemory)?;
    for index in 0..count {
        let start = index * frame_bytes;
        let slice = pictures
            .get(start..start + frame_bytes)
            .ok_or(IoStatus::TruncatedPayload)?;
        frames.push(Frame::from_packed(description, slice)?);
    }
    let reel = Reel::new(timebase, frames)?;
    let reel = match sound {
        None => reel,
        Some(sound) => reel.with_sound(unpacked_sound(samples, sound)?)?,
    };
    // Every caption, which is what `decode` is for: the streaming reader is
    // where a range is asked for.
    reel.with_captions(unpacked_captions(
        words,
        spoken.count(),
        i64::MIN,
        i64::MAX,
    )?)
}

/// Tags as they appear in the file. Deliberately their own numbers: a frame's
/// digest must not change because this container renumbered something.
const fn format_tag(format: PixelFormat) -> u8 {
    match format {
        PixelFormat::Rgba8 => 1,
        PixelFormat::Rgb8 => 2,
        PixelFormat::Gray8 => 3,
        PixelFormat::Yuv420p8 => 4,
        PixelFormat::Yuv422p8 => 5,
        PixelFormat::Yuv444p8 => 6,
    }
}

fn read_format(tag: u8) -> Result<PixelFormat> {
    match tag {
        1 => Ok(PixelFormat::Rgba8),
        2 => Ok(PixelFormat::Rgb8),
        3 => Ok(PixelFormat::Gray8),
        4 => Ok(PixelFormat::Yuv420p8),
        5 => Ok(PixelFormat::Yuv422p8),
        6 => Ok(PixelFormat::Yuv444p8),
        other => Err(IoStatus::UnknownPixelFormat(other)),
    }
}

const fn primaries_tag(primaries: Primaries) -> u8 {
    match primaries {
        Primaries::Bt709 => 1,
        Primaries::Bt601Ntsc => 2,
        Primaries::Bt601Pal => 3,
        Primaries::Bt2020 => 4,
        Primaries::DciP3 => 5,
        Primaries::DisplayP3 => 6,
        Primaries::AcesAp0 => 7,
        Primaries::AcesAp1 => 8,
    }
}

fn read_primaries(tag: u8) -> Result<Primaries> {
    match tag {
        1 => Ok(Primaries::Bt709),
        2 => Ok(Primaries::Bt601Ntsc),
        3 => Ok(Primaries::Bt601Pal),
        4 => Ok(Primaries::Bt2020),
        5 => Ok(Primaries::DciP3),
        6 => Ok(Primaries::DisplayP3),
        7 => Ok(Primaries::AcesAp0),
        8 => Ok(Primaries::AcesAp1),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}

const fn transfer_tag(transfer: TransferFunction) -> u8 {
    match transfer {
        TransferFunction::Bt709 => 1,
        TransferFunction::Srgb => 2,
        TransferFunction::Bt2020Ten => 3,
        TransferFunction::PerceptualQuantiser => 4,
        TransferFunction::HybridLogGamma => 5,
        TransferFunction::Linear => 6,
        TransferFunction::Gamma22 => 7,
        TransferFunction::Gamma26 => 8,
    }
}

fn read_transfer(tag: u8) -> Result<TransferFunction> {
    match tag {
        1 => Ok(TransferFunction::Bt709),
        2 => Ok(TransferFunction::Srgb),
        3 => Ok(TransferFunction::Bt2020Ten),
        4 => Ok(TransferFunction::PerceptualQuantiser),
        5 => Ok(TransferFunction::HybridLogGamma),
        6 => Ok(TransferFunction::Linear),
        7 => Ok(TransferFunction::Gamma22),
        8 => Ok(TransferFunction::Gamma26),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}

const fn matrix_tag(matrix: MatrixCoefficients) -> u8 {
    match matrix {
        MatrixCoefficients::Identity => 1,
        MatrixCoefficients::Bt709 => 2,
        MatrixCoefficients::Bt601 => 3,
        MatrixCoefficients::Bt2020NonConstant => 4,
    }
}

fn read_matrix(tag: u8) -> Result<MatrixCoefficients> {
    match tag {
        1 => Ok(MatrixCoefficients::Identity),
        2 => Ok(MatrixCoefficients::Bt709),
        3 => Ok(MatrixCoefficients::Bt601),
        4 => Ok(MatrixCoefficients::Bt2020NonConstant),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}

const fn range_tag(range: Range) -> u8 {
    match range {
        Range::Limited => 1,
        Range::Full => 2,
    }
}

fn read_range(tag: u8) -> Result<Range> {
    match tag {
        1 => Ok(Range::Limited),
        2 => Ok(Range::Full),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}

const fn alpha_tag(alpha: Option<AlphaState>) -> u8 {
    match alpha {
        None => 0,
        Some(AlphaState::Straight) => 1,
        Some(AlphaState::Premultiplied) => 2,
    }
}

fn read_alpha(tag: u8) -> Result<Option<AlphaState>> {
    match tag {
        0 => Ok(None),
        1 => Ok(Some(AlphaState::Straight)),
        2 => Ok(Some(AlphaState::Premultiplied)),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}

const fn siting_tag(siting: Option<ChromaSiting>) -> u8 {
    match siting {
        None => 0,
        Some(ChromaSiting::Left) => 1,
        Some(ChromaSiting::Centre) => 2,
        Some(ChromaSiting::TopLeft) => 3,
    }
}

fn read_siting(tag: u8) -> Result<Option<ChromaSiting>> {
    match tag {
        0 => Ok(None),
        1 => Ok(Some(ChromaSiting::Left)),
        2 => Ok(Some(ChromaSiting::Centre)),
        3 => Ok(Some(ChromaSiting::TopLeft)),
        other => Err(IoStatus::UnknownColourTag(other)),
    }
}

/// A reel read a frame at a time, without being loaded.
///
/// [`decode`] builds every frame at once. A reel this build writes is bounded
/// at [`MAX_REEL_BYTES`] — five hundred and twelve mebibytes — against the
/// seventy-six kilobytes a Phipia program is mapped, which is **6,899 times**
/// what there is. It is not a tight fit or a thing to make smaller; it is a
/// function that cannot be called on the machine this program is for.
///
/// A spool holds a description, a rate and a count. **Nothing else.** Frame
/// `k` lives at `HEADER_BYTES + k × packed_bytes`, which is arithmetic rather
/// than a search, because every frame in a reel shares one description and is
/// therefore one size — the property the format's own module comment argues
/// for on other grounds, cashed here.
///
/// ## Where the ceiling goes
///
/// Streaming takes the *reel* out of the arithmetic and leaves the *frame*.
/// [`Spool::frame`] allocates one, and one frame of 1920×1080 eight-bit RGB is
/// 6,220,800 bytes — eighty times what a Phipia program is mapped. So this is
/// not the end of the story, and saying otherwise would be the kind of claim
/// the platform contract exists to prevent. What it changes is which number
/// bounds the cost: the picture's size rather than the material's length.
///
/// [`Spool::plane_row`] is the end of the story, for a reader that needs one:
/// a row of a 1920-wide RGB picture is 5,760 bytes, which is seven per cent of
/// a Phipia program's whole address space rather than eighty times it. It is what Phipia's own
/// bitmap reader does — "random row reads through the normal filesystem and
/// NVMe paths" — arrived at from the other side.
///
/// ## What `open` does not do
///
/// **It does not check the digest.** A reel's digest covers its description
/// and every sample, so checking it means reading everything, which is what a
/// spool exists to avoid. [`Spool::verify`] is there for a caller that can
/// afford it, and reads in windows so it can afford it on a small machine.
/// The same trade [`crate::vault::Catalogue`] makes, named the same way.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Spool {
    description: FrameDescription,
    timebase: Timebase,
    count: usize,
    frame_bytes: usize,
    sound: Option<Sound>,
    spoken: Spoken,
}

impl Spool {
    /// Read a reel's header, and nothing else.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedHeader`], [`IoStatus::NotAReel`],
    /// [`IoStatus::UnsupportedVersion`], [`IoStatus::ReservedFieldSet`],
    /// [`IoStatus::EmptyReel`], [`IoStatus::TooMany`] past [`MAX_FRAMES`],
    /// [`IoStatus::TruncatedPayload`] or [`IoStatus::TrailingBytes`] if the
    /// extent is not the length the header describes, and whatever the
    /// description itself refuses.
    pub fn open(extent: &dyn Extent) -> Result<Self> {
        let mut bytes = [0_u8; HEADER_BYTES];
        if extent.read_at(0, &mut bytes)? != HEADER_BYTES {
            return Err(IoStatus::TruncatedHeader);
        }
        let (description, timebase, count, sound, spoken) = read_header(&bytes)?;
        let frame_bytes = description.packed_bytes()?;
        let payload = frame_bytes
            .checked_mul(count)
            .and_then(|pictures| pictures.checked_add(sound.map_or(0, |s| s.bytes().unwrap_or(0))))
            .and_then(|sounded| sounded.checked_add(spoken.bytes()))
            .ok_or(IoStatus::PayloadTooLarge)?;
        let stated = u64::try_from(
            HEADER_BYTES
                .checked_add(payload)
                .and_then(|body| body.checked_add(TRAILER_BYTES))
                .ok_or(IoStatus::TooMany)?,
        )
        .map_err(|_| IoStatus::TooMany)?;
        if extent.length() < stated {
            return Err(IoStatus::TruncatedPayload);
        }
        if extent.length() > stated {
            return Err(IoStatus::TrailingBytes);
        }
        Ok(Self {
            description,
            timebase,
            count,
            frame_bytes,
            sound,
            spoken,
        })
    }

    /// What every frame in this reel is.
    #[must_use]
    pub const fn description(&self) -> &FrameDescription {
        &self.description
    }

    /// The rate the frames are counted at.
    #[must_use]
    pub const fn timebase(&self) -> Timebase {
        self.timebase
    }

    /// How many frames.
    #[must_use]
    pub const fn len(&self) -> usize {
        self.count
    }

    /// Whether the reel holds nothing. It never does; a reel with no frames
    /// cannot be written and is refused when read.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// How many bytes one frame occupies.
    #[must_use]
    pub const fn frame_bytes(&self) -> usize {
        self.frame_bytes
    }

    /// One frame, by position.
    ///
    /// # Errors
    ///
    /// [`IoStatus::FrameOutOfReel`] for a position past the count,
    /// [`IoStatus::TruncatedPayload`] if the extent ends inside it,
    /// [`IoStatus::OutOfMemory`], and whatever the frame itself refuses.
    pub fn frame(&self, extent: &dyn Extent, index: usize) -> Result<Frame> {
        if index >= self.count {
            return Err(IoStatus::FrameOutOfReel);
        }
        let mut packed = Vec::new();
        packed
            .try_reserve(self.frame_bytes)
            .map_err(|_| IoStatus::OutOfMemory)?;
        packed.resize(self.frame_bytes, 0);
        if extent.read_at(self.frame_at(index)?, &mut packed)? != self.frame_bytes {
            return Err(IoStatus::TruncatedPayload);
        }
        Ok(Frame::from_packed(self.description, &packed)?)
    }

    /// One row of one plane of one frame, into a buffer the caller owns.
    ///
    /// The smallest thing a reel can be read in, and the one that fits: a row
    /// of a 1920-wide eight-bit RGB picture is 5,760 bytes.
    ///
    /// The destination must be at least the row's length; a shorter one is
    /// refused rather than partly filled (R-1.4), because a caller that
    /// received half a row and no indication would draw half a row.
    ///
    /// # Errors
    ///
    /// [`IoStatus::FrameOutOfReel`], [`IoStatus::PlaneOutOfFrame`] for a plane
    /// this format does not have or a row past the plane's height,
    /// [`IoStatus::TooSmall`] for a destination shorter than the row,
    /// [`IoStatus::TruncatedPayload`], and whatever the geometry refuses.
    pub fn plane_row(
        &self,
        extent: &dyn Extent,
        index: usize,
        plane: usize,
        row: usize,
        into: &mut [u8],
    ) -> Result<usize> {
        if index >= self.count {
            return Err(IoStatus::FrameOutOfReel);
        }
        let format = self.description.format();
        let geometry = self.description.geometry();
        if plane >= format.plane_count() {
            return Err(IoStatus::PlaneOutOfFrame);
        }
        let width = format.plane_row_bytes(geometry, plane)?;
        let height = usize::try_from(format.plane_geometry(geometry, plane)?.height())
            .map_err(|_| IoStatus::TooMany)?;
        if row >= height {
            return Err(IoStatus::PlaneOutOfFrame);
        }
        if into.len() < width {
            return Err(IoStatus::TooSmall);
        }
        // The planes are packed in order, so the one asked for begins after
        // the whole of every plane before it.
        let mut before = 0_usize;
        for earlier in 0..plane {
            let rows = usize::try_from(format.plane_geometry(geometry, earlier)?.height())
                .map_err(|_| IoStatus::TooMany)?;
            let bytes = format
                .plane_row_bytes(geometry, earlier)?
                .checked_mul(rows)
                .ok_or(IoStatus::TooMany)?;
            before = before.checked_add(bytes).ok_or(IoStatus::TooMany)?;
        }
        let within = before
            .checked_add(row.checked_mul(width).ok_or(IoStatus::TooMany)?)
            .ok_or(IoStatus::TooMany)?;
        let at = self
            .frame_at(index)?
            .checked_add(u64::try_from(within).map_err(|_| IoStatus::TooMany)?)
            .ok_or(IoStatus::TooMany)?;
        if extent.read_at(at, &mut into[..width])? != width {
            return Err(IoStatus::TruncatedPayload);
        }
        Ok(width)
    }

    /// Recompute the reel's digest over its description and every sample.
    ///
    /// The expensive answer, in windows of `chunk` bytes, so it runs on a reel
    /// far larger than the memory available.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TooMany`] for a window of nothing,
    /// [`IoStatus::OutOfMemory`], [`IoStatus::TruncatedPayload`], or
    /// [`IoStatus::DigestMismatch`].
    pub fn verify(&self, extent: &dyn Extent, chunk: usize) -> Result<()> {
        if chunk == 0 {
            return Err(IoStatus::TooMany);
        }
        let mut header = [0_u8; HEADER_BYTES];
        if extent.read_at(0, &mut header)? != HEADER_BYTES {
            return Err(IoStatus::TruncatedHeader);
        }
        // The trailer, from the end. `open` has already established that the
        // extent is exactly as long as the header says, so this offset is
        // arithmetic rather than a search -- the same property the frame
        // offsets rest on, at the other end of the file.
        let end = extent
            .length()
            .checked_sub(u64::try_from(TRAILER_BYTES).map_err(|_| IoStatus::TooMany)?)
            .ok_or(IoStatus::TruncatedTrailer)?;
        let mut tail = [0_u8; TRAILER_BYTES];
        if extent.read_at(end, &mut tail)? != TRAILER_BYTES {
            return Err(IoStatus::TruncatedTrailer);
        }
        let stated = Digest::new(tail);
        let mut hasher = media_editor_core::Sha256::new();
        hasher.update(&header);
        let mut window = Vec::new();
        window
            .try_reserve(chunk)
            .map_err(|_| IoStatus::OutOfMemory)?;
        window.resize(chunk, 0);
        let mut at = u64::try_from(HEADER_BYTES).map_err(|_| IoStatus::TooMany)?;
        while at < end {
            let wanted = usize::try_from(end - at).unwrap_or(usize::MAX).min(chunk);
            let read = extent.read_at(at, &mut window[..wanted])?;
            if read != wanted {
                return Err(IoStatus::TruncatedPayload);
            }
            hasher.update(&window[..wanted]);
            at += u64::try_from(wanted).map_err(|_| IoStatus::TooMany)?;
        }
        if hasher.finish() != stated {
            return Err(IoStatus::DigestMismatch);
        }
        Ok(())
    }

    /// What this reel's sound is, if it has any.
    #[must_use]
    pub const fn sound(&self) -> Option<Sound> {
        self.sound
    }

    /// One frame's worth of sound, by position.
    ///
    /// The sound counterpart of [`Spool::plane_row`], and it exists for the
    /// same reason: ten seconds of 48 kHz stereo is 3,840,000 bytes against
    /// the seventy-six kilobytes a Phipia program is mapped, so a reader that
    /// took the whole sound section is a reader that cannot run. One frame's
    /// worth is 1602 samples of two channels — 12,816 bytes.
    ///
    /// Which samples belong to frame `k` is the arithmetic the mixer uses:
    /// from [`media_editor_core::Instant::floor_into`] of `k` up to the same of
    /// `k + 1`, so the blocks tile the take exactly and each one's end is the
    /// next one's beginning. **Where** they are in the file is that count
    /// summed from nought, which is why this walks rather than multiplies:
    /// blocks are 1601 samples or 1602, so there is no stride to multiply by.
    ///
    /// The walk is arithmetic rather than reading, so it costs nothing on the
    /// disk — but it is `k` steps rather than one, and that is the price of a
    /// frame not holding a whole number of samples.
    ///
    /// # Errors
    ///
    /// [`IoStatus::SoundNotDeclared`] for a reel with no sound,
    /// [`IoStatus::FrameOutOfReel`] past the count,
    /// [`IoStatus::TruncatedPayload`] if the extent ends inside it, and
    /// [`IoStatus::OutOfMemory`].
    pub fn sound_block(&self, extent: &dyn Extent, index: usize) -> Result<AudioBuffer> {
        let Some(sound) = self.sound else {
            return Err(IoStatus::SoundNotDeclared);
        };
        if index >= self.count {
            return Err(IoStatus::FrameOutOfReel);
        }
        let (before, here) = self.block_span(index, sound)?;
        let stride = sound
            .channels()
            .checked_mul(SAMPLE_BYTES)
            .ok_or(IoStatus::TooMany)?;
        let at = self
            .sound_at()?
            .checked_add(
                u64::try_from(before.checked_mul(stride).ok_or(IoStatus::TooMany)?)
                    .map_err(|_| IoStatus::TooMany)?,
            )
            .ok_or(IoStatus::TooMany)?;
        let wanted = here.checked_mul(stride).ok_or(IoStatus::TooMany)?;
        let mut bytes = Vec::new();
        bytes
            .try_reserve(wanted)
            .map_err(|_| IoStatus::OutOfMemory)?;
        bytes.resize(wanted, 0);
        if extent.read_at(at, &mut bytes)? != wanted {
            return Err(IoStatus::TruncatedPayload);
        }
        unpacked_sound(&bytes, Sound::new(sound.rate(), sound.channels(), here)?)
    }

    /// A run of samples, wherever it starts and however long it is.
    ///
    /// What a *mixer* asks for, where [`Spool::sound_block`] is what a
    /// *recorder* produces — and the two are different questions because a
    /// mixer's block boundaries are its own. It reads one stored block at a
    /// time and keeps the part it wants, so the largest thing in memory is the
    /// caller's buffer plus one block: 12,816 bytes at 48 kHz stereo.
    ///
    /// # Errors
    ///
    /// [`IoStatus::SoundNotDeclared`] for a reel with no sound,
    /// [`IoStatus::FrameOutOfReel`] for a run that reaches past the take,
    /// [`IoStatus::TruncatedPayload`], and [`IoStatus::OutOfMemory`].
    pub fn samples(&self, extent: &dyn Extent, start: i64, count: usize) -> Result<AudioBuffer> {
        let Some(sound) = self.sound else {
            return Err(IoStatus::SoundNotDeclared);
        };
        let start = usize::try_from(start).map_err(|_| IoStatus::FrameOutOfReel)?;
        let end = start.checked_add(count).ok_or(IoStatus::TooMany)?;
        // There was a guard here refusing a run that reached past the take,
        // and its control could not be made to fail. The walk below refuses
        // the same run with the same status a moment later: it reads blocks
        // until it runs out of them, and asking for the block past the last is
        // already `FrameOutOfReel`. A guard whose absence changes no answer is
        // a guard no test can hold -- the fifth this project has found and the
        // fifth it has deleted.
        let mut channels: Vec<Vec<i32>> = Vec::new();
        channels
            .try_reserve(sound.channels())
            .map_err(|_| IoStatus::OutOfMemory)?;
        for _ in 0..sound.channels() {
            let mut channel = Vec::new();
            channel
                .try_reserve(count)
                .map_err(|_| IoStatus::OutOfMemory)?;
            channels.push(channel);
        }
        let mut block = self.block_of(start, sound)?;
        while channels[0].len() < count {
            let held = self.sound_block(extent, block)?;
            let (before, length) = self.block_span(block, sound)?;
            // Which part of this block the run wants: everything from where
            // the run begins, or from the block's start once it is past that.
            let from = start.saturating_sub(before);
            let to = length.min(end.saturating_sub(before));
            for (index, channel) in channels.iter_mut().enumerate() {
                let samples = held.channel(index).map_err(IoStatus::Sound)?;
                channel.extend_from_slice(samples.get(from..to).ok_or(IoStatus::TruncatedPayload)?);
            }
            block = block.checked_add(1).ok_or(IoStatus::TooMany)?;
            if block > self.count {
                break;
            }
        }
        if channels[0].len() != count {
            return Err(IoStatus::TruncatedPayload);
        }
        AudioBuffer::new(sound.rate(), channels).map_err(IoStatus::Sound)
    }

    /// Which block a sample falls in.
    ///
    /// By bisection over [`sound_bounds`] rather than by dividing, and the
    /// division is where this went wrong first: blocks are 1601 samples or
    /// 1602, so `sample / 1601.6` is not the block number. Sample 1601 is the
    /// first of block **one** — because block nought runs to `floor(1601.6)` —
    /// and the division gives nought.
    ///
    /// The sequence `floor(k*r)` is non-decreasing in `k`, so the largest `k`
    /// whose start is at or below a sample is found by halving. That is the
    /// third place in this program where an integer sequence with no rational
    /// inverse is inverted by search, and they are all the same shape.
    fn block_of(&self, sample: usize, sound: Sound) -> Result<usize> {
        let (mut low, mut high) = (0_usize, self.count);
        while low < high {
            let middle = low + (high - low).div_ceil(2);
            if middle >= self.count {
                break;
            }
            let (start, _) = sound_bounds(middle, self.timebase, sound.rate())?;
            if start <= sample {
                low = middle;
            } else {
                high = middle - 1;
            }
        }
        Ok(low)
    }

    /// What this reel's transcript is.
    #[must_use]
    pub const fn spoken(&self) -> Spoken {
        self.spoken
    }

    /// The captions covering any of `[from, to)` of the recording.
    ///
    /// **Scanned rather than indexed**, which is what the section is laid out
    /// for and is the one place in this format where an offset is not
    /// arithmetic. A caption is a variable-length record, so there is no
    /// `k`-th caption at a computed place — and an index would buy nothing,
    /// because a projection never wants caption `k`. It wants every caption
    /// over a range, and finding those means looking at all of them whatever
    /// the layout.
    ///
    /// It also does not *hold* them all. The section is read in windows of
    /// [`CAPTION_WINDOW`] bytes, and the largest thing in memory is that
    /// window plus one record — because a record can **straddle a boundary**,
    /// which is what makes this more than a loop. A window that ends part way
    /// through a caption keeps the remainder, refills behind it, and carries
    /// on; the remainder is smaller than one record by construction, so the
    /// carry is bounded whatever the transcript is.
    ///
    /// 8.7 megabytes is what the largest transcript this format allows comes
    /// to. This reads it in 4,629 bytes.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TranscriptNotDeclared`] for a reel with no transcript,
    /// [`IoStatus::OutOfMemory`], [`IoStatus::TruncatedPayload`], and whatever
    /// a caption itself refuses.
    pub fn captions(&self, extent: &dyn Extent, from: i64, to: i64) -> Result<Vec<Caption>> {
        if self.spoken.count() == 0 {
            return Err(IoStatus::TranscriptNotDeclared);
        }
        let mut held = Vec::new();
        held.try_reserve(CAPTION_WINDOW + CAPTION_LARGEST)
            .map_err(|_| IoStatus::OutOfMemory)?;
        let mut found = Vec::new();
        let mut at = self.transcript_at()?;
        let end = at
            .checked_add(u64::try_from(self.spoken.bytes()).map_err(|_| IoStatus::TooMany)?)
            .ok_or(IoStatus::TooMany)?;
        let mut read = 0_usize;
        while read < self.spoken.count() {
            // Refill behind whatever the last window left part way through.
            let carried = held.len();
            let wanted = usize::try_from(end - at)
                .unwrap_or(usize::MAX)
                .min(CAPTION_WINDOW);
            held.resize(carried + wanted, 0);
            if extent.read_at(at, &mut held[carried..])? != wanted {
                return Err(IoStatus::TruncatedPayload);
            }
            at += u64::try_from(wanted).map_err(|_| IoStatus::TooMany)?;
            let (taken, matched) =
                scan_captions(&held, self.spoken.count() - read, from, to, &mut found)?;
            read += matched;
            if taken == 0 && wanted == 0 {
                // Nothing left to read and nothing complete in what is held:
                // the section ends inside a record.
                return Err(IoStatus::TruncatedPayload);
            }
            held.drain(..taken);
        }
        Ok(found)
    }

    /// Where the transcript begins: after every picture and every sample.
    fn transcript_at(&self) -> Result<u64> {
        let sound = self.sound.map_or(Ok(0), Sound::bytes)?;
        self.sound_at()?
            .checked_add(u64::try_from(sound).map_err(|_| IoStatus::TooMany)?)
            .ok_or(IoStatus::TooMany)
    }

    /// How many samples precede frame `index`, and how many it holds.
    fn block_span(&self, index: usize, sound: Sound) -> Result<(usize, usize)> {
        let (before, _) = sound_bounds(index, self.timebase, sound.rate())?;
        let (through, _) = sound_bounds(
            index.checked_add(1).ok_or(IoStatus::TooMany)?,
            self.timebase,
            sound.rate(),
        )?;
        Ok((
            before,
            through.checked_sub(before).ok_or(IoStatus::TooMany)?,
        ))
    }

    /// Where the sound section begins.
    fn sound_at(&self) -> Result<u64> {
        self.frame_at(self.count)
    }

    /// Where a frame begins.
    fn frame_at(&self, index: usize) -> Result<u64> {
        let within = self
            .frame_bytes
            .checked_mul(index)
            .ok_or(IoStatus::TooMany)?;
        u64::try_from(HEADER_BYTES.checked_add(within).ok_or(IoStatus::TooMany)?)
            .map_err(|_| IoStatus::TooMany)
    }
}

/// Everything a reel's header says, checked.
///
/// Out of [`decode`] and [`Spool::open`] both, so the two cannot drift: a
/// streaming reader that admitted a header the loading one refused would be a
/// second, weaker door into the same format.
fn read_header(
    bytes: &[u8; HEADER_BYTES],
) -> Result<(FrameDescription, Timebase, usize, Option<Sound>, Spoken)> {
    let mut header = Reader::new(bytes);
    if header.take(4)? != MAGIC {
        return Err(IoStatus::NotAReel);
    }
    let version = header.u16()?;
    if version != FORMAT_VERSION {
        return Err(IoStatus::UnsupportedVersion(version));
    }
    if header.u16()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let width = header.u32()?;
    let height = header.u32()?;
    let format = read_format(header.u8()?)?;
    let colour = ColourDescription::new(
        read_primaries(header.u8()?)?,
        read_transfer(header.u8()?)?,
        read_matrix(header.u8()?)?,
        read_range(header.u8()?)?,
    );
    let siting = read_siting(header.u8()?)?;
    let alpha = read_alpha(header.u8()?)?;
    if header.u8()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let pixel_aspect = Rational::new(header.i64()?, header.i64()?)?;
    let timebase = Timebase::new(Rational::new(header.i64()?, header.i64()?)?)?;
    let declared = header.u64()?;
    let geometry = Geometry::new(width, height)?;
    let description = FrameDescription::new(geometry, format, colour, siting, alpha, pixel_aspect)?;
    let count = usize::try_from(declared).map_err(|_| IoStatus::TooMany)?;
    if count > MAX_FRAMES {
        return Err(IoStatus::TooMany);
    }
    if count == 0 {
        return Err(IoStatus::EmptyReel);
    }
    let rate = header.u8()?;
    let channels = header.u8()?;
    for _ in 0..6 {
        if header.u8()? != 0 {
            return Err(IoStatus::ReservedFieldSet);
        }
    }
    let sound = read_sound(rate, channels, header.u64()?)?;
    // A reel whose sound is a second shorter than its pictures is a broken
    // take, and this is where it is refused -- before a sample is read, and
    // by arithmetic the mixer that produced them uses too.
    if let Some(sound) = sound {
        if !sound.fits(count, timebase)? {
            return Err(IoStatus::SoundRunsPastPicture);
        }
    }
    // Every check the descriptor makes, made by the descriptor: a count with
    // no bytes, bytes with no count, or a length too small for the fixed
    // fields is a header that disagrees with itself.
    let spoken = Spoken::new(
        usize::try_from(header.u32()?).map_err(|_| IoStatus::TooMany)?,
        usize::try_from(header.u32()?).map_err(|_| IoStatus::TooMany)?,
    )?;
    Ok((description, timebase, count, sound, spoken))
}

/// A reel wound onto a sink, a row at a time.
///
/// [`encode`] builds the whole file in memory and hands it back. At the
/// format's limit that is five hundred and twelve mebibytes against the
/// seventy-six kilobytes a Phipia program is mapped — **6,899 times** what
/// there is — which is the same sentence [`Spool`] opens with, because this is
/// the same problem from the other side. [`Spool`] is how a reel is read
/// without being held; this is how one is written without being built.
///
/// A winder holds a description, a rate, a count, where it has got to, and a
/// hash in progress. **Nothing else** — no frame, no row, no buffer. One row
/// of a 1920-wide RGB picture is 5,760 bytes, and that row belongs to the
/// caller: it arrives, it is hashed, it is appended, and the winder forgets
/// it.
///
/// ## What it refuses, and why each one is a refusal rather than a repair
///
/// [`IoStatus::NotOnePlane`] — a packed frame is plane nought entire, then
/// plane one entire. Rows arrive interleaved across planes; a file wants them
/// segregated. Writing a three-plane reel forwards would mean holding two
/// thirds of every picture until its last row, which is the allocation this
/// exists to avoid, spelled differently. A planar reel is [`encode`]'s to
/// write, and that is a real limitation rather than a temporary one.
///
/// [`IoStatus::RowOutOfOrder`] — every row names the frame and the row it is.
/// A winder could take whatever came next and be shorter for it; then a
/// caller's off-by-one would produce a file whose pictures are wrong and whose
/// digest is *right*, which is the worst failure this format can have. So the
/// caller says which row it thinks it is sending, and being wrong is an error
/// (R-1.3).
///
/// [`IoStatus::IncompleteReel`] — the count goes in the header, before a
/// single sample is written, because that is what makes frame `k` live at
/// `HEADER_BYTES + k × packed_bytes` and not at the end of a search. So a
/// winder that stopped early would have written a header that lies. Finishing
/// one is refused; there is no truncation, and no rewriting the count.
///
/// [`IoStatus::SinkNotEmpty`] — a reel is the whole file. Winding one onto
/// bytes that are already there would produce a file with a prefix, whose
/// header is not at offset nought.
///
/// ## What it does not do
///
/// **It does not verify.** The digest it returns is the one it computed from
/// what it was *given*, and the one in the file is a copy of it. Nothing about
/// that says the bytes reached the disk. [`Spool::verify`] over what was
/// actually stored is the other half, and [`crate::save`] is where the two are
/// put together — the same division [`Spool::open`] makes, for the same
/// reason.
pub struct Winder {
    /// What every frame in this reel is.
    description: FrameDescription,
    /// What every row handed to [`Winder::row`] must be: the above, one row
    /// high. Computed once at [`Winder::begin`] rather than per row, because
    /// it is the same answer every time and a row is where the work is.
    line: FrameDescription,
    /// How many frames were declared, and therefore how many must arrive.
    count: usize,
    /// How many rows each frame has.
    rows: usize,
    /// Which frame the next row belongs to.
    frame: usize,
    /// Which row of it.
    row: usize,
    /// What this reel's sound is, if it has any.
    sound: Option<Sound>,
    /// Which block of sound the next one is. A block is one frame's worth, so
    /// this counts to the frame count and not to the sample count.
    block: usize,
    /// How many samples of sound have arrived, per channel.
    ///
    /// Counted rather than derived, because a block is 1601 samples or 1602
    /// and only the total is fixed. It is what [`Winder::finish`] compares
    /// against the count in the header.
    sounded: usize,
    /// The two lengths one block may be, worked out once at [`Winder::begin`].
    block_bounds: (usize, usize),
    /// What the header says the transcript is.
    spoken: Spoken,
    /// How many captions have arrived.
    said: usize,
    /// How many bytes they came to.
    said_bytes: usize,
    /// The digest so far, over the header and every sample appended since.
    hasher: media_editor_core::Sha256,
}

impl Winder {
    /// Write a reel's header, and prepare to take its rows.
    ///
    /// The count is declared **now**, before a sample exists, because it is a
    /// header field and the header is written first. That is not a concession
    /// to streaming: it is what lets a reader find frame `k` by arithmetic,
    /// and the format argued for it long before anything wrote one this way.
    ///
    /// # Errors
    ///
    /// [`IoStatus::EmptyReel`], [`IoStatus::TooMany`] past [`MAX_FRAMES`],
    /// [`IoStatus::NotOnePlane`], [`IoStatus::PayloadTooLarge`] past
    /// [`MAX_REEL_BYTES`], [`IoStatus::SinkNotEmpty`], and whatever the sink
    /// refuses.
    pub fn begin(
        sink: &mut dyn Sink,
        description: FrameDescription,
        timebase: Timebase,
        count: usize,
        sound: Option<Sound>,
        spoken: Spoken,
    ) -> Result<Self> {
        if count == 0 {
            return Err(IoStatus::EmptyReel);
        }
        if count > MAX_FRAMES {
            return Err(IoStatus::TooMany);
        }
        if description.format().plane_count() != 1 {
            return Err(IoStatus::NotOnePlane);
        }
        let frame_bytes = description.packed_bytes()?;
        let payload = frame_bytes
            .checked_mul(count)
            .and_then(|pictures| pictures.checked_add(sound.map_or(Ok(0), Sound::bytes).ok()?))
            .and_then(|sounded| sounded.checked_add(spoken.bytes()))
            .ok_or(IoStatus::PayloadTooLarge)?;
        if payload > MAX_REEL_BYTES {
            return Err(IoStatus::PayloadTooLarge);
        }
        // The sound's length is checked against the pictures' before the
        // header is written, so a take that could never have been a take is
        // refused rather than half written.
        let block_bounds = match sound {
            None => (0, 0),
            Some(sound) => {
                if !sound.fits(count, timebase)? {
                    return Err(IoStatus::SoundRunsPastPicture);
                }
                sound_bounds(1, timebase, sound.rate())?
            }
        };
        if sink.written() != 0 {
            return Err(IoStatus::SinkNotEmpty);
        }
        // The same function `row_description` gives a scan, rather than a
        // second statement of what a row is. A writer and a renderer that
        // disagreed about the description of one row would produce a file
        // whose every frame is a row short, or refuse every row of a picture
        // that was perfectly good.
        let line =
            media_editor_render::row_description(description, 0).map_err(IoStatus::Render)?;
        let rows =
            usize::try_from(description.geometry().height()).map_err(|_| IoStatus::TooMany)?;
        let head = header(description, timebase, count, sound, spoken)?;
        let mut hasher = media_editor_core::Sha256::new();
        hasher.update(&head);
        sink.append(&head)?;
        Ok(Self {
            description,
            line,
            count,
            rows,
            frame: 0,
            row: 0,
            sound,
            block: 0,
            sounded: 0,
            block_bounds,
            spoken,
            said: 0,
            said_bytes: 0,
            hasher,
        })
    }

    /// Which frame and which row of it the winder is waiting for.
    #[must_use]
    pub const fn at(&self) -> (usize, usize) {
        (self.frame, self.row)
    }

    /// What every row handed to [`Winder::row`] must be described as.
    #[must_use]
    pub const fn line(&self) -> &FrameDescription {
        &self.line
    }

    /// Whether every row of every declared frame has arrived.
    #[must_use]
    pub const fn pictures_complete(&self) -> bool {
        self.frame == self.count
    }

    /// Whether every block of sound has arrived, or none was declared.
    #[must_use]
    pub fn sound_complete(&self) -> bool {
        match self.sound {
            None => true,
            Some(sound) => self.block == self.count && self.sounded == sound.samples(),
        }
    }

    /// Whether both halves of the reel have arrived.
    #[must_use]
    pub fn is_complete(&self) -> bool {
        self.pictures_complete() && self.sound_complete() && self.transcript_complete()
    }

    /// What this reel's sound is, if it has any.
    #[must_use]
    pub const fn sound(&self) -> Option<Sound> {
        self.sound
    }

    /// Which block of sound the winder is waiting for.
    #[must_use]
    pub const fn block(&self) -> usize {
        self.block
    }

    /// Write one frame's worth of sound.
    ///
    /// **After every picture**, because that is where the samples live in the
    /// file. A block is one frame's worth of every channel, interleaved, and
    /// it is 1601 samples or 1602 at 30000/1001 into 48 kHz — so the winder
    /// checks the bound rather than the number, and checks the *total*
    /// against the header when the reel is closed.
    ///
    /// # Errors
    ///
    /// [`IoStatus::SoundNotDeclared`] for a reel whose header says it has no
    /// sound, [`IoStatus::RowOutOfOrder`] if any picture is still owed,
    /// [`IoStatus::SoundOutOfOrder`] for any block but the next,
    /// [`IoStatus::SoundBlockWrongLength`] for a block no frame could cover,
    /// [`IoStatus::Sound`] if the buffer is not the rate or the channel count
    /// the header declared, and whatever the sink refuses.
    pub fn sound_block(
        &mut self,
        sink: &mut dyn Sink,
        block: usize,
        buffer: &AudioBuffer,
    ) -> Result<()> {
        let Some(sound) = self.sound else {
            return Err(IoStatus::SoundNotDeclared);
        };
        // The pictures first, and this is the one ordering rule that is about
        // the *file* rather than about the caller: samples live after every
        // frame, so a block that arrived early would land in the middle of a
        // picture.
        if !self.pictures_complete() {
            return Err(IoStatus::RowOutOfOrder);
        }
        if self.block == self.count || block != self.block {
            return Err(IoStatus::SoundOutOfOrder);
        }
        if buffer.rate() != sound.rate() || buffer.channel_count() != sound.channels() {
            return Err(IoStatus::Sound(
                media_editor_audio::AudioStatus::ChannelCountUnsupported,
            ));
        }
        let (least, most) = self.block_bounds;
        if buffer.len() < least || buffer.len() > most {
            return Err(IoStatus::SoundBlockWrongLength);
        }
        let bytes = packed_sound(buffer)?;
        self.hasher.update(&bytes);
        sink.append(&bytes)?;
        self.block += 1;
        self.sounded = self
            .sounded
            .checked_add(buffer.len())
            .ok_or(IoStatus::TooMany)?;
        Ok(())
    }

    /// Write one row.
    ///
    /// `frame` and `row` say which row the caller believes it is sending, and
    /// disagreeing with the winder is an error rather than a hint.
    ///
    /// # Errors
    ///
    /// [`IoStatus::RowOutOfOrder`] for any row but the next one — including
    /// any row at all once the reel is complete —
    /// [`IoStatus::ReelDescriptionMismatch`] for a row that is not described
    /// the way [`Winder::line`] says, and whatever the sink refuses.
    pub fn row(
        &mut self,
        sink: &mut dyn Sink,
        frame: usize,
        row: usize,
        line: &Frame,
    ) -> Result<()> {
        if self.pictures_complete() || frame != self.frame || row != self.row {
            return Err(IoStatus::RowOutOfOrder);
        }
        if line.description() != &self.line {
            return Err(IoStatus::ReelDescriptionMismatch);
        }
        let bytes = line.packed()?;
        self.hasher.update(&bytes);
        sink.append(&bytes)?;
        self.row += 1;
        if self.row == self.rows {
            self.row = 0;
            self.frame += 1;
        }
        Ok(())
    }

    /// Whether every caption has arrived, or none was declared.
    #[must_use]
    pub const fn transcript_complete(&self) -> bool {
        self.said == self.spoken.count() && self.said_bytes == self.spoken.bytes()
    }

    /// Write one caption.
    ///
    /// **After every picture and every sample**, because that is where the
    /// words live in the file. They arrive one at a time rather than as a set,
    /// for the reason [`Spoken`] gives: sixteen thousand of them is 8.7
    /// megabytes, and a writer that held them to write them would be holding
    /// a hundred and fourteen times what the machine has.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TranscriptNotDeclared`] for a reel whose header says it has
    /// no transcript, [`IoStatus::RowOutOfOrder`] or
    /// [`IoStatus::SoundOutOfOrder`] if any picture or block of sound is still
    /// owed, [`IoStatus::CaptionOutOfOrder`] for any caption but the next or
    /// for one that would take the section past the length declared, and
    /// whatever the sink refuses.
    pub fn caption(&mut self, sink: &mut dyn Sink, index: usize, caption: &Caption) -> Result<()> {
        if self.spoken.count() == 0 {
            return Err(IoStatus::TranscriptNotDeclared);
        }
        if !self.pictures_complete() {
            return Err(IoStatus::RowOutOfOrder);
        }
        if !self.sound_complete() {
            return Err(IoStatus::SoundOutOfOrder);
        }
        if self.said == self.spoken.count() || index != self.said {
            return Err(IoStatus::CaptionOutOfOrder);
        }
        let bytes = packed_captions(core::slice::from_ref(caption))?;
        let after = self
            .said_bytes
            .checked_add(bytes.len())
            .ok_or(IoStatus::TooMany)?;
        // The declared length is a promise about the whole section, so a
        // caption that would overrun it is refused before it is written rather
        // than discovered by a reader.
        if after > self.spoken.bytes() {
            return Err(IoStatus::CaptionOutOfOrder);
        }
        self.hasher.update(&bytes);
        sink.append(&bytes)?;
        self.said += 1;
        self.said_bytes = after;
        Ok(())
    }

    /// Close the reel with its digest, and hand that digest back.
    ///
    /// # Errors
    ///
    /// [`IoStatus::IncompleteReel`] if any row of any declared frame is
    /// missing, and whatever the sink refuses.
    pub fn finish(self, sink: &mut dyn Sink) -> Result<Digest> {
        if !self.pictures_complete() {
            return Err(IoStatus::IncompleteReel);
        }
        // A separate refusal from the pictures', because a reel that is short
        // of sound and a reel that is short of pictures are different
        // mistakes and a caller fixing one wants to know which it has.
        if !self.sound_complete() {
            return Err(IoStatus::SoundRunsPastPicture);
        }
        // A third refusal, because a reel short of words and a reel short of
        // sound are different mistakes and a caller fixing one wants to know
        // which it has.
        if !self.transcript_complete() {
            return Err(IoStatus::TranscriptNotDeclared);
        }
        let digest = self.hasher.finish();
        sink.append(digest.bytes())?;
        Ok(digest)
    }

    /// What every frame in this reel is.
    #[must_use]
    pub const fn description(&self) -> &FrameDescription {
        &self.description
    }
}
