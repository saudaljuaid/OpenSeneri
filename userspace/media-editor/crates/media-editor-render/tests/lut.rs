// SPDX-License-Identifier: GPL-3.0-only
//! What a three-dimensional lookup table is allowed to claim.
//!
//! The important test here is not that interpolation works. It is that
//! tetrahedral interpolation keeps a grey grey and trilinear does not, run
//! against the same table — because that difference is the entire reason for
//! choosing one, and a design decision with no test showing what the rejected
//! option does is a preference rather than a decision.

use media_editor_core::{Fixed, Rational};
use media_editor_render::lut::{Interpolation, Lut3D, MAX_SIZE, MIN_SIZE};
use media_editor_render::{Colour, RenderStatus};

/// A fixed-point value from a fraction.
fn at(numerator: i64, denominator: i64) -> Fixed {
    Fixed::from_rational(Rational::new(numerator, denominator).expect("a ratio")).expect("a value")
}

/// A colour from three fractions over one denominator.
fn rgb(red: i64, green: i64, blue: i64, denominator: i64) -> Colour {
    [
        at(red, denominator),
        at(green, denominator),
        at(blue, denominator),
    ]
}

/// How far apart two colours are, in raw fixed-point units.
fn apart(left: Colour, right: Colour) -> i64 {
    (0..3)
        .map(|channel| (left[channel].raw() - right[channel].raw()).abs())
        .max()
        .unwrap_or(0)
}

/// A table that is neutral on its diagonal and emphatically not elsewhere.
///
/// Off the diagonal it swaps red and blue and lifts green, which is a strong
/// cross-channel look — the kind that makes an interpolation's faults visible.
/// On the diagonal it is left alone, so every grey in the *table* is a grey.
///
/// That combination is the point: a table which is neutral where it is sampled
/// but full of cross-channel content between those samples is exactly the case
/// where the two interpolations disagree about what happens to a grey.
fn neutral_but_lively(size: usize) -> Lut3D {
    let mut samples = std::vec::Vec::new();
    let last = i64::try_from(size - 1).expect("a size");
    for blue in 0..size {
        for green in 0..size {
            for red in 0..size {
                let (r, g, b) = (
                    i64::try_from(red).expect("an index"),
                    i64::try_from(green).expect("an index"),
                    i64::try_from(blue).expect("an index"),
                );
                samples.push(if r == g && g == b {
                    // Untouched on the diagonal, so the table's own greys are
                    // grey.
                    rgb(r, r, r, last)
                } else {
                    // Red and blue swapped, green pushed towards the top.
                    rgb(b, (g + last) / 2, r, last)
                });
            }
        }
    }
    Lut3D::new(size, samples).expect("a table")
}

#[test]
fn a_grey_stays_grey_only_under_tetrahedral() {
    // The reason tetrahedral is the default, stated as the difference it
    // makes rather than as a preference.
    //
    // On the neutral axis the four tetrahedral terms telescope:
    //
    //   c000 + (c100-c000)f + (c110-c100)f + (c111-c110)f = c000 + (c111-c000)f
    //
    // which is a straight interpolation between the cell's two diagonal
    // corners. Both of those are grey in this table, so all three channels
    // evaluate the identical expression and round identically — the result is
    // exactly grey, in fixed point, without a tolerance.
    //
    // Trilinear mixes all eight corners, six of which are the swapped-and-
    // lifted ones, so it drifts off neutral. A colourist sees that as a tint
    // in the greys, which is the first thing anybody notices in a grade.
    let table = neutral_but_lively(5);
    assert!(
        table.is_neutral().expect("a table"),
        "the table is not neutral"
    );

    let mut tinted = 0;
    for step in 1..40 {
        let grey = rgb(step, step, step, 40);

        let exact = table
            .look_up(grey, Interpolation::Tetrahedral)
            .expect("a colour");
        assert_eq!(
            exact[0], exact[1],
            "tetrahedral tinted a grey at {step}/40: {exact:?}"
        );
        assert_eq!(exact[1], exact[2], "tetrahedral tinted a grey at {step}/40");

        let eight = table
            .look_up(grey, Interpolation::Trilinear)
            .expect("a colour");
        if eight[0] != eight[1] || eight[1] != eight[2] {
            tinted += 1;
        }
    }
    // Twenty-nine of the thirty-nine, measured. The ten it leaves alone are
    // the ones that land on or very near a lattice point, where the two
    // interpolations agree by construction. The threshold is well below the
    // real number on purpose: it is a guard against this table quietly
    // ceasing to distinguish the two, not a pinned value.
    assert!(
        tinted > 20,
        "trilinear tinted only {tinted} of 39 greys, so this table does not \
         actually distinguish the two interpolations and the test proves nothing"
    );
}

#[test]
fn both_interpolations_reproduce_the_lattice_exactly() {
    // A sample sitting on a lattice point must come back as that point's
    // value, untouched. Every weight is nought or one there, so nothing is
    // multiplied and fixed point loses nothing — which is why this is an
    // equality rather than a tolerance.
    let table = neutral_but_lively(5);
    let last = 4_i64;
    for how in [Interpolation::Tetrahedral, Interpolation::Trilinear] {
        for blue in 0..=last {
            for green in 0..=last {
                for red in 0..=last {
                    let input = rgb(red, green, blue, last);
                    let held = table
                        .sample(
                            usize::try_from(red).expect("an index"),
                            usize::try_from(green).expect("an index"),
                            usize::try_from(blue).expect("an index"),
                        )
                        .expect("a sample");
                    assert_eq!(
                        table.look_up(input, how).expect("a colour"),
                        held,
                        "{how:?} did not reproduce lattice point {red},{green},{blue}"
                    );
                }
            }
        }
    }
}

#[test]
fn an_identity_table_changes_nothing() {
    // The table that does nothing must do nothing, at every size, under both
    // interpolations, and not only at its lattice points. Between them the
    // arithmetic still has to come back where it started.
    for size in [MIN_SIZE, 3, 5, 17, 33] {
        let table = Lut3D::identity(size).expect("a table");
        assert!(table.is_neutral().expect("a table"));
        for how in [Interpolation::Tetrahedral, Interpolation::Trilinear] {
            for step in 0..=64 {
                let value = rgb(step, 64 - step, (step * 7) % 65, 64);
                let held = table.look_up(value, how).expect("a colour");
                assert!(
                    apart(held, value) <= 2,
                    "identity at size {size} under {how:?} moved {value:?} to {held:?}"
                );
            }
        }
    }
}

#[test]
fn the_far_corner_is_reached_exactly() {
    // The top of the range is the awkward one: it lands in the *last* cell at
    // a fraction of one rather than in a cell past the end at nought. Both
    // name the same point, and this way the tetrahedral terms telescope to the
    // far corner exactly instead of reading past the cube.
    let table = neutral_but_lively(4);
    let white = [Fixed::ONE, Fixed::ONE, Fixed::ONE];
    let corner = table.sample(3, 3, 3).expect("a sample");
    for how in [Interpolation::Tetrahedral, Interpolation::Trilinear] {
        assert_eq!(
            table.look_up(white, how).expect("a colour"),
            corner,
            "{how:?} did not reach the far corner"
        );
    }
}

#[test]
fn a_colour_outside_the_table_is_held_at_its_edge() {
    // A table holds what it holds. There is no sample beyond its edge, and
    // extrapolating from the last cell would invent a look nobody authored —
    // so out of range saturates at a real boundary, which is a different thing
    // from a decision hidden from the caller.
    let table = neutral_but_lively(4);
    let corner = table.sample(3, 3, 3).expect("a sample");
    let base = table.sample(0, 0, 0).expect("a sample");
    for how in [Interpolation::Tetrahedral, Interpolation::Trilinear] {
        let above = [at(3, 2), at(9, 8), at(2, 1)];
        assert_eq!(table.look_up(above, how).expect("a colour"), corner);
        let below = [at(-1, 2), at(-1, 8), at(-3, 1)];
        assert_eq!(table.look_up(below, how).expect("a colour"), base);
    }
}

#[test]
fn every_tetrahedron_is_reached_and_none_of_them_is_wrong() {
    // The six branches are the six orderings of the three fractions. A subtle
    // error in one of them shows in one sixth of one cell of the colour space
    // — nowhere a still frame reveals it — so this walks a grid fine enough to
    // land inside every ordering and checks each result against the property
    // every tetrahedral answer must have: it lies within the range its cell's
    // corners span, channel by channel.
    let table = neutral_but_lively(4);
    let mut seen = std::collections::BTreeSet::new();
    for red in 1..12_i64 {
        for green in 1..12_i64 {
            for blue in 1..12_i64 {
                // Which ordering this sample falls in, recorded so the test
                // can say whether it actually reached all six.
                let mut order = [(red % 3, 0), (green % 3, 1), (blue % 3, 2)];
                order.sort_unstable();
                seen.insert([order[0].1, order[1].1, order[2].1]);

                let input = rgb(red, green, blue, 12);
                let held = table
                    .look_up(input, Interpolation::Tetrahedral)
                    .expect("a colour");
                let cell =
                    [red, green, blue].map(|axis| usize::try_from(axis * 3 / 12).expect("a cell"));
                let mut low = [i64::MAX; 3];
                let mut high = [i64::MIN; 3];
                for step in 0..8 {
                    let corner = table
                        .sample(
                            cell[0] + (step & 1),
                            cell[1] + ((step >> 1) & 1),
                            cell[2] + ((step >> 2) & 1),
                        )
                        .expect("a sample");
                    for channel in 0..3 {
                        low[channel] = low[channel].min(corner[channel].raw());
                        high[channel] = high[channel].max(corner[channel].raw());
                    }
                }
                for channel in 0..3 {
                    assert!(
                        held[channel].raw() >= low[channel] && held[channel].raw() <= high[channel],
                        "at {red},{green},{blue} channel {channel} landed outside \
                         the corners of its own cell"
                    );
                }
            }
        }
    }
    assert_eq!(seen.len(), 6, "only {} orderings were reached", seen.len());
}

#[test]
fn the_surface_is_continuous_across_every_tetrahedron_boundary() {
    // What a wrong vertex set in one of the six branches actually breaks, and
    // the test that catches it. The bounding-box check above does not: a
    // tetrahedron given the wrong vertices still lands inside its own cell's
    // corners, so that test passed under a mutation aimed straight at it.
    //
    // The tetrahedra meet on the planes where two fractions are equal. Both
    // branches either side of such a plane must agree *on* it, or the
    // interpolated surface has a step in it — which in a grade is a hard edge
    // through a smooth gradient, the exact artefact tetrahedral interpolation
    // is chosen to avoid.
    //
    // So: walk a line that crosses all three planes and assert consecutive
    // samples never jump. The step is a hundredth of a cell, and the bound is
    // a tenth of the widest a cell spans — far above the real change per step
    // and far below a discontinuity.
    let table = neutral_but_lively(4);

    let mut widest = 0_i64;
    for blue in 0..3 {
        for green in 0..3 {
            for red in 0..3 {
                let mut low = [i64::MAX; 3];
                let mut high = [i64::MIN; 3];
                for step in 0..8 {
                    let corner = table
                        .sample(
                            red + (step & 1),
                            green + ((step >> 1) & 1),
                            blue + ((step >> 2) & 1),
                        )
                        .expect("a sample");
                    for channel in 0..3 {
                        low[channel] = low[channel].min(corner[channel].raw());
                        high[channel] = high[channel].max(corner[channel].raw());
                    }
                }
                for channel in 0..3 {
                    widest = widest.max(high[channel] - low[channel]);
                }
            }
        }
    }
    let allowed = widest / 10;
    assert!(
        allowed > 0,
        "the fixture spans nothing, so this proves nothing"
    );

    // Three lines, each sweeping one channel while the other two sit at
    // different offsets — so the sweep crosses fr = fg, fg = fb and fr = fb at
    // different places rather than all at once.
    let mut crossings = 0;
    for (fixed_green, fixed_blue) in [(37_i64, 61_i64), (61, 37), (50, 50)] {
        let mut previous: Option<Colour> = None;
        for step in 0..=300_i64 {
            let held = table
                .look_up(
                    [at(step, 300), at(fixed_green, 100), at(fixed_blue, 100)],
                    Interpolation::Tetrahedral,
                )
                .expect("a colour");
            if let Some(before) = previous {
                let jump = apart(before, held);
                assert!(
                    jump <= allowed,
                    "the surface steps by {jump} at {step}/300 with green                      {fixed_green}/100 and blue {fixed_blue}/100, where a                      hundredth of a cell should move it by far less than                      {allowed}"
                );
                if jump > 0 {
                    crossings += 1;
                }
            }
            previous = Some(held);
        }
    }
    assert!(
        crossings > 100,
        "the sweep barely moved at all ({crossings} changes), so a          discontinuity would not have shown"
    );
}

#[test]
fn a_table_that_is_not_a_cube_is_refused() {
    // The one relationship a file can get wrong in a way that still parses.
    let short = std::vec![[Fixed::ZERO; 3]; 26];
    assert_eq!(
        Lut3D::new(3, short).map(|_| ()),
        Err(RenderStatus::LutNotACube)
    );
    let long = std::vec![[Fixed::ZERO; 3]; 28];
    assert_eq!(
        Lut3D::new(3, long).map(|_| ()),
        Err(RenderStatus::LutNotACube)
    );
    assert!(Lut3D::new(3, std::vec![[Fixed::ZERO; 3]; 27]).is_ok());
}

#[test]
fn a_side_this_build_does_not_carry_is_refused() {
    for size in [0, 1, MAX_SIZE + 1, 1_000_000] {
        assert_eq!(
            Lut3D::identity(size).map(|_| ()),
            Err(RenderStatus::LutSizeUnsupported),
            "a side of {size} was accepted"
        );
    }
    assert!(Lut3D::identity(MIN_SIZE).is_ok());
    assert!(Lut3D::identity(MAX_SIZE).is_ok());
}

#[test]
fn a_lattice_point_outside_the_table_is_refused() {
    let table = Lut3D::identity(4).expect("a table");
    assert_eq!(
        table.sample(4, 0, 0).map(|_| ()),
        Err(RenderStatus::LutIndexOutOfRange)
    );
    assert_eq!(
        table.sample(0, 4, 0).map(|_| ()),
        Err(RenderStatus::LutIndexOutOfRange)
    );
    assert_eq!(
        table.sample(0, 0, 4).map(|_| ()),
        Err(RenderStatus::LutIndexOutOfRange)
    );
    assert!(table.sample(3, 3, 3).is_ok());
}

#[test]
fn a_table_that_tints_its_own_greys_says_so() {
    // A look that deliberately warms the greys is a legitimate look, and
    // nothing here should stop one. What a caller needs is to be able to tell
    // which kind of table it is holding, because "a grey stays grey" is a
    // promise about the *interpolation* and it has nothing to carry if the
    // table's own diagonal is not neutral.
    let mut samples = std::vec::Vec::new();
    for _ in 0..27 {
        samples.push(rgb(1, 2, 3, 4));
    }
    let tinting = Lut3D::new(3, samples).expect("a table");
    assert!(!tinting.is_neutral().expect("a table"));
    assert!(
        Lut3D::identity(3)
            .expect("a table")
            .is_neutral()
            .expect("a table")
    );
}

#[test]
fn a_lookup_is_the_same_lookup_every_time() {
    // Integer arithmetic throughout, so there is nowhere for two machines to
    // disagree and nowhere for the same machine to disagree with itself.
    let table = neutral_but_lively(9);
    let probes: std::vec::Vec<Colour> = (0..50)
        .map(|step| rgb(step, (step * 3) % 50, (step * 7) % 50, 50))
        .collect();
    for how in [Interpolation::Tetrahedral, Interpolation::Trilinear] {
        let first: std::vec::Vec<Colour> = probes
            .iter()
            .map(|colour| table.look_up(*colour, how).expect("a colour"))
            .collect();
        let again: std::vec::Vec<Colour> = probes
            .iter()
            .rev()
            .map(|colour| table.look_up(*colour, how).expect("a colour"))
            .collect();
        let mut reversed = again;
        reversed.reverse();
        assert_eq!(first, reversed, "{how:?} is not deterministic");
    }
}
