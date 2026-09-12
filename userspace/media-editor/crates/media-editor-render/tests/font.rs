// SPDX-License-Identifier: GPL-3.0-only
//! The face: what it sets, what it refuses, and the property the whole design
//! rests on.
//!
//! That property is **disjointness**. A glyph is drawn as a sum of convex
//! pieces, and a sum is the union's area only if the pieces do not overlap.
//! Every other exactness claim here — that a letter is the same shape at every
//! size, that its coverage integrates to its area — is downstream of it, so it
//! is not asserted by inspection or by looking at a rendering. It is measured:
//! for every pair of pieces in every glyph, the exact area of their
//! intersection, which must be nought.

use media_editor_core::Rational;
use media_editor_render::RenderStatus;
use media_editor_render::font::{
    self, ADVANCE, BASELINE, BOX_WIDTH, DESCENDER, Face, GRID, LINE_SPACING, MAX_TEXT, X_LINE,
};
use media_editor_render::shape::Shape;

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

fn face() -> Face {
    Face::stencil()
}

/// The exact area of a convex region, by summing pixel coverages over a grid.
///
/// Exact whatever the grid, as long as it contains the region: a pixel's
/// coverage is the exact area of that pixel inside the shape, and the pixels
/// tile the plane.
fn area_of(shape: &Shape, width: i64, height: i64) -> Rational {
    let mut total = Rational::ZERO;
    for y in 0..height {
        for x in 0..width {
            total = total
                .checked_add(shape.coverage(x, y).expect("a coverage"))
                .expect("a sum");
        }
    }
    total
}

/// The two shapes' intersection: one region bounded by both sets of edges.
fn intersection(first: &Shape, second: &Shape) -> Shape {
    let mut edges = first.edges().to_vec();
    edges.extend_from_slice(second.edges());
    Shape::new(edges).expect("an intersection")
}

/// Every piece of one character, placed at a size, as shapes.
fn pieces_of(character: char, size: i64) -> Vec<Shape> {
    let face = face();
    let run = font::place(
        face,
        &character.to_string(),
        r(size, 1),
        (Rational::ZERO, Rational::ZERO),
    )
    .expect("a run");
    let glyph = face.glyph(character).expect("a glyph");
    assert_eq!(run.len(), glyph.len(), "one shape per piece");
    // Rebuilt from the glyph rather than read out of the run, because the run
    // does not hand out its shapes -- and the placement is what is under test
    // in the tests that need it, not here.
    let mut out = Vec::new();
    for piece in &glyph.pieces().expect("pieces") {
        let corners: Vec<(Rational, Rational)> = piece
            .iter()
            .map(|(x, y)| {
                (
                    x.checked_mul(r(size, 1)).expect("x"),
                    y.checked_mul(r(size, 1)).expect("y"),
                )
            })
            .collect();
        out.push(media_editor_render::shape::convex(&corners).expect("a shape"));
    }
    out
}

#[test]
fn every_glyph_is_made_of_pieces_that_do_not_overlap() {
    // The property the whole design rests on, measured rather than trusted.
    // A sliver of overlap far too small to fill a pixel would still make the
    // summed coverage wrong, and would still be caught here, because an exact
    // area is exact at any size.
    const SIZE: i64 = 8;
    let face = face();
    let mut pairs = 0_usize;
    for character in face.repertoire() {
        let pieces = pieces_of(character, SIZE);
        for (index, first) in pieces.iter().enumerate() {
            for second in &pieces[index + 1..] {
                let shared = area_of(&intersection(first, second), SIZE + 2, 2 * SIZE + 4);
                assert_eq!(
                    shared,
                    Rational::ZERO,
                    "{character:?} has two pieces that overlap"
                );
                pairs += 1;
            }
        }
    }
    assert!(pairs > 100, "only {pairs} pairs were compared");
}

#[test]
fn a_glyphs_coverage_integrates_to_its_area() {
    // The other side of disjointness, and the one that would catch a *gap*
    // rather than an overlap: what the rasteriser draws, summed over the
    // picture, is exactly the area the glyph's pieces come to.
    const SIZE: i64 = 11;
    let face = face();
    for character in ['A', 'W', 'S', '4', '(', ',', 'X'] {
        let glyph = face.glyph(character).expect("a glyph");
        let run = font::place(
            face,
            &character.to_string(),
            r(SIZE, 1),
            (Rational::ZERO, Rational::ZERO),
        )
        .expect("a run");
        let mut drawn = Rational::ZERO;
        for y in 0..2 * SIZE {
            for x in 0..SIZE {
                drawn = drawn
                    .checked_add(run.coverage(x, y).expect("a coverage"))
                    .expect("a sum");
            }
        }
        let expected = glyph
            .area()
            .expect("an area")
            .checked_mul(r(SIZE * SIZE, 1))
            .expect("scaled");
        assert_eq!(drawn, expected, "{character:?}");
    }
}

#[test]
fn a_glyph_at_twice_the_size_covers_four_times_the_area() {
    // Which is what "the same shape at every size" means when it is written as
    // something a test can fail. A face that snapped to a pixel grid -- every
    // bitmap face, and every hinted outline -- would not have this property at
    // all, and that is the trade this one is making.
    let face = face();
    for character in ['R', '8', 'Z'] {
        let glyph = face.glyph(character).expect("a glyph");
        let area = glyph.area().expect("an area");
        let mut previous: Option<Rational> = None;
        for size in [6_i64, 12] {
            let run = font::place(
                face,
                &character.to_string(),
                r(size, 1),
                (Rational::ZERO, Rational::ZERO),
            )
            .expect("a run");
            let mut drawn = Rational::ZERO;
            for y in 0..2 * size {
                for x in 0..size {
                    drawn = drawn
                        .checked_add(run.coverage(x, y).expect("a coverage"))
                        .expect("a sum");
                }
            }
            assert_eq!(drawn, area.checked_mul(r(size * size, 1)).expect("scaled"));
            if let Some(smaller) = previous {
                assert_eq!(
                    drawn,
                    smaller.checked_mul(r(4, 1)).expect("four times"),
                    "{character:?} at twice the size"
                );
            }
            previous = Some(drawn);
        }
    }
}

#[test]
fn a_glyph_never_reaches_into_the_next_one() {
    // Between glyphs, disjointness is not authored -- it is the advance being
    // wider than the box. Worth pinning, because narrowing the advance to
    // tighten the setting would silently start summing two letters' coverage
    // into one pixel, and the first sign of it would be a bold-looking join.
    let face = face();
    let limit = r(BOX_WIDTH, GRID);
    assert!(
        limit < r(ADVANCE, GRID),
        "the box has to fit inside the advance"
    );
    for character in face.repertoire() {
        for piece in &face
            .glyph(character)
            .expect("a glyph")
            .pieces()
            .expect("pieces")
        {
            for (x, _) in piece {
                assert!(
                    *x >= Rational::ZERO,
                    "{character:?} reaches left of its box"
                );
                assert!(*x <= limit, "{character:?} reaches past its box");
            }
        }
    }
}

#[test]
fn the_face_draws_the_whole_repertoire_without_refusing() {
    // Not a smoke test. `Run::plane` quantises, and `quantise` refuses a
    // coverage above full -- so a face whose pieces overlapped enough to fill
    // one pixel past one would be *refused here by name* rather than drawn
    // slightly wrong. This is the cheap half of the disjointness proof and it
    // runs over every glyph at three sizes.
    let face = face();
    let text: String = face.repertoire().into_iter().collect();
    for size in [4_i64, 9, 23] {
        let run =
            font::place(face, &text, r(size, 1), (Rational::ZERO, Rational::ZERO)).expect("a run");
        let width = usize::try_from(size).expect("a size") * (face.repertoire().len() + 1);
        let height = usize::try_from(size * 2).expect("a size");
        let plane = run.plane(width, height).expect("a plane");
        assert_eq!(plane.len(), width * height);
        assert!(
            plane.iter().any(|value| *value > 0),
            "something is drawn at size {size}"
        );
    }
}

#[test]
fn at_four_pixels_to_the_em_nothing_is_solid() {
    // A measured fact rather than an opinion, and the reason a caption needs a
    // floor rather than just a size. A stroke here is two of sixteen units, so
    // at an em of `n` pixels it is `n/8` pixels wide, and at four pixels that
    // is half a pixel. The face is entirely grey: still exact, still the same
    // shape, and no longer something anybody can read.
    //
    // The obvious generalisation -- "nothing is solid below eight pixels to
    // the em" -- was asserted here first and is **false**, which this fixture
    // found at seven. Two strokes that abut make a region thicker than either,
    // and a pixel inside the corner where a stem meets a bar is covered by
    // both and therefore full. The stroke width bounds what one *piece* can
    // fill, not what the letter can.
    let face = face();
    let text: String = face.repertoire().into_iter().collect();
    for (size, solid) in [(4_i64, false), (24, true)] {
        let run =
            font::place(face, &text, r(size, 1), (Rational::ZERO, Rational::ZERO)).expect("a run");
        let width = usize::try_from(size).expect("a size") * (face.repertoire().len() + 1);
        let plane = run
            .plane(width, usize::try_from(size * 2).expect("a size"))
            .expect("a plane");
        assert_eq!(plane.contains(&255), solid, "at an em of {size} pixels");
    }
}

#[test]
fn every_glyph_but_the_space_draws_something() {
    let face = face();
    for character in face.repertoire() {
        let glyph = face.glyph(character).expect("a glyph");
        if character == ' ' {
            assert!(glyph.is_empty(), "a space draws nothing");
            assert_eq!(glyph.area().expect("an area"), Rational::ZERO);
        } else {
            assert!(!glyph.is_empty(), "{character:?} draws nothing");
            assert!(
                glyph.area().expect("an area").is_positive(),
                "{character:?} encloses nothing"
            );
        }
    }
}

#[test]
fn a_space_advances_without_drawing() {
    let face = face();
    assert_eq!(
        face.measure("A A").expect("a width"),
        face.measure("AAA").expect("a width"),
        "a space is as wide as a letter, because this face is monospaced"
    );
    let run = font::place(face, "   ", r(10, 1), (Rational::ZERO, Rational::ZERO)).expect("a run");
    assert!(run.is_empty());
    assert!(!run.plane(30, 20).expect("a plane").contains(&255));
    assert_eq!(
        run.plane(30, 20).expect("a plane").iter().copied().max(),
        Some(0),
        "a space leaves the picture entirely alone"
    );
}

#[test]
fn a_run_is_as_wide_as_its_characters() {
    let face = face();
    assert_eq!(face.measure("").expect("a width"), Rational::ZERO);
    assert_eq!(face.measure("A").expect("a width"), r(ADVANCE, GRID));
    assert_eq!(
        face.measure("MEDIAEDTO").expect("a width"),
        r(9 * ADVANCE, GRID)
    );
}

#[test]
fn the_second_glyph_starts_exactly_one_advance_along() {
    // Measured from what is drawn rather than from the arithmetic that placed
    // it: the same letter twice, and the second one's ink is the first one's
    // ink moved by exactly the advance.
    let face = face();
    let size = r(16, 1);
    let step = 12_i64;
    let single = font::place(face, "H", size, (Rational::ZERO, Rational::ZERO))
        .expect("a run")
        .plane(40, 20)
        .expect("a plane");
    let double = font::place(face, "HH", size, (Rational::ZERO, Rational::ZERO))
        .expect("a run")
        .plane(40, 20)
        .expect("a plane");
    for y in 0..20 {
        for x in 0..40 - step {
            let moved = double[y * 40 + usize::try_from(x + step).expect("a column")];
            let original = single[y * 40 + usize::try_from(x).expect("a column")];
            assert_eq!(moved, original, "at {x}, {y}");
        }
    }
}

#[test]
fn a_character_this_face_cannot_set_is_refused() {
    // Rather than drawn as a box, or skipped. Both put a picture on a slate
    // that says something other than what it was given, which is the one thing
    // a slate must not do.
    let face = face();
    for character in ['@', '\u{e9}', '\u{5}', '_', '?', '&'] {
        assert_eq!(face.glyph(character), Err(RenderStatus::NoSuchGlyph));
        assert!(!face.has(character));
        assert_eq!(
            face.measure(&character.to_string()),
            Err(RenderStatus::NoSuchGlyph)
        );
        assert_eq!(
            font::place(
                face,
                &character.to_string(),
                r(10, 1),
                (Rational::ZERO, Rational::ZERO)
            )
            .err(),
            Some(RenderStatus::NoSuchGlyph)
        );
    }
}

#[test]
fn the_face_sets_a_digest_and_a_timecode() {
    // The two things it exists to set, asked as a question about the
    // repertoire rather than left to be discovered by a slate that could not
    // print itself.
    let face = face();
    for character in "0123456789ABCDEF".chars() {
        assert!(face.has(character), "hex needs {character:?}");
    }
    for character in "00:00:00:00".chars() {
        assert!(face.has(character), "a timecode needs {character:?}");
    }
    assert!(
        face.measure("MEDIA OFFLINE").is_ok(),
        "and the message it goes under"
    );
}

#[test]
fn type_has_to_be_some_size() {
    let face = face();
    for size in [Rational::ZERO, r(-1, 1)] {
        assert_eq!(
            font::place(face, "A", size, (Rational::ZERO, Rational::ZERO)).err(),
            Some(RenderStatus::SizeNotPositive)
        );
    }
}

#[test]
fn a_run_longer_than_this_draws_is_refused() {
    let face = face();
    let long: String = core::iter::repeat_n('A', MAX_TEXT + 1).collect();
    assert_eq!(face.measure(&long), Err(RenderStatus::TextTooLong));
    let fits: String = core::iter::repeat_n('A', MAX_TEXT).collect();
    assert!(face.measure(&fits).is_ok(), "and one that fits does not");
}

#[test]
fn placing_a_run_somewhere_else_moves_it_and_nothing_more() {
    let face = face();
    let size = r(12, 1);
    let here = font::place(face, "PHIP", size, (Rational::ZERO, Rational::ZERO))
        .expect("a run")
        .plane(60, 30)
        .expect("a plane");
    let there = font::place(face, "PHIP", size, (r(7, 1), r(5, 1)))
        .expect("a run")
        .plane(60, 30)
        .expect("a plane");
    for y in 0..25 {
        for x in 0..53 {
            assert_eq!(there[(y + 5) * 60 + x + 7], here[y * 60 + x], "at {x}, {y}");
        }
    }
}

#[test]
fn a_run_placed_off_the_picture_draws_what_is_on_it() {
    // Not a refusal. A caption wider than the frame is an ordinary thing for a
    // caption to be, and clipping it is what a picture does to everything
    // else; refusing would make a long file name unprintable rather than
    // truncated.
    let face = face();
    let run = font::place(face, "MEDIAEDTO", r(20, 1), (r(-30, 1), r(-4, 1))).expect("a run");
    let plane = run.plane(40, 20).expect("a plane");
    assert!(plane.iter().any(|value| *value > 0), "some of it lands");
    assert!(plane.contains(&0), "and some does not");
}

/// The topmost and bottommost half-unit a glyph's ink reaches.
fn extent_of(character: char) -> (Rational, Rational) {
    let face = face();
    let grid = r(GRID, 1);
    let glyph = face.glyph(character).expect("a glyph");
    let mut top: Option<Rational> = None;
    let mut bottom: Option<Rational> = None;
    for piece in &glyph.pieces().expect("pieces") {
        for (_, y) in piece {
            let units = y.checked_mul(grid).expect("half-units");
            top = Some(top.map_or(units, |held| if units < held { units } else { held }));
            bottom = Some(bottom.map_or(units, |held| if units > held { units } else { held }));
        }
    }
    (top.expect("some ink"), bottom.expect("some ink"))
}

#[test]
fn the_metrics_describe_the_face_rather_than_decorating_it() {
    // Four numbers that would otherwise be a comment. A face whose glyphs
    // drifted off them would still draw, still be disjoint, and still be
    // exact -- and would set a line of type that did not sit on anything.
    for character in face().repertoire() {
        if character == ' ' {
            continue;
        }
        let (top, bottom) = extent_of(character);
        assert!(
            top >= Rational::ZERO,
            "{character:?} reaches above the cap line"
        );
        assert!(
            bottom <= r(BASELINE + DESCENDER, 1),
            "{character:?} hangs below the descender"
        );
    }
}

#[test]
fn a_capital_runs_from_the_cap_line_to_the_baseline() {
    // Which is what made capitals alone one set of metrics: there is nothing
    // else to say about where one sits.
    for character in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789".chars() {
        let (top, bottom) = extent_of(character);
        assert_eq!(
            top,
            Rational::ZERO,
            "{character:?} does not start at the cap"
        );
        assert_eq!(
            bottom,
            r(BASELINE, 1),
            "{character:?} does not sit on the baseline"
        );
    }
}

#[test]
fn a_lowercase_body_sits_on_the_x_line() {
    // The letters with neither an ascender nor a descender, which are the ones
    // that define the x-height by sitting on it.
    for character in "acemnorsuvwxz".chars() {
        let (top, bottom) = extent_of(character);
        assert_eq!(
            top,
            r(X_LINE, 1),
            "{character:?} does not start at the x-line"
        );
        assert_eq!(
            bottom,
            r(BASELINE, 1),
            "{character:?} does not sit on the baseline"
        );
    }
}

#[test]
fn an_ascender_reaches_the_cap_line_and_a_descender_the_descender() {
    for character in "bdfhklt".chars() {
        let (top, _) = extent_of(character);
        assert!(
            top < r(X_LINE, 1),
            "{character:?} has no ascender, and it should"
        );
    }
    for character in "gjpqy".chars() {
        let (_, bottom) = extent_of(character);
        assert_eq!(
            bottom,
            r(BASELINE + DESCENDER, 1),
            "{character:?} does not reach the descender"
        );
    }
}

#[test]
fn a_line_of_type_leaves_room_for_the_line_under_it() {
    // Line spacing set at the em -- which is what "line height equals font
    // size" means everywhere it is offered -- would put every `g` in one line
    // through every `A` in the next. This face descends, so it needs more.
    assert!(
        r(LINE_SPACING, 1) > r(BASELINE + DESCENDER, 1),
        "a line has to clear the descender above it"
    );
    let (_, deepest) = extent_of('g');
    assert!(
        deepest < r(LINE_SPACING, 1),
        "a descender reaches into the next line"
    );
}

#[test]
fn the_face_sets_a_name() {
    // The thing a title card is usually for, and the reason capitals alone
    // were not enough. A face that could set a slate and not a name would be
    // a face for slates.
    let face = face();
    for words in ["Media Editor", "The End", "Directed by"] {
        assert!(face.measure(words).is_ok(), "{words:?}");
        assert!(
            !font::place(face, words, r(20, 1), (Rational::ZERO, Rational::ZERO))
                .expect("a run")
                .is_empty()
        );
    }
}

/// The face, obtained in a constant.
///
/// This line is the test. A `const` cannot allocate, cannot call a fallible
/// function, and cannot run a loop over a table building anything — so if the
/// face ever goes back to being *built* rather than *pointed at*, this file
/// stops compiling. That is a stronger statement than any assertion could
/// make, and it is the property four pages of the image were bought with.
const FACE: Face = Face::stencil();

#[test]
fn the_face_is_a_table_rather_than_a_program_that_builds_one() {
    assert!(FACE.has('A'), "and it is the same face");
    assert_eq!(FACE.repertoire(), face().repertoire());
    assert_eq!(
        FACE.measure("Media Editor").expect("a width"),
        face().measure("Media Editor").expect("a width")
    );
}

#[test]
fn a_glyphs_pieces_are_the_same_pieces_every_time_they_are_asked_for() {
    // They are computed on demand now rather than held, so "the same glyph"
    // has to mean the same corners each time. A face that drew a letter
    // slightly differently on its second use would be a face whose cache keys
    // were honest and whose pictures were not.
    for character in ['A', '1', ',', 'g', 'W'] {
        let glyph = FACE.glyph(character).expect("a glyph");
        assert_eq!(
            glyph.pieces().expect("pieces"),
            glyph.pieces().expect("pieces"),
            "{character:?}"
        );
        assert_eq!(glyph.len(), glyph.pieces().expect("pieces").len());
    }
}

#[test]
fn the_three_glyphs_that_are_not_only_strokes_still_have_their_extra_piece() {
    // An `A`'s crossbar and a `1`'s flag are not strokes and are not stored as
    // coordinates -- one is computed from the legs it meets and the other is a
    // triangle. Moving the face into a table is exactly the change that would
    // drop them, and it would drop them silently: the letters would still
    // draw, still be disjoint, and still be legible.
    for (character, pieces) in [('A', 3), ('1', 4), (',', 2)] {
        let glyph = FACE.glyph(character).expect("a glyph");
        assert_eq!(glyph.len(), pieces, "{character:?}");
        assert_eq!(
            glyph.pieces().expect("pieces").len(),
            pieces,
            "{character:?} counts a piece it does not draw"
        );
    }
    // And the crossbar is between the legs rather than beside them, which is
    // the part that could go wrong without changing the count.
    let a = FACE.glyph('A').expect("a glyph");
    let pieces = a.pieces().expect("pieces");
    let bar = &pieces[2];
    let inside = bar
        .iter()
        .all(|(x, _)| *x > r(0, 1) && *x < r(BOX_WIDTH, GRID));
    assert!(inside, "the crossbar reaches outside the legs");
}

#[test]
fn a_row_of_a_run_is_that_row_of_the_whole_plane() {
    // The row form of the rasteriser, checked against the whole form it is
    // supposed to be a slice of. A run is laid out against the frame it
    // belongs to, so both are asked for the same extent and only the range
    // differs.
    let run = media_editor_render::font::title(
        &["MEDIAEDTO", "take two"],
        Rational::new(1, 4).expect("a size"),
        Rational::new(1, 2).expect("a place"),
        Rational::new(1, 2).expect("a place"),
        media_editor_render::font::Alignment::Centre,
        40,
        24,
    )
    .expect("a run");
    let whole = run.plane(40, 24).expect("a plane");
    for row in 0..24 {
        assert_eq!(
            run.plane_row(40, 24, row).expect("a row"),
            whole[row * 40..(row + 1) * 40],
            "row {row} of the run is not row {row} of its plane"
        );
    }
    // And a row past the bottom is refused rather than rasterised against
    // nothing. A caller reaching this directly has no other bound: the graph
    // checks the row against the description, and this function does not see
    // one.
    assert_eq!(
        run.plane_row(40, 24, 24),
        Err(media_editor_render::RenderStatus::OutsideDomain)
    );
    assert_eq!(
        run.plane_row(40, 24, 1000),
        Err(media_editor_render::RenderStatus::OutsideDomain)
    );
}
