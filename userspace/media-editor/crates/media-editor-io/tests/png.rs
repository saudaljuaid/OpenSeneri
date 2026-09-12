// SPDX-License-Identifier: GPL-3.0-only
//! Writing a frame out so that a failure can be looked at.
//!
//! Every expectation here is arrived at from the PNG and zlib specifications
//! rather than from running the code — the checksums especially, because a
//! checksum test that used the implementation's own answer would pass for any
//! implementation.

use media_editor_io::IoStatus;
use media_editor_io::png::{self, SIGNATURE};
use media_editor_media::colour::AlphaState;
use media_editor_media::{ColourDescription, Frame, FrameDescription, Geometry, PixelFormat};

fn described(format: PixelFormat, alpha: Option<AlphaState>) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(4, 3).expect("a geometry"),
        format,
        ColourDescription::srgb_full(),
        None,
        alpha,
    )
    .expect("a description")
}

/// A frame of one repeated pixel.
fn flat(description: FrameDescription, pixel: &[u8]) -> Frame {
    let wanted = description.packed_bytes().expect("a size");
    let bytes: std::vec::Vec<u8> = pixel.iter().copied().cycle().take(wanted).collect();
    Frame::from_packed(description, &bytes).expect("a frame")
}

/// The chunks of a file, as (name, data).
fn chunks(file: &[u8]) -> std::vec::Vec<(std::string::String, std::vec::Vec<u8>)> {
    let mut found = std::vec::Vec::new();
    let mut at = SIGNATURE.len();
    while at + 12 <= file.len() {
        let length = u32::from_be_bytes([file[at], file[at + 1], file[at + 2], file[at + 3]]);
        let length = usize::try_from(length).expect("a length");
        let name = std::string::String::from_utf8(file[at + 4..at + 8].to_vec()).expect("a name");
        let data = file[at + 8..at + 8 + length].to_vec();
        found.push((name, data));
        at += 12 + length;
    }
    found
}

#[test]
fn a_capture_is_a_png_by_its_own_first_eight_bytes() {
    // The signature is not arbitrary: the high bit catches a seven-bit
    // transport, the carriage return and line feed pair catches a transfer
    // that translated line endings, and the substitute character stops a file
    // being printed to a terminal. Each byte is checked because each one is
    // there for a reason.
    let file =
        png::encode(&flat(described(PixelFormat::Rgb8, None), &[10, 20, 30])).expect("a capture");
    assert_eq!(
        &file[..8],
        &[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]
    );
}

#[test]
fn the_header_says_what_the_frame_is() {
    // Thirteen bytes, in the order the format fixes: width, height, bit depth,
    // colour type, then three zeroes that say the only compression, filtering
    // and interlacing this file uses are the ones every reader must support.
    for (format, alpha, expected_type) in [
        (PixelFormat::Gray8, None, 0_u8),
        (PixelFormat::Rgb8, None, 2),
        (PixelFormat::Rgba8, Some(AlphaState::Straight), 6),
    ] {
        let pixel = std::vec![7_u8; 4];
        let file = png::encode(&flat(described(format, alpha), &pixel)).expect("a capture");
        let held = chunks(&file);
        assert_eq!(held[0].0, "IHDR");
        let header = &held[0].1;
        assert_eq!(header.len(), 13);
        assert_eq!(
            u32::from_be_bytes([header[0], header[1], header[2], header[3]]),
            4
        );
        assert_eq!(
            u32::from_be_bytes([header[4], header[5], header[6], header[7]]),
            3
        );
        assert_eq!(header[8], 8, "bit depth");
        assert_eq!(header[9], expected_type, "colour type for {format:?}");
        assert_eq!(&header[10..], &[0, 0, 0], "compression, filter, interlace");
    }
}

#[test]
fn the_chunks_are_the_three_a_reader_needs_and_no_others() {
    let file =
        png::encode(&flat(described(PixelFormat::Rgb8, None), &[1, 2, 3])).expect("a capture");
    let held = chunks(&file);
    let names: std::vec::Vec<&str> = held.iter().map(|(name, _)| name.as_str()).collect();
    assert_eq!(names, std::vec!["IHDR", "IDAT", "IEND"]);
    // And the file ends exactly at the end of the last chunk, with nothing
    // after it: a reader that stopped at IEND would not notice trailing
    // rubbish, so this is checked here rather than trusted.
    let total: usize = held.iter().map(|(_, data)| data.len() + 12).sum();
    assert_eq!(file.len(), SIGNATURE.len() + total);
}

#[test]
fn every_chunk_carries_the_crc_the_format_defines() {
    // The one thing about PNG's chunk layout that is easy to get wrong: the
    // length covers the data alone and the CRC covers the *name and* the data.
    // Computed here from the polynomial rather than from the implementation,
    // because a checksum test that used the implementation's own answer would
    // pass for any implementation.
    fn crc32(bytes: &[u8]) -> u32 {
        let mut value = 0xFFFF_FFFF_u32;
        for byte in bytes {
            value ^= u32::from(*byte);
            for _ in 0..8 {
                let carry = value & 1;
                value >>= 1;
                if carry != 0 {
                    value ^= 0xEDB8_8320;
                }
            }
        }
        value ^ 0xFFFF_FFFF
    }

    let file = png::encode(&flat(
        described(PixelFormat::Rgba8, Some(AlphaState::Straight)),
        &[9, 8, 7, 255],
    ))
    .expect("a capture");

    let mut at = SIGNATURE.len();
    let mut checked = 0;
    while at + 12 <= file.len() {
        let length = usize::try_from(u32::from_be_bytes([
            file[at],
            file[at + 1],
            file[at + 2],
            file[at + 3],
        ]))
        .expect("a length");
        let covered = &file[at + 4..at + 8 + length];
        let stored = u32::from_be_bytes([
            file[at + 8 + length],
            file[at + 9 + length],
            file[at + 10 + length],
            file[at + 11 + length],
        ]);
        assert_eq!(stored, crc32(covered), "chunk at {at}");
        at += 12 + length;
        checked += 1;
    }
    assert_eq!(checked, 3, "not every chunk was checked");
}

#[test]
fn the_pixel_stream_is_a_legal_zlib_stream_of_the_scanlines() {
    // Decoded here by hand rather than by a library: two header bytes, then
    // stored DEFLATE blocks, then Adler-32 of what was stored. Every scanline
    // begins with its filter type, which is nought — the filter that does
    // nothing, so the bytes on disc still resemble the pixels.
    let pixel = [11_u8, 22, 33];
    let frame = flat(described(PixelFormat::Rgb8, None), &pixel);
    let file = png::encode(&frame).expect("a capture");
    let data = &chunks(&file)[1].1;

    assert_eq!(data[0], 0x78, "not a zlib header");
    assert_eq!(data[1], 0x01, "not the level this writes");
    // The two header bytes together are a multiple of thirty-one, which is the
    // check they carry.
    assert_eq!((u16::from(data[0]) * 256 + u16::from(data[1])) % 31, 0);

    let mut raw = std::vec::Vec::new();
    let mut at = 2;
    loop {
        let last = data[at] & 1;
        assert_eq!(data[at] & 0b110, 0, "not a stored block");
        let length = usize::from(u16::from_le_bytes([data[at + 1], data[at + 2]]));
        let complement = u16::from_le_bytes([data[at + 3], data[at + 4]]);
        assert_eq!(
            complement,
            !u16::try_from(length).expect("a length"),
            "a stored block's only redundancy is wrong"
        );
        raw.extend_from_slice(&data[at + 5..at + 5 + length]);
        at += 5 + length;
        if last == 1 {
            break;
        }
    }

    // Adler-32 of the uncompressed data, computed from its definition.
    let mut low = 1_u32;
    let mut high = 0_u32;
    for byte in &raw {
        low = (low + u32::from(*byte)) % 65_521;
        high = (high + low) % 65_521;
    }
    let expected = (high << 16) | low;
    let stored = u32::from_be_bytes([data[at], data[at + 1], data[at + 2], data[at + 3]]);
    assert_eq!(stored, expected, "the Adler-32 is wrong");
    assert_eq!(at + 4, data.len(), "there are bytes after the checksum");

    // Three rows of four pixels, each row a filter byte and twelve samples.
    assert_eq!(raw.len(), 3 * (1 + 12));
    for row in 0..3 {
        let start = row * 13;
        assert_eq!(raw[start], 0, "row {row} is filtered");
        for column in 0..4 {
            let at = start + 1 + column * 3;
            assert_eq!(&raw[at..at + 3], &pixel, "row {row} pixel {column}");
        }
    }
}

#[test]
fn premultiplied_coverage_is_refused_rather_than_written_dark() {
    // A PNG stores non-premultiplied colour. A premultiplied frame written as
    // though it were straight is a picture darker than the one that was
    // rendered — worst exactly where coverage is lowest, which is the edge
    // somebody is usually looking at.
    //
    // Undoing the association here would be lossy, and lossy in a reference
    // capture is worse than anywhere else: the whole point is to see what
    // actually happened.
    let straight = described(PixelFormat::Rgba8, Some(AlphaState::Straight));
    assert!(png::encode(&flat(straight, &[200, 100, 50, 128])).is_ok());

    let premultiplied = described(PixelFormat::Rgba8, Some(AlphaState::Premultiplied));
    assert_eq!(
        png::encode(&flat(premultiplied, &[100, 50, 25, 128])).map(|_| ()),
        Err(IoStatus::PngPremultiplied)
    );
}

#[test]
fn a_format_a_png_cannot_hold_is_refused() {
    // A luma-chroma frame needs the matrix taken out of it first, which is a
    // named step and lives elsewhere. Writing its planes into a PNG as though
    // they were colour would produce a file that opens and is wrong.
    for format in [
        PixelFormat::Yuv444p8,
        PixelFormat::Yuv422p8,
        PixelFormat::Yuv420p8,
    ] {
        let description = FrameDescription::square(
            Geometry::new(4, 2).expect("a geometry"),
            format,
            ColourDescription::bt709_limited(),
            if format.is_subsampled() {
                Some(media_editor_media::ChromaSiting::Centre)
            } else {
                None
            },
            None,
        )
        .expect("a description");
        assert_eq!(
            png::encode(&Frame::blank(description).expect("a frame")).map(|_| ()),
            Err(IoStatus::PngFormatUnsupported),
            "{format:?} was accepted"
        );
    }
}

#[test]
fn every_row_is_its_own_row_and_the_checksum_wraps() {
    // Vary every row and use enough data to wrap both Adler-32 accumulators.
    let width = 64_usize;
    let height = 40_usize;
    let description = FrameDescription::square(
        Geometry::new(
            u32::try_from(width).expect("a width"),
            u32::try_from(height).expect("a height"),
        )
        .expect("a geometry"),
        PixelFormat::Gray8,
        ColourDescription::srgb_full(),
        None,
        None,
    )
    .expect("a description");
    let mut packed = std::vec::Vec::new();
    for row in 0..height {
        for column in 0..width {
            // Differs down *and* across, so neither a repeated row nor a
            // repeated column survives.
            packed.push(u8::try_from((row * 7 + column * 3) % 256).expect("a sample"));
        }
    }
    let frame = Frame::from_packed(description, &packed).expect("a frame");
    let file = png::encode(&frame).expect("a capture");
    let data = &chunks(&file)[1].1;

    let mut raw = std::vec::Vec::new();
    let mut at = 2;
    loop {
        let last = data[at] & 1;
        let length = usize::from(u16::from_le_bytes([data[at + 1], data[at + 2]]));
        raw.extend_from_slice(&data[at + 5..at + 5 + length]);
        at += 5 + length;
        if last == 1 {
            break;
        }
    }

    for row in 0..height {
        let start = row * (1 + width);
        assert_eq!(raw[start], 0, "row {row} is filtered");
        assert_eq!(
            &raw[start + 1..start + 1 + width],
            &packed[row * width..(row + 1) * width],
            "row {row} is not the row that was rendered"
        );
    }

    // Adler-32 over the whole stream, and this time the sums genuinely wrap:
    // the running total of the bytes alone passes 65,521 many times.
    let mut low = 1_u32;
    let mut high = 0_u32;
    let mut wrapped = false;
    for byte in &raw {
        low += u32::from(*byte);
        if low >= 65_521 {
            wrapped = true;
        }
        low %= 65_521;
        high = (high + low) % 65_521;
    }
    assert!(
        wrapped,
        "the fixture never reaches the modulus, so this proves nothing"
    );
    let expected = (high << 16) | low;
    let stored = u32::from_be_bytes([data[at], data[at + 1], data[at + 2], data[at + 3]]);
    assert_eq!(stored, expected, "the Adler-32 is wrong");
}

#[test]
fn a_capture_is_the_same_capture_every_time() {
    let frame = flat(described(PixelFormat::Rgb8, None), &[3, 5, 7]);
    assert_eq!(
        png::encode(&frame).expect("a capture"),
        png::encode(&frame).expect("a capture")
    );
}
