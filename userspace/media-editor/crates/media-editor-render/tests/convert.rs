// SPDX-License-Identifier: GPL-3.0-only
//! Converting frames between descriptions.

use media_editor_media::colour::{MatrixCoefficients, Primaries, Range, TransferFunction};
use media_editor_media::{
    AlphaState, ChromaSiting, ColourDescription, Frame, FrameDescription, Geometry, PixelFormat,
    TestPattern,
};
use media_editor_render::{RenderStatus, convert};

fn described(
    width: u32,
    height: u32,
    format: PixelFormat,
    colour: ColourDescription,
) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(width, height).expect("a geometry"),
        format,
        colour,
        None,
        if format.has_alpha() {
            Some(AlphaState::Straight)
        } else {
            None
        },
    )
    .expect("a description")
}

fn srgb(width: u32, height: u32, format: PixelFormat) -> FrameDescription {
    described(width, height, format, ColourDescription::srgb_full())
}

/// How far a converted sample may sit from its expected code value.
///
/// One code value. A round trip through light and back is two quantisations
/// and a pile of exact arithmetic between them, and landing within one step of
/// where it started is what an eight-bit pipeline can promise.
const TOLERANCE: i32 = 1;

fn close(actual: &[u8], expected: &[u8], note: &str) {
    assert_eq!(actual.len(), expected.len(), "{note}: different lengths");
    for (index, (left, right)) in actual.iter().zip(expected.iter()).enumerate() {
        let difference = i32::from(*left) - i32::from(*right);
        assert!(
            difference.abs() <= TOLERANCE,
            "{note}: sample {index} is {left}, expected {right}"
        );
    }
}

#[test]
fn converting_to_the_same_description_changes_nothing() {
    // Not approximately nothing. A pipeline that shifted samples when asked to
    // do nothing would shift them again on every pass.
    for format in [PixelFormat::Rgb8, PixelFormat::Rgba8, PixelFormat::Gray8] {
        let description = srgb(16, 9, format);
        let frame = TestPattern::Bars.render(description).expect("a frame");
        let converted = convert(&frame, description).expect("a conversion");
        assert_eq!(
            converted.digest(),
            frame.digest(),
            "{format:?} was disturbed by a conversion to itself"
        );
    }
}

#[test]
fn dropping_and_regaining_alpha_is_not_a_conversion() {
    // Dropping coverage requires compositing against a background; restoring
    // it requires choosing transparent pixels. Neither is a colour conversion
    // (R-1.3).
    let with_alpha = srgb(8, 4, PixelFormat::Rgba8);
    let without = srgb(8, 4, PixelFormat::Rgb8);
    let frame = TestPattern::Bars.render(with_alpha).expect("a frame");
    assert_eq!(
        convert(&frame, without),
        Err(RenderStatus::ConversionUnavailable)
    );

    let opaque = TestPattern::Bars.render(without).expect("a frame");
    assert_eq!(
        convert(&opaque, with_alpha),
        Err(RenderStatus::ConversionUnavailable)
    );

    // The colour still converts, with the alpha channel kept as it was.
    let wide = described(
        8,
        4,
        PixelFormat::Rgba8,
        ColourDescription::new(
            Primaries::Bt2020,
            TransferFunction::Bt2020Ten,
            MatrixCoefficients::Identity,
            Range::Full,
        ),
    );
    let converted = convert(&frame, wide).expect("a conversion");
    let before = frame.to_packed().expect("bytes");
    let after = converted.to_packed().expect("bytes");
    for (one, other) in before.chunks_exact(4).zip(after.chunks_exact(4)) {
        assert_eq!(one[3], other[3], "coverage is carried, not recomputed");
    }
}

#[test]
fn a_change_of_transfer_function_keeps_the_highlights_and_crushes_the_shadows() {
    // Eight-bit linear light is a bad container and this test says how bad.
    // sRGB spends most of its code values below middle grey, and linear
    // spends almost none there: sRGB 17 is about six thousandths of full
    // scale, which is one and a half linear code values. Coming back gives 13.
    //
    // That is not a defect in the conversion. It is the reason every
    // intermediate format in this industry is ten bits, twelve bits, or
    // floating point, and it is asserted here so nobody later "fixes" it.
    let source = srgb(16, 9, PixelFormat::Rgb8);
    let linear = described(
        16,
        9,
        PixelFormat::Rgb8,
        ColourDescription::new(
            Primaries::Bt709,
            TransferFunction::Linear,
            MatrixCoefficients::Identity,
            Range::Full,
        ),
    );
    let frame = TestPattern::Ramp.render(source).expect("a frame");
    let in_light = convert(&frame, linear).expect("a conversion");
    let back = convert(&in_light, source).expect("a conversion");

    let original = frame.to_packed().expect("bytes");
    let returned = back.to_packed().expect("bytes");
    for (index, (left, right)) in returned.iter().zip(original.iter()).enumerate() {
        let difference = i32::from(*left) - i32::from(*right);
        if *right >= 128 {
            assert!(
                difference.abs() <= TOLERANCE,
                "sample {index} is bright and should survive: {left} from {right}"
            );
        } else {
            // Below middle grey the loss grows toward black, and it is bounded
            // by what linear eight-bit can hold.
            assert!(
                difference.abs() <= 6,
                "sample {index} lost more than eight-bit linear explains: {left} from {right}"
            );
        }
    }
    assert_eq!(returned[0], 0, "black is still black");
    assert_eq!(
        *returned.last().expect("a sample"),
        255,
        "white is still white"
    );
}

#[test]
fn a_change_of_gamut_survives_a_round_trip() {
    let narrow = srgb(16, 9, PixelFormat::Rgb8);
    let wide = described(
        16,
        9,
        PixelFormat::Rgb8,
        ColourDescription::new(
            Primaries::Bt2020,
            TransferFunction::Srgb,
            MatrixCoefficients::Identity,
            Range::Full,
        ),
    );
    // A ramp is grey, so it survives a gamut change exactly; bars contain
    // saturated primaries that BT.709 cannot hold once they have been through
    // BT.2020, which is a real property of gamuts rather than a defect.
    let frame = TestPattern::Ramp.render(narrow).expect("a frame");
    let widened = convert(&frame, wide).expect("a conversion");
    let back = convert(&widened, narrow).expect("a conversion");
    close(
        &back.to_packed().expect("bytes"),
        &frame.to_packed().expect("bytes"),
        "709 to 2020 and back",
    );
}

#[test]
fn a_gamut_change_actually_changes_saturated_colour() {
    // A round trip proves nothing on its own: skipping the matrix in both
    // directions is also a round trip. This is the test that notices.
    //
    // BT.709's red sits well inside BT.2020, so expressing it in BT.2020
    // needs green and blue as well — its coordinates there are roughly
    // (0.627, 0.069, 0.016) in light, which is a visibly less saturated
    // triplet. A pipeline that skipped the matrix would hand back pure red.
    let narrow = srgb(2, 2, PixelFormat::Rgb8);
    let wide = described(
        2,
        2,
        PixelFormat::Rgb8,
        ColourDescription::new(
            Primaries::Bt2020,
            TransferFunction::Srgb,
            MatrixCoefficients::Identity,
            Range::Full,
        ),
    );
    let red = Frame::from_packed(
        narrow,
        &std::vec![255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0],
    )
    .expect("a frame");
    let widened = convert(&red, wide).expect("a conversion");
    let bytes = widened.to_packed().expect("bytes");

    assert!(bytes[0] < 240, "red must come down: it was {}", bytes[0]);
    assert!(bytes[1] > 40, "green must come up: it was {}", bytes[1]);
    assert!(bytes[2] > 20, "blue must come up: it was {}", bytes[2]);
    assert!(
        bytes[0] > bytes[1] && bytes[1] > bytes[2],
        "and it is still recognisably red: {bytes:?}"
    );

    // The other direction takes it back out again.
    let back = convert(&widened, narrow).expect("a conversion");
    let returned = back.to_packed().expect("bytes");
    assert!(
        returned[0] >= 253 && returned[1] <= 2 && returned[2] <= 2,
        "the round trip should land back on red: {returned:?}"
    );
}

#[test]
fn grey_stays_grey_across_a_gamut_change() {
    // Every gamut agrees about its own white point, so a neutral value must
    // come out neutral. A matrix applied to the wrong side of the transfer
    // function is the classic way to break this, and it tints the greys.
    let narrow = srgb(4, 4, PixelFormat::Rgb8);
    let wide = described(
        4,
        4,
        PixelFormat::Rgb8,
        ColourDescription::new(
            Primaries::Bt2020,
            TransferFunction::Srgb,
            MatrixCoefficients::Identity,
            Range::Full,
        ),
    );
    for level in [0_u8, 64, 128, 192, 255] {
        let frame = TestPattern::Flat { value: level }
            .render(narrow)
            .expect("a frame");
        let widened = convert(&frame, wide).expect("a conversion");
        let bytes = widened.to_packed().expect("bytes");
        for pixel in bytes.chunks_exact(3) {
            assert!(
                i32::from(pixel[0].max(pixel[1]).max(pixel[2]))
                    - i32::from(pixel[0].min(pixel[1]).min(pixel[2]))
                    <= TOLERANCE,
                "grey {level} came out tinted: {pixel:?}"
            );
        }
    }
}

#[test]
fn limited_range_and_full_range_round_trip() {
    let full = srgb(16, 9, PixelFormat::Rgb8);
    let limited = described(
        16,
        9,
        PixelFormat::Rgb8,
        ColourDescription::new(
            Primaries::Bt709,
            TransferFunction::Srgb,
            MatrixCoefficients::Identity,
            Range::Limited,
        ),
    );
    let frame = TestPattern::Ramp.render(full).expect("a frame");
    let narrowed = convert(&frame, limited).expect("a conversion");

    // Limited range must actually be limited: nothing below sixteen or above
    // two hundred and thirty-five.
    for sample in narrowed.to_packed().expect("bytes") {
        assert!(
            (16..=235).contains(&sample),
            "sample {sample} left the range"
        );
    }

    let back = convert(&narrowed, full).expect("a conversion");
    // A trip through limited range loses about a seventh of the code values,
    // so the tolerance here is the quantisation rather than an error.
    let original = frame.to_packed().expect("bytes");
    let returned = back.to_packed().expect("bytes");
    for (index, (left, right)) in returned.iter().zip(original.iter()).enumerate() {
        let difference = i32::from(*left) - i32::from(*right);
        assert!(
            difference.abs() <= 2,
            "sample {index} came back as {left} from {right}"
        );
    }
}

#[test]
fn greys_survive_a_trip_through_luma_and_chroma() {
    let rgb = srgb(16, 9, PixelFormat::Rgb8);
    let ycbcr = described(
        16,
        9,
        PixelFormat::Yuv444p8,
        ColourDescription::bt709_limited(),
    );
    let frame = TestPattern::Ramp.render(rgb).expect("a frame");
    let converted = convert(&frame, ycbcr).expect("a conversion");
    let back = convert(&converted, rgb).expect("a conversion");

    let original = frame.to_packed().expect("bytes");
    let returned = back.to_packed().expect("bytes");
    for (index, (left, right)) in returned.iter().zip(original.iter()).enumerate() {
        let difference = i32::from(*left) - i32::from(*right);
        assert!(
            difference.abs() <= 2,
            "sample {index} came back as {left} from {right}"
        );
    }
}

#[test]
fn saturated_primaries_clip_at_the_legal_chroma_limits() {
    // Full-range saturated primaries do not fit inside limited-range chroma.
    // Yellow drives Cb to its floor, blue drives it to its ceiling, and red
    // and cyan do the same to Cr. That is a property of the format, not of
    // this implementation, and it is why a colourist legalises before
    // delivery rather than after.
    let rgb = srgb(16, 1, PixelFormat::Rgb8);
    let ycbcr = described(
        16,
        1,
        PixelFormat::Yuv444p8,
        ColourDescription::bt709_limited(),
    );
    let frame = TestPattern::Bars.render(rgb).expect("a frame");
    let converted = convert(&frame, ycbcr).expect("a conversion");
    let bytes = converted.to_packed().expect("bytes");
    let luma = |x: usize| bytes[x];
    let blue_difference = |x: usize| bytes[16 + x];
    let red_difference = |x: usize| bytes[32 + x];

    // Two pixels per bar at this width: white, yellow, cyan, green, magenta,
    // red, blue, black.
    assert_eq!(
        (luma(0), blue_difference(0), red_difference(0)),
        (235, 128, 128),
        "white"
    );
    assert_eq!(
        (luma(14), blue_difference(14), red_difference(14)),
        (16, 128, 128),
        "black"
    );
    assert_eq!(blue_difference(2), 16, "yellow drives Cb to the floor");
    assert_eq!(red_difference(4), 16, "cyan drives Cr to the floor");
    assert_eq!(blue_difference(12), 240, "blue drives Cb to the ceiling");
    assert_eq!(red_difference(10), 240, "red drives Cr to the ceiling");

    // Luma falls monotonically across the bars, which is the shape of the
    // signal every waveform monitor in the world shows for this pattern.
    for bar in 1..8 {
        assert!(
            luma(bar * 2) < luma((bar - 1) * 2),
            "bar {bar} is not below the one before it"
        );
    }
}

#[test]
fn white_and_black_land_where_the_range_says() {
    let rgb = srgb(2, 2, PixelFormat::Rgb8);
    let ycbcr = described(
        2,
        2,
        PixelFormat::Yuv444p8,
        ColourDescription::bt709_limited(),
    );

    let white = TestPattern::Flat { value: 255 }
        .render(rgb)
        .expect("a frame");
    let converted = convert(&white, ycbcr).expect("a conversion");
    let bytes = converted.to_packed().expect("bytes");
    assert!(
        (234..=236).contains(&i32::from(bytes[0])),
        "white should be luma 235 in limited range, was {}",
        bytes[0]
    );
    assert!(
        (127..=129).contains(&i32::from(bytes[4])),
        "white is neutral, so chroma should be 128, was {}",
        bytes[4]
    );

    let black = TestPattern::Flat { value: 0 }.render(rgb).expect("a frame");
    let converted = convert(&black, ycbcr).expect("a conversion");
    let bytes = converted.to_packed().expect("bytes");
    assert!(
        (15..=17).contains(&i32::from(bytes[0])),
        "black should be luma 16 in limited range, was {}",
        bytes[0]
    );
}

#[test]
fn a_single_channel_target_holds_luminance_not_the_red_channel() {
    let rgb = srgb(8, 1, PixelFormat::Rgb8);
    let grey = srgb(8, 1, PixelFormat::Gray8);
    let frame = TestPattern::Bars.render(rgb).expect("a frame");
    let converted = convert(&frame, grey).expect("a conversion");
    let bytes = converted.to_packed().expect("bytes");
    assert_eq!(bytes[0], 255, "white");
    assert_eq!(bytes[7], 0, "black");
    assert!(bytes[3] > bytes[6], "green is brighter than blue");
    assert!(
        bytes[5] < bytes[3],
        "red is darker than green, which is what luminance means"
    );
}

#[test]
fn a_scaler_and_a_chroma_filter_are_refused_rather_than_guessed() {
    let small = srgb(8, 8, PixelFormat::Rgb8);
    let large = srgb(16, 16, PixelFormat::Rgb8);
    let frame = TestPattern::Bars.render(small).expect("a frame");
    assert_eq!(
        convert(&frame, large),
        Err(RenderStatus::ConversionUnavailable),
        "resizing is a scaler, and a scaler is a filter with a name"
    );

    let subsampled = FrameDescription::square(
        Geometry::new(8, 8).expect("a geometry"),
        PixelFormat::Yuv420p8,
        ColourDescription::bt709_limited(),
        Some(ChromaSiting::Left),
        None,
    )
    .expect("a description");
    assert_eq!(
        convert(&frame, subsampled),
        Err(RenderStatus::ConversionUnavailable),
        "chroma subsampling is a filter too"
    );
    let planar = Frame::blank(subsampled).expect("a frame");
    assert_eq!(
        convert(&planar, small),
        Err(RenderStatus::ConversionUnavailable)
    );
}

#[test]
fn a_conversion_gives_the_same_answer_twice() {
    let source = srgb(16, 9, PixelFormat::Rgb8);
    let target = described(
        16,
        9,
        PixelFormat::Yuv444p8,
        ColourDescription::bt709_limited(),
    );
    let frame = TestPattern::Checkerboard { square: 3 }
        .render(source)
        .expect("a frame");
    let first = convert(&frame, target).expect("a conversion");
    let second = convert(&frame, target).expect("a conversion");
    assert_eq!(first.digest(), second.digest());
}
