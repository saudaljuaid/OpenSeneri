// SPDX-License-Identifier: GPL-3.0-only
//! A title's colour, and what naming it in light actually buys.
//!
//! The model holds three fractions of full light and says the reason is that a
//! code value is a number in an encoding. This is where that claim is either
//! true or empty: the **same** ink is drawn into two frames whose encodings
//! differ, and it has to come out as two different numbers, each one the
//! number that frame spells that colour with.
//!
//! Type was packed as `u8::MAX` before there was an ink, and 255 is not a
//! legal code value in limited range, where white is 235 and the codes above
//! it are reserved. What that cost was measured rather than assumed, and it
//! turned out to be two different things in the two places type is drawn —
//! nothing at all for a title, and a refusal for a slate caption. Both are
//! below, each saying which.

use media_editor_core::Rational;
use media_editor_media::colour::{MatrixCoefficients, Primaries, Range, TransferFunction};
use media_editor_media::{
    AlphaState, ColourDescription, Frame, FrameDescription, FramePool, Geometry, PixelFormat,
};
use media_editor_render::{Graph, Look, Node, RenderStatus, font::Alignment};

/// A library that refuses everything: a title is drawn, never fetched.
struct NoMedia;

impl media_editor_render::Library for NoMedia {
    fn frame(
        &mut self,
        _media: media_editor_core::Digest,
        _tick: i64,
        _description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        Err(RenderStatus::UnknownNode)
    }

    fn look(&mut self, _look: media_editor_core::Digest) -> Result<Look, RenderStatus> {
        Err(RenderStatus::UnknownNode)
    }
}

fn described(colour: ColourDescription) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(64, 32).expect("a geometry"),
        PixelFormat::Rgba8,
        colour,
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description")
}

/// The same encoding as [`ColourDescription::srgb_full`] but limited-range.
///
/// Only the range differs, deliberately: if the two whites come out different
/// numbers it is because of the range and not because a transfer function
/// changed underneath the comparison. The matrix stays `Identity`, because an
/// RGB pixel format has no luma-chroma matrix to apply and the description
/// refuses the pairing by name.
fn srgb_limited() -> ColourDescription {
    ColourDescription::new(
        Primaries::Bt709,
        TransferFunction::Srgb,
        MatrixCoefficients::Identity,
        Range::Limited,
    )
}

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

/// The card, at whatever encoding and in whatever ink.
fn card(description: FrameDescription, ink: [Rational; 3]) -> Frame {
    let mut graph = Graph::new();
    let node = graph
        .add(Node::Type {
            description,
            lines: std::vec!["H".into()],
            size: r(1, 2),
            across: r(1, 2),
            down: r(1, 2),
            alignment: Alignment::Centre,
            ink,
        })
        .expect("a node");
    graph
        .evaluate(node, &mut FramePool::new(8, 1 << 20), &mut NoMedia)
        .expect("a frame")
}

/// The colour of the most opaque pixel in a straight frame.
///
/// The most opaque rather than the first, because most of a card is
/// transparent and a transparent pixel's colour is not what was asked about.
fn ink_of(frame: &Frame) -> [u8; 3] {
    let packed = frame.to_packed().expect("the samples");
    let mut best = ([0, 0, 0], 0);
    for pixel in packed.chunks_exact(4) {
        if pixel[3] > best.1 {
            best = ([pixel[0], pixel[1], pixel[2]], pixel[3]);
        }
    }
    assert_eq!(best.1, u8::MAX, "a card has some fully covered pixels");
    best.0
}

#[test]
fn the_same_white_is_a_different_number_in_two_encodings() {
    // The whole argument for naming a colour in light, in one assertion. Full
    // range spells full light 255; limited range spells it 235, and the codes
    // above 235 are reserved for things that are not picture.
    let white = [Rational::ONE; 3];
    assert_eq!(
        ink_of(&card(described(ColourDescription::srgb_full()), white)),
        [255, 255, 255]
    );
    assert_eq!(
        ink_of(&card(described(srgb_limited()), white)),
        [235, 235, 235]
    );
}

#[test]
fn a_limited_range_card_composites_to_this_frames_white() {
    // And it did before the ink existed too, which is worth saying plainly
    // rather than claiming a fix that was not one. A card's letters are drawn
    // from a hard-edged stencil -- the coverage plane holds 0 and 255 and
    // nothing between -- so the only premultiplied samples are full light at
    // full coverage and no light at none. `encode(decode(255))` searches only
    // the legal codes and lands on 235, so the illegal byte was clamped away
    // on the way out and nobody ever saw it.
    //
    // The ink makes that correct by construction instead of by accident. This
    // test holds the outcome either way, which is the point: it is the claim
    // about the *encoding*, and the claim about the bug is the caption's.
    let description = FrameDescription::square(
        Geometry::new(64, 32).expect("a geometry"),
        PixelFormat::Rgba8,
        srgb_limited(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    let mut graph = Graph::new();
    let ground = graph.add(Node::Blank { description }).expect("a blank");
    let letters = graph
        .add(Node::Type {
            description,
            lines: std::vec!["H".into()],
            size: r(1, 2),
            across: r(1, 2),
            down: r(1, 2),
            alignment: Alignment::Centre,
            ink: [Rational::ONE; 3],
        })
        .expect("a node");
    let over = graph
        .add(Node::Over {
            layers: [ground, letters],
        })
        .expect("an over");
    let frame = graph
        .evaluate(over, &mut FramePool::new(8, 1 << 20), &mut NoMedia)
        .expect("a card over a ground");
    let packed = frame.to_packed().expect("the samples");
    // The premise of the comment above, asserted rather than assumed: the
    // stencil is hard-edged, so there is no partly covered pixel to go wrong.
    let letters_only = graph
        .evaluate(letters, &mut FramePool::new(8, 1 << 20), &mut NoMedia)
        .expect("the letters");
    let mut coverage: std::vec::Vec<u8> = letters_only
        .to_packed()
        .expect("the samples")
        .chunks_exact(4)
        .map(|pixel| pixel[3])
        .collect();
    coverage.sort_unstable();
    coverage.dedup();
    assert_eq!(coverage, [0, u8::MAX], "a card's letters have no soft edge");
    let brightest = packed
        .chunks_exact(4)
        .flat_map(|pixel| pixel[..3].iter().copied())
        .max()
        .expect("some samples");
    assert_eq!(
        brightest, 235,
        "and what comes out is this frame's white, not a code above it"
    );
}

#[test]
fn an_ink_reaches_the_picture_channel_by_channel() {
    // Red at full light, nothing else. If the three channels were packed in
    // the wrong order, or one of them were dropped, this is where it shows.
    let red = ink_of(&card(
        described(ColourDescription::srgb_full()),
        [Rational::ONE, Rational::ZERO, Rational::ZERO],
    ));
    assert_eq!(red, [255, 0, 0]);
    let green = ink_of(&card(
        described(ColourDescription::srgb_full()),
        [Rational::ZERO, Rational::ONE, Rational::ZERO],
    ));
    assert_eq!(green, [0, 255, 0]);
    let blue = ink_of(&card(
        described(ColourDescription::srgb_full()),
        [Rational::ZERO, Rational::ZERO, Rational::ONE],
    ));
    assert_eq!(blue, [0, 0, 255]);
}

#[test]
fn half_the_light_is_not_half_the_code_value() {
    // sRGB bends, which is the reason none of this can be done in code values
    // and expected to mean anything. Half of full light is 188, not 128 --
    // derived from the definition rather than from the code: sRGB encodes
    // 0.5 as 1.055 x 0.5^(1/2.4) - 0.055 = 0.7354, and 0.7354 x 255 = 187.5,
    // which the table's nearest-in-light search lands on 188.
    let grey = ink_of(&card(
        described(ColourDescription::srgb_full()),
        [r(1, 2); 3],
    ));
    assert_eq!(grey, [188, 188, 188]);
    assert_ne!(grey[0], 128, "which is what a code-value half would give");
}

#[test]
fn a_mid_tone_is_spelled_differently_in_the_two_ranges() {
    // White cannot show this and neither can black: both sit at an end of the
    // scale, and the premultiply on the way out re-encodes through the frame's
    // own table and quietly lands on the legal extreme whatever was packed. A
    // half is the case where the table that was consulted actually shows.
    //
    // Derived from the definitions rather than from the code. sRGB encodes
    // half of full light as 1.055 x 0.5^(1/2.4) - 0.055 = 0.73536. Full range
    // spells that 0.73536 x 255 = 187.5, and the table's nearest-in-light
    // search takes 188. Limited range spells it 16 + 0.73536 x 219 = 177.0,
    // which is 177.
    let half = [r(1, 2); 3];
    assert_eq!(
        ink_of(&card(described(ColourDescription::srgb_full()), half)),
        [188, 188, 188]
    );
    assert_eq!(
        ink_of(&card(described(srgb_limited()), half)),
        [177, 177, 177]
    );
}

#[test]
fn two_inks_are_two_cache_keys() {
    // A key that ignored the ink would serve a white card to somebody who
    // asked for a red one, and the fault would look like a caching bug rather
    // than like a missing field.
    let mut graph = Graph::new();
    let mut keys = std::vec::Vec::new();
    for ink in [
        [Rational::ONE; 3],
        [Rational::ONE, Rational::ZERO, Rational::ZERO],
    ] {
        let node = graph
            .add(Node::Type {
                description: described(ColourDescription::srgb_full()),
                lines: std::vec!["H".into()],
                size: r(1, 2),
                across: r(1, 2),
                down: r(1, 2),
                alignment: Alignment::Centre,
                ink,
            })
            .expect("a node");
        keys.push(graph.identity(node).expect("an identity"));
    }
    assert_ne!(keys[0], keys[1]);
}

#[test]
fn a_slate_caption_asks_the_table_for_white_as_well() {
    // A legend carries no colour and never will -- a caption's look is not
    // something anybody chose, which is the whole distinction from a title.
    // But "white" is still a code value, and the one it packed was 255.
    //
    // Here that *did* cost something, and the reason is the difference from
    // the card above: a caption's coverage plane is antialiased and holds
    // values between nought and full. A partly covered pixel premultiplied
    // from a code of 255 holds `encode(decode(255) x coverage)`, and
    // `decode(255)` in limited range is about 1.09 of full light -- so the
    // sample sits above the ceiling its own coverage allows.
    // `checked_premultiplied` refuses that on the way into `over`, by name.
    //
    // So a limited-range slate did not draw a slightly wrong caption. It
    // failed with `NotPremultiplied`, which is what this test would report if
    // the table were taken back out.
    let description = FrameDescription::square(
        Geometry::new(320, 64).expect("a geometry"),
        PixelFormat::Rgba8,
        srgb_limited(),
        None,
        // Premultiplied, because a legend composites itself `over` what it is
        // captioning and `over` works on associated coverage only.
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    let mut graph = Graph::new();
    let blank = graph.add(Node::Blank { description }).expect("a blank");
    let captioned = graph
        .add(Node::Legend {
            input: blank,
            text: "OFFLINE".into(),
            brief: "OFF".into(),
        })
        .expect("a legend");
    let frame = graph
        .evaluate(captioned, &mut FramePool::new(8, 1 << 20), &mut NoMedia)
        .expect("a frame");
    let packed = frame.to_packed().expect("the samples");
    let brightest = packed
        .chunks_exact(4)
        .flat_map(|pixel| pixel[..3].iter().copied())
        .max()
        .expect("some samples");
    assert_eq!(
        brightest, 235,
        "the caption is as bright as this frame's white and no brighter"
    );
}
