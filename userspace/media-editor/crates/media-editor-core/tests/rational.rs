// SPDX-License-Identifier: GPL-3.0-only
//! Exact rational arithmetic, and the refusals that keep it exact.

use media_editor_core::{CoreStatus, Rational};

#[test]
fn construction_reduces_and_normalises_sign() {
    let value = Rational::new(6, -4).expect("a reducible fraction");
    assert_eq!(value.numerator(), -3);
    assert_eq!(value.denominator(), 2);

    let already = Rational::new(-3, 2).expect("a reduced fraction");
    assert_eq!(value, already, "equality is structural after reduction");
}

#[test]
fn a_zero_denominator_is_refused() {
    assert_eq!(Rational::new(1, 0), Err(CoreStatus::ZeroDenominator));
}

#[test]
fn zero_is_zero_however_it_is_written() {
    assert_eq!(Rational::new(0, 7).expect("zero"), Rational::ZERO);
    assert_eq!(Rational::new(0, -7).expect("zero"), Rational::ZERO);
    assert!(Rational::ZERO.is_zero());
    assert!(!Rational::ZERO.is_positive());
}

#[test]
fn arithmetic_is_exact() {
    let third = Rational::new(1, 3).expect("a third");
    let sixth = Rational::new(1, 6).expect("a sixth");
    assert_eq!(
        third.checked_add(sixth).expect("sum"),
        Rational::new(1, 2).expect("a half")
    );
    assert_eq!(
        third.checked_sub(sixth).expect("difference"),
        Rational::new(1, 6).expect("a sixth")
    );
    assert_eq!(
        third.checked_mul(sixth).expect("product"),
        Rational::new(1, 18).expect("an eighteenth")
    );
    assert_eq!(
        third.checked_div(sixth).expect("quotient"),
        Rational::from_integer(2)
    );
}

#[test]
fn a_third_summed_three_times_is_exactly_one() {
    let third = Rational::new(1, 3).expect("a third");
    let sum = third
        .checked_add(third)
        .and_then(|partial| partial.checked_add(third))
        .expect("sum");
    assert_eq!(sum, Rational::ONE, "no floating point, no residue");
}

#[test]
fn division_by_zero_is_refused() {
    assert_eq!(
        Rational::ONE.checked_div(Rational::ZERO),
        Err(CoreStatus::DivideByZero)
    );
    assert_eq!(
        Rational::ZERO.checked_reciprocal(),
        Err(CoreStatus::DivideByZero)
    );
}

#[test]
fn overflow_is_refused_rather_than_wrapped() {
    let large = Rational::new(i64::MAX, 1).expect("a large integer");
    assert_eq!(large.checked_add(large), Err(CoreStatus::Overflow));
    assert_eq!(large.checked_mul(large), Err(CoreStatus::Overflow));
    assert_eq!(
        Rational::new(i64::MIN, 1)
            .expect("the smallest integer")
            .checked_neg(),
        Err(CoreStatus::Overflow)
    );
}

#[test]
fn comparison_does_not_overflow_on_large_denominators() {
    let left = Rational::new(i64::MAX, i64::MAX - 1).expect("just over one");
    let right = Rational::new(i64::MAX - 1, i64::MAX).expect("just under one");
    assert!(left > right);
    assert!(right < Rational::ONE);
    assert!(left > Rational::ONE);
}

#[test]
fn floor_rounds_toward_negative_infinity() {
    assert_eq!(
        Rational::new(7, 2).expect("three and a half").floor(),
        Ok(3)
    );
    assert_eq!(
        Rational::new(-7, 2)
            .expect("minus three and a half")
            .floor(),
        Ok(-4)
    );
    assert_eq!(Rational::from_integer(-4).floor(), Ok(-4));
}

#[test]
fn ntsc_rates_are_never_approximated() {
    let ntsc = Rational::new(24_000, 1001).expect("the NTSC film rate");
    assert_eq!(ntsc.numerator(), 24_000);
    assert_eq!(ntsc.denominator(), 1001);
    assert_eq!(
        ntsc.to_integer(),
        None,
        "it is not a whole number of frames"
    );

    // One hour of NTSC film frames is not a whole number of seconds, and the
    // exact answer says so rather than rounding it away.
    let hour_of_frames = Rational::from_integer(86_400);
    let seconds = hour_of_frames.checked_div(ntsc).expect("exact seconds");
    assert_eq!(
        seconds,
        Rational::new(86_400 * 1001, 24_000).expect("exact")
    );
    assert_eq!(seconds.to_integer(), None);
}

#[test]
fn display_shows_the_fraction() {
    extern crate std;
    assert_eq!(
        std::format!("{}", Rational::new(24_000, 1001).expect("rate")),
        "24000/1001"
    );
    assert_eq!(std::format!("{}", Rational::from_integer(25)), "25");
}
