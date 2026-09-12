// SPDX-License-Identifier: GPL-3.0-only
//! Panning, against the law it claims to obey.

use media_editor_audio::{AudioStatus, Pan};
use media_editor_core::{Fixed, Rational};

fn at(numerator: i64, denominator: i64) -> Pan {
    Pan::new(Rational::new(numerator, denominator).expect("a ratio")).expect("a position")
}

/// `left² + right²`, which the law says must be one.
fn power(pan: Pan) -> Fixed {
    let (left, right) = pan.gains().expect("gains");
    left.checked_mul(left)
        .expect("a square")
        .checked_add(right.checked_mul(right).expect("a square"))
        .expect("a sum")
}

#[test]
fn the_power_is_constant_all_the_way_across() {
    // The defining property, and the whole reason the law exists: moving a
    // source across the image must not change how loud it is. Checked at every
    // hundredth of the travel rather than at the three positions anybody
    // remembers to check.
    //
    // The tolerance is three bits, and it is the arithmetic's rather than the
    // law's — worked out rather than tuned. Each square root rounds by up to
    // half a bit; squaring a value near one roughly doubles that error, so a
    // bit each; each multiply rounds again by up to half a bit; and the two
    // are added. One and a half per side, three in total. Measured over two
    // thousand positions the worst case is exactly three, so the bound is
    // tight rather than generous.
    for step in -1000..=1000 {
        let apart = (power(at(step, 1000)).raw() - Fixed::ONE.raw()).abs();
        assert!(
            apart <= 3,
            "at {step}/1000 the power is {apart} bits away from unity"
        );
    }
}

#[test]
fn hard_left_and_hard_right_are_all_of_it_and_none_of_it() {
    let (left, right) = Pan::left().expect("a position").gains().expect("gains");
    assert_eq!(left, Fixed::ONE, "hard left sends everything left");
    assert_eq!(right, Fixed::ZERO, "and nothing right");

    let (left, right) = Pan::right().expect("a position").gains().expect("gains");
    assert_eq!(left, Fixed::ZERO);
    assert_eq!(right, Fixed::ONE);
}

#[test]
fn the_centre_is_three_decibels_down_and_not_zero_or_six() {
    // The pan law question, answered. Send full level to both speakers and the
    // centre is 3 dB louder than the sides, because two speakers carrying the
    // same signal sum in pressure. Send half to each and it is 6 dB quieter in
    // power. Constant power sends the square root of a half to each — about
    // 0.7071 — which is the "3 dB pan law" every console has a switch for.
    let (left, right) = Pan::CENTRE.gains().expect("gains");
    assert_eq!(left, right, "centre is symmetric");

    let root_half = Fixed::from_rational(Rational::new(1, 2).expect("a ratio"))
        .expect("a value")
        .sqrt()
        .expect("a root");
    assert_eq!(left, root_half);

    // And it is neither of the two wrong answers, by a wide margin.
    assert!(
        (left.raw() - Fixed::ONE.raw()).abs() > 1_000_000_000,
        "the centre is not full level"
    );
    let half = Fixed::from_rational(Rational::new(1, 2).expect("a ratio")).expect("a value");
    assert!(
        (left.raw() - half.raw()).abs() > 800_000_000,
        "nor is it half level"
    );
}

#[test]
fn the_image_is_symmetric() {
    // A source at a position and the same source mirrored must be mirror
    // images of each other, exactly. An asymmetric pan law pulls a whole mix
    // to one side and nobody can say why.
    for step in 0..=100 {
        let one = at(step, 100);
        let other = one.mirrored().expect("a mirror");
        let (left, right) = one.gains().expect("gains");
        let (mirrored_left, mirrored_right) = other.gains().expect("gains");
        assert_eq!(left, mirrored_right, "at {step}/100");
        assert_eq!(right, mirrored_left, "at {step}/100");
    }
}

#[test]
fn the_gains_move_the_right_way() {
    // Left falls and right rises as a source travels left to right, without a
    // dead spot or a reversal anywhere.
    let (mut previous_left, mut previous_right) =
        Pan::left().expect("a position").gains().expect("gains");
    for step in -99..=100 {
        let (left, right) = at(step, 100).gains().expect("gains");
        assert!(left.raw() < previous_left.raw(), "left at {step}/100");
        assert!(right.raw() > previous_right.raw(), "right at {step}/100");
        previous_left = left;
        previous_right = right;
    }
}

#[test]
fn the_image_has_edges() {
    assert_eq!(
        Pan::new(Rational::new(101, 100).expect("a ratio")),
        Err(AudioStatus::PanOutOfRange)
    );
    assert_eq!(
        Pan::new(Rational::new(-101, 100).expect("a ratio")),
        Err(AudioStatus::PanOutOfRange)
    );
    assert!(Pan::new(Rational::ONE).is_ok());
}

#[test]
fn a_position_is_the_same_position_every_time() {
    for step in [-100_i64, -37, 0, 37, 100] {
        assert_eq!(
            at(step, 100).gains().expect("gains"),
            at(step, 100).gains().expect("gains")
        );
    }
}
