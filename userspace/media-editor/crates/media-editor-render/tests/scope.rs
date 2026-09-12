// SPDX-License-Identifier: GPL-3.0-only
//! Scopes, against counts worked out independently.
//!
//! A scope is a measurement, so every expectation here is arrived at by
//! counting rather than by running the code and writing down what it said.

use media_editor_media::colour::{MatrixCoefficients, Primaries, Range, TransferFunction};
use media_editor_media::{
    AlphaState, ChromaSiting, ColourDescription, Frame, FrameDescription, Geometry, PixelFormat,
    TestPattern,
};
use media_editor_render::{LumaWeights, RenderStatus, histogram, waveform};

fn rgb(width: u32, height: u32, format: PixelFormat) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(width, height).expect("a geometry"),
        format,
        ColourDescription::srgb_full(),
        None,
        if format.has_alpha() {
            Some(AlphaState::Straight)
        } else {
            None
        },
    )
    .expect("a description")
}

#[test]
fn the_weights_sum_to_the_scale_exactly() {
    // If they did not, a white field would not measure as white, and every
    // waveform in the application would sit slightly below the top.
    for matrix in [
        MatrixCoefficients::Bt709,
        MatrixCoefficients::Bt601,
        MatrixCoefficients::Bt2020NonConstant,
        MatrixCoefficients::Identity,
    ] {
        let colour = ColourDescription::new(
            Primaries::Bt709,
            TransferFunction::Bt709,
            matrix,
            Range::Full,
        );
        let weights = LumaWeights::of(colour).expect("weights");
        assert_eq!(
            weights.red() + weights.green() + weights.blue(),
            1 << 16,
            "{matrix:?}"
        );
        assert_eq!(weights.luma_of(255, 255, 255), 255, "white is white");
        assert_eq!(weights.luma_of(0, 0, 0), 0, "black is black");
    }
}

#[test]
fn the_specified_coefficients_are_used_where_a_standard_specifies_them() {
    // BT.601's matrix uses 0.299, 0.587 and 0.114 whatever its primaries are.
    let colour = ColourDescription::new(
        Primaries::Bt601Ntsc,
        TransferFunction::Bt709,
        MatrixCoefficients::Bt601,
        Range::Full,
    );
    let weights = LumaWeights::of(colour).expect("weights");
    // 0.299 × 65536 = 19595.264 and 0.114 × 65536 = 7471.104, each rounded to
    // nearest. Written as integers because a test of an integer pipeline that
    // reached for a float to check it would be checking the wrong thing.
    assert_eq!(weights.red(), (299 * 65536 + 500) / 1000);
    assert_eq!(weights.blue(), (114 * 65536 + 500) / 1000);

    // And an RGB frame in the same primaries, which has no matrix
    // coefficients, uses what the primaries derive to instead - which is a
    // different number, on purpose.
    let identity = ColourDescription::new(
        Primaries::Bt601Ntsc,
        TransferFunction::Bt709,
        MatrixCoefficients::Identity,
        Range::Full,
    );
    let derived = LumaWeights::of(identity).expect("weights");
    assert_ne!(derived.red(), weights.red());
    assert_ne!(derived.green(), weights.green());
}

#[test]
fn a_flat_field_lands_in_one_bin() {
    let frame = TestPattern::Flat { value: 137 }
        .render(rgb(16, 9, PixelFormat::Gray8))
        .expect("a frame");
    let counts = histogram(&frame).expect("a histogram");
    assert_eq!(counts.channels(), 1);
    assert_eq!(counts.count(0, 137), Ok(16 * 9));
    assert_eq!(counts.total(0), Ok(16 * 9));
    for level in 0..256 {
        if level != 137 {
            assert_eq!(counts.count(0, level), Ok(0), "level {level}");
        }
    }
}

#[test]
fn a_ramp_fills_every_bin_once() {
    // Two hundred and fifty-six pixels wide, one tall: each column is its own
    // level, so every bin holds exactly one sample.
    let frame = TestPattern::Ramp
        .render(rgb(256, 1, PixelFormat::Gray8))
        .expect("a frame");
    let counts = histogram(&frame).expect("a histogram");
    for level in 0..256 {
        assert_eq!(counts.count(0, level), Ok(1), "level {level}");
    }
    assert_eq!(counts.total(0), Ok(256));
}

#[test]
fn the_bars_count_out_as_the_bars() {
    // Eight bars, eight pixels wide, four tall: each bar is one pixel wide, so
    // every colour appears four times. Red, green and blue each appear in four
    // of the eight bars at full, so each channel holds sixteen samples at 255
    // and sixteen at zero.
    let frame = TestPattern::Bars
        .render(rgb(8, 4, PixelFormat::Rgb8))
        .expect("a frame");
    let counts = histogram(&frame).expect("a histogram");
    assert_eq!(counts.channels(), 3);
    for channel in 0..3 {
        assert_eq!(counts.count(channel, 255), Ok(16), "channel {channel} high");
        assert_eq!(counts.count(channel, 0), Ok(16), "channel {channel} low");
        assert_eq!(counts.total(channel), Ok(32));
    }
}

#[test]
fn alpha_is_counted_too() {
    let frame = TestPattern::Bars
        .render(rgb(8, 2, PixelFormat::Rgba8))
        .expect("a frame");
    let counts = histogram(&frame).expect("a histogram");
    assert_eq!(counts.channels(), 4);
    assert_eq!(
        counts.count(3, 255),
        Ok(16),
        "the pattern draws an opaque frame, and a colourist should be able to see that"
    );
}

#[test]
fn a_planar_frame_counts_each_plane() {
    let description = FrameDescription::square(
        Geometry::new(8, 8).expect("a geometry"),
        PixelFormat::Yuv420p8,
        ColourDescription::bt709_limited(),
        Some(ChromaSiting::Left),
        None,
    )
    .expect("a description");
    let frame = Frame::blank(description).expect("a frame");
    let counts = histogram(&frame).expect("a histogram");
    assert_eq!(counts.channels(), 3);
    assert_eq!(counts.total(0), Ok(64), "luma is full resolution");
    assert_eq!(counts.total(1), Ok(16), "chroma is a quarter of it");
    assert_eq!(counts.total(2), Ok(16));
}

#[test]
fn a_waveform_has_one_column_per_column_of_picture() {
    let frame = TestPattern::Bars
        .render(rgb(64, 9, PixelFormat::Gray8))
        .expect("a frame");
    let scope = waveform(&frame).expect("a waveform");
    assert_eq!(scope.columns(), 64);
    for column in 0..64 {
        let total: u32 = (0..256)
            .map(|level| scope.count(column, level).expect("a count"))
            .sum();
        assert_eq!(total, 9, "column {column} counted nine rows");
    }
}

#[test]
fn a_waveform_of_bars_steps_down_the_way_the_bars_do() {
    // Eight pixels wide, so one column per bar, and the peaks must follow the
    // luminances of white, yellow, cyan, green, magenta, red, blue, black -
    // which fall monotonically, and that is what a waveform of bars looks
    // like on any monitor in any suite.
    let frame = TestPattern::Bars
        .render(rgb(8, 4, PixelFormat::Rgb8))
        .expect("a frame");
    let scope = waveform(&frame).expect("a waveform");
    let peaks: std::vec::Vec<usize> = (0..8)
        .map(|column| scope.peak(column).expect("a peak").expect("a level"))
        .collect();
    assert_eq!(peaks[0], 255, "white");
    assert_eq!(peaks[7], 0, "black");
    for index in 1..peaks.len() {
        assert!(
            peaks[index] < peaks[index - 1],
            "bar {index} is not below bar {}",
            index - 1
        );
    }
}

#[test]
fn a_waveform_of_a_luma_chroma_frame_reads_the_luma_plane() {
    // No weights, no conversion: plane zero already is luma, and computing it
    // again from chroma would be both wrong and slower.
    let description = FrameDescription::square(
        Geometry::new(4, 4).expect("a geometry"),
        PixelFormat::Yuv444p8,
        ColourDescription::bt709_limited(),
        None,
        None,
    )
    .expect("a description");
    let mut samples = std::vec![0_u8; 48];
    for (index, sample) in samples.iter_mut().take(16).enumerate() {
        *sample = u8::try_from(index * 16).unwrap_or(255);
    }
    let frame = Frame::from_packed(description, &samples).expect("a frame");
    let scope = waveform(&frame).expect("a waveform");
    assert_eq!(
        scope.peak(0),
        Ok(Some(12 * 16)),
        "the last row of column zero"
    );
    assert_eq!(scope.peak(3), Ok(Some(15 * 16)));
}

#[test]
fn a_scope_reads_the_same_frame_the_same_way_twice() {
    let frame = TestPattern::Checkerboard { square: 3 }
        .render(rgb(31, 17, PixelFormat::Rgba8))
        .expect("a frame");
    assert_eq!(histogram(&frame), histogram(&frame));
    assert_eq!(waveform(&frame), waveform(&frame));
}

#[test]
fn asking_a_scope_for_something_that_is_not_there_is_refused() {
    let frame = TestPattern::Ramp
        .render(rgb(8, 8, PixelFormat::Gray8))
        .expect("a frame");
    let counts = histogram(&frame).expect("a histogram");
    assert_eq!(counts.count(1, 0), Err(RenderStatus::OutsideDomain));
    assert_eq!(counts.count(0, 256), Err(RenderStatus::OutsideDomain));
    assert_eq!(counts.total(9), Err(RenderStatus::OutsideDomain));

    let scope = waveform(&frame).expect("a waveform");
    assert_eq!(scope.count(8, 0), Err(RenderStatus::OutsideDomain));
    assert_eq!(scope.peak(8), Err(RenderStatus::OutsideDomain));
}
