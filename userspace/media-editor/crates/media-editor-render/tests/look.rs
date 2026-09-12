// SPDX-License-Identifier: GPL-3.0-only
//! Applying a lookup table to a frame.
//!
//! The cube's own tests are about interpolation. These are about the three
//! decisions that have to be made before a single sample reaches it: which
//! encoding the table was authored for, what happens to coverage, and what a
//! table does to the channels it was not given.

use media_editor_core::{Fixed, Rational};
use media_editor_media::colour::{Range, TransferFunction};
use media_editor_media::{
    AlphaState, ColourDescription, Frame, FrameDescription, Geometry, PixelFormat,
};
use media_editor_render::lut::{Colour, Interpolation, Lut3D};
use media_editor_render::{Look, RenderStatus};

fn described(format: PixelFormat, colour: ColourDescription) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(4, 2).expect("a geometry"),
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

/// A fixed-point value from a fraction.
fn at(numerator: i64, denominator: i64) -> Fixed {
    Fixed::from_rational(Rational::new(numerator, denominator).expect("a ratio")).expect("a value")
}

/// A frame whose pixels are the bytes given, repeated to fill it.
fn frame(description: &FrameDescription, pattern: &[u8]) -> Frame {
    let wanted = description.packed_bytes().expect("a size");
    let bytes: std::vec::Vec<u8> = pattern.iter().copied().cycle().take(wanted).collect();
    Frame::from_packed(*description, &bytes).expect("a frame")
}

/// A table that swaps red and blue, leaving green and the diagonal alone.
///
/// Neutral on its own diagonal, so a grey must survive it, and violently
/// non-neutral everywhere else, so anything that reaches the table wrongly
/// shows.
fn swap_red_and_blue(size: usize) -> Lut3D {
    let last = i64::try_from(size - 1).expect("a size");
    let mut samples = std::vec::Vec::new();
    for blue in 0..size {
        for green in 0..size {
            for red in 0..size {
                let (r, g, b) = (
                    i64::try_from(red).expect("an index"),
                    i64::try_from(green).expect("an index"),
                    i64::try_from(blue).expect("an index"),
                );
                samples.push([at(b, last), at(g, last), at(r, last)] as Colour);
            }
        }
    }
    Lut3D::new(size, samples).expect("a table")
}

#[test]
fn an_identity_look_leaves_a_frame_alone() {
    // Not "nearly": every sample must come back the code value it went in as.
    // A table that changes nothing, applied in the space it was authored for,
    // has nothing to round — the lattice reproduces exactly and the
    // normalisation and quantisation are each other's inverse on a code value.
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgb8, colour);
    let original = frame(&description, &[0, 17, 128, 200, 255, 64]);
    let look = Look::new(
        Lut3D::identity(17).expect("a table"),
        colour,
        Interpolation::Tetrahedral,
    );
    let after = look.apply(&original, Rational::ONE).expect("a frame");
    assert_eq!(
        after.to_packed().expect("bytes"),
        original.to_packed().expect("bytes"),
        "an identity table moved a sample"
    );
    assert_eq!(after.description(), original.description());
}

#[test]
fn a_look_changes_the_numbers_and_not_what_they_mean() {
    // The output is described exactly as the input was. A table authored for a
    // display encoding takes display-encoded values to display-encoded values,
    // and claiming the output was in some other space would be a conversion
    // nobody performed.
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgb8, colour);
    let original = frame(&description, &[200, 40, 10]);
    let look = Look::new(swap_red_and_blue(5), colour, Interpolation::Tetrahedral);
    let after = look.apply(&original, Rational::ONE).expect("a frame");

    assert_eq!(
        after.description(),
        original.description(),
        "the description moved"
    );
    let bytes = after.to_packed().expect("bytes");
    assert_eq!(
        &bytes[..3],
        &[10, 40, 200],
        "red and blue did not swap, so the table did not reach the pixel"
    );
}

#[test]
fn a_grey_survives_a_look_that_is_neutral_on_its_diagonal() {
    // The property the whole interpolation choice was made for, end to end
    // through a frame rather than in the cube on its own. Every grey in, every
    // grey out.
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgb8, colour);
    let look = Look::new(swap_red_and_blue(5), colour, Interpolation::Tetrahedral);
    for level in [0_u8, 1, 37, 128, 199, 254, 255] {
        let grey = frame(&description, &[level, level, level]);
        let after = look.apply(&grey, Rational::ONE).expect("a frame");
        let bytes = after.to_packed().expect("bytes");
        assert_eq!(
            bytes[0],
            bytes[1],
            "a grey at {level} came out tinted: {:?}",
            &bytes[..3]
        );
        assert_eq!(bytes[1], bytes[2], "a grey at {level} came out tinted");
    }
}

#[test]
fn a_frame_in_another_encoding_is_refused() {
    // A show LUT built for a camera's log curve, applied to display-referred
    // pictures, is the wrong look on every pixel — and nothing crashes, and
    // there is nothing to compare against. So the look carries the encoding it
    // was authored for and a frame that does not match is refused rather than
    // fed in anyway.
    let authored = ColourDescription::srgb_full();
    let look = Look::new(
        Lut3D::identity(5).expect("a table"),
        authored,
        Interpolation::Tetrahedral,
    );

    let mut other = authored;
    other.transfer = TransferFunction::Gamma22;
    let description = described(PixelFormat::Rgb8, other);
    assert_eq!(
        look.apply(&frame(&description, &[128, 128, 128]), Rational::ONE)
            .map(|_| ()),
        Err(RenderStatus::LookSpaceMismatch)
    );

    // A different range is a different encoding too: the same code value means
    // a different amount of light.
    let mut limited = authored;
    limited.range = Range::Limited;
    let described_limited = described(PixelFormat::Rgb8, limited);
    assert_eq!(
        look.apply(&frame(&described_limited, &[128, 128, 128]), Rational::ONE)
            .map(|_| ()),
        Err(RenderStatus::LookSpaceMismatch)
    );
}

#[test]
fn premultiplied_coverage_is_refused_rather_than_quietly_undone() {
    // A table is a non-linear function applied per pixel. On premultiplied
    // samples that computes f(ac) where the answer wanted is a·f(c), and those
    // agree only when f is linear or a is one — and full coverage is exactly
    // what a test made of opaque bars would cover, which is how the same
    // mistake reached the conversion path once already.
    //
    // Unpremultiplying here would be worse than refusing: it is lossy, so
    // doing it silently spends a real quantity of the caller's picture on a
    // step the caller did not ask for.
    let colour = ColourDescription::srgb_full();
    let straight = described(PixelFormat::Rgba8, colour);
    let look = Look::new(
        Lut3D::identity(5).expect("a table"),
        colour,
        Interpolation::Tetrahedral,
    );
    assert!(
        look.apply(&frame(&straight, &[10, 20, 30, 128]), Rational::ONE)
            .is_ok(),
        "straight coverage was refused"
    );

    let premultiplied = FrameDescription::square(
        Geometry::new(4, 2).expect("a geometry"),
        PixelFormat::Rgba8,
        colour,
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    assert_eq!(
        look.apply(&frame(&premultiplied, &[10, 20, 30, 128]), Rational::ONE)
            .map(|_| ()),
        Err(RenderStatus::LookPremultiplied)
    );
}

#[test]
fn coverage_is_carried_through_untouched() {
    // The last time something in this crate wrote a constant into an alpha
    // byte instead of carrying it, every keyed frame that went through came
    // out a solid rectangle — and the test that should have caught it used
    // opaque bars, which have nothing to lose. So this one is keyed, at four
    // different coverages, and the table is one that changes the colour so a
    // pass-through cannot be mistaken for the table doing nothing.
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgba8, colour);
    let original = frame(
        &description,
        &[
            200, 40, 10, 0, 200, 40, 10, 90, 200, 40, 10, 200, 200, 40, 10, 255,
        ],
    );
    let look = Look::new(swap_red_and_blue(5), colour, Interpolation::Tetrahedral);
    let after = look.apply(&original, Rational::ONE).expect("a frame");

    let before = original.to_packed().expect("bytes");
    let bytes = after.to_packed().expect("bytes");
    for pixel in 0..8 {
        assert_eq!(
            bytes[pixel * 4 + 3],
            before[pixel * 4 + 3],
            "pixel {pixel} had its coverage rewritten"
        );
        assert_ne!(
            &bytes[pixel * 4..pixel * 4 + 3],
            &before[pixel * 4..pixel * 4 + 3],
            "pixel {pixel} was not touched at all, so this proves nothing"
        );
    }
}

#[test]
fn a_format_that_is_not_red_green_blue_is_refused() {
    // A table maps three colour channels to three. A luma-chroma frame needs
    // the matrix taken out of it first, and a grey frame has nowhere to put a
    // colour — each of those is a named step of its own rather than something
    // to do on the way past.
    //
    // Each look is built for the *same* description as the frame it is given,
    // so what refuses is the format check and not the encoding check. A test
    // that let the encodings differ would pass for the wrong reason and would
    // go on passing if the format check were deleted.
    let grey_colour = ColourDescription::srgb_full();
    let video_colour = ColourDescription::bt709_limited();
    for (format, colour) in [
        (PixelFormat::Gray8, grey_colour),
        (PixelFormat::Yuv444p8, video_colour),
        (PixelFormat::Yuv422p8, video_colour),
    ] {
        let description = FrameDescription::square(
            Geometry::new(4, 2).expect("a geometry"),
            format,
            colour,
            if format.is_subsampled() {
                Some(media_editor_media::ChromaSiting::Centre)
            } else {
                None
            },
            None,
        )
        .expect("a description");
        let look = Look::new(
            Lut3D::identity(5).expect("a table"),
            colour,
            Interpolation::Tetrahedral,
        );
        assert_eq!(
            look.apply(&Frame::blank(description).expect("a frame"), Rational::ONE)
                .map(|_| ()),
            Err(RenderStatus::LookNotRgb),
            "{format:?} was accepted"
        );
    }
}

#[test]
fn a_look_is_the_same_look_every_time() {
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgba8, colour);
    let original = frame(&description, &[3, 130, 250, 77, 199, 8, 44, 255]);
    let look = Look::new(swap_red_and_blue(9), colour, Interpolation::Tetrahedral);
    let first = look.apply(&original, Rational::ONE).expect("a frame");
    let second = look.apply(&original, Rational::ONE).expect("a frame");
    assert_eq!(first.digest(), second.digest());
}

/// A table that takes every colour to black.
///
/// Chosen because it makes the strength arithmetic readable by hand: at a
/// strength of `s` the answer is the input scaled by `1 - s`, in whatever
/// space the mix happens in, and the two candidate spaces disagree loudly
/// about what that means.
fn to_black(size: usize) -> Lut3D {
    let samples = std::vec![[Fixed::ZERO; 3] as Colour; size * size * size];
    Lut3D::new(size, samples).expect("a table")
}

#[test]
fn half_a_look_mixes_code_values_and_not_light() {
    // Derive the expected value from the code-space mixing definition.
    //
    // A mid-grey of 128 through a table that takes everything to black, half
    // on. In *code values* the mix is `c + s(f(c) - c)` with `c = 128/255`,
    // `f(c) = 0` and `s = 1/2`, which is `64/255`, which quantises to **64**.
    //
    // In *linear light* it would be sRGB's 128 decoded — 0.215861 — halved to
    // 0.107930 and encoded again, which is 92.374 and quantises to 92.
    //
    // A mid-grey rather than black or white on purpose. At both of those the
    // two spaces agree exactly, because nought and one are the fixed points of
    // every transfer curve, and this file's own notes record a resampling test
    // that could not tell light from code values for precisely that reason.
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgb8, colour);
    let look = Look::new(to_black(9), colour, Interpolation::Tetrahedral);

    let after = look
        .apply(
            &frame(&description, &[128, 128, 128]),
            Rational::new(1, 2).expect("a ratio"),
        )
        .expect("a frame");
    let packed = after.to_packed().expect("bytes");
    assert_eq!(
        &packed[..3],
        &[64, 64, 64],
        "half a look landed somewhere other than half way along the table's \
         own output; 92 would mean the mix moved into linear light"
    );
    assert_ne!(
        packed[0], 92,
        "and this is the answer it must not be, named so that a reader can \
         see the two are far apart rather than taking it on trust"
    );
}

#[test]
fn a_look_at_no_strength_hands_back_the_frame_exactly() {
    // Exactly, not nearly, and for every code value there is. That is a
    // stronger claim than it looks: it says normalising a byte and quantising
    // it back is the identity across the whole range, which is what lets the
    // strength be a mix rather than a special case with a short circuit in
    // front of it.
    //
    // It is also why the planner adds the node at every strength. Skipping it
    // at nought would be an optimisation whose absence changes no answer, and
    // a guard no test can hold is a guard this project has learned to delete.
    let colour = ColourDescription::srgb_full();
    let description = FrameDescription::square(
        Geometry::new(256, 1).expect("a geometry"),
        PixelFormat::Rgb8,
        colour,
        None,
        None,
    )
    .expect("a description");
    let every: std::vec::Vec<u8> = (0..=255_u8)
        .flat_map(|code| [code, 255 - code, 128])
        .collect();
    let original = Frame::from_packed(description, &every).expect("a frame");
    let look = Look::new(to_black(9), colour, Interpolation::Tetrahedral);

    let after = look.apply(&original, Rational::ZERO).expect("a frame");
    assert_eq!(
        after.to_packed().expect("bytes"),
        every,
        "a look nobody turned on changed the picture"
    );
    // And the fixture can tell: at full strength the same table flattens it.
    let graded = look.apply(&original, Rational::ONE).expect("a frame");
    assert!(
        graded
            .to_packed()
            .expect("bytes")
            .iter()
            .all(|code| *code == 0),
        "the table does not reach the pixels, so the comparison above proves \
         nothing"
    );
}

#[test]
fn a_look_at_full_strength_is_the_look_applied_flat() {
    // The property every project written before a strength existed depends on:
    // a grade nobody animated reads one, and one has to be the picture this
    // crate produced when there was no strength at all — byte for byte, not
    // within a code value.
    //
    // It holds because a `Fixed` multiplied by one is shifted back to exactly
    // where it started -- a property of the multiply rather than of how the
    // mix is arranged. `mix`'s comment says so, and says that the first
    // version of it claimed more.
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgb8, colour);
    let original = frame(&description, &[7, 61, 128, 200, 255, 33]);
    let look = Look::new(swap_red_and_blue(9), colour, Interpolation::Tetrahedral);

    let full = look.apply(&original, Rational::ONE).expect("a frame");
    // Computed the long way round, from the table alone, so this is not the
    // mix compared against itself: red and blue swapped, green untouched.
    let expected: std::vec::Vec<u8> = original
        .to_packed()
        .expect("bytes")
        .chunks_exact(3)
        .flat_map(|pixel| [pixel[2], pixel[1], pixel[0]])
        .collect();
    assert_eq!(
        full.to_packed().expect("bytes"),
        expected,
        "a look all the way on is not the look"
    );
}

#[test]
fn a_strength_outside_none_to_all_is_refused() {
    // Refused here rather than clamped, and the model clamps rather than
    // refusing, and the two are not one guard written twice. A curve's
    // verticals are deliberately unclamped so an ease may overshoot, and that
    // overshoot is the model's to absorb. A *caller* asking for a picture on
    // the far side of a table that was never sampled there is asking for
    // arithmetic rather than for a grade.
    let colour = ColourDescription::srgb_full();
    let description = described(PixelFormat::Rgb8, colour);
    let held = frame(&description, &[128, 128, 128]);
    let look = Look::new(to_black(9), colour, Interpolation::Tetrahedral);

    for outside in [
        Rational::new(3, 2).expect("a ratio"),
        Rational::new(-1, 100).expect("a ratio"),
    ] {
        assert_eq!(
            look.apply(&held, outside).expect_err("a refusal"),
            RenderStatus::LookStrengthOutOfRange,
            "a strength of {outside:?} was accepted"
        );
    }
    // And the ends themselves are inside. Making either comparison strict is
    // the mutation that turns "none of it" and "all of it" — the two strengths
    // every project actually holds — into the two that are refused.
    for inside in [Rational::ZERO, Rational::ONE] {
        assert!(
            look.apply(&held, inside).is_ok(),
            "a strength of {inside:?} is at an end of the range, not outside it"
        );
    }
}
