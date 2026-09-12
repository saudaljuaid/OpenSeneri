// SPDX-License-Identifier: GPL-3.0-only
//! The vectorscope, against the graticule it is supposed to agree with.
//!
//! Every expectation here is either an exact property derived from the
//! definition of `Y'CbCr`, or a count arrived at by counting. Nothing is a
//! number read off a run of the code.

use media_editor_core::Rational;
use media_editor_media::colour::{MatrixCoefficients, Primaries, Range, TransferFunction};
use media_editor_media::{
    ChromaSiting, ColourDescription, Frame, FrameDescription, Geometry, PixelFormat, TestPattern,
};
use media_editor_render::{RenderStatus, chroma_of, vectorscope};

/// The three luma matrices a colourist meets, and the description each needs.
fn matrices() -> [(&'static str, ColourDescription); 3] {
    [
        (
            "BT.601",
            ColourDescription::new(
                Primaries::Bt601Pal,
                TransferFunction::Bt709,
                MatrixCoefficients::Bt601,
                Range::Limited,
            ),
        ),
        ("BT.709", ColourDescription::bt709_limited()),
        (
            "BT.2020",
            ColourDescription::new(
                Primaries::Bt2020,
                TransferFunction::Bt2020Ten,
                MatrixCoefficients::Bt2020NonConstant,
                Range::Limited,
            ),
        ),
    ]
}

fn ratio(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a ratio")
}

fn one() -> Rational {
    Rational::ONE
}

fn zero() -> Rational {
    Rational::ZERO
}

fn half() -> Rational {
    ratio(1, 2)
}

#[test]
fn the_primaries_land_on_the_axes_in_every_matrix() {
    // The four fixed marks of every vectorscope graticule ever built, and they
    // are not a convention — they fall out of the definition. Cr is
    // (R' - Y')/2(1 - Kr), and full red makes Y' equal to Kr exactly, so the
    // coefficient cancels and Cr is one half whatever Kr happens to be.
    //
    // Asserted for three matrices with three different sets of coefficients,
    // because a test that checked only one could not tell a derivation from a
    // lookup table.
    for (name, colour) in matrices() {
        let (_, red) = chroma_of(colour, one(), zero(), zero()).expect("chroma");
        assert_eq!(red, half(), "{name}: full red sits at Cr = +1/2");

        let (_, cyan) = chroma_of(colour, zero(), one(), one()).expect("chroma");
        assert_eq!(
            cyan,
            half().checked_neg().expect("a negation"),
            "{name}: cyan sits at Cr = -1/2"
        );

        let (blue, _) = chroma_of(colour, zero(), zero(), one()).expect("chroma");
        assert_eq!(blue, half(), "{name}: full blue sits at Cb = +1/2");

        let (yellow, _) = chroma_of(colour, one(), one(), zero()).expect("chroma");
        assert_eq!(
            yellow,
            half().checked_neg().expect("a negation"),
            "{name}: yellow sits at Cb = -1/2"
        );
    }
}

#[test]
fn complementary_colours_sit_opposite_each_other() {
    // A vectorscope is a diagram of a vector space, so opposites are negations
    // — red is directly across the graticule from cyan, green from magenta,
    // blue from yellow. Exactly across, not nearly.
    for (name, colour) in matrices() {
        for ((r, g, b), (cr, cg, cb)) in [
            ((one(), zero(), zero()), (zero(), one(), one())),
            ((zero(), one(), zero()), (one(), zero(), one())),
            ((zero(), zero(), one()), (one(), one(), zero())),
        ] {
            let (blue_one, red_one) = chroma_of(colour, r, g, b).expect("chroma");
            let (blue_other, red_other) = chroma_of(colour, cr, cg, cb).expect("chroma");
            assert_eq!(
                blue_one,
                blue_other.checked_neg().expect("a negation"),
                "{name}: blue-difference"
            );
            assert_eq!(
                red_one,
                red_other.checked_neg().expect("a negation"),
                "{name}: red-difference"
            );
        }
    }
}

#[test]
fn every_neutral_sits_exactly_on_the_origin() {
    // The property that makes the middle of the graticule mean "no colour".
    // Every grey, at every brightness, under every matrix — not a sample of
    // them, all of them at one part in a thousand.
    for (name, colour) in matrices() {
        for level in 0..=1000 {
            let value = ratio(level, 1000);
            let (blue, red) = chroma_of(colour, value, value, value).expect("chroma");
            assert_eq!(blue, zero(), "{name} at {level}/1000");
            assert_eq!(red, zero(), "{name} at {level}/1000");
        }
    }
}

#[test]
fn green_moves_between_matrices_and_the_axes_do_not() {
    // This is why a colourist can tell BT.601 material from BT.709 by looking
    // at the scope: the six boxes do not all sit in the same places. The four
    // on the axes are fixed by the definition; green and magenta are fixed by
    // the coefficients, and the coefficients differ.
    //
    // The values below are derived by hand from Kr and Kb:
    //   Cb(green) = -Kg x Kb-span... which is to say, worked out from
    //   Cb = (B' - Y')/2(1 - Kb) with B' = 0 and Y' = Kg.
    // BT.709: Kg = 0.7152, 1 - Kb = 0.9278, so Cb = -0.7152/1.8556 = -1788/4639.
    // BT.601: Kg = 0.587,  1 - Kb = 0.886,  so Cb = -0.587/1.772  = -587/1772.
    let (green_709, _) =
        chroma_of(ColourDescription::bt709_limited(), zero(), one(), zero()).expect("chroma");
    assert_eq!(green_709, ratio(-1788, 4639));

    let bt601 = matrices()[0].1;
    let (green_601, _) = chroma_of(bt601, zero(), one(), zero()).expect("chroma");
    assert_eq!(green_601, ratio(-587, 1772));

    assert_ne!(
        green_601, green_709,
        "if these agreed, the scope could not tell the two apart"
    );
}

/// An RGB description at full range.
fn rgb(width: u32, height: u32, format: PixelFormat) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(width, height).expect("a geometry"),
        format,
        ColourDescription::srgb_full(),
        None,
        if format.has_alpha() {
            Some(media_editor_media::AlphaState::Straight)
        } else {
            None
        },
    )
    .expect("a description")
}

#[test]
fn a_neutral_frame_puts_everything_in_the_middle() {
    // A flat grey, a black field and a white field are all colourless, so all
    // three land on one bin — the middle one — and nowhere else.
    for value in [0_u8, 77, 128, 255] {
        let frame = TestPattern::Flat { value }
            .render(rgb(16, 16, PixelFormat::Rgb8))
            .expect("a frame");
        let scope = vectorscope(&frame, 64).expect("a scope");
        assert_eq!(scope.total(), 256, "one point per pixel");
        assert_eq!(scope.occupied(), 1, "one colour is one point");
        assert_eq!(
            scope
                .count(scope.neutral(), scope.neutral())
                .expect("count"),
            256,
            "and that point is the origin, at value {value}"
        );
    }
}

#[test]
fn a_monochrome_frame_has_no_saturation_and_says_so() {
    // A ramp in a single-channel format runs from black to white, which is a
    // journey through brightness and not through colour. Every sample is on
    // the origin — the honest answer, rather than a refusal.
    let frame = TestPattern::Ramp
        .render(rgb(64, 4, PixelFormat::Gray8))
        .expect("a frame");
    let scope = vectorscope(&frame, 32).expect("a scope");
    assert_eq!(scope.total(), 256);
    assert_eq!(scope.occupied(), 1);
    assert_eq!(
        scope
            .count(scope.neutral(), scope.neutral())
            .expect("count"),
        256
    );
}

#[test]
fn colour_bars_land_where_the_graticule_says() {
    // The bars are the fixture the whole industry checks a scope against. This
    // one asserts each bar's chroma from `chroma_of` — exact rationals — and
    // then requires the counted scope to have put that bar's samples in the
    // bin those rationals name. Two independent routes to the same answer.
    let described = rgb(64, 16, PixelFormat::Rgb8);
    let frame = TestPattern::Bars.render(described).expect("a frame");
    let scope = vectorscope(&frame, 64).expect("a scope");

    // Eight bars across 64 pixels is eight columns of 8, sixteen rows deep:
    // 128 samples per bar, counted rather than measured.
    assert_eq!(scope.total(), 64 * 16);

    let colour = described.colour();
    let bars: [(&str, Rational, Rational, Rational); 8] = [
        ("white", one(), one(), one()),
        ("yellow", one(), one(), zero()),
        ("cyan", zero(), one(), one()),
        ("green", zero(), one(), zero()),
        ("magenta", one(), zero(), one()),
        ("red", one(), zero(), zero()),
        ("blue", zero(), zero(), one()),
        ("black", zero(), zero(), zero()),
    ];
    // White and black are both colourless, so they name the same bin. Summing
    // per bar would count that bin twice — so the bins are collected first and
    // then counted once each.
    let mut occupied = std::vec::Vec::new();
    for (name, red, green, blue) in bars {
        let (difference_blue, difference_red) =
            chroma_of(colour, red, green, blue).expect("chroma");
        let column = scope.bin_of(difference_blue).expect("a bin");
        let row = scope.bin_of(difference_red).expect("a bin");
        assert!(
            scope.count(column, row).expect("a count") > 0,
            "{name} is missing from the scope"
        );
        if !occupied.contains(&(column, row)) {
            occupied.push((column, row));
        }
    }
    let accounted: u64 = occupied
        .iter()
        .map(|(column, row)| scope.count(*column, *row).expect("a count"))
        .sum();
    assert_eq!(
        accounted,
        scope.total(),
        "every sample is accounted for by a bar, so the scope is plotting the \
         picture rather than something near it"
    );

    // White and black are both colourless, so they share the middle bin and
    // the eight bars occupy seven points, not eight. Counting, not running.
    assert_eq!(scope.occupied(), 7);
}

#[test]
fn a_subsampled_frame_is_counted_once_per_chroma_sample() {
    // A 4:2:0 frame holds one chroma pair for every four pixels. A scope that
    // plotted four copies of each would report a saturation four times too
    // confident, and a colourist grading to a broadcast limit would believe it.
    let described = FrameDescription::square(
        Geometry::new(16, 8).expect("a geometry"),
        PixelFormat::Yuv420p8,
        ColourDescription::bt709_limited(),
        Some(ChromaSiting::Centre),
        None,
    )
    .expect("a description");
    let frame = Frame::blank(described).expect("a frame");
    let scope = vectorscope(&frame, 32).expect("a scope");
    assert_eq!(scope.total(), (16 / 2) * (8 / 2));

    let full = FrameDescription::square(
        Geometry::new(16, 8).expect("a geometry"),
        PixelFormat::Yuv444p8,
        ColourDescription::bt709_limited(),
        None,
        None,
    )
    .expect("a description");
    let frame = Frame::blank(full).expect("a frame");
    assert_eq!(
        vectorscope(&frame, 32).expect("a scope").total(),
        16 * 8,
        "and 4:4:4 holds one per pixel"
    );
}

#[test]
fn a_blank_luma_chroma_frame_is_neutral() {
    // `Frame::blank` writes the code value that means neutral, which for
    // limited-range chroma is 128 rather than 0. If it wrote zero, this frame
    // would be violently green, so this test is also a check on what "blank"
    // means.
    let described = FrameDescription::square(
        Geometry::new(8, 8).expect("a geometry"),
        PixelFormat::Yuv444p8,
        ColourDescription::bt709_limited(),
        None,
        None,
    )
    .expect("a description");
    let frame = Frame::blank(described).expect("a frame");
    let scope = vectorscope(&frame, 64).expect("a scope");
    assert_eq!(scope.occupied(), 1);
    assert_eq!(
        scope
            .count(scope.neutral(), scope.neutral())
            .expect("count"),
        64
    );
}

#[test]
fn a_scope_reads_the_same_frame_the_same_way_twice() {
    let frame = TestPattern::Bars
        .render(rgb(32, 8, PixelFormat::Rgba8))
        .expect("a frame");
    assert_eq!(
        vectorscope(&frame, 48).expect("a scope"),
        vectorscope(&frame, 48).expect("a scope")
    );
}

#[test]
fn the_grid_is_bounded_and_says_so() {
    let frame = TestPattern::Bars
        .render(rgb(8, 8, PixelFormat::Rgb8))
        .expect("a frame");
    assert_eq!(vectorscope(&frame, 0), Err(RenderStatus::OutsideDomain));
    assert_eq!(
        vectorscope(&frame, media_editor_render::MAX_BINS + 1),
        Err(RenderStatus::OutsideDomain)
    );
    let scope = vectorscope(&frame, media_editor_render::MAX_BINS).expect("a scope");
    assert_eq!(scope.bins(), media_editor_render::MAX_BINS);
    assert_eq!(
        scope.count(media_editor_render::MAX_BINS, 0),
        Err(RenderStatus::OutsideDomain)
    );
}

#[test]
fn a_finer_grid_separates_what_a_coarse_one_merges() {
    // Two nearly identical colours are one point on a coarse scope and two on
    // a fine one. That is the whole reason the bin count is a parameter, and
    // it is asserted rather than assumed.
    let described = rgb(2, 1, PixelFormat::Rgb8);
    let frame = Frame::from_packed(described, &[255, 0, 0, 250, 0, 0]).expect("a frame");
    assert_eq!(vectorscope(&frame, 4).expect("a scope").occupied(), 1);
    assert_eq!(vectorscope(&frame, 256).expect("a scope").occupied(), 2);
}
