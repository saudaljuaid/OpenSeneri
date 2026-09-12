// SPDX-License-Identifier: GPL-3.0-only
//! Sine and cosine, against identities rather than against a table.
//!
//! Every expectation here is either exact by construction — the reduction
//! lands the argument on zero before any series runs — or an identity that
//! must hold at every angle. Nothing is a number read off a run of the code.

use media_editor_core::{Fixed, Rational};

/// How close two values must be, in units of the last bit.
///
/// Two. The series is summed at forty-eight fractional bits and rounded once,
/// so a value is right to within a bit or so, and an identity combining two of
/// them to within a couple.
const TOLERANCE: i64 = 2;

fn close(left: Fixed, right: Fixed, what: &str) {
    let apart = (left.raw() - right.raw()).abs();
    assert!(
        apart <= TOLERANCE,
        "{what}: {} and {} are {apart} bits apart",
        left.raw(),
        right.raw()
    );
}

fn turns(numerator: i64, denominator: i64) -> Fixed {
    Fixed::from_rational(Rational::new(numerator, denominator).expect("a ratio")).expect("a value")
}

#[test]
fn the_quarter_turns_are_exact() {
    // Exact, not nearly exact, and exact *by construction*: the reduction puts
    // each of these on the boundary of an octant, so the argument to the
    // series is zero and the series returns its first term untouched.
    //
    // A sine that gave 0.9999999 for a right angle would put a wobble into
    // every oscillator built on it, and the wobble would be inaudible until it
    // accumulated.
    assert_eq!(Fixed::ZERO.sin_turns().expect("a sine"), Fixed::ZERO);
    assert_eq!(turns(1, 4).sin_turns().expect("a sine"), Fixed::ONE);
    assert_eq!(turns(1, 2).sin_turns().expect("a sine"), Fixed::ZERO);
    assert_eq!(
        turns(3, 4).sin_turns().expect("a sine"),
        Fixed::ZERO.checked_sub(Fixed::ONE).expect("minus one")
    );

    assert_eq!(Fixed::ZERO.cos_turns().expect("a cosine"), Fixed::ONE);
    assert_eq!(turns(1, 4).cos_turns().expect("a cosine"), Fixed::ZERO);
    assert_eq!(
        turns(1, 2).cos_turns().expect("a cosine"),
        Fixed::ZERO.checked_sub(Fixed::ONE).expect("minus one")
    );
    assert_eq!(turns(3, 4).cos_turns().expect("a cosine"), Fixed::ZERO);
}

#[test]
fn the_angles_a_school_book_names_come_out_right() {
    // Thirty degrees is a twelfth of a turn and its sine is exactly one half;
    // sixty degrees is a sixth and its cosine is the same. Both are facts about
    // an equilateral triangle rather than about this implementation, which is
    // why they are worth asserting.
    let half = Fixed::from_rational(Rational::new(1, 2).expect("a ratio")).expect("a value");
    close(turns(1, 12).sin_turns().expect("a sine"), half, "sin 30");
    close(turns(1, 6).cos_turns().expect("a cosine"), half, "cos 60");

    // Forty-five degrees: sine and cosine are equal, and each is the square
    // root of a half.
    let eighth = turns(1, 8);
    let sine = eighth.sin_turns().expect("a sine");
    let cosine = eighth.cos_turns().expect("a cosine");
    close(sine, cosine, "sine and cosine at 45 degrees");
    close(sine, half.sqrt().expect("a root"), "sin 45");
}

#[test]
fn the_squares_sum_to_one_everywhere() {
    // The identity, and the sharpest available check on both functions at
    // once: nothing about the reduction, the series, or the rounding can go
    // wrong without this drifting. Two thousand angles across a whole turn,
    // rather than the handful anybody remembers.
    for step in 0..2000 {
        let angle = turns(step, 2000);
        let sine = angle.sin_turns().expect("a sine");
        let cosine = angle.cos_turns().expect("a cosine");
        let total = sine
            .checked_mul(sine)
            .expect("a square")
            .checked_add(cosine.checked_mul(cosine).expect("a square"))
            .expect("a sum");
        let apart = (total.raw() - Fixed::ONE.raw()).abs();
        assert!(apart <= 3, "at {step}/2000 the squares sum {apart} off one");
    }
}

#[test]
fn a_turn_is_a_turn_however_many_of_them_have_passed() {
    // Reduction in turns is masking bits, so it is exact — and it stays exact
    // however large the angle is. That is the whole reason for the unit: in
    // radians the same reduction is a division by an irrational number, and it
    // loses a little more accuracy for every revolution.
    let base = turns(37, 360);
    for revolutions in [1_i64, 2, 5, 100, 1000, 10_000] {
        let far = base
            .checked_add(Fixed::from_integer(revolutions).expect("a value"))
            .expect("an angle");
        assert_eq!(
            far.sin_turns().expect("a sine"),
            base.sin_turns().expect("a sine"),
            "after {revolutions} turns"
        );
    }
}

#[test]
fn the_sine_is_odd_and_the_cosine_is_even() {
    for step in 1..500 {
        let angle = turns(step, 500);
        let negated = Fixed::ZERO.checked_sub(angle).expect("a negation");
        let sine = angle.sin_turns().expect("a sine");
        let mirrored = negated.sin_turns().expect("a sine");
        close(
            mirrored,
            Fixed::ZERO.checked_sub(sine).expect("a negation"),
            "the sine is odd",
        );
        close(
            negated.cos_turns().expect("a cosine"),
            angle.cos_turns().expect("a cosine"),
            "the cosine is even",
        );
    }
}

#[test]
fn the_quadrants_have_the_signs_they_should() {
    // A sign error in one octant of the reduction is the classic way this goes
    // wrong, and it is invisible in any test that only looks at the first
    // quarter turn.
    for step in 1..250 {
        let first = turns(step, 1000);
        assert!(
            first.sin_turns().expect("a sine").raw() > 0,
            "first quadrant"
        );
        assert!(first.cos_turns().expect("a cosine").raw() > 0);

        let second = turns(250 + step, 1000);
        assert!(second.sin_turns().expect("a sine").raw() > 0, "second");
        assert!(second.cos_turns().expect("a cosine").raw() < 0);

        let third = turns(500 + step, 1000);
        assert!(third.sin_turns().expect("a sine").raw() < 0, "third");
        assert!(third.cos_turns().expect("a cosine").raw() < 0);

        let fourth = turns(750 + step, 1000);
        assert!(fourth.sin_turns().expect("a sine").raw() < 0, "fourth");
        assert!(fourth.cos_turns().expect("a cosine").raw() > 0);
    }
}

#[test]
fn the_sine_rises_and_falls_where_it_should() {
    // Monotone up to the quarter turn and down after it, with no dead spot and
    // no reversal — which is what catches an octant assembled the wrong way
    // round even when its sign is right.
    let mut previous = Fixed::ZERO.sin_turns().expect("a sine");
    for step in 1..=250 {
        let now = turns(step, 1000).sin_turns().expect("a sine");
        assert!(now.raw() > previous.raw(), "rising at {step}/1000");
        previous = now;
    }
    for step in 251..=500 {
        let now = turns(step, 1000).sin_turns().expect("a sine");
        assert!(now.raw() < previous.raw(), "falling at {step}/1000");
        previous = now;
    }
}

#[test]
fn the_addition_formula_holds() {
    // sin(a + b) = sin a cos b + cos a sin b. Independent of everything the
    // other tests check, and it fails loudly if two octants disagree about
    // where they meet.
    for (first, second) in [(1_i64, 7_i64), (37, 91), (123, 456), (700, 800)] {
        let a = turns(first, 1000);
        let b = turns(second, 1000);
        let together = a
            .checked_add(b)
            .expect("an angle")
            .sin_turns()
            .expect("a sine");
        let apart = a
            .sin_turns()
            .expect("a sine")
            .checked_mul(b.cos_turns().expect("a cosine"))
            .expect("a product")
            .checked_add(
                a.cos_turns()
                    .expect("a cosine")
                    .checked_mul(b.sin_turns().expect("a sine"))
                    .expect("a product"),
            )
            .expect("a sum");
        close(together, apart, "the addition formula");
    }
}

#[test]
fn an_angle_is_the_same_angle_every_time() {
    for step in [0_i64, 1, 125, 250, 375, 500, 999] {
        let angle = turns(step, 1000);
        assert_eq!(angle.sin_turns(), angle.sin_turns());
        assert_eq!(angle.cos_turns(), angle.cos_turns());
    }
}
