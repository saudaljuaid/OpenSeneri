// SPDX-License-Identifier: GPL-3.0-only
//! The one picture format Phipia and this program already agree on.
//!
//! Every refusal here is one Phipia's own importer makes, checked against
//! `media_source_load_preview` in `src/kernel/ui.c` at 2.1.0. A bitmap this accepts
//! is one Phipia accepts; a bitmap it refuses is refused for the same reason.

use media_editor_core::Rational;
use media_editor_io::{IoStatus, bmp};
use media_editor_media::colour::{
    ColourDescription, MatrixCoefficients, Primaries, Range, TransferFunction,
};
use media_editor_media::{Frame, FrameDescription, Geometry, PixelFormat, Plane};

fn described(width: u32, height: u32) -> FrameDescription {
    FrameDescription::new(
        Geometry::new(width, height).expect("a geometry"),
        PixelFormat::Rgb8,
        ColourDescription {
            primaries: Primaries::Bt709,
            transfer: TransferFunction::Srgb,
            matrix: MatrixCoefficients::Identity,
            range: Range::Full,
        },
        None,
        None,
        Rational::ONE,
    )
    .expect("a description")
}

fn picture(width: u32, height: u32) -> Frame {
    let stride = (width as usize) * 3;
    let mut samples = std::vec::Vec::new();
    for row in 0..height as usize {
        for column in 0..width as usize {
            samples.push(u8::try_from((column * 7) % 256).expect("a byte"));
            samples.push(u8::try_from((row * 13) % 256).expect("a byte"));
            samples.push(u8::try_from((row + column) % 256).expect("a byte"));
        }
    }
    Frame::new(
        described(width, height),
        std::vec![Plane::new(samples, stride).expect("a plane")],
    )
    .expect("a frame")
}

#[test]
fn a_bitmap_goes_out_and_comes_back_the_same_picture() {
    // Three widths, and the middle one is the case that matters: a row of
    // five pixels is fifteen bytes, which is padded to sixteen. A decoder
    // that forgot the padding is exactly right at four and eight and wrong at
    // five, which is why a single test image proves nothing.
    for width in [4_u32, 5, 8] {
        let frame = picture(width, 3);
        let file = bmp::encode(&frame).expect("a bitmap");
        assert_eq!(
            bmp::decode(&file).expect("a frame"),
            frame,
            "a picture {width} wide did not survive"
        );
    }
}

#[test]
fn the_rows_run_bottom_up_and_the_channels_run_backwards() {
    // Both of this format's warts, pinned by hand rather than by round trip:
    // the first pixel in the file is the *bottom* left one, and its bytes are
    // blue, green, red.
    let frame = picture(2, 2);
    let file = bmp::encode(&frame).expect("a bitmap");
    let plane = frame.plane(0).expect("a plane");
    let samples = plane.samples();
    // The last row of the frame, first pixel: red, green, blue at stride.
    let bottom_left = &samples[plane.stride()..plane.stride() + 3];
    assert_eq!(
        &file[54..57],
        &[bottom_left[2], bottom_left[1], bottom_left[0]],
        "the file does not begin at the bottom, in blue-green-red order"
    );
}

#[test]
fn a_top_down_bitmap_is_read_the_other_way() {
    // A negative height means the rows run top-down. Written by taking a
    // bottom-up file, negating the height and reversing the rows: the same
    // picture, said the other way, and the decoder must agree.
    let frame = picture(4, 3);
    let file = bmp::encode(&frame).expect("a bitmap");
    let stride = 4_usize * 3;
    let padded = stride.div_ceil(4) * 4;
    let rows: std::vec::Vec<std::vec::Vec<u8>> = (0..3_usize)
        .map(|row| file[54 + row * padded..54 + (row + 1) * padded].to_vec())
        .collect();
    let mut flipped = file[..54].to_vec();
    flipped[22..26].copy_from_slice(&(-3_i32).to_le_bytes());
    for row in rows.iter().rev() {
        flipped.extend_from_slice(row);
    }
    assert_eq!(bmp::decode(&flipped).expect("a frame"), frame);
}

#[test]
fn what_is_not_a_bitmap_is_refused() {
    assert_eq!(bmp::decode(&[]), Err(IoStatus::NotABitmap));
    assert_eq!(bmp::decode(&[0_u8; 54]), Err(IoStatus::NotABitmap));
    let mut wrong = bmp::encode(&picture(4, 3)).expect("a bitmap");
    wrong[0] = b'X';
    assert_eq!(bmp::decode(&wrong), Err(IoStatus::NotABitmap));
}

#[test]
fn the_shapes_this_build_does_not_read_are_refused_as_such() {
    // Each of these is a real bitmap that this build declines, and the status
    // says "unsupported" rather than "not a bitmap" -- which is the difference
    // between "your file is broken" and "this program cannot read it yet".
    let good = bmp::encode(&picture(4, 3)).expect("a bitmap");
    for (at, bytes, what) in [
        (14_usize, 12_u32.to_le_bytes(), "an old-style header"),
        (28, 8_u32.to_le_bytes(), "eight bits a pixel"),
        (28, 32_u32.to_le_bytes(), "thirty-two bits a pixel"),
        (30, 1_u32.to_le_bytes(), "run-length encoding"),
        (26, 2_u32.to_le_bytes(), "two colour planes"),
    ] {
        let mut file = good.clone();
        // The plane and depth fields are sixteen bits, so only the low half
        // of each pattern is spliced where they sit.
        let width = if at == 26 || at == 28 { 2 } else { 4 };
        file[at..at + width].copy_from_slice(&bytes[..width]);
        assert_eq!(
            bmp::decode(&file),
            Err(IoStatus::BitmapUnsupported),
            "{what} was not refused as unsupported"
        );
    }
}

#[test]
fn a_bitmap_past_phipias_bounds_is_refused() {
    // 1920 by 1080 is what Phipia's importer accepts, so one pixel past it in
    // either direction is refused rather than read.
    let mut file = bmp::encode(&picture(4, 3)).expect("a bitmap");
    file[18..22].copy_from_slice(&1921_i32.to_le_bytes());
    assert_eq!(bmp::decode(&file), Err(IoStatus::BitmapTooLarge));
    let mut tall = bmp::encode(&picture(4, 3)).expect("a bitmap");
    tall[22..26].copy_from_slice(&1081_i32.to_le_bytes());
    assert_eq!(bmp::decode(&tall), Err(IoStatus::BitmapTooLarge));
    // And exactly the bound is accepted, in the header at least: the pixels
    // are not there, so it stops at the truncation instead.
    let mut edge = bmp::encode(&picture(4, 3)).expect("a bitmap");
    edge[18..22].copy_from_slice(&1920_i32.to_le_bytes());
    assert_eq!(bmp::decode(&edge), Err(IoStatus::TruncatedField));
}

#[test]
fn a_bitmap_that_ends_inside_its_pixels_is_refused() {
    let file = bmp::encode(&picture(8, 4)).expect("a bitmap");
    for short in [54_usize, 60, file.len() - 1] {
        assert_eq!(
            bmp::decode(&file[..short]),
            Err(IoStatus::TruncatedField),
            "a file cut at {short} was read anyway"
        );
    }
}

#[test]
fn a_height_that_cannot_be_turned_the_right_way_up_is_refused() {
    // The one arithmetic trap in the header: `i32::MIN` has no positive
    // counterpart, so a top-down picture of that height has no height at all.
    // Phipia refuses it by name and so does this.
    let mut file = bmp::encode(&picture(4, 3)).expect("a bitmap");
    file[22..26].copy_from_slice(&i32::MIN.to_le_bytes());
    assert_eq!(bmp::decode(&file), Err(IoStatus::NotABitmap));
    let mut nothing = bmp::encode(&picture(4, 3)).expect("a bitmap");
    nothing[22..26].copy_from_slice(&0_i32.to_le_bytes());
    assert_eq!(bmp::decode(&nothing), Err(IoStatus::NotABitmap));
    let mut backwards = bmp::encode(&picture(4, 3)).expect("a bitmap");
    backwards[18..22].copy_from_slice(&(-4_i32).to_le_bytes());
    assert_eq!(bmp::decode(&backwards), Err(IoStatus::NotABitmap));
}

#[test]
fn a_pixel_offset_that_points_outside_the_file_is_refused() {
    let mut file = bmp::encode(&picture(4, 3)).expect("a bitmap");
    let past = u32::try_from(file.len() + 1).expect("a length");
    file[10..14].copy_from_slice(&past.to_le_bytes());
    assert_eq!(bmp::decode(&file), Err(IoStatus::NotABitmap));
    let mut inside = bmp::encode(&picture(4, 3)).expect("a bitmap");
    inside[10..14].copy_from_slice(&10_u32.to_le_bytes());
    assert_eq!(
        bmp::decode(&inside),
        Err(IoStatus::NotABitmap),
        "an offset inside the headers points at the headers"
    );
}

#[test]
fn only_eight_bit_rgb_is_written() {
    // The encoder refuses what it cannot say rather than converting quietly:
    // a conversion chosen by a writer is a conversion nobody asked for.
    let grey = Frame::blank(
        FrameDescription::new(
            Geometry::new(4, 3).expect("a geometry"),
            PixelFormat::Gray8,
            ColourDescription {
                primaries: Primaries::Bt709,
                transfer: TransferFunction::Srgb,
                matrix: MatrixCoefficients::Identity,
                range: Range::Full,
            },
            None,
            None,
            Rational::ONE,
        )
        .expect("a description"),
    )
    .expect("a frame");
    assert_eq!(bmp::encode(&grey), Err(IoStatus::BitmapUnsupported));
}
