// SPDX-License-Identifier: GPL-3.0-only
//! Test patterns, which are this project's fixtures.
//!
//! The digests below are golden. A pattern that draws one pixel differently
//! changes one of them, which is the point: every other test in the media
//! pipeline is built on these frames, so they have to be nailed down.

use media_editor_media::{
    AlphaState, ColourDescription, Frame, FrameDescription, Geometry, MediaStatus, PixelFormat,
    TestPattern,
};

/// The alpha tag a format demands: present exactly when the format has alpha.
fn alpha_for(format: PixelFormat) -> Option<AlphaState> {
    if format.has_alpha() {
        Some(AlphaState::Straight)
    } else {
        None
    }
}

fn description(width: u32, height: u32, format: PixelFormat) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(width, height).expect("a geometry"),
        format,
        ColourDescription::srgb_full(),
        None,
        alpha_for(format),
    )
    .expect("a description")
}

fn render(pattern: TestPattern, width: u32, height: u32, format: PixelFormat) -> Frame {
    pattern
        .render(description(width, height, format))
        .expect("a frame")
}

#[test]
fn the_bars_are_the_bars() {
    let frame = render(TestPattern::Bars, 8, 2, PixelFormat::Rgb8);
    let bytes = frame.to_packed().expect("bytes");
    // One pixel per bar, so the row is the bar list itself.
    assert_eq!(
        &bytes[..24],
        &[
            255, 255, 255, // white
            255, 255, 0, // yellow
            0, 255, 255, // cyan
            0, 255, 0, // green
            255, 0, 255, // magenta
            255, 0, 0, // red
            0, 0, 255, // blue
            0, 0, 0, // black
        ]
    );
    assert_eq!(&bytes[24..], &bytes[..24], "every row is the same");
}

#[test]
fn the_bars_divide_a_width_that_is_not_a_multiple_of_eight() {
    // Integer arithmetic, so the boundaries land on the same pixels
    // everywhere. A width of 100 gives bars of 13, 12, 13, 12, 13, 12, 13, 12.
    let frame = render(TestPattern::Bars, 100, 1, PixelFormat::Gray8);
    let row = frame.to_packed().expect("bytes");
    let mut widths = std::vec::Vec::new();
    let mut run = 1_usize;
    for index in 1..row.len() {
        if row[index] == row[index - 1] {
            run += 1;
        } else {
            widths.push(run);
            run = 1;
        }
    }
    widths.push(run);
    assert_eq!(widths.iter().sum::<usize>(), 100);
    assert_eq!(widths.len(), 8, "eight bars, whatever the width");
}

#[test]
fn the_ramp_runs_from_black_to_white() {
    let frame = render(TestPattern::Ramp, 256, 1, PixelFormat::Gray8);
    let row = frame.to_packed().expect("bytes");
    assert_eq!(row[0], 0);
    assert_eq!(row[255], 255);
    for index in 1..row.len() {
        assert!(row[index] >= row[index - 1], "the ramp never goes back");
    }
}

#[test]
fn a_flat_field_is_flat() {
    let frame = render(TestPattern::Flat { value: 137 }, 9, 7, PixelFormat::Gray8);
    assert!(frame.to_packed().expect("bytes").iter().all(|b| *b == 137));
}

#[test]
fn a_checkerboard_alternates() {
    let frame = render(
        TestPattern::Checkerboard { square: 2 },
        4,
        4,
        PixelFormat::Gray8,
    );
    let bytes = frame.to_packed().expect("bytes");
    assert_eq!(&bytes[0..4], &[16, 16, 235, 235]);
    assert_eq!(&bytes[8..12], &[235, 235, 16, 16], "the third row flips");
}

#[test]
fn alpha_is_opaque_where_a_format_has_it() {
    let frame = render(TestPattern::Bars, 8, 1, PixelFormat::Rgba8);
    let bytes = frame.to_packed().expect("bytes");
    for pixel in bytes.chunks_exact(4) {
        assert_eq!(pixel[3], 255);
    }
}

#[test]
fn grey_is_the_luma_of_the_colour() {
    // White stays white and black stays black - a rounding mistake in the
    // luma coefficients shows up at exactly those two ends first.
    let frame = render(TestPattern::Bars, 8, 1, PixelFormat::Gray8);
    let row = frame.to_packed().expect("bytes");
    assert_eq!(row[0], 255, "white");
    assert_eq!(row[7], 0, "black");
    assert!(row[3] > row[6], "green is brighter than blue");
    assert!(row[1] > row[3], "yellow is brighter than green");
}

#[test]
fn a_pattern_is_a_pure_function_of_its_description() {
    for format in [PixelFormat::Rgba8, PixelFormat::Rgb8, PixelFormat::Gray8] {
        for pattern in [
            TestPattern::Bars,
            TestPattern::Ramp,
            TestPattern::Checkerboard { square: 3 },
            TestPattern::Flat { value: 42 },
        ] {
            let first = render(pattern, 17, 11, format);
            let second = render(pattern, 17, 11, format);
            assert_eq!(first.digest(), second.digest(), "{pattern:?} {format:?}");
        }
    }
}

#[test]
fn the_golden_digests_hold() {
    // Pinned. These frames are what the rest of the media pipeline is tested
    // against, so a change to any of them is a deliberate commit (R-14.4).
    // A digest here covers the description as well as the samples, so a change
    // to what the numbers mean fails this too. All four moved when the
    // description gained its alpha tag; the samples themselves were shown
    // unchanged, byte for byte, before these were rewritten.
    let cases: [(TestPattern, PixelFormat, &str); 4] = [
        (
            TestPattern::Bars,
            PixelFormat::Rgba8,
            "4BBA59375E3964A329FC64C43326F114234550CA0464792D02CBFA18319D6F4B",
        ),
        (
            TestPattern::Ramp,
            PixelFormat::Gray8,
            "F826C63D9D0CE535ECCBDA9CE9A355F8288C666C88383BD5F68C93537D14A1EF",
        ),
        (
            TestPattern::Checkerboard { square: 4 },
            PixelFormat::Rgb8,
            "3C7E0A21357AEA2D6078CF95713D09D022303B95C4E9322C2FCDA6E5702E8030",
        ),
        (
            TestPattern::Flat { value: 128 },
            PixelFormat::Gray8,
            "36A64F4C11DF974D20BF941E31A3B4FA6DA7E1765DA257709D3244E9F768D935",
        ),
    ];
    for (pattern, format, expected) in cases {
        let frame = render(pattern, 64, 36, format);
        assert_eq!(
            std::format!("{}", frame.digest()),
            expected,
            "{pattern:?} in {format:?}"
        );
    }
}

#[test]
fn a_luma_chroma_format_is_refused_rather_than_guessed() {
    let description = FrameDescription::square(
        Geometry::new(16, 16).expect("a geometry"),
        PixelFormat::Yuv420p8,
        ColourDescription::bt709_limited(),
        Some(media_editor_media::ChromaSiting::Left),
        None,
    )
    .expect("a description");
    assert_eq!(
        TestPattern::Bars.render(description),
        Err(MediaStatus::PatternFormatUnsupported),
        "drawing luma and chroma means choosing a matrix and a range, and that \
         is the colour pipeline's decision rather than a fixture generator's"
    );
}

#[test]
fn the_offline_slate_stripes_at_every_size() {
    // The property a slate needs: it must be unmistakable for footage at
    // whatever size it is drawn. A fixed stripe period fails that on a small
    // frame, where the whole picture falls inside one band and the slate is a
    // solid colour -- which is exactly the case where "the drive is not
    // mounted" looks like a shot of a red wall.
    for (width, height) in [(4_u32, 4_u32), (16, 9), (64, 36), (320, 180), (1920, 1080)] {
        let frame = render(TestPattern::Offline, width, height, PixelFormat::Rgb8);
        let packed = frame.to_packed().expect("bytes");
        let values: std::collections::BTreeSet<u8> =
            packed.chunks_exact(3).map(|pixel| pixel[0]).collect();
        assert_eq!(
            values.len(),
            2,
            "both bands appear at {width} by {height}, got {values:?}"
        );
    }
}

#[test]
fn the_offline_slate_runs_diagonally() {
    // Down *and* across, at an angle no camera produces. A pattern that varied
    // along only one axis would be bars, which is a thing a programme
    // legitimately contains.
    let frame = render(TestPattern::Offline, 64, 64, PixelFormat::Rgb8);
    let packed = frame.to_packed().expect("bytes");
    let at = |x: usize, y: usize| packed[(y * 64 + x) * 3];
    let across: std::collections::BTreeSet<u8> = (0..64).map(|x| at(x, 0)).collect();
    let down: std::collections::BTreeSet<u8> = (0..64).map(|y| at(0, y)).collect();
    assert_eq!(across.len(), 2, "it varies across");
    assert_eq!(down.len(), 2, "and down");
    // And a step of one in each direction lands on the same band, which is
    // what makes it a diagonal rather than a checkerboard.
    for start in [0_usize, 5, 17, 40] {
        assert_eq!(at(start, 0), at(start.saturating_sub(1), 1));
    }
}

#[test]
fn a_row_of_a_pattern_is_that_row_of_the_whole_frame() {
    // The row form, checked against the whole form it is a slice of. A pattern
    // is placed against the whole picture -- bars are eighths of the width, a
    // checkerboard is counted from the top -- so both forms are asked for the
    // same description and only the range differs.
    let description = FrameDescription::square(
        Geometry::new(19, 7).expect("a geometry"),
        PixelFormat::Rgb8,
        ColourDescription::srgb_full(),
        None,
        None,
    )
    .expect("a description");
    for pattern in [
        TestPattern::Bars,
        TestPattern::Ramp,
        TestPattern::Offline,
        TestPattern::Checkerboard { square: 3 },
        TestPattern::Flat { value: 77 },
    ] {
        let whole = pattern.render(description).expect("a frame");
        let bytes = whole.to_packed().expect("bytes");
        let stride = 19 * 3;
        for row in 0..7_u32 {
            let one = pattern.render_row(description, row).expect("a row");
            assert_eq!(one.description().geometry().height(), 1);
            assert_eq!(
                one.to_packed().expect("bytes"),
                bytes[row as usize * stride..(row as usize + 1) * stride],
                "{pattern:?} row {row} is not row {row} of the frame"
            );
        }
        // And a row past the bottom is refused rather than drawn against
        // arithmetic that happens to work. A caller reaching this directly has
        // no other bound.
        assert!(pattern.render_row(description, 7).is_err());
        assert!(pattern.render_row(description, 1000).is_err());
    }
}
