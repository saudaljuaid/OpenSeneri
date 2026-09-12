// SPDX-License-Identifier: GPL-3.0-only
//! A reel read a frame at a time, and a row at a time.
//!
//! `decode` builds every frame at once. A reel this build writes is bounded at
//! five hundred and twelve mebibytes, against the seventy-six kilobytes a
//! Phipia program is mapped — 6,899 times what there is, which makes it a
//! function that cannot be called on the machine this program is for.
//!
//! A spool holds a description, a rate and a count. Frame `k` lives at
//! `96 + k × packed_bytes`, which is arithmetic rather than a search, because
//! every frame in a reel shares one description and is therefore one size.

use media_editor_abi::seam::Slot;
use media_editor_core::{Digest, Rational, Timebase};
use media_editor_io::sprw::{HEADER_BYTES, MAX_FRAMES, Spool, TRAILER_BYTES};
use media_editor_io::vault::{Catalogue, Material, Stacks, Vault, store};
use media_editor_io::{Extent, IoStatus, MemoryStorage, Reel, sprw};
use media_editor_media::colour::{
    ColourDescription, MatrixCoefficients, Primaries, Range, TransferFunction,
};
use media_editor_media::{Frame, FrameDescription, Geometry, PixelFormat, Plane};

const RATE: Timebase = Timebase::FILM_24;

fn described(width: u32, height: u32, format: PixelFormat) -> FrameDescription {
    FrameDescription::new(
        Geometry::new(width, height).expect("a geometry"),
        format,
        ColourDescription {
            primaries: Primaries::Bt709,
            transfer: TransferFunction::Srgb,
            matrix: if format == PixelFormat::Yuv420p8 {
                MatrixCoefficients::Bt709
            } else {
                MatrixCoefficients::Identity
            },
            range: Range::Full,
        },
        if format == PixelFormat::Yuv420p8 {
            Some(media_editor_media::colour::ChromaSiting::Centre)
        } else {
            None
        },
        if format == PixelFormat::Rgba8 {
            Some(media_editor_media::colour::AlphaState::Premultiplied)
        } else {
            None
        },
        Rational::ONE,
    )
    .expect("a description")
}

/// A frame whose every byte is a function of where it is, so a read from the
/// wrong place is visible rather than plausible.
fn picture(width: u32, height: u32, format: PixelFormat, tint: u8) -> Frame {
    let description = described(width, height, format);
    let geometry = description.geometry();
    let mut planes = std::vec::Vec::new();
    for plane in 0..format.plane_count() {
        let stride = format.plane_row_bytes(geometry, plane).expect("a stride");
        let rows = format
            .plane_geometry(geometry, plane)
            .expect("a geometry")
            .height() as usize;
        let mut samples = std::vec::Vec::new();
        for row in 0..rows {
            for column in 0..stride {
                // Premultiplied coverage has to be at least as bright as the
                // colour beside it, so the alpha byte is full and the rest is
                // bounded below it.
                let value = if format == PixelFormat::Rgba8 && column % 4 == 3 {
                    255
                } else {
                    (row * 31 + column * 7 + plane * 101 + tint as usize) % 251
                };
                samples.push(u8::try_from(value).expect("a byte"));
            }
        }
        planes.push(Plane::new(samples, stride).expect("a plane"));
    }
    Frame::new(description, planes).expect("a frame")
}

fn reel(count: u8, width: u32, height: u32, format: PixelFormat) -> std::vec::Vec<u8> {
    let frames: std::vec::Vec<Frame> = (0..count)
        .map(|tint| picture(width, height, format, tint))
        .collect();
    sprw::encode(&Reel::new(RATE, frames).expect("a reel")).expect("an encoding")
}

#[test]
fn a_spool_reads_a_header_and_no_more() {
    let file = reel(6, 8, 4, PixelFormat::Rgb8);
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    assert_eq!(spool.len(), 6);
    assert!(!spool.is_empty());
    assert_eq!(spool.timebase(), RATE);
    assert_eq!(spool.description(), &described(8, 4, PixelFormat::Rgb8));
    // Eight by four, three bytes a pixel: ninety-six a frame, and six of them
    // between an eighty-eight-byte header and a thirty-two-byte trailer is the
    // whole file -- 88 + 576 + 32 = 696.
    assert_eq!(spool.frame_bytes(), 96);
    assert_eq!(file.len(), HEADER_BYTES + 6 * 96 + TRAILER_BYTES);
    assert_eq!(file.len(), 696);
}

#[test]
fn every_frame_comes_back_the_one_that_went_in() {
    for format in [
        PixelFormat::Rgb8,
        PixelFormat::Rgba8,
        PixelFormat::Gray8,
        PixelFormat::Yuv420p8,
    ] {
        let file = reel(5, 8, 4, format);
        let spool = Spool::open(&file.as_slice()).expect("a spool");
        for index in 0..5 {
            assert_eq!(
                spool.frame(&file.as_slice(), index).expect("a frame"),
                picture(8, 4, format, u8::try_from(index).expect("a tint")),
                "frame {index} of a {format:?} reel is not the one that went in"
            );
        }
    }
}

#[test]
fn a_spool_and_a_decode_agree_frame_for_frame() {
    // The two doors into one format, checked against each other. A streaming
    // reader that disagreed with the loading one would be a second answer to
    // the same question.
    let file = reel(4, 6, 5, PixelFormat::Rgba8);
    let loaded = sprw::decode(&file).expect("a decoding");
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    assert_eq!(spool.len(), loaded.len());
    assert_eq!(spool.description(), loaded.description());
    assert_eq!(spool.timebase(), loaded.timebase());
    for index in 0..loaded.len() {
        assert_eq!(
            &spool.frame(&file.as_slice(), index).expect("a frame"),
            &loaded.frames()[index]
        );
    }
}

#[test]
fn a_frame_past_the_reel_is_refused() {
    let file = reel(3, 4, 4, PixelFormat::Rgb8);
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    assert_eq!(
        spool.frame(&file.as_slice(), 3),
        Err(IoStatus::FrameOutOfReel)
    );
    assert_eq!(
        spool.plane_row(&file.as_slice(), 3, 0, 0, &mut [0_u8; 64]),
        Err(IoStatus::FrameOutOfReel)
    );
}

#[test]
fn a_row_comes_back_on_its_own() {
    // The read that actually fits: a row of a 1920-wide RGB picture is 5,760
    // bytes, against the seventy-six kilobytes a Phipia program is mapped.
    let file = reel(3, 8, 4, PixelFormat::Rgb8);
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    let mut row = [0_u8; 64];
    for index in 0..3_usize {
        let frame = picture(
            8,
            4,
            PixelFormat::Rgb8,
            u8::try_from(index).expect("a tint"),
        );
        let plane = frame.plane(0).expect("a plane");
        for line in 0..4_usize {
            let read = spool
                .plane_row(&file.as_slice(), index, 0, line, &mut row)
                .expect("a row");
            assert_eq!(read, 24, "eight pixels of three bytes");
            assert_eq!(
                &row[..read],
                &plane.samples()[line * plane.stride()..(line + 1) * plane.stride()],
                "row {line} of frame {index} came from the wrong place"
            );
        }
    }
}

#[test]
fn a_row_of_a_planar_frame_finds_the_right_plane() {
    // The case the offset arithmetic exists for: the planes are packed one
    // after another, so the chroma of a 420 frame begins after the whole of
    // its luma.
    let file = reel(2, 8, 4, PixelFormat::Yuv420p8);
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    let frame = picture(8, 4, PixelFormat::Yuv420p8, 1);
    let mut row = [0_u8; 64];
    for plane in 0..3_usize {
        let held = frame.plane(plane).expect("a plane");
        let rows = held.samples().len() / held.stride();
        for line in 0..rows {
            let read = spool
                .plane_row(&file.as_slice(), 1, plane, line, &mut row)
                .expect("a row");
            assert_eq!(read, held.stride());
            assert_eq!(
                &row[..read],
                &held.samples()[line * held.stride()..(line + 1) * held.stride()],
                "plane {plane} row {line} came from the wrong place"
            );
        }
    }
    // Luma is eight by four and chroma is four by two, so the frame is
    // 32 + 8 + 8 = 48 bytes and the second plane begins at 32.
    assert_eq!(spool.frame_bytes(), 48);
}

#[test]
fn a_plane_or_row_a_frame_does_not_have_is_refused() {
    let file = reel(2, 8, 4, PixelFormat::Rgb8);
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    let mut row = [0_u8; 64];
    assert_eq!(
        spool.plane_row(&file.as_slice(), 0, 1, 0, &mut row),
        Err(IoStatus::PlaneOutOfFrame),
        "an interleaved format has one plane"
    );
    assert_eq!(
        spool.plane_row(&file.as_slice(), 0, 0, 4, &mut row),
        Err(IoStatus::PlaneOutOfFrame),
        "a picture four high has no fifth row"
    );
}

#[test]
fn a_destination_shorter_than_a_row_is_refused() {
    // Refused rather than partly filled (R-1.4): a caller that received half
    // a row and no indication would draw half a row.
    let file = reel(2, 8, 4, PixelFormat::Rgb8);
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    let mut small = [0_u8; 23];
    assert_eq!(
        spool.plane_row(&file.as_slice(), 0, 0, 0, &mut small),
        Err(IoStatus::TooSmall)
    );
    let mut exact = [0_u8; 24];
    assert_eq!(
        spool.plane_row(&file.as_slice(), 0, 0, 0, &mut exact),
        Ok(24),
        "exactly the row is the case a bound gets wrong"
    );
}

#[test]
fn a_spool_refuses_what_a_decode_refuses() {
    // One door. Every header refusal is the same function's, so a streaming
    // reader cannot admit a file the loading one turns away.
    let good = reel(3, 4, 4, PixelFormat::Rgb8);
    for (at, byte, what) in [
        (0_usize, b'X', "the magic"),
        (4, 9, "the version"),
        (6, 1, "a reserved word"),
        (16, 99, "the pixel format"),
    ] {
        let mut file = good.clone();
        file[at] = byte;
        let loading = sprw::decode(&file);
        let streaming = Spool::open(&file.as_slice());
        assert!(loading.is_err(), "{what} was accepted by decode");
        assert_eq!(
            streaming.err(),
            loading.err(),
            "{what} is refused differently by the two doors"
        );
    }
}

#[test]
fn a_reel_that_is_not_the_length_its_header_says_is_refused() {
    let file = reel(3, 4, 4, PixelFormat::Rgb8);
    assert_eq!(
        Spool::open(&&file[..file.len() - 1]),
        Err(IoStatus::TruncatedPayload)
    );
    let mut longer = file.clone();
    longer.push(0);
    assert_eq!(
        Spool::open(&longer.as_slice()),
        Err(IoStatus::TrailingBytes)
    );
    assert_eq!(
        Spool::open(&&file[..HEADER_BYTES - 1]),
        Err(IoStatus::TruncatedHeader)
    );
}

#[test]
fn a_reel_of_no_frames_or_too_many_is_refused() {
    let mut none = reel(3, 4, 4, PixelFormat::Rgb8);
    none[56..64].copy_from_slice(&0_u64.to_le_bytes());
    assert_eq!(Spool::open(&none.as_slice()), Err(IoStatus::EmptyReel));
    let mut many = reel(3, 4, 4, PixelFormat::Rgb8);
    many[56..64].copy_from_slice(
        &u64::try_from(MAX_FRAMES + 1)
            .expect("a count")
            .to_le_bytes(),
    );
    assert_eq!(Spool::open(&many.as_slice()), Err(IoStatus::TooMany));
}

#[test]
fn opening_does_not_check_the_digest_and_verify_does() {
    // The trade, demonstrated rather than asserted. Checking the digest means
    // reading every sample, which is what a spool exists to avoid.
    let mut file = reel(3, 4, 4, PixelFormat::Rgb8);
    file[HEADER_BYTES + 5] ^= 0xFF;
    let spool = Spool::open(&file.as_slice()).expect("it opens");
    assert_eq!(spool.len(), 3, "and reads perfectly well");
    spool
        .frame(&file.as_slice(), 0)
        .expect("and hands back frames");
    assert_eq!(
        spool.verify(&file.as_slice(), 64),
        Err(IoStatus::DigestMismatch),
        "only verify looks"
    );
    // And `decode` does look, which is why the two exist.
    assert_eq!(sprw::decode(&file), Err(IoStatus::DigestMismatch));
}

#[test]
fn verify_walks_a_reel_in_windows() {
    let file = reel(4, 6, 5, PixelFormat::Rgba8);
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    for chunk in [1_usize, 7, 64, 4096] {
        spool
            .verify(&file.as_slice(), chunk)
            .unwrap_or_else(|refusal| panic!("a window of {chunk} refused: {refusal:?}"));
    }
    assert_eq!(spool.verify(&file.as_slice(), 0), Err(IoStatus::TooMany));
    // The digest covers the description as well as the samples, so a header
    // byte breaks it too. Every byte of the header is now inside that span:
    // the thirty-two that used to be the exception are at the other end of the
    // file, which is what version three moved.
    let mut described_change = file.clone();
    described_change[HEADER_BYTES - 1] ^= 0x01;
    let broken = Spool::open(&described_change.as_slice());
    assert!(
        broken.is_err()
            || broken
                .expect("a spool")
                .verify(&described_change.as_slice(), 64)
                .is_err(),
        "a change inside the described span passed"
    );
}

#[test]
fn a_reel_in_a_vault_is_read_without_loading_either() {
    // The whole chain, and the reason every link of it exists: a catalogue
    // holds a count rather than a vault, a material is one entry's bytes
    // rather than a vault's, a spool holds a description rather than a reel,
    // and a frame is one frame. Nothing on this path ever holds the file.
    use media_editor_render::Library;

    let file = reel(5, 8, 4, PixelFormat::Rgb8);
    let mut vault = Vault::new();
    let digest = vault
        .insert("a long walk on the beach.spr", &file)
        .expect("room");
    let mut storage = MemoryStorage::new(1 << 20);
    store(&vault, &mut storage).expect("a store");
    let before = storage.whole_reads();

    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    let entry = catalogue
        .find(&storage, digest)
        .expect("a search")
        .expect("the entry");
    assert_eq!(entry.name(), "a long walk on the beach.spr");

    let material = Material::new(&catalogue, &entry, &storage);
    assert_eq!(
        material.length(),
        u64::try_from(file.len()).expect("a length")
    );
    let spool = Spool::open(&material).expect("a spool");
    assert_eq!(spool.len(), 5);
    for index in 0..5 {
        assert_eq!(
            spool.frame(&material, index).expect("a frame"),
            picture(
                8,
                4,
                PixelFormat::Rgb8,
                u8::try_from(index).expect("a tint")
            ),
            "frame {index} through the whole chain is wrong"
        );
    }
    spool.verify(&material, 32).expect("a verification");
    assert_eq!(
        storage.whole_reads(),
        before,
        "something on this path read a whole slot"
    );

    // And the render's own questions, through the same chain.
    let looks: std::vec::Vec<(Digest, media_editor_render::Look)> = std::vec::Vec::new();
    let mut stacks = Stacks::new(&catalogue, &storage, &looks);
    assert!(stacks.available(digest));
    assert!(!stacks.available(Digest::of(b"never pasted in")));
    assert_eq!(
        stacks
            .frame(digest, 2, described(8, 4, PixelFormat::Rgb8))
            .expect("a frame"),
        picture(8, 4, PixelFormat::Rgb8, 2)
    );
    assert_eq!(
        stacks.frame(digest, 5, described(8, 4, PixelFormat::Rgb8)),
        Err(media_editor_render::RenderStatus::FrameAbsent)
    );
    assert_eq!(
        stacks.frame(digest, 0, described(4, 4, PixelFormat::Rgb8)),
        Err(media_editor_render::RenderStatus::SourceDescriptionMismatch)
    );
    assert_eq!(
        stacks.frame(
            Digest::of(b"never pasted in"),
            0,
            described(8, 4, PixelFormat::Rgb8)
        ),
        Err(media_editor_render::RenderStatus::MediaAbsent)
    );
    assert_eq!(
        storage.whole_reads(),
        before,
        "the render read a whole slot"
    );
}

#[test]
fn material_that_is_not_a_reel_says_unreadable() {
    use media_editor_render::Library;

    let mut vault = Vault::new();
    let digest = vault
        .insert("damaged.spr", b"SPRW and then nonsense")
        .expect("room");
    let mut storage = MemoryStorage::new(1 << 20);
    store(&vault, &mut storage).expect("a store");
    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    let looks: std::vec::Vec<(Digest, media_editor_render::Look)> = std::vec::Vec::new();
    let mut stacks = Stacks::new(&catalogue, &storage, &looks);
    assert!(stacks.available(digest), "the vault does hold it");
    assert_eq!(
        stacks.frame(digest, 0, described(8, 4, PixelFormat::Rgb8)),
        Err(media_editor_render::RenderStatus::MediaUnreadable)
    );
}

#[test]
fn an_extent_over_bytes_is_short_at_the_end() {
    let file = reel(2, 4, 4, PixelFormat::Rgb8);
    let extent = file.as_slice();
    let mut into = [0_u8; 32];
    assert_eq!(
        extent.length(),
        u64::try_from(file.len()).expect("a length")
    );
    let at = file.len() as u64 - 4;
    assert_eq!(extent.read_at(at, &mut into).expect("a read"), 4);
    assert_eq!(
        extent
            .read_at(file.len() as u64, &mut into)
            .expect("a read"),
        0
    );
    assert_eq!(extent.read_at(u64::MAX, &mut into).expect("a read"), 0);
}

#[test]
fn a_row_is_rendered_from_storage_without_loading_anything() {
    // The end of the whole chain, and the reason every link of it exists. A
    // graph asks for one row; the stacks ask a spool for one plane row; the
    // spool asks a material for a run of bytes; the material asks the
    // catalogue, which asks storage. Nothing on that path holds a vault, a
    // reel, or a frame.
    //
    // For a 1920-wide RGB picture the largest thing that moves is 5,760 bytes.
    use media_editor_render::{Graph, Node};

    let file = reel(4, 8, 4, PixelFormat::Rgba8);
    let mut vault = Vault::new();
    let digest = vault
        .insert("a shot with a long name.spr", &file)
        .expect("room");
    let mut storage = MemoryStorage::new(1 << 20);
    store(&vault, &mut storage).expect("a store");
    let before = storage.whole_reads();

    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    let looks: std::vec::Vec<(Digest, media_editor_render::Look)> = std::vec::Vec::new();
    let mut stacks = Stacks::new(&catalogue, &storage, &looks);

    let description = described(8, 4, PixelFormat::Rgba8);
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media: digest,
            tick: 2,
            description,
        })
        .expect("a node");
    let faded = graph
        .add(Node::Fade {
            input: source,
            opacity: Rational::new(1, 2).expect("an opacity"),
        })
        .expect("a node");

    // The whole frame and the rows must be the same picture, computed the two
    // ways, and the frame path is what says the rows are right.
    let whole = graph
        .evaluate(
            faded,
            &mut media_editor_media::FramePool::new(8, 1 << 16),
            &mut stacks,
        )
        .expect("a whole frame");
    let plane = whole.plane(0).expect("a plane");
    for row in 0..4_usize {
        let one = graph.row(faded, row, &mut stacks).expect("a row");
        assert_eq!(
            one.plane(0).expect("a plane").samples(),
            &plane.samples()[row * plane.stride()..(row + 1) * plane.stride()],
            "row {row} through storage disagrees with the whole frame"
        );
    }
    assert_eq!(
        storage.whole_reads(),
        before,
        "something on the row path read a whole slot"
    );
}

#[test]
fn a_row_of_material_that_is_not_there_says_so() {
    use media_editor_render::{Graph, Library, Node};

    let mut vault = Vault::new();
    vault
        .insert("present.spr", &reel(2, 4, 4, PixelFormat::Rgb8))
        .expect("room");
    let mut storage = MemoryStorage::new(1 << 20);
    store(&vault, &mut storage).expect("a store");
    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    let looks: std::vec::Vec<(Digest, media_editor_render::Look)> = std::vec::Vec::new();
    let mut stacks = Stacks::new(&catalogue, &storage, &looks);
    let description = described(4, 4, PixelFormat::Rgb8);
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media: Digest::of(b"never pasted in"),
            tick: 0,
            description,
        })
        .expect("a node");
    assert_eq!(
        graph.row(source, 0, &mut stacks),
        Err(media_editor_render::RenderStatus::MediaAbsent)
    );
    assert!(!Library::available(
        &mut stacks,
        Digest::of(b"never pasted in")
    ));
}

#[test]
fn a_row_path_never_needs_more_than_a_row_at_once() {
    // The number that decides whether a reader fits, and the one the counts
    // above cannot say: not how many reads, but how large the largest of them
    // was. A path that performs a thousand small reads runs on a small
    // machine; one that performs a single large read does not.
    use media_editor_render::{Graph, Node};

    // Sixty-four wide, so a row is 256 bytes -- larger than a catalogue entry
    // at 112 and a reel header at 96, which is what makes the assertion below
    // about the picture rather than about the bookkeeping.
    let file = reel(4, 64, 4, PixelFormat::Rgba8);
    let mut vault = Vault::new();
    let digest = vault.insert("a wide shot.spr", &file).expect("room");
    let description = described(64, 4, PixelFormat::Rgba8);

    let mut rows_only = MemoryStorage::new(1 << 20);
    store(&vault, &mut rows_only).expect("a store");
    let catalogue = Catalogue::open(&rows_only, Slot::Vault).expect("a catalogue");
    // From here, not from the store: a save reads its own scratch slot back
    // whole, and that would be the largest thing in every later measurement.
    rows_only.forget_reads();
    let looks: std::vec::Vec<(Digest, media_editor_render::Look)> = std::vec::Vec::new();
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media: digest,
            tick: 1,
            description,
        })
        .expect("a node");
    {
        let mut stacks = Stacks::new(&catalogue, &rows_only, &looks);
        for row in 0..4 {
            graph.row(source, row, &mut stacks).expect("a row");
        }
    }
    // A row of sixty-four RGBA pixels is 256 bytes, and nothing on the path
    // ever asked for more at once.
    assert_eq!(
        rows_only.largest_read(),
        256,
        "the row path needed more than one row at once"
    );

    // The whole-frame path through the same chain needs a frame: 64 x 4 x 4.
    let mut whole = MemoryStorage::new(1 << 20);
    store(&vault, &mut whole).expect("a store");
    let other = Catalogue::open(&whole, Slot::Vault).expect("a catalogue");
    whole.forget_reads();
    {
        let mut stacks = Stacks::new(&other, &whole, &looks);
        graph
            .evaluate(
                source,
                &mut media_editor_media::FramePool::new(8, 1 << 16),
                &mut stacks,
            )
            .expect("a whole frame");
    }
    assert_eq!(whole.largest_read(), 1024, "a frame is four rows of it");
    assert!(
        rows_only.largest_read() * 4 == whole.largest_read(),
        "the two paths differ by exactly the height of the picture"
    );
}

/// A reel with sound as long as its pictures, at a rate where a frame is not a
/// whole number of samples.
fn sounded(count: u8, timebase: Timebase) -> std::vec::Vec<u8> {
    use media_editor_audio::{AudioBuffer, SampleRate};
    let frames: std::vec::Vec<Frame> = (0..count)
        .map(|tint| picture(8, 4, PixelFormat::Rgb8, tint))
        .collect();
    let reel = Reel::new(timebase, frames).expect("a reel");
    let (samples, _) =
        sprw::sound_bounds(reel.len(), timebase, SampleRate::Hz48000).expect("a bound");
    let voices = (0..2_usize)
        .map(|channel| {
            (0..samples)
                .map(|index| {
                    // A function of position, so a run fetched from the wrong
                    // place is a different number rather than the same silence.
                    i32::try_from((index * 13 + channel * 5003) % 60_000).expect("a sample")
                        - 30_000
                })
                .collect()
        })
        .collect();
    let reel = reel
        .with_sound(AudioBuffer::new(SampleRate::Hz48000, voices).expect("a buffer"))
        .expect("sound as long as the pictures");
    sprw::encode(&reel).expect("an encoding")
}

/// An extent that says how many reads it served.
///
/// So that "which block does this run start in" can be checked by the number
/// of blocks read rather than only by the samples that came back. A reader
/// that started one block early would hand back the right samples after
/// throwing away a block, which is a wrong answer to a different question.
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

#[test]
fn a_run_reads_only_the_blocks_it_spans() {
    // The block a run starts in is found by halving rather than dividing,
    // because block nought runs to floor(1601.6) and sample 1601 is the first
    // of block *one* -- which a division puts in block nought. A reader that
    // started a block early would still hand back the right samples, having
    // read and discarded a block, so the samples alone cannot say.
    let file = sounded(6, Timebase::NTSC_30);
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    // Blocks begin at 0, 1601, 3203, 4804, 6406, 8008.
    for (start, count, blocks) in [
        (0_i64, 1_usize, 1_usize),
        (1600, 1, 1),
        (1601, 1, 1),
        (1601, 1602, 1),
        (1600, 3, 2),
        // 0..4805 reaches sample 4804, which is the *first* of block three --
        // so it is four blocks and not three. Worked out from the starts
        // above, and got wrong on the first attempt.
        (0, 4805, 4),
        (0, 4804, 3),
        // 4804..8008 is block three entire and block four entire.
        (4804, 3204, 2),
    ] {
        let counted = Counted {
            bytes: &file,
            reads: core::cell::Cell::new(0),
        };
        spool.samples(&counted, start, count).expect("a run");
        assert_eq!(
            counted.reads.get(),
            blocks,
            "a run of {count} from {start} read {} blocks, not {blocks}",
            counted.reads.get()
        );
    }
}

#[test]
fn a_run_of_samples_is_read_across_the_blocks_it_spans() {
    // What a *mixer* asks for, against what a recorder produced. A block is
    // 1601 samples or 1602 at 30000/1001, and a mixer's own block boundaries
    // are its own -- so a run of 2000 from sample 900 spans two stored blocks
    // and part of a third.
    let file = sounded(6, Timebase::NTSC_30);
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    let whole = sprw::decode(&file).expect("a reel");
    let sound = whole.sound().expect("sound");

    for (start, count) in [
        (0_i64, 1_usize),
        (0, 4805),
        (900, 2000),
        (1601, 1),
        (1600, 3),
        (5000, 4609),
    ] {
        let run = spool
            .samples(&file.as_slice(), start, count)
            .expect("a run of samples");
        assert_eq!(run.len(), count, "a run of {count} from {start}");
        for channel in 0..2 {
            let at = usize::try_from(start).expect("a start");
            assert_eq!(
                run.channel(channel).expect("a channel"),
                &sound.channel(channel).expect("a channel")[at..at + count],
                "channel {channel} of a run of {count} from {start}"
            );
        }
    }
    // Sample 1601 is the first of block *one*, because block nought runs to
    // floor(1601.6). A reader that divided by 1601.6 would put it in block
    // nought, which is why the block is found by halving rather than dividing.
    assert_eq!(
        spool
            .samples(&file.as_slice(), 1601, 1)
            .expect("a sample")
            .channel(0)
            .expect("a channel")[0],
        sound.channel(0).expect("a channel")[1601]
    );
    // A run past the end of the take is refused rather than short.
    assert_eq!(
        spool.samples(&file.as_slice(), 9600, 100).err(),
        Some(IoStatus::FrameOutOfReel)
    );
    // And a reel with no sound has none to give.
    let silent = Spool::open(&reel(3, 8, 4, PixelFormat::Rgb8).as_slice()).expect("a spool");
    assert!(silent.samples(&file.as_slice(), 0, 1).is_err());
}

#[test]
fn a_vault_serves_sound_without_loading_the_reel() {
    // The sound half of the chain the picture side has had since M8.33, and
    // the last link that was missing: a catalogue holds a count, a material is
    // one entry's bytes, a spool holds a description, and what comes off the
    // storage is one block of samples. Nothing on this path holds a vault, a
    // reel, or a take's sound.
    use media_editor_audio::SampleSource;

    let file = sounded(8, Timebase::NTSC_30);
    let mut vault = Vault::new();
    let digest = vault.insert("interview a.spr", &file).expect("room");
    let mut storage = MemoryStorage::new(1 << 20);
    store(&vault, &mut storage).expect("a store");
    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    let looks: std::vec::Vec<(Digest, media_editor_render::Look)> = std::vec::Vec::new();
    let mut stacks = Stacks::new(&catalogue, &storage, &looks);
    storage.forget_reads();

    let whole = sprw::decode(&file).expect("a reel");
    let sound = whole.sound().expect("sound");
    let run = stacks.samples(digest, 2000, 1500).expect("samples");
    assert_eq!(run.len(), 1500);
    assert_eq!(run.channel_count(), 2);
    assert_eq!(
        run.channel(1).expect("a channel"),
        &sound.channel(1).expect("a channel")[2000..3500]
    );

    // Not one whole-slot read, and the largest ranged read is one block of
    // samples: 1602 x 2 x 4 = 12,816 bytes. The vault itself is 12,896 bytes
    // of sound plus the pictures and the store's own header, and none of it
    // was ever held.
    assert_eq!(storage.whole_reads(), 0, "something loaded a slot");
    assert!(
        storage.largest_read() <= 12_816,
        "the largest read was {} bytes",
        storage.largest_read()
    );
    assert!(storage.ranged_reads() > 0, "nothing was read at all");

    // And media the vault does not hold is refused rather than answered with
    // silence, which is the same decision the picture side makes.
    assert!(
        stacks
            .samples(media_editor_core::Digest::of(b"not here"), 0, 10)
            .is_err()
    );
}

#[test]
fn a_vault_serves_the_words_without_loading_the_reel() {
    // The third thing this chain serves, and the last a projection needs: a
    // picture, a run of samples, and the words that were said. None of them
    // holds the vault, the reel, or the take.
    use media_editor_model::caption::Transcript;

    let frames: std::vec::Vec<Frame> = (0..4)
        .map(|tint| picture(8, 4, PixelFormat::Rgb8, tint))
        .collect();
    let take = Reel::new(RATE, frames)
        .expect("a reel")
        .with_captions(std::vec![
            media_editor_model::caption::Caption::new(0, 10, 0, "so I said to him")
                .expect("a caption"),
            media_editor_model::caption::Caption::new(10, 20, 1, "and what did he say")
                .expect("a caption"),
        ])
        .expect("a transcript");
    let file = sprw::encode(&take).expect("an encoding");

    let mut vault = Vault::new();
    let digest = vault.insert("interview b.spr", &file).expect("room");
    let mut storage = MemoryStorage::new(1 << 20);
    store(&vault, &mut storage).expect("a store");
    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    let looks: std::vec::Vec<(Digest, media_editor_render::Look)> = std::vec::Vec::new();
    let mut stacks = Stacks::new(&catalogue, &storage, &looks);
    storage.forget_reads();

    let over = |stacks: &mut Stacks<'_>, from, to| {
        stacks
            .captions(digest, from, to)
            .expect("a read")
            .into_iter()
            .map(|caption| caption.text().to_string())
            .collect::<std::vec::Vec<_>>()
    };
    assert_eq!(over(&mut stacks, 0, 5), std::vec!["so I said to him"]);
    assert_eq!(over(&mut stacks, 12, 14), std::vec!["and what did he say"]);
    assert!(over(&mut stacks, 30, 40).is_empty());
    assert_eq!(storage.whole_reads(), 0, "something loaded a slot");

    // Media the vault does not hold, and media that holds no transcript, both
    // answer with nothing rather than refusing -- which is what lets a
    // programme of captioned and uncaptioned shots be asked about at all.
    assert!(
        stacks
            .captions(media_editor_core::Digest::of(b"not here"), 0, 100)
            .expect("a read")
            .is_empty()
    );
    let silent = reel(3, 8, 4, PixelFormat::Rgb8);
    let mut vault = Vault::new();
    let quiet = vault.insert("b roll.spr", &silent).expect("room");
    let mut storage = MemoryStorage::new(1 << 20);
    store(&vault, &mut storage).expect("a store");
    let catalogue = Catalogue::open(&storage, Slot::Vault).expect("a catalogue");
    let mut stacks = Stacks::new(&catalogue, &storage, &looks);
    assert!(stacks.captions(quiet, 0, 100).expect("a read").is_empty());
}
