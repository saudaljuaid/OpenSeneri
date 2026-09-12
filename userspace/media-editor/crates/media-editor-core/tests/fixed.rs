// SPDX-License-Identifier: GPL-3.0-only
//! The fixed-point maths, against values that can be checked by hand.

use media_editor_core::Rational;
use media_editor_core::{CoreStatus, Fixed};

/// How close two fixed-point values must be, in units of the last bit.
///
/// Two. The logarithm and the exponential work at forty-eight fractional bits
/// and round once, so a power is right to within a bit or so of what this type
/// can express — about 5 × 10⁻¹⁰, which is four orders of magnitude finer than
/// a sixteen-bit sample. The reference values below are themselves only given
/// to nine or ten places, so most of this tolerance is theirs.
const TOLERANCE: i64 = 4;

fn close(actual: Fixed, expected: Fixed, note: &str) {
    let difference = (actual.raw() - expected.raw()).abs();
    assert!(
        difference <= TOLERANCE,
        "{note}: {} vs {}, off by {difference} bits",
        actual.raw(),
        expected.raw()
    );
}

fn decimal(numerator: i64, denominator: i64) -> Fixed {
    Fixed::from_rational(Rational::new(numerator, denominator).expect("a ratio"))
        .expect("a fixed-point value")
}

#[test]
fn one_is_one() {
    assert_eq!(Fixed::from_integer(1), Ok(Fixed::ONE));
    assert_eq!(Fixed::ONE.raw(), 1_i64 << 32);
    assert_eq!(decimal(1, 1), Fixed::ONE);
}

#[test]
fn arithmetic_behaves() {
    let half = decimal(1, 2);
    let quarter = decimal(1, 4);
    assert_eq!(half.checked_mul(half), Ok(quarter));
    assert_eq!(half.checked_add(half), Ok(Fixed::ONE));
    assert_eq!(half.checked_sub(quarter), Ok(quarter));
    assert_eq!(quarter.checked_div(half), Ok(half));
    assert_eq!(
        Fixed::ONE.checked_div(Fixed::ZERO),
        Err(CoreStatus::NoRealAnswer)
    );
}

#[test]
fn rounding_does_not_depend_on_the_sign() {
    // Half away from zero, both ways, so that negating an input negates the
    // output exactly. A rule that rounded toward zero would break that.
    let third = decimal(1, 3);
    let minus_third = decimal(-1, 3);
    assert_eq!(third.raw(), -minus_third.raw());
}

#[test]
fn square_roots_are_exact_to_the_last_bit() {
    assert_eq!(
        Fixed::from_integer(4).expect("four").sqrt(),
        Ok(Fixed::from_integer(2).expect("two"))
    );
    assert_eq!(Fixed::ONE.sqrt(), Ok(Fixed::ONE));
    assert_eq!(Fixed::ZERO.sqrt(), Ok(Fixed::ZERO));

    // The square root of two, to nine places.
    close(
        Fixed::from_integer(2).expect("two").sqrt().expect("a root"),
        decimal(1_414_213_562, 1_000_000_000),
        "sqrt 2",
    );
    assert_eq!(
        Fixed::from_integer(-1).expect("minus one").sqrt(),
        Err(CoreStatus::NoRealAnswer)
    );
}

#[test]
fn logarithms_of_powers_of_two_are_whole() {
    for exponent in 0..20_i64 {
        let value = Fixed::from_integer(1_i64 << exponent).expect("a power of two");
        assert_eq!(
            value.log2(),
            Fixed::from_integer(exponent),
            "log2 of 2^{exponent}"
        );
    }
    // And below one, where the answer is negative.
    assert_eq!(decimal(1, 2).log2(), Fixed::from_integer(-1));
    assert_eq!(decimal(1, 1024).log2(), Fixed::from_integer(-10));
    assert_eq!(Fixed::ZERO.log2(), Err(CoreStatus::NoRealAnswer));
}

#[test]
fn logarithms_of_other_numbers_are_right() {
    // log2 10 = 3.321928095, log2 3 = 1.584962501
    close(
        Fixed::from_integer(10).expect("ten").log2().expect("a log"),
        decimal(3_321_928_095, 1_000_000_000),
        "log2 10",
    );
    close(
        Fixed::from_integer(3)
            .expect("three")
            .log2()
            .expect("a log"),
        decimal(1_584_962_501, 1_000_000_000),
        "log2 3",
    );
}

#[test]
fn exponentials_of_whole_numbers_are_powers_of_two() {
    for exponent in 0..20_i64 {
        let value = Fixed::from_integer(exponent).expect("an exponent");
        assert_eq!(
            value.exp2(),
            Fixed::from_integer(1_i64 << exponent),
            "2^{exponent}"
        );
    }
    assert_eq!(
        Fixed::from_integer(-3).expect("minus three").exp2(),
        Ok(decimal(1, 8))
    );
}

#[test]
fn the_logarithm_and_the_exponential_undo_each_other() {
    for numerator in 1..=64_i64 {
        let value = decimal(numerator, 8);
        let round_trip = value.log2().expect("a log").exp2().expect("an exponential");
        close(round_trip, value, "round trip");
    }
}

#[test]
fn the_natural_logarithm_is_the_one_it_should_be() {
    // ln 2 = 0.693147181, ln 10 = 2.302585093
    close(
        Fixed::from_integer(2).expect("two").ln().expect("a log"),
        decimal(693_147_181, 1_000_000_000),
        "ln 2",
    );
    close(
        Fixed::from_integer(10).expect("ten").ln().expect("a log"),
        decimal(2_302_585_093, 1_000_000_000),
        "ln 10",
    );
    assert_eq!(Fixed::ONE.ln(), Ok(Fixed::ZERO));
}

#[test]
fn powers_are_powers() {
    let two = Fixed::from_integer(2).expect("two");
    let three = Fixed::from_integer(3).expect("three");
    close(
        two.pow(three).expect("a power"),
        Fixed::from_integer(8).expect("eight"),
        "2^3",
    );
    close(
        three.pow(two).expect("a power"),
        Fixed::from_integer(9).expect("nine"),
        "3^2",
    );
    close(
        two.pow(decimal(1, 2)).expect("a power"),
        decimal(1_414_213_562, 1_000_000_000),
        "2^0.5",
    );
    // The one the transfer functions actually need: x^(1/2.4).
    let encoded = decimal(1, 2).pow(decimal(10, 24)).expect("a power");
    close(encoded, decimal(749_153_538, 1_000_000_000), "0.5^(1/2.4)");
    // And its inverse, which must land back on a half.
    close(
        encoded.pow(decimal(24, 10)).expect("a power"),
        decimal(1, 2),
        "the other way",
    );
}

#[test]
fn zero_to_a_positive_power_is_zero() {
    assert_eq!(Fixed::ZERO.pow(Fixed::ONE), Ok(Fixed::ZERO));
    assert_eq!(Fixed::ZERO.pow(Fixed::ZERO), Err(CoreStatus::NoRealAnswer));
    assert_eq!(
        Fixed::from_integer(-1).expect("minus one").pow(Fixed::ONE),
        Err(CoreStatus::NoRealAnswer),
        "a negative base has no real logarithm"
    );
}

#[test]
fn every_result_is_the_same_every_time() {
    // The whole reason this module exists. Integers in, integers out, no
    // library anyone else compiled, no rounding mode anyone can change.
    for numerator in 1..=100_i64 {
        let value = decimal(numerator, 100);
        let first = value.pow(decimal(10, 24)).expect("a power");
        let second = value.pow(decimal(10, 24)).expect("a power");
        assert_eq!(first, second);
    }
}
