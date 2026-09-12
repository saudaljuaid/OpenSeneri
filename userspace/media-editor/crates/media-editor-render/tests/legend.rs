// SPDX-License-Identifier: GPL-3.0-only
//! The legend: a caption on a slate, and the one arithmetic claim it makes.
//!
//! That claim is that the type is **premultiplied in light**, not in code
//! values. Writing the coverage byte into the colour channels is the obvious
//! way to build white type and it is wrong everywhere the coverage is partial,
//! by exactly the amount the transfer curve bends. It looks like a slightly
//! thin font rather than like a bug, which is why there is a test for it that
//! does not go through the same code the drawing does.

use media_editor_core::{Digest, Rational};
use media_editor_media::{
    AlphaState, ColourDescription, Frame, FrameDescription, Geometry, PixelFormat, TestPattern,
    pool::FramePool,
};
use media_editor_render::font::{self, Alignment, MIN_CAPTION_EM};
use media_editor_render::{Graph, Library, Look, Node, RenderStatus};

const CAPTION: &str = "MEDIA OFFLINE 4F3C9A21";
const BRIEF: &str = "4F3C9A21";

fn described(width: u32, height: u32) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(width, height).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description")
}

/// A library nothing asks anything of.
struct Nothing;

impl Library for Nothing {
    fn frame(
        &mut self,
        _media: Digest,
        _tick: i64,
        _description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        Err(RenderStatus::OutsideDomain)
    }

    fn look(&mut self, _look: Digest) -> Result<Look, RenderStatus> {
        Err(RenderStatus::OutsideDomain)
    }
}

fn pool() -> FramePool {
    FramePool::new(64, 1 << 24)
}

/// A slate at a size, with a caption on it.
fn slated(width: u32, height: u32, text: &str, brief: &str) -> Frame {
    let description = described(width, height);
    let mut graph = Graph::new();
    let under = graph
        .add(Node::Pattern {
            pattern: TestPattern::Offline,
            description,
        })
        .expect("a slate");
    let legend = graph
        .add(Node::Legend {
            input: under,
            text: text.to_string(),
            brief: brief.to_string(),
        })
        .expect("a legend");
    graph
        .evaluate(legend, &mut pool(), &mut Nothing)
        .expect("a frame")
}

/// The same slate with no caption on it.
fn bare(width: u32, height: u32) -> Frame {
    TestPattern::Offline
        .render(described(width, height))
        .expect("a slate")
}

#[test]
fn a_caption_is_set_across_a_frame_that_can_hold_it() {
    let with = slated(320, 180, CAPTION, BRIEF).to_packed().expect("bytes");
    let without = bare(320, 180).to_packed().expect("bytes");
    assert_ne!(with, without, "the caption is on the picture");
    let changed = with
        .iter()
        .zip(&without)
        .filter(|(one, other)| one != other)
        .count();
    assert!(
        changed > 2_000,
        "only {changed} bytes changed, which is not a sentence"
    );
}

#[test]
fn a_frame_too_small_for_the_whole_caption_gets_the_part_that_matters() {
    // Not a compromise between the two. Setting the long caption small enough
    // to fit a proxy is a grey smear, and dropping straight to nothing throws
    // away the eight characters somebody actually needs to find the clip.
    assert!(
        font::caption(CAPTION, 320, 180)
            .expect("a caption")
            .is_some(),
        "the whole sentence fits at 320"
    );
    assert!(
        font::caption(CAPTION, 160, 90)
            .expect("a caption")
            .is_none(),
        "and does not at 160"
    );
    assert!(
        font::caption(BRIEF, 160, 90).expect("a caption").is_some(),
        "where the digest alone does"
    );
    let narrow = slated(160, 90, CAPTION, BRIEF).to_packed().expect("bytes");
    assert_ne!(narrow, bare(160, 90).to_packed().expect("bytes"));
}

#[test]
fn a_frame_too_small_for_either_is_left_exactly_alone() {
    // Byte for byte, not nearly. A slate that drew a grey smear would have
    // told the viewer something false about how much it knows, and the stripes
    // underneath already say the one thing that matters.
    assert!(font::caption(BRIEF, 64, 36).expect("a caption").is_none());
    assert_eq!(
        slated(64, 36, CAPTION, BRIEF).to_packed().expect("bytes"),
        bare(64, 36).to_packed().expect("bytes")
    );
}

#[test]
fn the_type_is_premultiplied_in_light_and_not_in_code_values() {
    // The claim this file exists for. White type at coverage `a` over black is
    // light `a`, and its *code value* is the transfer curve applied to that --
    // which for sRGB is far above `a`. Writing the coverage byte into the
    // colour channels, which is the obvious way to build white type, gives a
    // code value of exactly `a` at every pixel and a caption that is too dark
    // along every edge it has.
    //
    // So the discriminator does not need the curve's numbers at all: the wrong
    // arithmetic makes colour equal coverage everywhere, and the right one
    // makes it strictly greater wherever there is partial coverage.
    let description = described(240, 120)
        .with_alpha(AlphaState::Premultiplied)
        .expect("a description");
    let mut graph = Graph::new();
    let black = graph
        .add(Node::Pattern {
            pattern: TestPattern::Flat { value: 0 },
            description,
        })
        .expect("a field");
    let legend = graph
        .add(Node::Legend {
            input: black,
            text: BRIEF.to_string(),
            brief: BRIEF.to_string(),
        })
        .expect("a legend");
    let frame = graph
        .evaluate(legend, &mut pool(), &mut Nothing)
        .expect("a frame");
    let packed = frame.to_packed().expect("bytes");

    // Inspect caption coverage directly because compositing over opaque black
    // makes the output alpha 255 everywhere.
    let coverage = font::caption(BRIEF, 240, 120)
        .expect("a caption")
        .expect("a run")
        .plane(240, 120)
        .expect("a plane");

    let mut partial = 0_usize;
    for (pixel, ink) in packed.chunks_exact(4).zip(&coverage) {
        let red = pixel[0];
        assert_eq!(pixel[1], red, "white type has no hue");
        assert_eq!(pixel[2], red, "white type has no hue");
        assert_eq!(pixel[3], 255, "over an opaque field the answer is opaque");
        if *ink == 0 {
            assert_eq!(red, 0, "nothing where there is no ink");
        } else if *ink < 255 {
            partial += 1;
            assert!(
                red > *ink,
                "a code value of {red} at a coverage of {ink}: white \
                 premultiplied in code values rather than in light"
            );
        } else {
            assert_eq!(red, 255, "solid where the ink is solid");
        }
    }
    assert!(partial > 500, "only {partial} pixels are partly covered");
}

#[test]
fn a_caption_stays_inside_the_frame() {
    // With a margin, which is what makes it read as a caption rather than as
    // something that has been cut off.
    let width = 320_usize;
    let run = font::caption(CAPTION, width, 180)
        .expect("a caption")
        .expect("a run");
    let plane = run.plane(width, 180).expect("a plane");
    for row in plane.chunks_exact(width) {
        assert_eq!(row[0], 0, "nothing touches the left edge");
        assert_eq!(row[width - 1], 0, "nothing touches the right edge");
    }
    let inked: Vec<usize> = (0..width)
        .filter(|x| plane.chunks_exact(width).any(|row| row[*x] > 0))
        .collect();
    let left = *inked.first().expect("some ink");
    let right = *inked.last().expect("some ink");
    let margin = width - 1 - right;
    // Within a side bearing. The line is centred on its *type block* -- the
    // boxes the glyphs are drawn in -- and a glyph whose own ink stops short
    // of its box, which "1" and "." both do, shifts the visible ink inside it.
    // Chasing that would make centring depend on which letters a caption
    // happens to end with.
    assert!(
        left.abs_diff(margin) <= 3,
        "the caption is centred: {left} on the left, {margin} on the right"
    );
}

#[test]
fn a_short_caption_on_a_wide_frame_is_not_a_billboard() {
    // Without a ceiling the size is whatever fits the width, and two
    // characters across a wide frame fit at an enormous one -- so "OK" on a
    // 4K slate would be six hundred pixels tall and mostly off the bottom.
    // The cap is a fraction of the *height*, which is the dimension a caption
    // has to share with the picture.
    let (width, height) = (480_usize, 270_usize);
    let run = font::caption("OK", width, height)
        .expect("a caption")
        .expect("a run");
    let plane = run.plane(width, height).expect("a plane");
    let inked = plane
        .chunks_exact(width)
        .filter(|row| row.iter().any(|value| *value > 0))
        .count();
    assert!(inked > 0, "it is drawn");
    assert!(
        inked <= height / 6 + 1,
        "{inked} rows of {height} are inked, which is a billboard"
    );
}

#[test]
fn a_caption_of_nothing_draws_nothing() {
    // Refusing would be wrong -- an empty caption is a perfectly ordinary
    // thing to ask for -- and dividing by its width would be worse.
    assert!(font::caption("", 320, 180).expect("a caption").is_none());
    // A caption of *spaces* is a different case and reaches the same answer
    // without a special case for it: it has a width, it is placed, and it
    // draws nothing because a space has no pieces. Asserting it was `None`
    // here was wrong, and the fixture said so.
    let spaces = font::caption("   ", 320, 180)
        .expect("a caption")
        .expect("a run");
    assert!(spaces.is_empty(), "a run of spaces has no pieces");
    for (text, brief) in [("", ""), ("   ", "   ")] {
        assert_eq!(
            slated(320, 180, text, brief).to_packed().expect("bytes"),
            bare(320, 180).to_packed().expect("bytes"),
            "{text:?} left the slate alone"
        );
    }
}

#[test]
fn a_caption_this_face_cannot_set_is_refused() {
    assert_eq!(
        font::caption("MEDIA OFFLINE!", 320, 180).err(),
        Some(RenderStatus::NoSuchGlyph),
        "a mark this face has no glyph for is not silently dropped or boxed"
    );
}

#[test]
fn two_legends_saying_different_things_are_two_nodes() {
    // The caption is in the identity, so a pool holding one slate does not
    // hand it back for another. Without this, a timeline with two clips
    // offline would show the first one's digest on both.
    let description = described(120, 60);
    let mut graph = Graph::new();
    let under = graph
        .add(Node::Pattern {
            pattern: TestPattern::Offline,
            description,
        })
        .expect("a slate");
    let mut identities = Vec::new();
    for (text, brief) in [
        ("MEDIA OFFLINE 4F3C9A21", "4F3C9A21"),
        ("MEDIA OFFLINE 4F3C9A22", "4F3C9A22"),
        // The same bytes, split between the two captions differently. Without
        // a length in the digest these would hash the same, and two slates
        // that say different things would share a cache entry.
        ("MEDIA OFFLINE 4F3C9A2", "14F3C9A21"),
    ] {
        let legend = graph
            .add(Node::Legend {
                input: under,
                text: text.to_string(),
                brief: brief.to_string(),
            })
            .expect("a legend");
        identities.push(graph.identity(legend).expect("an identity"));
    }
    for (index, one) in identities.iter().enumerate() {
        for other in &identities[index + 1..] {
            assert_ne!(one, other);
        }
    }
}

#[test]
fn the_same_legend_twice_is_the_same_node() {
    let description = described(120, 60);
    let mut graph = Graph::new();
    let under = graph
        .add(Node::Pattern {
            pattern: TestPattern::Offline,
            description,
        })
        .expect("a slate");
    let mut identities = Vec::new();
    for _ in 0..2 {
        let legend = graph
            .add(Node::Legend {
                input: under,
                text: CAPTION.to_string(),
                brief: BRIEF.to_string(),
            })
            .expect("a legend");
        identities.push(graph.identity(legend).expect("an identity"));
    }
    assert_eq!(identities[0], identities[1]);
}

#[test]
fn the_caption_floor_is_where_a_stroke_is_a_pixel() {
    // The floor is not a taste. A stroke is an eighth of the em, so at the
    // floor it is just over a pixel -- and a test elsewhere measures that at
    // four pixels to the em nothing in the face is solid at all.
    assert_eq!(MIN_CAPTION_EM, 9);
    let stroke = Rational::new(MIN_CAPTION_EM, 8).expect("a stroke");
    assert!(
        stroke > Rational::new(1, 1).expect("a pixel"),
        "a stroke at the floor is wider than a pixel"
    );
}

/// Where a title's ink actually lands: left, right, top, bottom, in pixels.
fn ink_box(plane: &[u8], width: usize) -> (usize, usize, usize, usize) {
    let mut left = width;
    let mut right = 0;
    let mut top = usize::MAX;
    let mut bottom = 0;
    for (y, row) in plane.chunks_exact(width).enumerate() {
        for (x, value) in row.iter().enumerate() {
            if *value > 0 {
                left = left.min(x);
                right = right.max(x);
                top = top.min(y);
                bottom = bottom.max(y);
            }
        }
    }
    (left, right, top, bottom)
}

#[test]
fn a_title_is_placed_where_it_was_told_in_fractions_of_the_frame() {
    // Fractions rather than pixels, for the reason a transform's move is: a
    // title laid out on a proxy is the same title on the finish, rather than a
    // pixel count that stopped meaning the same thing when the picture got
    // bigger. So the same title at two sizes has its ink in the same *place*,
    // measured as a fraction.
    let mut placed = Vec::new();
    for scale in [1_usize, 3] {
        let (width, height) = (160 * scale, 90 * scale);
        let run = font::title(
            &["PHIP"],
            Rational::new(1, 5).expect("a size"),
            Rational::new(1, 4).expect("a place"),
            Rational::new(2, 3).expect("a place"),
            Alignment::Centre,
            width,
            height,
        )
        .expect("a run");
        let plane = run.plane(width, height).expect("a plane");
        let (left, right, top, bottom) = ink_box(&plane, width);
        placed.push((
            (left + right) * 1000 / (2 * width),
            (top + bottom) * 1000 / (2 * height),
            (right - left) * 1000 / width,
        ));
    }
    // A quarter across and two thirds down, to a thousandth, at both sizes.
    assert!(
        placed[0].0.abs_diff(250) <= 4 && placed[1].0.abs_diff(250) <= 4,
        "across: {placed:?}"
    );
    assert!(
        placed[0].1.abs_diff(666) <= 6 && placed[1].1.abs_diff(666) <= 6,
        "down: {placed:?}"
    );
    assert!(
        placed[0].2.abs_diff(placed[1].2) <= 4,
        "and the same width, as a fraction: {placed:?}"
    );
}

#[test]
fn a_titles_size_is_a_fraction_of_the_height() {
    // The height rather than the width, so a card set for 16:9 does not shrink
    // when the same programme is delivered 4:3 -- the dimension type shares
    // with the picture is the vertical one.
    let (width, height) = (400_usize, 300_usize);
    for (numerator, denominator) in [(1_i64, 10_i64), (1, 5)] {
        let run = font::title(
            &["H"],
            Rational::new(numerator, denominator).expect("a size"),
            Rational::new(1, 2).expect("a place"),
            Rational::new(1, 2).expect("a place"),
            Alignment::Centre,
            width,
            height,
        )
        .expect("a run");
        let plane = run.plane(width, height).expect("a plane");
        let (_, _, top, bottom) = ink_box(&plane, width);
        let tall = bottom - top + 1;
        let wanted =
            usize::try_from(i64::try_from(height).expect("a height") * numerator / denominator)
                .expect("an em");
        assert!(
            tall.abs_diff(wanted) <= 1,
            "an em of {wanted} drew {tall} rows"
        );
    }
}

#[test]
fn a_title_has_no_floor_and_no_ceiling() {
    // Unlike a caption. A caption's size is chosen *for* the reader, so it has
    // to refuse to be illegible; a title's size is the editor's own decision,
    // and a program that quietly declined to draw somebody's title because it
    // judged it too small would be worse than one that drew it.
    let tiny = font::title(
        &["A"],
        Rational::new(1, 100).expect("a size"),
        Rational::new(1, 2).expect("a place"),
        Rational::new(1, 2).expect("a place"),
        Alignment::Centre,
        200,
        200,
    )
    .expect("a run");
    assert!(!tiny.is_empty(), "it is drawn at any size");
    let huge = font::title(
        &["A"],
        Rational::new(3, 1).expect("a size"),
        Rational::new(1, 2).expect("a place"),
        Rational::new(1, 2).expect("a place"),
        Alignment::Centre,
        200,
        200,
    )
    .expect("a run");
    assert!(!huge.is_empty(), "including one that runs off the frame");
}

#[test]
fn a_title_at_no_size_is_refused() {
    for size in [Rational::ZERO, Rational::new(-1, 6).expect("a size")] {
        assert_eq!(
            font::title(
                &["A"],
                size,
                Rational::new(1, 2).expect("a place"),
                Rational::new(1, 2).expect("a place"),
                Alignment::Centre,
                200,
                200
            )
            .err(),
            Some(RenderStatus::SizeNotPositive)
        );
    }
}

/// Which rows of a plane have ink in them.
fn inked_rows(plane: &[u8], width: usize) -> Vec<usize> {
    plane
        .chunks_exact(width)
        .enumerate()
        .filter(|(_, row)| row.iter().any(|value| *value > 0))
        .map(|(y, _)| y)
        .collect()
}

/// Where the ink of a run starts and ends across the frame.
fn inked_columns(plane: &[u8], width: usize) -> (usize, usize) {
    let mut left = width;
    let mut right = 0;
    for row in plane.chunks_exact(width) {
        for (x, value) in row.iter().enumerate() {
            if *value > 0 {
                left = left.min(x);
                right = right.max(x);
            }
        }
    }
    (left, right)
}

#[test]
fn a_block_stacks_its_lines_at_the_faces_own_line_spacing() {
    // Not at the em. This face descends, so lines set an em apart would put
    // every `g` in one line through every `A` in the next -- and the coverage
    // of the two would sum past full, so the card would be *refused* rather
    // than drawn heavy. The spacing is the face's own, which knows that.
    let (width, height) = (400_usize, 300_usize);
    let size = Rational::new(1, 12).expect("a size");
    let one = font::title(
        &["H"],
        size,
        Rational::new(1, 2).expect("a place"),
        Rational::new(1, 2).expect("a place"),
        Alignment::Centre,
        width,
        height,
    )
    .expect("a run")
    .plane(width, height)
    .expect("a plane");
    let two = font::title(
        &["H", "H"],
        size,
        Rational::new(1, 2).expect("a place"),
        Rational::new(1, 2).expect("a place"),
        Alignment::Centre,
        width,
        height,
    )
    .expect("a run")
    .plane(width, height)
    .expect("a plane");

    let single = inked_rows(&one, width);
    let double = inked_rows(&two, width);
    let em = height / 12;
    assert!(
        single.len().abs_diff(em) <= 1,
        "one line is one em tall: {} rows against {em}",
        single.len()
    );
    // Two lines, a gap between them, and the whole block taller than two ems.
    let gap = double
        .windows(2)
        .filter(|pair| pair[1] != pair[0] + 1)
        .count();
    assert_eq!(gap, 1, "two lines with one gap between them");
    let span = double.last().expect("ink") - double.first().expect("ink") + 1;
    assert!(
        span > 2 * em,
        "{span} rows for two {em}-row lines is not spacing"
    );
}

#[test]
fn the_lines_of_a_block_never_touch() {
    // The strongest form of the same claim, and it does not need a
    // measurement: a descender in one line landing on a cap in the next would
    // cover a pixel twice, and the rasteriser refuses a coverage above full.
    // So a card of descenders over capitals either draws or is refused, and
    // there is no third outcome for this test to be wrong about.
    let (width, height) = (400_usize, 300_usize);
    let run = font::title(
        &["gjpqy", "AHXWM", "gjpqy"],
        Rational::new(1, 10).expect("a size"),
        Rational::new(1, 2).expect("a place"),
        Rational::new(1, 2).expect("a place"),
        Alignment::Centre,
        width,
        height,
    )
    .expect("a run");
    let plane = run.plane(width, height).expect("descenders clear the caps");
    assert!(plane.contains(&255));
}

#[test]
fn left_centre_and_right_put_the_short_line_in_three_places() {
    // And the *long* line in one. That is what alignment means: the block is
    // where the block is, and the lines sit against one another inside it.
    let (width, height) = (400_usize, 200_usize);
    let mut placed = Vec::new();
    for alignment in [Alignment::Left, Alignment::Centre, Alignment::Right] {
        let plane = font::title(
            &["MMMMMMMM", "I"],
            Rational::new(1, 10).expect("a size"),
            Rational::new(1, 2).expect("a place"),
            Rational::new(1, 2).expect("a place"),
            alignment,
            width,
            height,
        )
        .expect("a run")
        .plane(width, height)
        .expect("a plane");
        let rows = inked_rows(&plane, width);
        let split = rows
            .windows(2)
            .position(|pair| pair[1] != pair[0] + 1)
            .expect("two lines");
        let second = rows[split + 1] * width;
        placed.push((
            inked_columns(&plane[..second], width),
            inked_columns(&plane[second..], width),
        ));
    }
    let long: Vec<_> = placed.iter().map(|held| held.0).collect();
    assert_eq!(long[0], long[1], "the long line does not move");
    assert_eq!(long[1], long[2], "the long line does not move");

    let short: Vec<_> = placed.iter().map(|held| held.1).collect();
    // Within a side bearing, not exactly: alignment lines up the *boxes* the
    // glyphs are drawn in, and an `I` has its own ink well inside its box.
    // Chasing the ink would make alignment depend on which letter a line
    // happens to start with, which is the same argument centring makes.
    let bearing = height / 10 / 4;
    assert!(
        short[0].0.abs_diff(long[0].0) <= bearing,
        "left: the short line starts where the long one does, {short:?}"
    );
    assert!(
        short[2].1.abs_diff(long[0].1) <= bearing,
        "right: it ends where the long one does, {short:?}"
    );
    assert!(
        short[1].0 > short[0].0 && short[1].1 < short[2].1,
        "centre: it sits between the two, at {short:?}"
    );
}

#[test]
fn a_block_straddles_the_point_it_was_placed_at() {
    // Rather than hanging below it. A two-line card placed at the middle of
    // the frame has one line above the middle and one below, which is what
    // anybody dragging a card to the centre means by the centre.
    let (width, height) = (400_usize, 300_usize);
    let plane = font::title(
        &["ONE", "TWO"],
        Rational::new(1, 12).expect("a size"),
        Rational::new(1, 2).expect("a place"),
        Rational::new(1, 2).expect("a place"),
        Alignment::Centre,
        width,
        height,
    )
    .expect("a run")
    .plane(width, height)
    .expect("a plane");
    let rows = inked_rows(&plane, width);
    let top = *rows.first().expect("ink");
    let bottom = *rows.last().expect("ink");
    let middle = height / 2;
    assert!(
        top < middle && bottom > middle,
        "{top}..{bottom} about {middle}"
    );
    assert!(
        (top + bottom).abs_diff(2 * middle) <= 2,
        "and evenly: {top}..{bottom} about {middle}"
    );
}

#[test]
fn a_card_of_one_line_is_set_where_one_line_used_to_be() {
    // Keep the single-line layout compatible with existing projects.
    let (width, height) = (320_usize, 180_usize);
    let size = Rational::new(1, 6).expect("a size");
    let centre = Rational::new(1, 2).expect("a place");
    let block = font::title(
        &["PHIP"],
        size,
        centre,
        centre,
        Alignment::Centre,
        width,
        height,
    )
    .expect("a run")
    .plane(width, height)
    .expect("a plane");
    let face = font::Face::stencil();
    let em = Rational::new(i64::try_from(height).expect("a height"), 1)
        .expect("a height")
        .checked_mul(size)
        .expect("an em");
    let single = font::centred(
        face,
        "PHIP",
        em,
        (
            Rational::new(i64::try_from(width).expect("a width"), 2).expect("a middle"),
            Rational::new(i64::try_from(height).expect("a height"), 2).expect("a middle"),
        ),
    )
    .expect("a run")
    .plane(width, height)
    .expect("a plane");
    assert_eq!(block, single);
}
