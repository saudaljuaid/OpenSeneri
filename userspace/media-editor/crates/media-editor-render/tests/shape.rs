// SPDX-License-Identifier: GPL-3.0-only
//! Shapes, against the areas they are supposed to have.
//!
//! Coverage here is an *area*, so the tests are about areas: a shape's
//! coverage summed over a picture is the exact area of the shape inside it,
//! a half-plane and its complement partition every pixel exactly, and the two
//! independent implementations agree pixel for pixel over whole frames.
//!
//! None of those is a tolerance. Every one is an equality between rationals,
//! which is the point of doing the arithmetic exactly: a bound would be
//! satisfied by the bug as well as by the fix.

use media_editor_core::Rational;
use media_editor_render::RenderStatus;
use media_editor_render::shape::{
    Edge, FULL, MAX_EDGES, Shape, half_plane_coverage, plane, quantise,
};

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

fn n(value: i64) -> Rational {
    Rational::from_integer(value)
}

/// `x <= at`: everything to the left of a vertical line.
fn left_of(at: Rational) -> Shape {
    Shape::half_plane(Edge::new(Rational::ONE, Rational::ZERO, at).expect("an edge"))
        .expect("a shape")
}

/// `x + y <= at`: the diagonal, which is the case with no axis to hide behind.
fn diagonal(at: Rational) -> Shape {
    Shape::half_plane(Edge::new(Rational::ONE, Rational::ONE, at).expect("an edge"))
        .expect("a shape")
}

#[test]
fn a_pixel_wholly_inside_is_covered_exactly() {
    let shape = left_of(n(10));
    assert_eq!(shape.coverage(0, 0).expect("a coverage"), Rational::ONE);
    assert_eq!(shape.coverage(8, 5).expect("a coverage"), Rational::ONE);
}

#[test]
fn a_pixel_wholly_outside_is_covered_not_at_all() {
    let shape = left_of(n(2));
    assert_eq!(shape.coverage(5, 0).expect("a coverage"), Rational::ZERO);
}

#[test]
fn a_vertical_line_cuts_one_column_and_leaves_the_rest_whole() {
    // At x = 3.25 the pixel from 3 to 4 keeps a quarter, everything left of it
    // is whole and everything right of it is nothing. An off-by-one in which
    // column is partial is the classic rasteriser bug and this is what names
    // it.
    let shape = left_of(r(13, 4));
    assert_eq!(shape.coverage(2, 0).expect("a coverage"), Rational::ONE);
    assert_eq!(shape.coverage(3, 0).expect("a coverage"), r(1, 4));
    assert_eq!(shape.coverage(4, 0).expect("a coverage"), Rational::ZERO);
}

#[test]
fn a_diagonal_through_a_pixel_corner_covers_half_of_it() {
    // x + y <= 1 passes through (1, 0) and (0, 1), so the pixel from (0,0) to
    // (1,1) keeps the triangle below it: exactly one half.
    assert_eq!(
        diagonal(Rational::ONE).coverage(0, 0).expect("a coverage"),
        r(1, 2)
    );
}

#[test]
fn a_diagonal_cutting_a_corner_off_covers_the_rest() {
    // x + y <= 1.5 through the pixel leaves the triangle above the line, whose
    // legs are half a pixel: an eighth. So the coverage is seven eighths.
    assert_eq!(
        diagonal(r(3, 2)).coverage(0, 0).expect("a coverage"),
        r(7, 8)
    );
}

#[test]
fn a_shallow_line_covers_a_trapezoid() {
    // 2x + y <= 2 crosses the pixel from (0.5, 1) to (1, 0), leaving a
    // triangle with legs 1 and 0.5 above it -- a quarter -- so three quarters
    // remain. A trapezoid is the third of the three shapes a line can leave,
    // and the one a triangle-only formula gets wrong.
    let shape =
        Shape::half_plane(Edge::new(n(2), Rational::ONE, n(2)).expect("an edge")).expect("a shape");
    assert_eq!(shape.coverage(0, 0).expect("a coverage"), r(3, 4));
}

#[test]
fn a_half_plane_and_its_complement_partition_every_pixel() {
    // The boundary belongs to both, and a line has no area, so the two
    // coverages sum to exactly one -- at every pixel, including the ones the
    // line passes through and the ones it does not.
    let edge = Edge::new(n(3), n(-7), r(23, 2)).expect("an edge");
    let inside = Shape::half_plane(edge).expect("a shape");
    let outside = Shape::half_plane(edge.complement().expect("a complement")).expect("a shape");
    for y in -2..8 {
        for x in -2..8 {
            let together = inside
                .coverage(x, y)
                .expect("a coverage")
                .checked_add(outside.coverage(x, y).expect("a coverage"))
                .expect("a sum");
            assert_eq!(together, Rational::ONE, "at ({x}, {y})");
        }
    }
}

#[test]
fn the_two_implementations_agree_pixel_for_pixel() {
    // The clipper and the closed form share no code. Agreeing over a whole
    // frame at six orientations -- axis-aligned both ways, both diagonals, and
    // two shallow ones -- is a much stronger statement than either agreeing
    // with a test somebody wrote after reading it.
    let edges = [
        Edge::new(Rational::ONE, Rational::ZERO, r(15, 4)).expect("an edge"),
        Edge::new(Rational::ZERO, Rational::ONE, r(9, 2)).expect("an edge"),
        Edge::new(Rational::ONE, Rational::ONE, r(17, 3)).expect("an edge"),
        Edge::new(n(-1), Rational::ONE, r(1, 5)).expect("an edge"),
        Edge::new(n(3), n(-7), r(23, 2)).expect("an edge"),
        Edge::new(n(-2), n(-5), r(-11, 4)).expect("an edge"),
    ];
    let mut compared = 0_u32;
    for edge in edges {
        let shape = Shape::half_plane(edge).expect("a shape");
        for y in 0..9 {
            for x in 0..12 {
                assert_eq!(
                    shape.coverage(x, y).expect("a coverage"),
                    half_plane_coverage(edge, x, y).expect("a coverage"),
                    "at ({x}, {y})"
                );
                compared += 1;
            }
        }
    }
    assert_eq!(compared, 648, "every pixel of every orientation");
}

#[test]
fn coverage_sums_to_the_area_the_shape_encloses() {
    // The strongest statement available, and a relationship rather than a
    // bound: a rectangle from (1.5, 2.25) to (5, 6) has area 3.5 x 3.75 =
    // 13.125, and the coverages of every pixel it touches sum to exactly that.
    // Any per-pixel error at all -- a wrong partial, a double-counted edge, a
    // missed column -- moves this total.
    let shape = Shape::rectangle(r(3, 2), r(9, 4), n(5), n(6)).expect("a shape");
    let mut total = Rational::ZERO;
    for y in 0..10 {
        for x in 0..10 {
            total = total
                .checked_add(shape.coverage(x, y).expect("a coverage"))
                .expect("a sum");
        }
    }
    assert_eq!(total, r(105, 8));
}

#[test]
fn a_half_plane_sums_to_the_area_it_cuts_from_the_picture() {
    // x + y <= 6 over an 8 x 8 picture keeps everything below the diagonal
    // through (6, 0) and (0, 6): a right triangle of legs six, area eighteen.
    let shape = diagonal(n(6));
    let mut total = Rational::ZERO;
    for y in 0..8 {
        for x in 0..8 {
            total = total
                .checked_add(shape.coverage(x, y).expect("a coverage"))
                .expect("a sum");
        }
    }
    assert_eq!(total, n(18));
}

#[test]
fn a_rectangle_is_the_product_of_its_two_overlaps() {
    // A third independent check, and one only an axis-aligned rectangle
    // allows: the area of a pixel inside it is exactly how much of the pixel's
    // width overlaps times how much of its height does. Anything computing the
    // two dimensions together would have to agree with a product it never
    // formed.
    let shape = Shape::rectangle(r(1, 3), r(2, 5), r(11, 3), r(17, 5)).expect("a shape");
    let overlap = |low: Rational, high: Rational, at: i64| -> Rational {
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
        let span = stop.checked_sub(start).expect("a difference");
        if span.is_positive() {
            span
        } else {
            Rational::ZERO
        }
    };
    for y in 0..5 {
        for x in 0..5 {
            let expected = overlap(r(1, 3), r(11, 3), x)
                .checked_mul(overlap(r(2, 5), r(17, 5), y))
                .expect("a product");
            assert_eq!(
                shape.coverage(x, y).expect("a coverage"),
                expected,
                "at ({x}, {y})"
            );
        }
    }
}

#[test]
fn an_edge_with_no_direction_is_refused() {
    assert_eq!(
        Edge::new(Rational::ZERO, Rational::ZERO, Rational::ONE),
        Err(RenderStatus::DegenerateEdge),
        "that condition is always true or always false and names no line"
    );
}

#[test]
fn a_shape_with_no_edges_is_refused() {
    assert_eq!(Shape::new(Vec::new()), Err(RenderStatus::DegenerateShape));
}

#[test]
fn a_rectangle_enclosing_nothing_is_refused() {
    assert_eq!(
        Shape::rectangle(n(4), n(1), n(4), n(6)),
        Err(RenderStatus::DegenerateShape),
        "a mask of nothing is a mistake rather than an empty picture"
    );
}

#[test]
fn a_shape_with_too_many_edges_is_refused() {
    let edge = Edge::new(Rational::ONE, Rational::ZERO, Rational::ONE).expect("an edge");
    let held = vec![edge; MAX_EDGES + 1];
    assert_eq!(Shape::new(held), Err(RenderStatus::ShapeTooComplex));
}

#[test]
fn a_shape_clipped_away_to_nothing_covers_nothing() {
    // Two edges facing each other with no room between them. The clipper runs
    // out of polygon partway through and must say so rather than taking the
    // area of a point.
    let shape = Shape::new(vec![
        Edge::new(Rational::ONE, Rational::ZERO, n(1)).expect("an edge"),
        Edge::new(n(-1), Rational::ZERO, n(-3)).expect("an edge"),
    ])
    .expect("a shape");
    assert_eq!(shape.coverage(0, 0).expect("a coverage"), Rational::ZERO);
    assert_eq!(shape.coverage(2, 0).expect("a coverage"), Rational::ZERO);
}

#[test]
fn quantising_rounds_half_away_from_zero() {
    assert_eq!(quantise(Rational::ZERO).expect("a byte"), 0);
    assert_eq!(quantise(Rational::ONE).expect("a byte"), FULL);
    // Half of 255 is 127.5, which rounds up rather than to even.
    assert_eq!(quantise(r(1, 2)).expect("a byte"), 128);
    // A third of 255 is 85 exactly, so nothing rounds at all.
    assert_eq!(quantise(r(1, 3)).expect("a byte"), 85);
    // Just under half a code value rounds down.
    assert_eq!(quantise(r(1, 511)).expect("a byte"), 0);
    assert_eq!(quantise(r(1, 510)).expect("a byte"), 1);
}

#[test]
fn a_coverage_outside_nought_to_one_is_refused() {
    assert_eq!(
        quantise(r(3, 2)),
        Err(RenderStatus::OutsideDomain),
        "no area of a unit square is more than one, so this means the arithmetic went wrong"
    );
    assert_eq!(quantise(r(-1, 2)), Err(RenderStatus::OutsideDomain));
}

#[test]
fn a_plane_is_one_byte_per_pixel_in_reading_order() {
    let held = plane(&left_of(r(5, 2)), 4, 3).expect("a plane");
    assert_eq!(held.len(), 12);
    // Columns 0 and 1 whole, column 2 half, column 3 nothing -- the same in
    // every row, because a vertical line does not vary down the picture.
    for row in 0..3 {
        assert_eq!(&held[row * 4..row * 4 + 4], &[FULL, FULL, 128, 0]);
    }
}

#[test]
fn a_plane_with_no_pixels_is_refused() {
    assert_eq!(
        plane(&left_of(n(1)), 0, 4),
        Err(RenderStatus::OutsideDomain)
    );
    assert_eq!(
        plane(&left_of(n(1)), 4, 0),
        Err(RenderStatus::OutsideDomain)
    );
}

#[test]
fn quantising_a_complement_twice_does_not_always_give_full_coverage() {
    // The exact coverages sum to one; their two roundings need not sum to 255.
    // At exactly half, both round up, and 128 + 128 is 256. That is why a wipe
    // must quantise one side and derive the other as `255 - q` rather than
    // rasterising the complement -- and why this is a test rather than a
    // sentence in a comment.
    let edge = Edge::new(Rational::ONE, Rational::ZERO, r(5, 2)).expect("an edge");
    let inside = quantise(
        Shape::half_plane(edge)
            .expect("a shape")
            .coverage(2, 0)
            .expect("a coverage"),
    )
    .expect("a byte");
    let outside = quantise(
        Shape::half_plane(edge.complement().expect("a complement"))
            .expect("a shape")
            .coverage(2, 0)
            .expect("a coverage"),
    )
    .expect("a byte");
    assert_eq!((inside, outside), (128, 128));
    assert_eq!(u16::from(inside) + u16::from(outside), 256);
}

#[test]
fn a_wipe_at_nothing_covers_nothing_and_at_all_covers_everything() {
    // Both ends reached exactly, by geometry rather than by arithmetic: at
    // nought the edge sits on the corner the direction points away from, at
    // one it has passed the opposite corner. A transition that showed a sliver
    // of the incoming clip before it started, or a sliver of the outgoing one
    // after it finished, is the same wasted frame the dissolve's fraction is
    // shaped to avoid.
    use media_editor_render::shape::sweeping;
    let none = sweeping(Rational::ONE, Rational::ZERO, Rational::ZERO, 6, 4).expect("a shape");
    assert_eq!(plane(&none, 6, 4).expect("a plane"), vec![0; 24]);
    let all = sweeping(Rational::ONE, Rational::ZERO, Rational::ONE, 6, 4).expect("a shape");
    assert_eq!(plane(&all, 6, 4).expect("a plane"), vec![FULL; 24]);
}

#[test]
fn a_wipe_halfway_across_covers_half_the_picture() {
    use media_editor_render::shape::sweeping;
    let shape = sweeping(Rational::ONE, Rational::ZERO, r(1, 2), 8, 3).expect("a shape");
    let mut total = Rational::ZERO;
    for y in 0..3 {
        for x in 0..8 {
            total = total
                .checked_add(shape.coverage(x, y).expect("a coverage"))
                .expect("a sum");
        }
    }
    assert_eq!(total, n(12), "half of twenty-four pixels");
    assert_eq!(
        plane(&shape, 8, 3).expect("a plane")[0..8],
        [FULL, FULL, FULL, FULL, 0, 0, 0, 0]
    );
}

#[test]
fn a_wipe_downward_sweeps_down_rather_than_across() {
    use media_editor_render::shape::sweeping;
    let shape = sweeping(Rational::ZERO, Rational::ONE, r(1, 2), 2, 4).expect("a shape");
    assert_eq!(
        plane(&shape, 2, 4).expect("a plane"),
        [FULL, FULL, FULL, FULL, 0, 0, 0, 0]
    );
}

#[test]
fn a_wipe_backwards_starts_at_the_other_side() {
    // The direction says which way the covered region grows, so negating it
    // has to start the edge at the opposite corner rather than covering the
    // complement of the same sweep.
    use media_editor_render::shape::sweeping;
    let shape = sweeping(n(-1), Rational::ZERO, r(1, 4), 4, 1).expect("a shape");
    assert_eq!(plane(&shape, 4, 1).expect("a plane"), [0, 0, 0, FULL]);
}

#[test]
fn a_wipes_direction_has_no_length() {
    // Doubling the vector is the same wipe, because the fraction sets the
    // edge's position rather than the vector's length doing it. A
    // normalisation that crept in would need a square root and would not be
    // exact, so this pins that there is none.
    use media_editor_render::shape::sweeping;
    let one = sweeping(Rational::ONE, Rational::ONE, r(3, 8), 5, 5).expect("a shape");
    let two = sweeping(n(2), n(2), r(3, 8), 5, 5).expect("a shape");
    assert_eq!(
        plane(&one, 5, 5).expect("a plane"),
        plane(&two, 5, 5).expect("a plane")
    );
}

#[test]
fn a_wipe_with_no_direction_is_refused() {
    use media_editor_render::shape::sweeping;
    assert_eq!(
        sweeping(Rational::ZERO, Rational::ZERO, r(1, 2), 4, 4),
        Err(RenderStatus::DegenerateEdge)
    );
}

#[test]
fn a_wipe_past_its_travel_is_refused() {
    use media_editor_render::shape::sweeping;
    assert_eq!(
        sweeping(Rational::ONE, Rational::ZERO, r(3, 2), 4, 4),
        Err(RenderStatus::OutsideDomain),
        "a fraction past one is a transition that has run off its own end"
    );
    assert_eq!(
        sweeping(Rational::ONE, Rational::ZERO, r(-1, 2), 4, 4),
        Err(RenderStatus::OutsideDomain)
    );
}

#[test]
fn no_softness_is_the_hard_edge_exactly() {
    // The limit, and the reason the zero case delegates rather than dividing
    // by a band of nothing. Not "close to": the same bytes.
    use media_editor_render::shape::{feathered, sweeping};
    let hard = plane(
        &sweeping(Rational::ONE, Rational::ZERO, r(2, 5), 9, 5).expect("a shape"),
        9,
        5,
    )
    .expect("a plane");
    let soft =
        feathered(Rational::ONE, Rational::ZERO, r(2, 5), Rational::ZERO, 9, 5).expect("a plane");
    assert_eq!(hard, soft);
}

#[test]
fn a_soft_edge_and_its_complement_still_partition_every_pixel() {
    // The ramp is symmetric, so what it takes from one side it gives to the
    // other -- exactly, at every pixel, not on average over the picture. This
    // is the property that lets a soft wipe be built from one plane and its
    // complement without the two disagreeing about a code value somewhere
    // along the edge.
    use media_editor_render::shape::Feather;
    let edge = Edge::new(n(3), n(-7), r(23, 2)).expect("an edge");
    let one = Feather::new(edge, r(5, 2)).expect("a feather");
    let other = Feather::new(edge.complement().expect("a complement"), r(5, 2)).expect("a feather");
    for y in -1..7 {
        for x in -1..7 {
            let together = one
                .coverage(x, y)
                .expect("a coverage")
                .checked_add(other.coverage(x, y).expect("a coverage"))
                .expect("a sum");
            assert_eq!(together, Rational::ONE, "at ({x}, {y})");
        }
    }
}

#[test]
fn a_pixel_inside_the_band_is_the_ramp_at_its_centre() {
    // For a pixel entirely within the band the integrand is affine over the
    // whole square, so the integral is the value at the square's centroid --
    // which is its centre. Hand-checkable, and it pins the moment arithmetic
    // against a case with a closed form.
    use media_editor_render::shape::Feather;
    // x <= 4 with a band of 6: the ramp runs from x = 1 to x = 7, so its value
    // is (7 - x)/6. The pixel from 3 to 4 has its centre at 3.5, giving
    // 3.5/6 = 7/12.
    let edge = Edge::new(Rational::ONE, Rational::ZERO, n(4)).expect("an edge");
    let feather = Feather::new(edge, n(6)).expect("a feather");
    assert_eq!(feather.coverage(3, 0).expect("a coverage"), r(7, 12));
    // The pixel from 4 to 5, centre 4.5: 2.5/6 = 5/12.
    assert_eq!(feather.coverage(4, 0).expect("a coverage"), r(5, 12));
    // And from 5 to 6, centre 5.5: 1.5/6 = 1/4.
    assert_eq!(feather.coverage(5, 0).expect("a coverage"), r(1, 4));
    // No pixel above sits at exactly a half, because the band's centre is on
    // a pixel boundary rather than a pixel centre. Moving the edge half a
    // pixel puts one there, which is worth pinning separately: it is the one
    // value a sign error in the moment would still produce by accident.
    let centred = Feather::new(
        Edge::new(Rational::ONE, Rational::ZERO, r(9, 2)).expect("an edge"),
        n(6),
    )
    .expect("a feather");
    assert_eq!(centred.coverage(4, 0).expect("a coverage"), r(1, 2));
}

#[test]
fn a_pixel_outside_the_band_is_untouched_by_softness() {
    use media_editor_render::shape::Feather;
    let edge = Edge::new(Rational::ONE, Rational::ZERO, n(4)).expect("an edge");
    let feather = Feather::new(edge, n(2)).expect("a feather");
    assert_eq!(feather.coverage(0, 0).expect("a coverage"), Rational::ONE);
    assert_eq!(feather.coverage(9, 0).expect("a coverage"), Rational::ZERO);
}

#[test]
fn softening_an_axis_aligned_wipe_conserves_the_total_coverage() {
    // The ramp is antisymmetric about its centre line, so what it removes on
    // one side it adds on the other. Over a rectangle the cross-section at
    // every position along an axis-aligned sweep is the same, so the two
    // cancel *exactly* -- softness moves coverage around the picture without
    // creating or destroying any.
    //
    // The condition is real and is why this says "axis-aligned": a diagonal
    // sweep meets triangular cross-sections near the corners, the two sides do
    // not cancel, and the total genuinely moves.
    use media_editor_render::shape::Feather;
    let edge = Edge::new(Rational::ONE, Rational::ZERO, n(5)).expect("an edge");
    let sum = |band: Rational| {
        let feather = Feather::new(edge, band).expect("a feather");
        let mut total = Rational::ZERO;
        for y in 0..3 {
            for x in 0..10 {
                total = total
                    .checked_add(feather.coverage(x, y).expect("a coverage"))
                    .expect("a sum");
            }
        }
        total
    };
    assert_eq!(sum(r(1, 2)), n(15));
    assert_eq!(sum(n(2)), n(15));
    assert_eq!(sum(n(4)), n(15));
}

#[test]
fn a_softness_past_its_own_range_is_refused() {
    use media_editor_render::shape::feathered;
    assert_eq!(
        feathered(Rational::ONE, Rational::ZERO, r(1, 2), r(3, 2), 4, 4),
        Err(RenderStatus::OutsideDomain)
    );
    assert_eq!(
        feathered(Rational::ONE, Rational::ZERO, r(1, 2), r(-1, 4), 4, 4),
        Err(RenderStatus::OutsideDomain)
    );
}

#[test]
fn a_band_of_nothing_is_refused_as_a_feather() {
    use media_editor_render::shape::Feather;
    let edge = Edge::new(Rational::ONE, Rational::ZERO, n(4)).expect("an edge");
    assert_eq!(
        Feather::new(edge, Rational::ZERO),
        Err(RenderStatus::DegenerateShape),
        "a ramp across no distance is a hard edge, and saying so is better than dividing by it"
    );
}

#[test]
fn a_softer_edge_spreads_further_and_stays_ordered() {
    // Monotone in two directions at once: across the picture the coverage only
    // falls, and at a fixed pixel a wider band moves it towards a half. A
    // rasteriser that got the moment's sign wrong passes neither.
    use media_editor_render::shape::Feather;
    let edge = Edge::new(Rational::ONE, Rational::ZERO, n(5)).expect("an edge");
    let narrow = Feather::new(edge, n(2)).expect("a feather");
    let wide = Feather::new(edge, n(8)).expect("a feather");
    let mut previous = Rational::ONE;
    for x in 0..10 {
        let held = wide.coverage(x, 0).expect("a coverage");
        assert!(
            !held
                .checked_sub(previous)
                .expect("a difference")
                .is_positive(),
            "coverage rose at {x}"
        );
        previous = held;
    }
    // Left of the edge the wider band has given more away; right of it, taken
    // more.
    assert!(
        narrow
            .coverage(2, 0)
            .expect("a coverage")
            .checked_sub(wide.coverage(2, 0).expect("a coverage"))
            .expect("a difference")
            .is_positive()
    );
    assert!(
        wide.coverage(7, 0)
            .expect("a coverage")
            .checked_sub(narrow.coverage(7, 0).expect("a coverage"))
            .expect("a difference")
            .is_positive()
    );
}

#[test]
fn a_vanishing_band_agrees_with_the_hard_edge() {
    // The zero case delegates to the hard edge to avoid dividing by a band of
    // nothing. This is the statement that the delegation is a *convenience*
    // rather than a patch over a discontinuity: a band a thousandth of the
    // travel wide already produces the same plane, byte for byte, so the soft
    // path converges on the hard one rather than jumping to it.
    //
    // Written as a test because the control for it kept passing. The mutation
    // -- delegate to a thousandth instead of to nought -- changed nothing, and
    // the reason it changed nothing is worth having as an assertion.
    use media_editor_render::shape::{feathered, sweeping};
    let hard = plane(
        &sweeping(Rational::ONE, Rational::ZERO, r(2, 5), 9, 5).expect("a shape"),
        9,
        5,
    )
    .expect("a plane");
    let nearly =
        feathered(Rational::ONE, Rational::ZERO, r(2, 5), r(1, 1_000), 9, 5).expect("a plane");
    assert_eq!(hard, nearly);
    // And a band wide enough to see does differ, so the comparison above is
    // not measuring a function that ignores its argument.
    let wide = feathered(Rational::ONE, Rational::ZERO, r(2, 5), r(1, 4), 9, 5).expect("a plane");
    assert_ne!(hard, wide);
}

#[test]
fn a_mask_covers_the_fraction_of_the_frame_it_encloses() {
    // Corners are fractions of the frame, so a half-by-half rectangle covers a
    // quarter of the pixels wherever it is placed and whatever the size.
    use media_editor_render::shape::masking;
    for (width, height) in [(8_usize, 8_usize), (12, 4), (5, 7)] {
        let held = masking(
            &[
                (r(1, 4), r(1, 4)),
                (r(3, 4), r(1, 4)),
                (r(3, 4), r(3, 4)),
                (r(1, 4), r(3, 4)),
            ],
            false,
            width,
            height,
        )
        .expect("a plane");
        let total: u32 = held.iter().map(|value| u32::from(*value)).sum();
        let expected = u32::try_from(width * height).expect("a count") * u32::from(FULL) / 4;
        assert_eq!(total, expected, "at {width} by {height}");
    }
}

#[test]
fn a_mask_reads_the_same_whichever_way_its_corners_run() {
    // The winding is measured from the polygon's own area rather than demanded
    // of the caller, so the same shape given both ways round is the same
    // plane. Getting this wrong inverts the mask, which is the single most
    // confusing failure a mask can have.
    use media_editor_render::shape::masking;
    let one = masking(
        &[(r(1, 4), r(1, 4)), (r(3, 4), r(1, 4)), (r(1, 2), r(3, 4))],
        false,
        9,
        9,
    )
    .expect("a plane");
    let other = masking(
        &[(r(1, 2), r(3, 4)), (r(3, 4), r(1, 4)), (r(1, 4), r(1, 4))],
        false,
        9,
        9,
    )
    .expect("a plane");
    assert_eq!(one, other);
    assert!(one.contains(&FULL), "something is inside");
    assert!(one.contains(&0), "something is outside");
}

#[test]
fn a_mask_and_its_inversion_sum_to_full_coverage_everywhere() {
    // Inverting the byte rather than the shape, so the two sides always sum to
    // exactly 255. Rasterising the complement instead would quantise twice and
    // could sum to 256 -- the same trap the wipe records, avoided the same way.
    use media_editor_render::shape::masking;
    let corners = [
        (r(1, 5), r(1, 7)),
        (r(4, 5), r(1, 3)),
        (r(2, 3), r(6, 7)),
        (r(1, 4), r(3, 4)),
    ];
    let inside = masking(&corners, false, 11, 9).expect("a plane");
    let outside = masking(&corners, true, 11, 9).expect("a plane");
    for (index, (one, other)) in inside.iter().zip(&outside).enumerate() {
        assert_eq!(
            u16::from(*one) + u16::from(*other),
            u16::from(FULL),
            "at pixel {index}"
        );
    }
    assert_ne!(inside, outside, "and they are not the same plane");
}

#[test]
fn a_masks_edge_is_anti_aliased_even_though_it_is_hard() {
    // "Hard" means no feather, not aliased: the coverage is still an exact
    // area, so a diagonal edge lands on partial values rather than on a
    // staircase of nought and full.
    use media_editor_render::shape::masking;
    let held = masking(
        &[(r(1, 8), r(1, 8)), (r(7, 8), r(3, 8)), (r(1, 8), r(7, 8))],
        false,
        16,
        16,
    )
    .expect("a plane");
    let partial = held
        .iter()
        .filter(|value| **value > 0 && **value < FULL)
        .count();
    assert!(
        partial > 8,
        "a triangle across sixteen pixels has a soft-looking edge, got {partial} partial pixels"
    );
}

#[test]
fn a_mask_with_too_few_corners_is_refused() {
    use media_editor_render::shape::masking;
    assert_eq!(
        masking(&[(r(0, 1), r(0, 1)), (r(1, 1), r(1, 1))], false, 4, 4),
        Err(RenderStatus::DegenerateShape)
    );
}

#[test]
fn a_mask_enclosing_no_area_is_refused() {
    use media_editor_render::shape::masking;
    assert_eq!(
        masking(
            &[(r(0, 1), r(0, 1)), (r(1, 2), r(1, 2)), (r(1, 1), r(1, 1)),],
            false,
            4,
            4
        ),
        Err(RenderStatus::DegenerateShape),
        "three points in a line describe no region"
    );
}
