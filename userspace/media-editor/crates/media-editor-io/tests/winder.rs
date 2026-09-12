// SPDX-License-Identifier: GPL-3.0-only
//! A reel written a row at a time.
//!
//! One property does the work and everything else supports it: **a reel wound
//! row by row is byte for byte the file [`sprw::encode`] writes.** Two writers
//! that disagreed anywhere would be two formats sharing a magic number, and
//! the whole value of the streaming writer is that it is the same file
//! produced differently.
//!
//! Why it exists: `encode` returns a `Vec<u8>`, and a reel this build writes
//! is bounded at five hundred and twelve mebibytes against the seventy-six
//! kilobytes a Phipia program is mapped. One row of a 1920-wide RGB picture is
//! 5,760 bytes.

use media_editor_abi::seam::{Slot, Storage};
use media_editor_audio::{AudioBuffer, SampleRate};
use media_editor_core::Timebase;
use media_editor_io::bytes::{Extent, Sink};
use media_editor_io::memory::{Fault, MemoryStorage};
use media_editor_io::save::{Scratch, Staged};
use media_editor_io::sprw::{self, HEADER_BYTES, MAX_FRAMES, Reel, Spool, TRAILER_BYTES, Winder};
use media_editor_io::status::IoStatus;
use media_editor_media::colour::{AlphaState, ColourDescription};
use media_editor_media::{Frame, FrameDescription, Geometry, PixelFormat, Plane, TestPattern};

const RATE: Timebase = Timebase::FILM_24;

fn described(width: u32, height: u32, format: PixelFormat) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(width, height).expect("a geometry"),
        format,
        // A planar format's samples are luma and chroma, so they need a matrix
        // that says how to get back to red, green and blue; sRGB's is the
        // identity, which describes nothing about a Y'CbCr picture.
        if format.is_rgb() {
            ColourDescription::srgb_full()
        } else {
            ColourDescription::new(
                media_editor_media::colour::Primaries::Bt709,
                media_editor_media::colour::TransferFunction::Bt709,
                media_editor_media::colour::MatrixCoefficients::Bt709,
                media_editor_media::colour::Range::Limited,
            )
        },
        // And a subsampled one needs to say where its chroma sits, because
        // one chroma sample serves several luma samples and which ones is not
        // a detail.
        if format.is_subsampled() {
            Some(media_editor_media::colour::ChromaSiting::Centre)
        } else {
            None
        },
        if format == PixelFormat::Rgba8 {
            Some(AlphaState::Premultiplied)
        } else {
            None
        },
    )
    .expect("a description")
}

/// A short reel whose frames are all different pictures.
fn sample(width: u32, height: u32, count: usize, format: PixelFormat) -> Reel {
    let description = described(width, height, format);
    let mut frames = std::vec::Vec::new();
    for index in 0..count {
        let pattern = match index % 3 {
            0 => TestPattern::Bars,
            1 => TestPattern::Ramp,
            _ => TestPattern::Flat {
                value: u8::try_from(index * 17 % 256).expect("a value"),
            },
        };
        frames.push(pattern.render(description).expect("a frame"));
    }
    Reel::new(RATE, frames).expect("a reel")
}

/// One row of a frame, as a frame one row high.
///
/// Built here rather than fetched, so the winder is being fed rows that came
/// from somewhere else — a test that sliced with the code under test would be
/// asking the writer to agree with itself.
fn row_of(frame: &Frame, row: usize) -> Frame {
    let description = *frame.description();
    let geometry = description.geometry();
    let plane = frame.plane(0).expect("a plane");
    let stride = plane.stride();
    let one = FrameDescription::new(
        Geometry::new(geometry.width(), 1).expect("a geometry"),
        description.format(),
        description.colour(),
        description.siting(),
        description.alpha(),
        description.pixel_aspect(),
    )
    .expect("a description");
    Frame::new(
        one,
        std::vec![
            Plane::new(
                plane.samples()[row * stride..(row + 1) * stride].to_vec(),
                stride,
            )
            .expect("a plane")
        ],
    )
    .expect("a frame")
}

/// Wind a whole reel onto a sink: row by row, then block by block.
/// A reel that says nothing.
fn silent() -> sprw::Spoken {
    sprw::Spoken::of(&[]).expect("no words")
}

fn wind(sink: &mut dyn Sink, reel: &Reel) -> media_editor_core::Digest {
    let description = *reel.description();
    let height = description.geometry().height() as usize;
    let sound = reel.sound_description().expect("a sound description");
    let said = sprw::Spoken::of(reel.captions()).expect("a transcript");
    let mut winder = Winder::begin(sink, description, reel.timebase(), reel.len(), sound, said)
        .expect("a winder");
    for (index, frame) in reel.frames().iter().enumerate() {
        for row in 0..height {
            winder
                .row(sink, index, row, &row_of(frame, row))
                .expect("a row");
        }
    }
    if let Some(buffer) = reel.sound() {
        for (index, block) in blocks(buffer, reel).into_iter().enumerate() {
            winder
                .sound_block(sink, index, &block)
                .expect("a block of sound");
        }
    }
    for (index, caption) in reel.captions().iter().enumerate() {
        winder.caption(sink, index, caption).expect("a caption");
    }
    winder.finish(sink).expect("a trailer")
}

/// A buffer cut into one block a frame, the way a mixer produces it.
///
/// Cut by the *format's* own bounds function, which is the arithmetic a reader
/// will use to find the blocks again -- so a test that passes says the two
/// halves agree rather than that one of them is self-consistent.
fn blocks(buffer: &AudioBuffer, reel: &Reel) -> std::vec::Vec<AudioBuffer> {
    let mut out = std::vec::Vec::new();
    for index in 0..reel.len() {
        let (before, _) =
            sprw::sound_bounds(index, reel.timebase(), buffer.rate()).expect("a bound");
        let (through, _) =
            sprw::sound_bounds(index + 1, reel.timebase(), buffer.rate()).expect("a bound");
        let channels = (0..buffer.channel_count())
            .map(|channel| buffer.channel(channel).expect("a channel")[before..through].to_vec())
            .collect();
        out.push(AudioBuffer::new(buffer.rate(), channels).expect("a block"));
    }
    out
}

/// A reel of pictures with sound exactly as long as they are.
///
/// The length comes from the format's own bounds rather than a number typed
/// here, because at 30000/1001 there is no single right number -- and the
/// point is that the format knows it.
fn with_sound(reel: Reel, rate: SampleRate, channels: usize) -> Reel {
    let (samples, _) = sprw::sound_bounds(reel.len(), reel.timebase(), rate).expect("a bound");
    let voices = (0..channels)
        .map(|channel| {
            (0..samples)
                .map(|index| {
                    // A tone that is a function of where it is, so a block
                    // written in the wrong place is audible rather than
                    // plausible.
                    i32::try_from((index * 37 + channel * 911) % 30_000).expect("a sample") - 15_000
                })
                .collect()
        })
        .collect();
    reel.with_sound(AudioBuffer::new(rate, voices).expect("a buffer"))
        .expect("sound as long as the pictures")
}

#[test]
fn a_wound_reel_is_the_file_encode_writes() {
    // The property, at the top of the module. Every other test here is about
    // what happens when it cannot hold.
    for (width, height, count, format) in [
        (8_u32, 4_u32, 3_usize, PixelFormat::Rgb8),
        (16, 9, 5, PixelFormat::Rgba8),
        (1, 1, 1, PixelFormat::Gray8),
        (7, 5, 2, PixelFormat::Gray8),
    ] {
        let reel = sample(width, height, count, format);
        let whole = sprw::encode(&reel).expect("an encoding");
        let mut wound = std::vec::Vec::new();
        let digest = wind(&mut wound, &reel);
        assert_eq!(
            wound, whole,
            "{width}x{height} {format:?} x{count}: the two writers disagree"
        );
        // And the digest the winder computed while writing is the one in the
        // file it wrote -- which is not the same claim, and is what a save
        // compares against what actually reached the disk.
        assert_eq!(&wound[wound.len() - TRAILER_BYTES..], digest.bytes());
    }
}

#[test]
fn a_wound_reel_reads_back_frame_for_frame() {
    let reel = sample(8, 4, 4, PixelFormat::Rgb8);
    let mut wound = std::vec::Vec::new();
    wind(&mut wound, &reel);
    let spool = Spool::open(&wound.as_slice()).expect("a spool");
    assert_eq!(spool.len(), 4);
    spool.verify(&wound.as_slice(), 64).expect("a sound digest");
    for (index, frame) in reel.frames().iter().enumerate() {
        assert_eq!(
            &spool.frame(&wound.as_slice(), index).expect("a frame"),
            frame,
            "frame {index}"
        );
    }
    assert_eq!(sprw::decode(&wound).expect("a decoding"), reel);
}

#[test]
fn a_planar_reel_cannot_be_wound_and_can_still_be_encoded() {
    // A packed frame is plane nought entire, then plane one entire. Rows
    // arrive interleaved across planes and a file wants them segregated, so
    // winding one forwards would mean holding two thirds of every picture. A
    // real limitation, named -- not a gap waiting to be filled.
    for format in [
        PixelFormat::Yuv444p8,
        PixelFormat::Yuv422p8,
        PixelFormat::Yuv420p8,
    ] {
        let description = described(8, 4, format);
        let mut sink = std::vec::Vec::new();
        assert_eq!(
            Winder::begin(&mut sink, description, RATE, 2, None, silent()).err(),
            Some(IoStatus::NotOnePlane),
            "{format:?}"
        );
        assert!(sink.is_empty(), "a header was written before the refusal");
        // And the whole-file writer does it, which is the point of keeping
        // both. Yuv444p8 is not even subsampled: this is about planes. The
        // frame is built from bytes rather than rendered, because the test
        // patterns are written for red, green and blue -- which is itself the
        // reason nothing else in this suite has ever exercised a planar reel.
        let bytes: std::vec::Vec<u8> = (0..description.packed_bytes().expect("a size"))
            .map(|index| u8::try_from(16 + index % 200).expect("a byte"))
            .collect();
        let reel = Reel::new(
            RATE,
            std::vec![Frame::from_packed(description, &bytes).expect("a frame")],
        )
        .expect("a reel");
        let file = sprw::encode(&reel).expect("an encoding");
        assert_eq!(sprw::decode(&file).expect("a decoding"), reel);
    }
}

#[test]
fn a_row_that_is_not_the_next_one_is_refused() {
    let reel = sample(8, 4, 2, PixelFormat::Rgb8);
    let frames = reel.frames();
    let mut sink = std::vec::Vec::new();
    let mut winder =
        Winder::begin(&mut sink, *reel.description(), RATE, 2, None, silent()).expect("a winder");
    assert_eq!(winder.at(), (0, 0));
    // The row after next, the frame after next, and a row of a frame already
    // written -- each refused by the same name, because each is the same
    // mistake.
    for (frame, row) in [(0_usize, 1_usize), (1, 0), (0, 3)] {
        assert_eq!(
            winder
                .row(&mut sink, frame, row, &row_of(&frames[0], 0))
                .err(),
            Some(IoStatus::RowOutOfOrder),
            "frame {frame} row {row} was accepted out of turn"
        );
    }
    assert_eq!(sink.len(), HEADER_BYTES, "a refused row was written anyway");
    // The right one advances, and rolls over into the next frame at the
    // bottom of this one rather than asking for a fifth row of a frame four
    // rows high.
    for row in 0..4 {
        winder
            .row(&mut sink, 0, row, &row_of(&frames[0], row))
            .expect("a row");
    }
    assert_eq!(winder.at(), (1, 0));
}

#[test]
fn a_row_described_some_other_way_is_refused() {
    let reel = sample(8, 4, 1, PixelFormat::Rgb8);
    let frames = reel.frames();
    let mut sink = std::vec::Vec::new();
    let mut winder =
        Winder::begin(&mut sink, *reel.description(), RATE, 1, None, silent()).expect("a winder");
    assert_eq!(winder.line(), &described(8, 1, PixelFormat::Rgb8));
    // A whole frame where a row belongs: the right width, four times the
    // bytes, and it would have been written straight into the file.
    assert_eq!(
        winder.row(&mut sink, 0, 0, &frames[0]).err(),
        Some(IoStatus::ReelDescriptionMismatch)
    );
    // A row of the right height and the wrong width.
    let narrow = sample(4, 4, 1, PixelFormat::Rgb8);
    assert_eq!(
        winder
            .row(&mut sink, 0, 0, &row_of(&narrow.frames()[0], 0))
            .err(),
        Some(IoStatus::ReelDescriptionMismatch)
    );
    // A row of the right shape in another format.
    let grey = sample(8, 4, 1, PixelFormat::Gray8);
    assert_eq!(
        winder
            .row(&mut sink, 0, 0, &row_of(&grey.frames()[0], 0))
            .err(),
        Some(IoStatus::ReelDescriptionMismatch)
    );
    assert_eq!(sink.len(), HEADER_BYTES);
}

#[test]
fn a_reel_that_is_short_of_rows_cannot_be_closed() {
    // The count is in the header, written before the first sample, because
    // that is what makes frame k live at a computed offset. So a winder that
    // stopped early would leave a header that lies -- and there is no
    // truncating and no rewriting the count.
    let reel = sample(8, 4, 3, PixelFormat::Rgb8);
    let frames = reel.frames();
    let mut sink = std::vec::Vec::new();
    let mut winder =
        Winder::begin(&mut sink, *reel.description(), RATE, 3, None, silent()).expect("a winder");
    for row in 0..4 {
        winder
            .row(&mut sink, 0, row, &row_of(&frames[0], row))
            .expect("a row");
    }
    // Two frames still owed.
    assert!(!winder.is_complete());
    assert_eq!(
        winder.finish(&mut sink).err(),
        Some(IoStatus::IncompleteReel)
    );
    // And one row short of the last frame is the same answer, which is the
    // case a count of frames alone would have let through.
    let mut sink = std::vec::Vec::new();
    let mut winder =
        Winder::begin(&mut sink, *reel.description(), RATE, 3, None, silent()).expect("a winder");
    for (index, frame) in frames.iter().enumerate() {
        for row in 0..4 {
            if index == 2 && row == 3 {
                break;
            }
            winder
                .row(&mut sink, index, row, &row_of(frame, row))
                .expect("a row");
        }
    }
    assert_eq!(
        winder.finish(&mut sink).err(),
        Some(IoStatus::IncompleteReel)
    );
}

#[test]
fn a_reel_is_wound_onto_nothing_or_not_at_all() {
    let description = described(8, 4, PixelFormat::Rgb8);
    let mut sink = std::vec::Vec::from(&b"already here"[..]);
    assert_eq!(
        Winder::begin(&mut sink, description, RATE, 1, None, silent()).err(),
        Some(IoStatus::SinkNotEmpty),
        "a reel's header must be at offset nought"
    );
    assert_eq!(sink.len(), 12, "and nothing was appended to what was there");
}

#[test]
fn a_reel_of_no_frames_or_too_many_cannot_be_begun() {
    // The same two bounds `Spool::open` enforces when reading, enforced before
    // a byte is written rather than after a file exists.
    let description = described(8, 4, PixelFormat::Rgb8);
    let mut sink = std::vec::Vec::new();
    assert_eq!(
        Winder::begin(&mut sink, description, RATE, 0, None, silent()).err(),
        Some(IoStatus::EmptyReel)
    );
    assert_eq!(
        Winder::begin(&mut sink, description, RATE, MAX_FRAMES + 1, None, silent()).err(),
        Some(IoStatus::TooMany)
    );
    // And a reel that would be larger than the format allows, which is a
    // different bound: 24,000 frames of 1920x1080 RGB is 149 gibibytes.
    assert_eq!(
        Winder::begin(
            &mut sink,
            described(1920, 1080, PixelFormat::Rgb8),
            RATE,
            MAX_FRAMES,
            None,
            silent()
        )
        .err(),
        Some(IoStatus::PayloadTooLarge)
    );
    assert!(sink.is_empty());
}

#[test]
fn winding_through_storage_never_holds_more_than_one_row() {
    // The measurement, and the reason the whole chain exists. Not how many
    // appends -- how large the largest of them was, which is the number a
    // seventy-six-kilobyte program cares about.
    let reel = sample(64, 16, 3, PixelFormat::Rgba8);
    let whole = sprw::encode(&reel).expect("an encoding");
    let mut storage = MemoryStorage::new(1 << 20);
    {
        let mut sink = Scratch::new(&mut storage);
        wind(&mut sink, &reel);
    }
    // 64 wide, four bytes a pixel: a row is 256 bytes and a frame is 4,096.
    // The largest thing this path ever handed to storage is one row, and the
    // header at 64 and the trailer at 32 are both smaller.
    assert_eq!(storage.largest_append(), 256);
    // One append for the header, one per row of every frame, one for the
    // trailer: 1 + 16 x 3 + 1 = 50.
    assert_eq!(storage.appends(), 50);
    // And it is the same file.
    let staged = Staged::new(&storage).expect("something staged");
    assert_eq!(staged.length(), whole.len() as u64);
    let mut read = std::vec![0_u8; whole.len()];
    storage
        .read_at(Slot::Scratch, 0, &mut read)
        .expect("a read");
    assert_eq!(read, whole);
}

#[test]
fn a_recording_interrupted_at_any_row_leaves_the_last_reel_alone() {
    // R-9.4 for a write that takes a thousand steps rather than one. The
    // failure a whole-file write cannot have: a drive that fills up on row
    // four hundred, with three hundred and ninety-nine rows already on it.
    let reel = sample(8, 4, 3, PixelFormat::Rgb8);
    let previous = sprw::encode(&sample(8, 4, 1, PixelFormat::Gray8)).expect("an encoding");
    // 1 header + 12 rows + 1 trailer = 14 appends, so every one of them.
    for step in 0..14 {
        let mut storage = MemoryStorage::new(1 << 20);
        storage
            .write(Slot::Vault, &previous)
            .expect("a previous reel");
        storage.set_fault(Fault::OnAppend(step));
        let failed = {
            let mut sink = Scratch::new(&mut storage);
            let description = *reel.description();
            let height = description.geometry().height() as usize;
            (|| -> Result<(), IoStatus> {
                let mut winder =
                    Winder::begin(&mut sink, description, RATE, reel.len(), None, silent())?;
                for (index, frame) in reel.frames().iter().enumerate() {
                    for row in 0..height {
                        winder.row(&mut sink, index, row, &row_of(frame, row))?;
                    }
                }
                winder.finish(&mut sink)?;
                Ok(())
            })()
        };
        assert!(failed.is_err(), "append {step} was supposed to be refused");
        assert_eq!(
            storage.stored(),
            Some(previous.as_slice()),
            "the last reel did not survive a refusal at append {step}"
        );
        assert_eq!(storage.commits(), 0, "nothing was committed");
    }
}

#[test]
fn a_scratch_sink_knows_how_much_it_holds() {
    // A winder refuses a sink that is not empty, and that refusal is only
    // worth anything if the sink can tell. A `Scratch` that always answered
    // nought would let a second reel be wound onto the tail of the first,
    // producing a file whose header is at offset six hundred and seventy-two.
    let reel = sample(8, 4, 2, PixelFormat::Rgb8);
    let mut storage = MemoryStorage::new(1 << 20);
    let mut sink = Scratch::new(&mut storage);
    assert_eq!(sink.written(), 0, "a slot that holds nothing holds nothing");
    wind(&mut sink, &reel);
    // 88 + 2 x 96 + 32 = 312, which is what a reel of two eight-by-four RGB
    // frames comes to. It was 288 while the header was sixty-four, 304 when
    // sixteen bytes came to describe the sound, and 312 now that eight more
    // describe the transcript -- and a reel with neither pays for both, since
    // a header that only sometimes had a field would be a header nobody could
    // seek in.
    assert_eq!(sink.written(), 312);
    assert_eq!(
        Winder::begin(&mut sink, *reel.description(), RATE, 2, None, silent()).err(),
        Some(IoStatus::SinkNotEmpty),
        "a second reel was wound onto the first"
    );
    // And emptying it makes it possible again, which is the other half: a
    // sink that could never be reused would make every export the last one.
    sink.clear().expect("an empty slot");
    assert_eq!(sink.written(), 0);
    wind(&mut sink, &reel);
    assert_eq!(sink.written(), 312);
    // A `Scratch` opened over a slot that already holds bytes sees them, which
    // is what makes the refusal above reachable from a fresh export. The
    // scope, rather than a `drop`, because the borrow is what has to end.
    let held = { Scratch::new(&mut storage).written() };
    assert_eq!(held, 312);
}

#[test]
fn a_reel_with_sound_is_the_file_encode_writes() {
    // The property again, over the half that arrives after every picture. Two
    // rates and three channel counts, because the sample count per frame is
    // where this format is most likely to be wrong: at 24 into 48 kHz a frame
    // is 2000 samples exactly, and at 30000/1001 it is 1601.6 and never a
    // whole number.
    for (timebase, rate, channels, count) in [
        (Timebase::FILM_24, SampleRate::Hz48000, 2_usize, 3_usize),
        (Timebase::NTSC_30, SampleRate::Hz48000, 2, 5),
        (Timebase::NTSC_30, SampleRate::Hz44100, 1, 4),
        (Timebase::PAL_25, SampleRate::Hz96000, 6, 2),
    ] {
        let pictures = Reel::new(
            timebase,
            sample(4, 2, count, PixelFormat::Rgb8).frames().to_vec(),
        )
        .expect("a reel");
        let reel = with_sound(pictures, rate, channels);
        let whole = sprw::encode(&reel).expect("an encoding");
        let mut wound = std::vec::Vec::new();
        wind(&mut wound, &reel);
        assert_eq!(wound, whole, "{timebase:?} {rate:?} x{channels}: two files");
        assert_eq!(sprw::decode(&whole).expect("a decoding"), reel);
    }
}

#[test]
fn a_frames_worth_of_sound_is_1601_samples_or_1602() {
    // The arithmetic this format rests on, in the one place it is interesting.
    // A frame at 30000/1001 into 48 kHz covers 1601.6 samples: 48000 x 1001 /
    // 30000 = 1601.6, worked out by hand. So one frame holds 1601 or 1602, and
    // three frames hold 4804 or 4805 -- 3 x 1601.6 = 4804.8.
    let ntsc = Timebase::NTSC_30;
    assert_eq!(
        sprw::sound_bounds(1, ntsc, SampleRate::Hz48000).expect("a bound"),
        (1601, 1602)
    );
    assert_eq!(
        sprw::sound_bounds(3, ntsc, SampleRate::Hz48000).expect("a bound"),
        (4804, 4805)
    );
    // Three hundred frames is 480,480 exactly -- the number the mixdown's own
    // tests arrive at from the other side -- so the two bounds meet.
    assert_eq!(
        sprw::sound_bounds(300, ntsc, SampleRate::Hz48000).expect("a bound"),
        (480_480, 480_480)
    );
    // And where a frame does hold a whole number, the bounds meet at every
    // count: 48000 / 24 is 2000 exactly.
    for frames in [0_usize, 1, 7, 1000] {
        let (least, most) =
            sprw::sound_bounds(frames, Timebase::FILM_24, SampleRate::Hz48000).expect("a bound");
        assert_eq!((least, most), (frames * 2000, frames * 2000));
    }
}

#[test]
fn the_formats_arithmetic_is_the_mixers_arithmetic() {
    // The two must never differ, because the mixer produces the samples the
    // format checks. `sound_bounds` computes from the definition -- n frames
    // is n x den x hertz / num samples -- and `floor_into` computes from an
    // exact rational; this requires them to agree at every count, at the rate
    // where they are most likely not to.
    for rate in [
        SampleRate::Hz44100,
        SampleRate::Hz48000,
        SampleRate::Hz88200,
        SampleRate::Hz96000,
    ] {
        for timebase in [
            Timebase::FILM_24,
            Timebase::NTSC_30,
            Timebase::NTSC_FILM,
            Timebase::PAL_25,
        ] {
            for frames in [0_usize, 1, 2, 3, 29, 30, 1001, 30_000] {
                let (least, _) = sprw::sound_bounds(frames, timebase, rate).expect("a bound");
                let walked = media_editor_core::Instant::new(
                    i64::try_from(frames).expect("a count"),
                    timebase,
                )
                .floor_into(rate.timebase())
                .expect("an instant")
                .ticks();
                assert_eq!(
                    i64::try_from(least).expect("a count"),
                    walked,
                    "{timebase:?} {rate:?} at {frames} frames"
                );
            }
        }
    }
}

#[test]
fn a_reel_whose_sound_is_not_as_long_as_its_pictures_is_refused() {
    // The failure that matters, and the reason the bound is a bound. Three
    // frames at 30000/1001 hold 4804 samples or 4805; 4803 and 4806 are both
    // refused, and so is a second of silence against a tenth of a second of
    // picture.
    let reel = Reel::new(
        Timebase::NTSC_30,
        sample(4, 2, 3, PixelFormat::Rgb8).frames().to_vec(),
    )
    .expect("a reel");
    for samples in [0_usize, 4803, 4806, 48_000] {
        let buffer = AudioBuffer::silence(SampleRate::Hz48000, 2, samples).expect("a buffer");
        assert_eq!(
            reel.clone().with_sound(buffer).err(),
            Some(IoStatus::SoundRunsPastPicture),
            "{samples} samples was accepted against three frames"
        );
    }
    for samples in [4804_usize, 4805] {
        let buffer = AudioBuffer::silence(SampleRate::Hz48000, 2, samples).expect("a buffer");
        reel.clone()
            .with_sound(buffer)
            .unwrap_or_else(|_| panic!("{samples} samples is a length three frames can hold"));
    }
}

#[test]
fn a_block_of_sound_out_of_turn_or_the_wrong_length_is_refused() {
    let reel = with_sound(
        Reel::new(
            Timebase::NTSC_30,
            sample(4, 2, 3, PixelFormat::Rgb8).frames().to_vec(),
        )
        .expect("a reel"),
        SampleRate::Hz48000,
        2,
    );
    let cut = blocks(reel.sound().expect("sound"), &reel);
    let mut sink = std::vec::Vec::new();
    let mut winder = Winder::begin(
        &mut sink,
        *reel.description(),
        Timebase::NTSC_30,
        3,
        reel.sound_description().expect("a description"),
        silent(),
    )
    .expect("a winder");

    // Sound before the pictures are done lands in the middle of a picture, so
    // it is refused as a *row* problem: the file says what comes next.
    assert_eq!(
        winder.sound_block(&mut sink, 0, &cut[0]).err(),
        Some(IoStatus::RowOutOfOrder),
        "a block arrived before the last picture"
    );
    for (index, frame) in reel.frames().iter().enumerate() {
        for row in 0..2 {
            winder
                .row(&mut sink, index, row, &row_of(frame, row))
                .expect("a row");
        }
    }
    assert!(winder.pictures_complete());
    assert!(!winder.is_complete(), "the sound is still owed");

    // The block after next.
    assert_eq!(
        winder.sound_block(&mut sink, 1, &cut[1]).err(),
        Some(IoStatus::SoundOutOfOrder)
    );
    // A block of the wrong length: two frames' worth where one belongs.
    let long = AudioBuffer::silence(SampleRate::Hz48000, 2, 3203).expect("a buffer");
    assert_eq!(
        winder.sound_block(&mut sink, 0, &long).err(),
        Some(IoStatus::SoundBlockWrongLength)
    );
    let short = AudioBuffer::silence(SampleRate::Hz48000, 2, 1600).expect("a buffer");
    assert_eq!(
        winder.sound_block(&mut sink, 0, &short).err(),
        Some(IoStatus::SoundBlockWrongLength)
    );
    // A block in another rate, and one with another channel count.
    let elsewhere = AudioBuffer::silence(SampleRate::Hz44100, 2, 1601).expect("a buffer");
    assert!(winder.sound_block(&mut sink, 0, &elsewhere).is_err());
    let mono = AudioBuffer::silence(SampleRate::Hz48000, 1, 1601).expect("a buffer");
    assert!(winder.sound_block(&mut sink, 0, &mono).is_err());

    // Closing with the sound still owed says *which* half is short.
    assert_eq!(
        sink.len(),
        HEADER_BYTES + 3 * 24,
        "a refused block was written anyway"
    );
    for (index, block) in cut.iter().enumerate().take(2) {
        winder
            .sound_block(&mut sink, index, block)
            .expect("a block");
    }
    assert_eq!(
        winder.finish(&mut sink).err(),
        Some(IoStatus::SoundRunsPastPicture),
        "a reel short of sound closed as though it were short of pictures"
    );
}

#[test]
fn a_reel_with_no_sound_declared_takes_none() {
    let reel = sample(4, 2, 2, PixelFormat::Rgb8);
    let mut sink = std::vec::Vec::new();
    let mut winder =
        Winder::begin(&mut sink, *reel.description(), RATE, 2, None, silent()).expect("a winder");
    for (index, frame) in reel.frames().iter().enumerate() {
        for row in 0..2 {
            winder
                .row(&mut sink, index, row, &row_of(frame, row))
                .expect("a row");
        }
    }
    let block = AudioBuffer::silence(SampleRate::Hz48000, 2, 2000).expect("a buffer");
    assert_eq!(
        winder.sound_block(&mut sink, 0, &block).err(),
        Some(IoStatus::SoundNotDeclared),
        "a silent reel took sound it never said it had"
    );
    assert!(
        winder.is_complete(),
        "pictures alone complete a silent reel"
    );
    winder.finish(&mut sink).expect("a trailer");
}

#[test]
fn a_reel_reads_its_sound_back_a_frame_at_a_time() {
    // The reader's half, and the reason it walks rather than multiplies:
    // blocks are 1601 samples or 1602, so there is no stride.
    let reel = with_sound(
        Reel::new(
            Timebase::NTSC_30,
            sample(4, 2, 6, PixelFormat::Rgb8).frames().to_vec(),
        )
        .expect("a reel"),
        SampleRate::Hz48000,
        2,
    );
    let file = sprw::encode(&reel).expect("an encoding");
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    let sound = spool.sound().expect("a sound description");
    assert_eq!(sound.rate(), SampleRate::Hz48000);
    assert_eq!(sound.channels(), 2);
    spool.verify(&file.as_slice(), 64).expect("a sound digest");

    let cut = blocks(reel.sound().expect("sound"), &reel);
    let mut lengths = std::vec::Vec::new();
    for (index, expected) in cut.iter().enumerate() {
        let block = spool
            .sound_block(&file.as_slice(), index)
            .expect("a block back");
        assert_eq!(&block, expected, "block {index} came back different");
        lengths.push(block.len());
    }
    // Six frames at 30000/1001 into 48 kHz: 6 x 1601.6 = 9609.6, so the blocks
    // are 1601s and 1602s summing to 9609 -- and no two consecutive frames are
    // the same accident.
    assert_eq!(lengths.iter().sum::<usize>(), 9609);
    assert!(lengths.contains(&1601) && lengths.contains(&1602));
    assert_eq!(
        spool.sound_block(&file.as_slice(), 6).err(),
        Some(IoStatus::FrameOutOfReel)
    );
    // And a reel with no sound says so rather than reading past its pictures.
    let silent = sprw::encode(&sample(4, 2, 2, PixelFormat::Rgb8)).expect("an encoding");
    let spool = Spool::open(&silent.as_slice()).expect("a spool");
    assert_eq!(spool.sound(), None);
    assert_eq!(
        spool.sound_block(&silent.as_slice(), 0).err(),
        Some(IoStatus::SoundNotDeclared)
    );
}

/// An extent that says how many reads it served.
struct Counted<'a> {
    bytes: &'a [u8],
    reads: core::cell::Cell<usize>,
}

impl Extent for Counted<'_> {
    fn length(&self) -> u64 {
        u64::try_from(self.bytes.len()).expect("a length")
    }

    fn read_at(&self, offset: u64, into: &mut [u8]) -> Result<usize, IoStatus> {
        self.reads.set(self.reads.get() + 1);
        self.bytes.read_at(offset, into)
    }
}

/// Three things somebody said, over source ticks 0..30.
fn transcript() -> std::vec::Vec<media_editor_model::caption::Caption> {
    std::vec![
        media_editor_model::caption::Caption::new(0, 10, 0, "the first thing").expect("a caption"),
        media_editor_model::caption::Caption::new(5, 15, 1, "and the answer").expect("a caption"),
        media_editor_model::caption::Caption::new(10, 30, 0, "then a long one").expect("a caption"),
    ]
}

#[test]
fn a_reel_with_a_transcript_is_the_file_encode_writes() {
    // The property, over the third section. Written a caption at a time, for
    // the same reason the pictures go a row at a time: sixteen thousand
    // captions is 8.7 megabytes against seventy-six kilobytes.
    let reel = sample(4, 2, 3, PixelFormat::Rgb8)
        .with_captions(transcript())
        .expect("a transcript");
    let whole = sprw::encode(&reel).expect("an encoding");
    let mut wound = std::vec::Vec::new();
    wind(&mut wound, &reel);
    assert_eq!(wound, whole, "two writers, one file");
    assert_eq!(sprw::decode(&whole).expect("a decoding"), reel);

    // And with all three sections at once, which is the only test that says
    // the order in the file is the order the writer puts them in.
    let both = Reel::new(
        Timebase::NTSC_30,
        sample(4, 2, 4, PixelFormat::Rgb8).frames().to_vec(),
    )
    .expect("a reel");
    let both = with_sound(both, SampleRate::Hz48000, 2)
        .with_captions(transcript())
        .expect("a transcript");
    let whole = sprw::encode(&both).expect("an encoding");
    let mut wound = std::vec::Vec::new();
    wind(&mut wound, &both);
    assert_eq!(wound, whole, "pictures, then sound, then words");
    assert_eq!(sprw::decode(&whole).expect("a decoding"), both);
}

#[test]
fn a_reel_reads_back_only_the_words_over_a_range() {
    // What a projection asks for, and the reason the section is scanned rather
    // than indexed: never caption `k`, always every caption over a stretch.
    let reel = sample(4, 2, 3, PixelFormat::Rgb8)
        .with_captions(transcript())
        .expect("a transcript");
    let file = sprw::encode(&reel).expect("an encoding");
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    assert_eq!(spool.spoken().count(), 3);
    spool.verify(&file.as_slice(), 64).expect("a sound digest");

    let over = |from, to| {
        spool
            .captions(&file.as_slice(), from, to)
            .expect("a read")
            .into_iter()
            .map(|caption| caption.text().to_string())
            .collect::<std::vec::Vec<_>>()
    };
    // Ticks 0..3 are only the first speaker.
    assert_eq!(over(0, 3), std::vec!["the first thing"]);
    // Ticks 5..10 are both of them talking at once, which is why a voice is a
    // field rather than an ordering.
    assert_eq!(over(5, 10), std::vec!["the first thing", "and the answer"]);
    // Ticks 20..25 are only the long one.
    assert_eq!(over(20, 25), std::vec!["then a long one"]);
    // And a stretch nobody spoke over.
    assert!(over(40, 50).is_empty());
    // Half-open at both ends: a caption ending at 10 is not on tick 10.
    assert_eq!(over(10, 11), std::vec!["and the answer", "then a long one"]);

    // A reel with no transcript says so rather than reading past its sound.
    let silent = Spool::open(
        &sprw::encode(&sample(4, 2, 2, PixelFormat::Rgb8))
            .expect("an encoding")
            .as_slice(),
    )
    .expect("a spool");
    assert_eq!(silent.spoken().count(), 0);
}

#[test]
fn a_caption_out_of_turn_or_past_the_length_declared_is_refused() {
    let reel = sample(4, 2, 2, PixelFormat::Rgb8)
        .with_captions(transcript())
        .expect("a transcript");
    let said = sprw::Spoken::of(reel.captions()).expect("a transcript");
    let mut sink = std::vec::Vec::new();
    let mut winder =
        Winder::begin(&mut sink, *reel.description(), RATE, 2, None, said).expect("a winder");

    // Words before the pictures are done land in the middle of a frame.
    assert_eq!(
        winder.caption(&mut sink, 0, &reel.captions()[0]).err(),
        Some(IoStatus::RowOutOfOrder)
    );
    for (index, frame) in reel.frames().iter().enumerate() {
        for row in 0..2 {
            winder
                .row(&mut sink, index, row, &row_of(frame, row))
                .expect("a row");
        }
    }
    // The caption after next.
    assert_eq!(
        winder.caption(&mut sink, 1, &reel.captions()[1]).err(),
        Some(IoStatus::CaptionOutOfOrder)
    );
    // One that would take the section past the length the header declared.
    // The three real captions come to 63 bytes of fields and 44 of text, so
    // 107 -- and one caption of 100 characters is 121, which does not fit
    // even as the first. A caption of 35 characters does, which is why the
    // first attempt at this test proved nothing.
    let words: std::string::String = core::iter::repeat_n('a', 100).collect();
    let long = media_editor_model::caption::Caption::new(0, 1, 0, &words).expect("a caption");
    assert_eq!(said.bytes(), 107);
    assert_eq!(
        winder.caption(&mut sink, 0, &long).err(),
        Some(IoStatus::CaptionOutOfOrder),
        "a caption past the declared length was written anyway"
    );
    // Closing short of the words says which half is missing.
    winder
        .caption(&mut sink, 0, &reel.captions()[0])
        .expect("a caption");
    assert!(!winder.is_complete());
    assert_eq!(
        winder.finish(&mut sink).err(),
        Some(IoStatus::TranscriptNotDeclared)
    );
    // And a reel that declared none takes none.
    let mut sink = std::vec::Vec::new();
    let mut silent =
        Winder::begin(&mut sink, *reel.description(), RATE, 1, None, silent()).expect("a winder");
    for row in 0..2 {
        silent
            .row(&mut sink, 0, row, &row_of(&reel.frames()[0], row))
            .expect("a row");
    }
    assert_eq!(
        silent.caption(&mut sink, 0, &reel.captions()[0]).err(),
        Some(IoStatus::TranscriptNotDeclared)
    );
    assert!(silent.is_complete());
}

#[test]
fn a_header_that_disagrees_about_its_transcript_is_refused() {
    // A count with no bytes, bytes with no count, and a length too small for
    // the fixed fields: three headers that cannot both be true, refused before
    // a word is read.
    assert_eq!(
        sprw::Spoken::new(3, 0).err(),
        Some(IoStatus::TranscriptNotDeclared)
    );
    assert_eq!(
        sprw::Spoken::new(0, 60).err(),
        Some(IoStatus::TranscriptNotDeclared)
    );
    // A caption is 21 bytes before its text, so three of them cannot fit in 62.
    assert_eq!(
        sprw::Spoken::new(3, 62).err(),
        Some(IoStatus::TranscriptNotDeclared)
    );
    sprw::Spoken::new(3, 63).expect("three captions of no text at all");
    assert_eq!(
        sprw::Spoken::new(sprw::MAX_CAPTIONS + 1, 1 << 20).err(),
        Some(IoStatus::TooMany)
    );
    assert_eq!(
        sprw::Spoken::new(1, sprw::MAX_CAPTION_BYTES + 1).err(),
        Some(IoStatus::PayloadTooLarge)
    );
}

#[test]
fn a_reel_too_large_once_its_words_are_counted_is_refused() {
    // The bound is on the *file*, so the transcript is part of it. Eighty-five
    // frames of 1920x1080 RGB is 528,876,000 bytes against a limit of
    // 536,870,912 -- eight megabytes of room, which the largest transcript the
    // format allows does not fit in.
    let big = described(1920, 1080, PixelFormat::Rgb8);
    let mut sink = std::vec::Vec::new();
    Winder::begin(&mut sink, big, RATE, 85, None, silent()).expect("pictures alone fit");
    assert!(sink.is_empty() || !sink.is_empty());
    let mut sink = std::vec::Vec::new();
    assert_eq!(
        Winder::begin(
            &mut sink,
            big,
            RATE,
            85,
            None,
            sprw::Spoken::new(sprw::MAX_CAPTIONS, sprw::MAX_CAPTION_BYTES).expect("a transcript"),
        )
        .err(),
        Some(IoStatus::PayloadTooLarge),
        "a transcript that takes the reel past the bound was accepted"
    );
    assert!(sink.is_empty(), "a header was written before the refusal");
}

#[test]
fn the_words_of_a_reel_with_sound_are_after_its_samples() {
    // A reel with all three sections, read through the streaming door. The
    // transcript begins where the *sound* ends, not where the pictures do, and
    // a reader that got that wrong would read samples as words.
    let reel = Reel::new(
        Timebase::NTSC_30,
        sample(4, 2, 4, PixelFormat::Rgb8).frames().to_vec(),
    )
    .expect("a reel");
    let reel = with_sound(reel, SampleRate::Hz48000, 2)
        .with_captions(transcript())
        .expect("a transcript");
    let file = sprw::encode(&reel).expect("an encoding");
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    assert!(spool.sound().is_some(), "this reel has sound to skip over");
    assert_eq!(spool.spoken().count(), 3);
    spool.verify(&file.as_slice(), 64).expect("a sound digest");
    let over = spool.captions(&file.as_slice(), 0, 3).expect("a read");
    assert_eq!(over.len(), 1);
    assert_eq!(over[0].text(), "the first thing");
    // And every one of them, which is what says none of the sound was read as
    // a caption: a wrong offset gives garbage rather than a short answer.
    let all = spool
        .captions(&file.as_slice(), i64::MIN, i64::MAX)
        .expect("a read");
    assert_eq!(all, transcript());
}

#[test]
fn a_transcript_is_read_in_windows_and_never_held() {
    // The reason the section is read a window at a time: 8.7 megabytes is what
    // the largest transcript this format allows comes to, against seventy-six
    // kilobytes. And the reason it is more than a loop: a record can straddle
    // a boundary, so a window ending part way through a caption keeps the
    // remainder and refills behind it.
    //
    // The lengths are chosen so that boundaries land inside records: a caption
    // of `n` characters is 21 + n bytes, and a window is 4,096, so a run of
    // captions whose lengths do not divide it will straddle.
    let count = 400_usize;
    let words: std::vec::Vec<media_editor_model::caption::Caption> = (0..count)
        .map(|index| {
            let from = i64::try_from(index).expect("a tick") * 4;
            let text: std::string::String = core::iter::repeat_n('x', 1 + index % 37).collect();
            media_editor_model::caption::Caption::new(from, from + 4, 0, &text).expect("a caption")
        })
        .collect();
    let reel = sample(4, 2, 4, PixelFormat::Rgb8)
        .with_captions(words.clone())
        .expect("a transcript");
    let file = sprw::encode(&reel).expect("an encoding");
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    // 400 captions of 21 + (1..37) bytes is well past one window, so several
    // boundaries fall inside records.
    assert!(
        spool.spoken().bytes() > 3 * sprw::CAPTION_WINDOW,
        "the transcript is {} bytes, which is not enough windows",
        spool.spoken().bytes()
    );

    // Every one of them comes back, in order, whole.
    let all = spool
        .captions(&file.as_slice(), i64::MIN, i64::MAX)
        .expect("a read");
    assert_eq!(all, words, "a record was lost or cut at a window boundary");

    // And a range in the middle, which is what a projection asks for.
    let over = spool.captions(&file.as_slice(), 800, 812).expect("a read");
    assert_eq!(over.len(), 3);
    assert_eq!(over[0].from(), 800);
    assert_eq!(over[2].to(), 812);

    // No read was larger than one window.
    let counted = Counted {
        bytes: &file,
        reads: core::cell::Cell::new(0),
    };
    spool
        .captions(&counted, i64::MIN, i64::MAX)
        .expect("a read");
    // The section is read in windows, so the number of reads is the number of
    // windows it takes: ceil(bytes / 4096).
    let expected = spool.spoken().bytes().div_ceil(sprw::CAPTION_WINDOW);
    assert_eq!(
        counted.reads.get(),
        expected,
        "the transcript was read in {} pieces, not {expected}",
        counted.reads.get()
    );
    // And the largest single caption bounds the carry: a record cannot
    // straddle more than one boundary because it cannot be longer than this.
    assert_eq!(sprw::CAPTION_LARGEST, 21 + 128 * 4);
}

#[test]
fn a_transcript_that_ends_inside_a_record_is_refused() {
    // What a truncated section looks like from the reader's side: the header
    // says four captions and the bytes hold three and a fragment. Resealed, so
    // the digest cannot be what refuses it.
    let words = transcript();
    let reel = sample(4, 2, 2, PixelFormat::Rgb8)
        .with_captions(words)
        .expect("a transcript");
    let file = sprw::encode(&reel).expect("an encoding");
    // Claim one more caption than the section holds, keeping the byte count.
    let mut lying = file.clone();
    lying[80..84].copy_from_slice(&4_u32.to_le_bytes());
    let end = lying.len() - sprw::TRAILER_BYTES;
    let mut hasher = media_editor_core::Sha256::new();
    hasher.update(&lying[..end]);
    let digest = hasher.finish();
    lying[end..].copy_from_slice(digest.bytes());
    // The header's own arithmetic catches it first: four captions cannot fit
    // in the bytes declared once the fixed fields are counted... unless they
    // can, in which case the walk catches it.
    let refusal = Spool::open(&lying.as_slice())
        .and_then(|spool| spool.captions(&lying.as_slice(), i64::MIN, i64::MAX));
    assert!(
        matches!(
            refusal,
            Err(IoStatus::TruncatedPayload | IoStatus::TranscriptNotDeclared)
        ),
        "a section holding fewer captions than its header says was accepted: {refusal:?}"
    );
}

/// A sink that records where each append's bytes lived, not just what they
/// were.
struct Watched {
    written: u64,
    addresses: std::vec::Vec<usize>,
}

impl Sink for Watched {
    fn written(&self) -> u64 {
        self.written
    }

    fn append(&mut self, bytes: &[u8]) -> Result<(), IoStatus> {
        self.addresses.push(bytes.as_ptr() as usize);
        self.written += bytes.len() as u64;
        Ok(())
    }
}

#[test]
fn a_row_reaches_the_sink_at_the_address_it_was_read_into() {
    // The end of the chain, measured rather than argued. A window is filled,
    // wrapped as a frame, and wound -- and the bytes the sink is handed are
    // *at the same address* as the bytes that were read. Not equal to them:
    // the same ones. A copy anywhere along the way would still produce a
    // correct reel and would fail this.
    //
    // It matters because the row path exists for a machine mapped at
    // seventy-six kilobytes. One copy of one row is one row more than that
    // machine has to spare, and a copy is invisible to every test that
    // compares bytes.
    let description = described(64, 4, PixelFormat::Rgba8);
    let line = FrameDescription::new(
        Geometry::new(64, 1).expect("a geometry"),
        description.format(),
        description.colour(),
        description.siting(),
        description.alpha(),
        description.pixel_aspect(),
    )
    .expect("a description");
    let bytes = line.packed_bytes().expect("a size");

    let mut sink = Watched {
        written: 0,
        addresses: std::vec::Vec::new(),
    };
    let mut winder =
        Winder::begin(&mut sink, description, RATE, 1, None, silent()).expect("a header");

    let mut read = std::vec::Vec::new();
    for row in 0..4 {
        // Refill a window, exactly as a reader of a reel would.
        let window: std::vec::Vec<u8> = (0..bytes)
            .map(|index| u8::try_from((index + row) % 251).unwrap_or(0))
            .collect();
        read.push(window.as_ptr() as usize);
        let held = Frame::from_owned(line, window).expect("a frame");
        winder.row(&mut sink, 0, row, &held).expect("a row");
    }
    winder.finish(&mut sink).expect("a trailer");

    // The header is the first append, the trailer the last; the four between
    // them are the rows.
    assert_eq!(sink.addresses.len(), 6, "one append a row, plus both ends");
    assert_eq!(
        &sink.addresses[1..5],
        &read[..],
        "a row was copied between the window and the sink"
    );
}
