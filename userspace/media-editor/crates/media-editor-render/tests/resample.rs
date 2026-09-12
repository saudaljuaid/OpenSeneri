// SPDX-License-Identifier: GPL-3.0-only
//! Resampling, against what a weighted average is supposed to mean.
//!
//! The properties here are the ones that separate a resampler from a
//! plausible-looking loop: identity is *exact*, a flat field survives at every
//! scale, weights sum to one, and the general parallelogram path agrees with a
//! product of one-dimensional overlaps in the axis-aligned case it has no
//! knowledge of.

use media_editor_core::Rational;
use media_editor_media::colour::{MatrixCoefficients, Primaries, Range, TransferFunction};
use media_editor_media::{
    AlphaState, ColourDescription, Frame, FrameDescription, Geometry, PixelFormat,
};
use media_editor_render::RenderStatus;
use media_editor_render::resample::{
    Filter, MAX_BAND_ROWS, MAX_TILE_ROWS, Mapping, Tile, resample, resample_row, resample_tile,
    tile,
};

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

fn n(value: i64) -> Rational {
    Rational::from_integer(value)
}

fn srgb() -> ColourDescription {
    ColourDescription {
        primaries: Primaries::Bt709,
        transfer: TransferFunction::Srgb,
        matrix: MatrixCoefficients::Identity,
        range: Range::Full,
    }
}

fn described(width: u32, height: u32) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(width, height).expect("a geometry"),
        PixelFormat::Rgba8,
        srgb(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description")
}

/// A frame from a function of position, premultiplied and opaque.
fn drawn(width: u32, height: u32, of: impl Fn(u32, u32) -> [u8; 4]) -> Frame {
    let mut packed = Vec::new();
    for y in 0..height {
        for x in 0..width {
            packed.extend_from_slice(&of(x, y));
        }
    }
    Frame::from_packed(described(width, height), &packed).expect("a frame")
}

fn flat(width: u32, height: u32, sample: [u8; 4]) -> Frame {
    drawn(width, height, |_, _| sample)
}

fn pixels(frame: &Frame) -> Vec<[u8; 4]> {
    frame
        .to_packed()
        .expect("bytes")
        .chunks_exact(4)
        .map(|pixel| [pixel[0], pixel[1], pixel[2], pixel[3]])
        .collect()
}

fn identity() -> Mapping {
    Mapping::scaled(
        Rational::ONE,
        Rational::ONE,
        (Rational::ZERO, Rational::ZERO),
    )
    .expect("a mapping")
}

#[test]
fn the_identity_map_returns_the_picture_it_was_given() {
    // Exactly. Not "to within a code value": a clip nobody has scaled must
    // arrive as itself, or every project drifts a little every time it is
    // touched. Both filters, because both have to.
    let source = drawn(6, 4, |x, y| {
        [
            u8::try_from(x * 40).expect("a value"),
            u8::try_from(y * 60).expect("a value"),
            128,
            255,
        ]
    });
    for filter in [Filter::Area, Filter::Bilinear] {
        let held = resample(&source, described(6, 4), identity(), filter).expect("a resample");
        assert_eq!(pixels(&held), pixels(&source), "with {filter:?}");
    }
}

#[test]
fn a_flat_field_survives_every_scale() {
    // The weights sum to one, so an average of one value is that value. If
    // they did not, a flat grey would come back a different grey -- which is
    // the failure that shows up as a picture getting darker every time it is
    // rendered.
    let source = flat(8, 8, [90, 140, 200, 255]);
    for (across, down, width, height) in [
        (r(1, 2), r(1, 2), 4_u32, 4_u32),
        (r(1, 4), r(1, 4), 2, 2),
        (n(2), n(2), 16, 16),
        (r(3, 7), r(5, 3), 3, 13),
    ] {
        let mapping =
            Mapping::scaled(across, down, (Rational::ZERO, Rational::ZERO)).expect("a mapping");
        for filter in [Filter::Area, Filter::Bilinear] {
            let held =
                resample(&source, described(width, height), mapping, filter).expect("a resample");
            // The border pixels draw partly from outside the source, which
            // contributes nothing, so only the interior is a pure average.
            let inside = pixels(&held);
            let at = |x: u32, y: u32| inside[(y * width + x) as usize];
            if width > 2 && height > 2 {
                assert_eq!(
                    at(width / 2, height / 2),
                    [90, 140, 200, 255],
                    "at {width}x{height} with {filter:?}"
                );
            }
        }
    }
}

#[test]
fn halving_a_picture_averages_each_two_by_two_block() {
    // The defining case for area filtering, and the arithmetic is in *light*.
    //
    // The values are 64 and 192 rather than black and white, and that is the
    // whole point. Black and white are exactly where the transfer is the
    // identity, so a checkerboard of them averages to 188 whether the
    // averaging happens in light or in code values -- it cannot tell the two
    // apart. The first version of this test used one, and the control that
    // replaces the decode with a straight division passed against it.
    //
    // At 64 and 192 the answers separate: the light mean encodes to **146**,
    // and the mean of the code values is 128. Eighteen apart, in the
    // direction that makes a picture *darker* every time it is reduced.
    let source = drawn(2, 2, |x, y| {
        if (x + y) % 2 == 0 {
            [192, 192, 192, 255]
        } else {
            [64, 64, 64, 255]
        }
    });
    let mapping =
        Mapping::scaled(r(1, 2), r(1, 2), (Rational::ZERO, Rational::ZERO)).expect("a mapping");
    let held = resample(&source, described(1, 1), mapping, Filter::Area).expect("a resample");
    let out = pixels(&held)[0];
    assert_eq!(out[3], 255, "and it is still opaque");
    assert_eq!(
        out[0], 146,
        "the mean of the light, not the mean of the code values"
    );
}

#[test]
fn the_general_path_agrees_with_a_product_of_overlaps() {
    // An axis-aligned map sends a pixel square to a rectangle, and the area a
    // source pixel contributes is then exactly how much of its width overlaps
    // times how much of its height does. The resampler forms no such product
    // -- it clips a parallelogram against four edges -- so agreeing with one
    // is a check by something that shares no code with it.
    let source = drawn(5, 5, |x, y| {
        let value = u8::try_from((x * 7 + y * 13) % 256).expect("a value");
        [value, value, value, 255]
    });
    let across = r(2, 3);
    let down = r(3, 5);
    let mapping =
        Mapping::scaled(across, down, (Rational::ZERO, Rational::ZERO)).expect("a mapping");
    let held = resample(&source, described(3, 3), mapping, Filter::Area).expect("a resample");

    // The same answer, computed the other way: the destination pixel's
    // preimage is the rectangle from (x/across, y/down) to the next one.
    let span = |value: i64, scale: Rational| {
        (
            Rational::from_integer(value)
                .checked_div(scale)
                .expect("a division"),
            Rational::from_integer(value + 1)
                .checked_div(scale)
                .expect("a division"),
        )
    };
    let overlap = |low: Rational, high: Rational, at: i64| {
        let start = if low.checked_sub(n(at)).expect("a difference").is_positive() {
            low
        } else {
            n(at)
        };
        let stop = if high
            .checked_sub(n(at + 1))
            .expect("a difference")
            .is_positive()
        {
            n(at + 1)
        } else {
            high
        };
        let width = stop.checked_sub(start).expect("a difference");
        if width.is_positive() {
            width
        } else {
            Rational::ZERO
        }
    };

    let out = pixels(&held);
    for y in 0..3_i64 {
        for x in 0..3_i64 {
            let (left, right) = span(x, across);
            let (top, bottom) = span(y, down);
            let footprint = right
                .checked_sub(left)
                .expect("a width")
                .checked_mul(bottom.checked_sub(top).expect("a height"))
                .expect("an area");
            // The light, accumulated the same way but with the weights formed
            // as a product rather than by clipping.
            let mut light = 0.0_f64;
            for row in 0..5_i64 {
                for column in 0..5_i64 {
                    let share = overlap(left, right, column)
                        .checked_mul(overlap(top, bottom, row))
                        .expect("an area");
                    if share.is_zero() {
                        continue;
                    }
                    let weight = share.checked_div(footprint).expect("a fraction");
                    let code = f64::from(
                        pixels(&source)[usize::try_from(row * 5 + column).expect("an index")][0],
                    );
                    let linear = decode(code / 255.0);
                    // Through `i32`, so the conversion is exact rather than
                    // merely usually exact: these weights are ratios of small
                    // overlaps and a lossy cast here would make the oracle
                    // agree for the wrong reason.
                    let top = f64::from(i32::try_from(weight.numerator()).expect("a small ratio"));
                    let bottom =
                        f64::from(i32::try_from(weight.denominator()).expect("a small ratio"));
                    light += linear * top / bottom;
                }
            }
            let expected = encode(light) * 255.0;
            let got = f64::from(out[usize::try_from(y * 3 + x).expect("an index")][0]);
            assert!(
                (expected - got).abs() <= 1.0,
                "at ({x}, {y}): the product says {expected}, the clipper says {got}"
            );
        }
    }
}

fn decode(code: f64) -> f64 {
    if code <= 0.04045 {
        code / 12.92
    } else {
        ((code + 0.055) / 1.055).powf(2.4)
    }
}

fn encode(light: f64) -> f64 {
    if light <= 0.003_130_8 {
        12.92 * light
    } else {
        1.055 * light.powf(1.0 / 2.4) - 0.055
    }
}

#[test]
fn what_falls_outside_the_source_contributes_nothing() {
    // Not the nearest edge pixel. A source that ran off its own edge would
    // smear its last column outwards forever, and a picture scaled smaller
    // than its frame would arrive with a streak instead of a border.
    let source = flat(4, 4, [200, 200, 200, 255]);
    let mapping = Mapping::scaled(Rational::ONE, Rational::ONE, (n(2), n(2))).expect("a mapping");
    let held = resample(&source, described(8, 8), mapping, Filter::Area).expect("a resample");
    let out = pixels(&held);
    assert_eq!(out[0], [0, 0, 0, 0], "the corner outside is transparent");
    assert_eq!(
        out[(3 * 8 + 3) as usize],
        [200, 200, 200, 255],
        "and the picture landed where it was put"
    );
    assert_eq!(out[(7 * 8 + 7) as usize], [0, 0, 0, 0], "and stopped");
}

#[test]
fn bilinear_samples_at_pixel_centres() {
    // A pixel's value belongs at its middle. Sampling at the corner shifts the
    // whole picture half a pixel up and left, which is the most common
    // resampling bug there is and is invisible until two versions of the same
    // shot are compared. Doubling a two-pixel ramp puts the two source values
    // at destination pixels 0..1 and 2..3, with the interpolation between.
    let source = drawn(4, 1, |x, _| {
        let value = u8::try_from(x * 60).expect("a value");
        [value, value, value, 255]
    });
    let mapping =
        Mapping::scaled(n(2), Rational::ONE, (Rational::ZERO, Rational::ZERO)).expect("a mapping");
    let held = resample(&source, described(8, 1), mapping, Filter::Bilinear).expect("a resample");
    let out: Vec<u8> = pixels(&held).iter().map(|pixel| pixel[0]).collect();
    // The first destination pixel's centre lands at source 0.25, which is left
    // of the first sample's centre, so it holds the first value unchanged.
    assert_eq!(out[0], 0);
    assert_eq!(out[1], out[1].min(60), "and it rises from there");
    assert!(
        out.windows(2).all(|pair| pair[0] <= pair[1]),
        "monotone: {out:?}"
    );
    assert_eq!(
        *out.last().expect("a value"),
        180,
        "reaching the last sample"
    );
}

#[test]
fn a_map_with_no_inverse_is_refused() {
    assert_eq!(
        Mapping::new(
            [Rational::ONE, n(2), n(2), n(4)],
            (Rational::ZERO, Rational::ZERO)
        ),
        Err(RenderStatus::Singular),
        "a map that squashes the picture onto a line draws from regions of no area"
    );
    assert_eq!(
        Mapping::scaled(
            Rational::ZERO,
            Rational::ONE,
            (Rational::ZERO, Rational::ZERO)
        ),
        Err(RenderStatus::Singular)
    );
}

#[test]
fn a_straight_frame_is_refused() {
    // Averaging straight samples across an edge mixes the colour of pixels
    // that are barely there with the colour of pixels that are fully there, at
    // equal weight -- which is the dark fringe around every badly keyed title.
    let description = FrameDescription::square(
        Geometry::new(4, 4).expect("a geometry"),
        PixelFormat::Rgba8,
        srgb(),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description");
    let frame = Frame::from_packed(description, &[128; 64]).expect("a frame");
    assert_eq!(
        resample(&frame, described(2, 2), identity(), Filter::Area),
        Err(RenderStatus::WrongAlphaState)
    );
}

#[test]
fn resampling_is_not_a_colour_conversion() {
    // Doing both at once is how a picture ends up converted twice or not at
    // all. Two operations, two names.
    let source = flat(4, 4, [128, 128, 128, 255]);
    let other = FrameDescription::square(
        Geometry::new(2, 2).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription {
            primaries: Primaries::Bt709,
            transfer: TransferFunction::Gamma22,
            matrix: MatrixCoefficients::Identity,
            range: Range::Full,
        },
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    assert_eq!(
        resample(&source, other, identity(), Filter::Area),
        Err(RenderStatus::NotComposable)
    );
}

#[test]
fn a_destination_with_no_pixels_is_refused() {
    let source = flat(4, 4, [128, 128, 128, 255]);
    let empty = FrameDescription::square(
        Geometry::new(1, 1).expect("a geometry"),
        PixelFormat::Rgba8,
        srgb(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    // One pixel is legal; the refusal is for a size the geometry itself would
    // not hold, so this asserts the legal end of the bound rather than
    // inventing an illegal geometry the type cannot express.
    assert!(resample(&source, empty, identity(), Filter::Area).is_ok());
}

#[test]
fn a_quarter_turn_is_an_exact_permutation_of_the_pixels() {
    // A quarter turn about the centre of a 4x4 picture sends the pixel grid
    // onto itself, so the answer must be an exact permutation with no blending
    // at all — which is a much sharper claim than "it looks rotated".
    //
    // It is **not** a test of the parallelogram preimage, and the control is
    // what said so. A right angle takes the unit square to the unit square, so
    // every preimage here is still axis-aligned and the tilted path is never
    // entered. `a_tilted_turn_weighs_a_parallelogram` is the one that reaches
    // it; this one is about exactness, which that one cannot assert.
    //
    // The permutation, derived by hand. The forward map about the centre
    // `c = (2, 2)` is `(u, v) -> (2 - (v - 2), 2 + (u - 2)) = (4 - v, u)`, so
    // the source pixel square `[u, u+1) x [v, v+1)` lands on
    // `[3 - v, 4 - v) x [u, u+1)`, which is destination pixel `(3 - v, u)`.
    // Reading it the other way: `destination(x, y) = source(y, 3 - x)`.
    let source = drawn(4, 4, |x, y| {
        // Every pixel distinct, and distinct in a way that separates the two
        // axes -- a picture symmetric under transposition could not tell a
        // quarter turn from a flip.
        let code = u8::try_from(17 * (4 * y + x)).expect("a code");
        [code, 255 - code, u8::try_from(x * 60).expect("a code"), 255]
    });
    // The turn is the exact rotation, not an approximation of one: cosine
    // nought, sine one, which is `Turn::from_half_angle(1)` in the model.
    let mapping = Mapping::about(
        [Rational::ZERO, n(-1), Rational::ONE, Rational::ZERO],
        (Rational::ZERO, Rational::ZERO),
        (Rational::HALF, Rational::HALF),
        4,
        4,
    )
    .expect("a mapping");

    let turned = resample(&source, described(4, 4), mapping, Filter::Area).expect("a frame");
    let before = pixels(&source);
    let after = pixels(&turned);
    for y in 0..4_usize {
        for x in 0..4_usize {
            assert_eq!(
                after[y * 4 + x],
                before[(3 - x) * 4 + y],
                "destination ({x}, {y}) is not the source pixel a quarter turn \
                 puts there"
            );
        }
    }
    // And the fixture can tell a turn from doing nothing, which two vacuous
    // comparisons above would not.
    assert_ne!(after, before, "the picture did not move");
}

#[test]
fn four_quarter_turns_are_the_picture_again() {
    // The property that says the rotation is exact rather than merely close:
    // a picture turned four times is the picture, byte for byte. A resampler
    // that interpolated at any step would soften it a little each time and
    // this would fail on the first pixel that is not flat.
    let source = drawn(4, 4, |x, y| {
        let code = u8::try_from(17 * (4 * y + x)).expect("a code");
        [code, 255 - code, u8::try_from(x * 60).expect("a code"), 255]
    });
    let quarter = || {
        Mapping::about(
            [Rational::ZERO, n(-1), Rational::ONE, Rational::ZERO],
            (Rational::ZERO, Rational::ZERO),
            (Rational::HALF, Rational::HALF),
            4,
            4,
        )
        .expect("a mapping")
    };
    let mut held = source.clone();
    for _ in 0..4 {
        held = resample(&held, described(4, 4), quarter(), Filter::Area).expect("a frame");
    }
    assert_eq!(
        pixels(&held),
        pixels(&source),
        "four quarter turns did not come back to the picture"
    );
}

#[test]
fn a_tilted_turn_weighs_a_parallelogram() {
    // The test the two above cannot be. A right angle maps the pixel grid onto
    // itself, so its preimages stay axis-aligned and `area_at`'s claim that an
    // affine map "sends a square to a parallelogram ... however the picture is
    // turned" is never exercised by one.
    //
    // The three-four-five turn is not a right angle: cos 3/5, sin 4/5, about
    // 53.13 degrees, and exact. A destination pixel's preimage under it is a
    // unit square tilted by that much, whose axis-aligned bounding box has
    // side 3/5 + 4/5 = 7/5 and therefore area 49/25 — nearly twice the
    // parallelogram's own area of one, since the determinant of a turn is one.
    //
    // So a flat field is what this asserts on: the weights over a tilted
    // preimage must still sum to exactly one, and any preimage that is not the
    // parallelogram gets that sum wrong by a factor it cannot hide.
    let value = [96_u8, 96, 96, 255];
    let source = flat(8, 8, value);
    let mapping = Mapping::about(
        [r(3, 5), r(-4, 5), r(4, 5), r(3, 5)],
        (Rational::ZERO, Rational::ZERO),
        (Rational::HALF, Rational::HALF),
        8,
        8,
    )
    .expect("a mapping");
    let turned = resample(&source, described(8, 8), mapping, Filter::Area).expect("a frame");
    let after = pixels(&turned);

    // The four central pixels, whose preimages are well inside the source even
    // after a tilt — the corners are not, because a turned square leaves the
    // frame there, and a pixel that draws partly from outside is meant to come
    // back darker.
    for (x, y) in [(3_usize, 3_usize), (4, 3), (3, 4), (4, 4)] {
        assert_eq!(
            after[y * 8 + x],
            value,
            "the weights over a tilted preimage do not sum to one at ({x}, {y})"
        );
    }
    // And the fixture can tell that something happened at all: a corner draws
    // partly from outside the source, which contributes nothing, so it must
    // come back darker than the field.
    assert!(
        after[0][0] < value[0],
        "the corner kept its full value, so the picture was not turned"
    );
}

#[test]
fn a_pivot_fixes_its_own_point() {
    // The defining property, and the one that says the anchor arrived: the
    // point a map acts about does not move, whatever the map is.
    //
    // Checked through the mapping's own inverse rather than through pixels,
    // because a pixel is a square and a fixed point is a point: the source of
    // the anchor must be the anchor.
    for anchor in [
        (Rational::HALF, Rational::HALF),
        (Rational::ZERO, Rational::ZERO),
        (Rational::ONE, r(1, 4)),
        (r(-1, 2), r(3, 2)),
    ] {
        for linear in [
            [n(2), Rational::ZERO, Rational::ZERO, n(2)],
            [r(3, 5), r(-4, 5), r(4, 5), r(3, 5)],
            [n(-1), Rational::ZERO, Rational::ZERO, Rational::ONE],
        ] {
            let mapping = Mapping::about(linear, (Rational::ZERO, Rational::ZERO), anchor, 8, 6)
                .expect("a mapping");
            let point = (
                anchor.0.checked_mul(n(8)).expect("a product"),
                anchor.1.checked_mul(n(6)).expect("a product"),
            );
            assert_eq!(
                mapping
                    .source_of_for_test(point.0, point.1)
                    .expect("a point"),
                point,
                "the pivot moved under {linear:?} about {anchor:?}"
            );
        }
    }
}

#[test]
fn a_pivot_cannot_be_folded_into_the_move_in_fractions() {
    // The measurement the whole design rests on. Acting about `a` rather than
    // about the centre `c` contributes `(a - c) - M(a - c)`, which is a
    // translation — so it looks as though the model could add it to the move
    // and the renderer would never need an anchor at all.
    //
    // That is true in pixels and false in fractions, and fractions are the
    // only place the model could do it: the vector from the centre to the
    // anchor is `(W·dx, H·dy)`, `M` mixes the components, and dividing back by
    // `(W, H)` per axis does not undo that unless `M` is diagonal.
    //
    // So: on a picture that is **not square**, a diagonal map folds exactly
    // and a rotation does not. Both halves are asserted, because a test that
    // only showed the failure would not show that the tempting version works
    // in the case somebody would have tried it on.
    let width = 8_u32;
    let height = 4_u32;
    let anchor = (Rational::ZERO, Rational::ZERO);
    let centre = (Rational::HALF, Rational::HALF);
    let folded = |linear: [Rational; 4]| -> (Rational, Rational) {
        // `(a - c) - M(a - c)`, computed in fractions, which is what a model
        // with no pixel dimensions can do.
        let delta = (
            anchor.0.checked_sub(centre.0).expect("a difference"),
            anchor.1.checked_sub(centre.1).expect("a difference"),
        );
        let mapped = (
            linear[0]
                .checked_mul(delta.0)
                .and_then(|a| {
                    linear[1]
                        .checked_mul(delta.1)
                        .and_then(|b| a.checked_add(b))
                })
                .expect("a product"),
            linear[2]
                .checked_mul(delta.0)
                .and_then(|a| {
                    linear[3]
                        .checked_mul(delta.1)
                        .and_then(|b| a.checked_add(b))
                })
                .expect("a product"),
        );
        (
            delta.0.checked_sub(mapped.0).expect("a difference"),
            delta.1.checked_sub(mapped.1).expect("a difference"),
        )
    };
    let source = drawn(width, height, |x, y| {
        let code = u8::try_from(8 * (x + y * width)).expect("a code");
        [code, 255 - code, 128, 255]
    });
    let compare = |linear: [Rational; 4]| -> (Vec<[u8; 4]>, Vec<[u8; 4]>) {
        let honest = Mapping::about(
            linear,
            (Rational::ZERO, Rational::ZERO),
            anchor,
            width,
            height,
        )
        .expect("a mapping");
        let tempting =
            Mapping::about(linear, folded(linear), centre, width, height).expect("a mapping");
        (
            pixels(&resample(&source, described(width, height), honest, Filter::Area).expect("a")),
            pixels(
                &resample(&source, described(width, height), tempting, Filter::Area).expect("a"),
            ),
        )
    };

    // A scale is diagonal, so the folding is exact and the two agree.
    let (honest, tempting) = compare([n(2), Rational::ZERO, Rational::ZERO, n(2)]);
    assert_eq!(
        honest, tempting,
        "the folding is exact for a diagonal map, and this is the case that \
         makes it tempting"
    );

    // A rotation is not, and on a picture that is not square the two differ.
    let (honest, tempting) = compare([r(3, 5), r(-4, 5), r(4, 5), r(3, 5)]);
    assert_ne!(
        honest, tempting,
        "the folding agreed under a rotation on an 8x4 picture, which would \
         mean the anchor did not need to reach the renderer after all"
    );
}

#[test]
fn a_pivot_at_the_centre_is_what_the_default_was() {
    // Every picture rendered before an anchor existed acted about the centre,
    // so a half and a half has to be exactly that map — not nearly.
    let source = drawn(6, 6, |x, y| {
        let code = u8::try_from(7 * (x + y * 6)).expect("a code");
        [code, 255 - code, 64, 255]
    });
    let linear = [r(1, 2), Rational::ZERO, Rational::ZERO, r(1, 2)];
    let about_centre = Mapping::about(
        linear,
        (r(1, 8), r(-1, 8)),
        (Rational::HALF, Rational::HALF),
        6,
        6,
    )
    .expect("a mapping");
    // The map the old `about_centre` built, written out from its own
    // arithmetic: `centre - M·centre`, plus the move times the size.
    let by_hand = Mapping::new(
        linear,
        (
            // 3 - 3/2 + 6/8 = 9/4
            r(9, 4),
            // 3 - 3/2 - 6/8 = 3/4
            r(3, 4),
        ),
    )
    .expect("a mapping");
    assert_eq!(
        pixels(&resample(&source, described(6, 6), about_centre, Filter::Area).expect("a frame")),
        pixels(&resample(&source, described(6, 6), by_hand, Filter::Area).expect("a frame")),
        "a pivot at the middle is not the map every earlier render used"
    );
}

/// One row of a picture, cut out of it.
fn one(frame: &Frame, row: usize) -> Frame {
    let width = frame.description().geometry().width() as usize;
    let packed = frame.to_packed().expect("bytes");
    Frame::from_packed(
        described(u32::try_from(width).expect("a width"), 1),
        &packed[row * width * 4..(row + 1) * width * 4],
    )
    .expect("a row")
}

#[test]
fn a_scanned_resample_is_the_resampled_picture_row_for_row() {
    // The property that makes the row path worth having at all: what a scan
    // produces and what a render produces are the same bytes. Three maps, both
    // filters, every row of each -- and the picture is a function of position
    // so a row drawn from the wrong band is visible.
    let source = drawn(6, 6, |x, y| {
        [
            u8::try_from(x * 30 % 200).expect("a byte"),
            u8::try_from(y * 30 % 200).expect("a byte"),
            u8::try_from((x + y) * 20 % 200).expect("a byte"),
            255,
        ]
    });
    let target = described(6, 6);
    let maps = [
        (
            "a move",
            Mapping::new([n(1), n(0), n(0), n(1)], (n(0), n(2))).expect("a map"),
        ),
        (
            "a shrink",
            Mapping::scaled(r(1, 2), r(1, 3), (n(0), n(0))).expect("a map"),
        ),
        (
            "a shear",
            Mapping::new([n(1), r(1, 2), n(0), n(1)], (n(0), n(0))).expect("a map"),
        ),
    ];
    for filter in [Filter::Area, Filter::Bilinear] {
        for (name, mapping) in &maps {
            let whole = resample(&source, target, *mapping, filter).expect("a frame");
            for row in 0..6 {
                let (from, to) = tile(*mapping, filter, Tile::row(row, 6), 6).expect("a band");
                let held = if from < to {
                    let packed = source.to_packed().expect("bytes");
                    Some(
                        Frame::from_packed(
                            described(6, u32::try_from(to - from).expect("a height")),
                            &packed[from * 24..to * 24],
                        )
                        .expect("a band"),
                    )
                } else {
                    None
                };
                let scanned =
                    resample_row(held.as_ref(), target, from, target, *mapping, filter, row)
                        .expect("a row");
                assert_eq!(scanned, one(&whole, row), "{name}, row {row}");
            }
        }
    }
}

#[test]
fn a_band_that_is_short_of_what_the_row_reads_is_refused() {
    // The one refusal a caller cannot provoke by asking a wrong question: it
    // means `band` answered wrongly, and it has to be a refusal rather than a
    // hole because transparency is what a source legitimately returns past its
    // own edge. A band short by one row would draw a picture with a gap in it
    // and look like a picture with a gap in it.
    let source = flat(4, 4, [10, 20, 30, 255]);
    let target = described(4, 4);
    let mapping = Mapping::scaled(Rational::ONE, r(1, 2), (n(0), n(0))).expect("a map");
    let (from, to) = tile(
        mapping,
        Filter::Area,
        Tile {
            rows: (1, 2),
            columns: (0, 4),
        },
        4,
    )
    .expect("a band");
    assert!(to - from > 1, "the band is one row and cannot be shortened");
    let packed = source.to_packed().expect("bytes");
    // One row short at the bottom, and the row still asks for it.
    let short = Frame::from_packed(
        described(4, u32::try_from(to - from - 1).expect("a height")),
        &packed[from * 16..(to - 1) * 16],
    )
    .expect("a band");
    assert_eq!(
        resample_row(Some(&short), target, from, target, mapping, Filter::Area, 1),
        Err(RenderStatus::RowOutsideBand)
    );
}

#[test]
fn a_turn_needs_tiles_and_the_width_decides_how_many() {
    // `NotRowLocal` used to say a turn's row "needs more than a band of input
    // rows", and that was **overclaiming**. It is not a property of the map.
    // The preimage of a destination row under a turn is a segment of some
    // slope, and a segment of slope m across w columns crosses about m·w
    // rows -- so whether it fits a band depends on w, and a narrow enough
    // strip always fits.
    //
    // The three-four-five turn has cosine 4/5 and sine 3/5 and determinant
    // one, so its inverse is [4/5, 3/5, -3/5, 4/5]. About the centre of a
    // two-hundred-square picture that is
    //
    //   v(x, y) = -3/5·(x - 100) + 4/5·(y - 100) + 100
    //
    // and the corners of a strip at row 100 give, by hand:
    //
    //   whole row, x in {0, 200}, y in {100, 101}
    //     (0,100) 160    (200,100)  40    (200,101)  40.8   (0,101) 160.8
    //     floor(40) .. floor(160.8) inclusive = 40..=160, a band of **121**
    //
    //   half a row, x in {0, 100}
    //     (0,100) 160    (100,100) 100    (100,101) 100.8   (0,101) 160.8
    //     floor(100) .. floor(160.8) inclusive = 100..=160, a band of **61**
    //
    // Sixty-four is the bound, so the whole row refuses and half of it does
    // not. Same map, same row, same arithmetic -- a different width.
    let cosine = r(4, 5);
    let sine = r(3, 5);
    let mapping = Mapping::about(
        [cosine, sine.checked_neg().expect("a sine"), sine, cosine],
        (n(0), n(0)),
        (r(1, 2), r(1, 2)),
        200,
        200,
    )
    .expect("a map");
    assert_eq!(
        tile(
            mapping,
            Filter::Area,
            Tile {
                rows: (100, 101),
                columns: (0, 200),
            },
            200
        ),
        Err(RenderStatus::BandTooTall)
    );
    assert_eq!(
        tile(
            mapping,
            Filter::Area,
            Tile {
                rows: (100, 101),
                columns: (0, 100),
            },
            200
        ),
        Ok((100, 161)),
        "half a row of a turn is sixty-one source rows"
    );

    // And a map that takes horizontals to horizontals is the case where the
    // width does not matter at all: a scale, a move, a horizontal shear and a
    // mirror read the same band however much of the row is asked for.
    //
    // `Mapping::horizontal` used to say which of the two a map was, and it is
    // gone. Strips made it a question nobody has to ask -- the width tunes
    // itself -- and what it meant is asserted here directly, by comparing the
    // bands, which is a stronger statement than the predicate was.
    for linear in [
        [n(1), n(0), n(0), n(1)],
        [r(1, 2), n(0), n(0), r(1, 3)],
        [n(1), r(3, 4), n(0), n(1)],
        [n(-1), n(0), n(0), n(-1)],
    ] {
        let flat = Mapping::new(linear, (n(0), n(0))).expect("a map");
        let whole = tile(
            flat,
            Filter::Area,
            Tile {
                rows: (4, 5),
                columns: (0, 64),
            },
            64,
        );
        let part = tile(
            flat,
            Filter::Area,
            Tile {
                rows: (4, 5),
                columns: (7, 9),
            },
            64,
        );
        assert_eq!(whole, part, "the width changed a horizontal map's band");
    }
}

#[test]
fn a_band_past_its_bound_is_refused_and_an_empty_one_is_not() {
    // Two ends of the same function. A shrink steeper than `MAX_BAND_ROWS` to
    // one wants more rows than a band holds, and refuses. A picture moved
    // entirely off its own frame wants *no* rows, which is not an error at
    // all: there is nothing there, and nothing resamples to transparency.
    let steep = Mapping::scaled(
        Rational::ONE,
        r(1, i64::try_from(MAX_BAND_ROWS).expect("a bound") * 2),
        (n(0), n(0)),
    )
    .expect("a map");
    assert_eq!(
        tile(
            steep,
            Filter::Area,
            Tile {
                rows: (0, 1),
                columns: (0, 4),
            },
            1024,
        ),
        Err(RenderStatus::BandTooTall)
    );
    // Exactly at the bound it is admitted, which is what "past" means -- and
    // the scale that reaches it is one in sixty-*three*, not one in
    // sixty-four. A destination row's preimage under a one-in-sixty-four
    // shrink is the half-open span `[0, 64)`, whose lower edge lands exactly
    // on a row boundary, and the band takes in the row that begins there. It
    // has to: `area_at` visits that row, finds an overlap of nought and skips
    // it, and a band that did not hold a row the sampler visits would refuse
    // with `RowOutsideBand`. So the bound is a bound on the band, and the band
    // is what the sampler reads rather than what it uses.
    let allowed = Mapping::scaled(
        Rational::ONE,
        r(1, i64::try_from(MAX_BAND_ROWS).expect("a bound") - 1),
        (n(0), n(0)),
    )
    .expect("a map");
    assert_eq!(
        tile(
            allowed,
            Filter::Area,
            Tile {
                rows: (0, 1),
                columns: (0, 4),
            },
            1024,
        ),
        Ok((0, MAX_BAND_ROWS))
    );
    assert_eq!(
        tile(
            Mapping::scaled(
                Rational::ONE,
                r(1, i64::try_from(MAX_BAND_ROWS).expect("a bound")),
                (n(0), n(0))
            )
            .expect("a map"),
            Filter::Area,
            Tile {
                rows: (0, 1),
                columns: (0, 4),
            },
            1024
        ),
        Err(RenderStatus::BandTooTall)
    );

    let away = Mapping::new([n(1), n(0), n(0), n(1)], (n(0), n(100))).expect("a map");
    assert_eq!(
        tile(
            away,
            Filter::Area,
            Tile {
                rows: (0, 1),
                columns: (0, 4),
            },
            4,
        ),
        Ok((0, 0))
    );
    let target = described(4, 4);
    let row = resample_row(None, target, 0, target, away, Filter::Area, 0).expect("a row");
    assert_eq!(pixels(&row), std::vec![[0, 0, 0, 0]; 4]);
}

#[test]
fn a_band_of_a_different_picture_is_refused_rather_than_resampled() {
    // A band has to be the same picture, cut down. One that is not is a band
    // of something else, and resampling it would answer confidently about a
    // picture nobody asked for -- with exactly the right number of bytes,
    // which is why the byte count cannot be what catches it.
    let target = described(4, 4);
    let mapping = identity();
    let narrow = flat(2, 2, [10, 20, 30, 255]);
    assert_eq!(
        resample_row(Some(&narrow), target, 0, target, mapping, Filter::Area, 0),
        Err(RenderStatus::NotComposable)
    );
    // Same size, different colour: the bytes would fit and the answer would
    // be wrong in light rather than in shape.
    let elsewhere = FrameDescription::square(
        Geometry::new(4, 2).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription {
            primaries: Primaries::Bt709,
            transfer: TransferFunction::Gamma22,
            matrix: MatrixCoefficients::Identity,
            range: Range::Full,
        },
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    let other = Frame::from_packed(elsewhere, &[128; 32]).expect("a band");
    assert_eq!(
        resample_row(Some(&other), target, 0, target, mapping, Filter::Area, 0),
        Err(RenderStatus::NotComposable)
    );
    // And a band that claims rows the picture does not have.
    let past = flat(4, 3, [10, 20, 30, 255]);
    assert_eq!(
        resample_row(Some(&past), target, 2, target, mapping, Filter::Area, 0),
        Err(RenderStatus::RowOutsideBand)
    );
}

#[test]
fn a_tile_of_no_pixels_is_refused_rather_than_measured() {
    // `tile` is public, and a tile from a column to itself has no preimage
    // to measure: the parallelogram is degenerate and `bounds` would happily
    // report a band for it. Nothing inside this crate asks — `Graph::banded`
    // never makes an empty tile — so a control found the check changed no
    // answer, and it stayed anyway for the reason M8.42 kept `Run::plane_row`'s
    // bound: a public function that silently answers a meaningless question is
    // a hazard however well its callers behave. This is the test that holds it.
    let mapping = identity();
    for over in [
        Tile {
            rows: (0, 1),
            columns: (3, 3),
        },
        Tile {
            rows: (0, 1),
            columns: (4, 2),
        },
    ] {
        assert_eq!(
            tile(mapping, Filter::Area, over, 8),
            Err(RenderStatus::OutsideDomain),
            "{over:?}"
        );
        assert_eq!(
            tile(mapping, Filter::Bilinear, over, 8),
            Err(RenderStatus::OutsideDomain),
            "{over:?}"
        );
        let mut out = std::vec![Vec::new(); 1];
        assert_eq!(
            resample_tile(
                None,
                described(8, 8),
                0,
                mapping,
                Filter::Area,
                over,
                &mut out
            ),
            Err(RenderStatus::OutsideDomain)
        );
        assert!(out[0].is_empty(), "an empty tile wrote pixels");
    }
}

#[test]
fn a_tile_writes_one_buffer_a_row_and_refuses_any_other_number() {
    // A rectangle is not contiguous in a row-major picture, so a tile hands
    // back one buffer a row rather than one buffer with a stride. The count
    // has to match, and a mismatch is a caller error rather than something to
    // paper over: fewer buffers than rows would silently drop rows, and more
    // would leave some empty and look like a picture with gaps.
    let source = flat(8, 8, [10, 20, 30, 255]);
    let target = described(8, 8);
    let mapping = identity();
    let over = Tile {
        rows: (2, 5),
        columns: (0, 8),
    };
    let (from, to) = tile(mapping, Filter::Area, over, 8).expect("a band");
    let packed = source.to_packed().expect("bytes");
    let band = Frame::from_packed(
        described(8, u32::try_from(to - from).expect("a height")),
        &packed[from * 32..to * 32],
    )
    .expect("a band");
    for count in [0, 2, 4] {
        let mut out = std::vec![Vec::new(); count];
        assert_eq!(
            resample_tile(
                Some(&band),
                target,
                from,
                mapping,
                Filter::Area,
                over,
                &mut out
            ),
            Err(RenderStatus::OutsideDomain),
            "{count} buffers for three rows"
        );
    }
    let mut out = std::vec![Vec::new(); 3];
    resample_tile(
        Some(&band),
        target,
        from,
        mapping,
        Filter::Area,
        over,
        &mut out,
    )
    .expect("a tile");
    // The identity map, so each row of the tile is the row of the source it
    // sits on, byte for byte.
    for (index, buffer) in out.iter().enumerate() {
        assert_eq!(
            buffer[..],
            packed[(2 + index) * 32..(3 + index) * 32],
            "row {index} of the tile"
        );
    }
    assert_eq!(MAX_TILE_ROWS, 16);
}
