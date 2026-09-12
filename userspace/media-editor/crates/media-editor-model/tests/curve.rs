// SPDX-License-Identifier: GPL-3.0-only
//! What a parameter that changes over time is allowed to claim.
//!
//! Every expected value here was worked out by hand from the Bézier's own
//! formula and is written in the test as the fraction it is. A test that
//! asserted whatever the code returned would pass for any code.

use media_editor_core::{Instant, Rational, Timebase};
use media_editor_model::ModelStatus;
use media_editor_model::curve::{Curve, EASE_BITS, Interpolation, Keyframe, MAX_KEYFRAMES};

const RATE: Timebase = Timebase::FILM_24;

fn at(frame: i64) -> Instant {
    Instant::new(frame, RATE)
}

fn value(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a value")
}

fn key(frame: i64, numerator: i64, denominator: i64, how: Interpolation) -> Keyframe {
    Keyframe::new(at(frame), value(numerator, denominator), how).expect("a keyframe")
}

/// A curve from nought to one over `span` frames, with one interpolation.
fn ramp(span: i64, how: Interpolation) -> Curve {
    Curve::new(std::vec![
        key(0, 0, 1, how),
        key(span, 1, 1, Interpolation::Hold),
    ])
    .expect("a curve")
}

#[test]
fn a_curve_passes_through_its_own_keyframes() {
    // The one thing a curve is for. A curve that did not give back exactly the
    // value somebody set, at the instant they set it, would make the numbers
    // in the interface a suggestion — and every interpolation has to manage
    // it, not just the simple ones.
    let eased = Interpolation::ease_in_out().expect("an ease");
    for how in [Interpolation::Hold, Interpolation::Linear, eased] {
        let curve = Curve::new(std::vec![
            key(0, 1, 4, how),
            key(10, 3, 4, how),
            key(24, 1, 8, Interpolation::Hold),
        ])
        .expect("a curve");
        for keyframe in curve.keyframes() {
            assert_eq!(
                curve.value_at(keyframe.at()).expect("a value"),
                keyframe.value(),
                "a curve missed its own keyframe with {how:?}"
            );
        }
    }
}

#[test]
fn a_curve_holds_at_both_ends_rather_than_continuing() {
    // Extrapolating past the last key is how a parameter set to reach 100% at
    // the end of a shot arrives at 340% two shots later. The editor who set
    // two keyframes described what happens between them and nothing else.
    let curve = ramp(24, Interpolation::Linear);
    for frame in [-1000, -24, -1, 0] {
        assert_eq!(curve.value_at(at(frame)).expect("a value"), value(0, 1));
    }
    for frame in [24, 25, 48, 100_000] {
        assert_eq!(curve.value_at(at(frame)).expect("a value"), value(1, 1));
    }
}

#[test]
fn a_constant_curve_is_that_value_at_every_instant() {
    // What a parameter is before anybody animates it, and the reason there is
    // no empty curve: a parameter always has a value.
    let curve = Curve::constant(at(12), value(3, 5)).expect("a curve");
    for frame in [-100, 0, 11, 12, 13, 1000] {
        assert_eq!(curve.value_at(at(frame)).expect("a value"), value(3, 5));
    }
}

#[test]
fn a_linear_ramp_is_exactly_linear() {
    // Not "close to": a rational fraction of a rational change is a rational,
    // and nothing here rounds. A twenty-four frame ramp from nought to one is
    // n/24 at frame n, on the nose, including the thirds and sevenths that a
    // binary fraction cannot hold.
    let curve = ramp(24, Interpolation::Linear);
    for frame in 0..=24 {
        assert_eq!(
            curve.value_at(at(frame)).expect("a value"),
            value(frame, 24),
            "frame {frame} of a linear ramp"
        );
    }
}

#[test]
fn a_hold_keeps_its_value_until_the_next_key_and_then_jumps() {
    // A blend mode or a two-state switch has no meaningful halfway, and an
    // editor who chose hold chose it because a ramp would be wrong.
    let curve = Curve::new(std::vec![
        key(0, 0, 1, Interpolation::Hold),
        key(10, 1, 1, Interpolation::Hold),
    ])
    .expect("a curve");
    for frame in 0..10 {
        assert_eq!(curve.value_at(at(frame)).expect("a value"), value(0, 1));
    }
    assert_eq!(curve.value_at(at(10)).expect("a value"), value(1, 1));
}

#[test]
fn the_default_ease_costs_no_inversion_at_all() {
    // Worth knowing and worth asserting. With the handles at a third and two
    // thirds, the *horizontal* Bézier reduces to `x(t) = t` identically:
    //
    //   3(1-t)²t(1/3) + 3(1-t)t²(2/3) + t³
    //     = (1-2t+t²)t + 2t² - 2t³ + t³
    //     = t - 2t² + t³ + 2t² - t³
    //     = t
    //
    // so the bisection lands on the exact parameter and the only rounding left
    // is the one at the end. The vertical is
    // `y(t) = 3(1-t)t² + t³`, which at a quarter is
    // `3·(3/4)·(1/16) + 1/64 = 9/64 + 1/64 = 5/32` — and 5/32 is a multiple of
    // 2^-20, so even the final rounding is exact here.
    let curve = ramp(24, Interpolation::ease_in_out().expect("an ease"));
    assert_eq!(curve.value_at(at(6)).expect("a value"), value(5, 32));
    assert_eq!(curve.value_at(at(12)).expect("a value"), value(1, 2));
    assert_eq!(curve.value_at(at(18)).expect("a value"), value(27, 32));

    // An ease is not a straight line, which is the entire point of having one.
    assert_ne!(curve.value_at(at(6)).expect("a value"), value(1, 4));
}

#[test]
fn an_ease_with_a_bent_horizontal_inverts_to_the_right_parameter() {
    // The case the bisection exists for. With the outgoing handle at a half
    // and the incoming at one:
    //
    //   x(t) = 3(1-t)²t(1/2) + 3(1-t)t²(1) + t³
    //   x(1/2) = 3·(1/4)·(1/2)·(1/2) + 3·(1/2)·(1/4) + 1/8
    //          = 3/16 + 6/16 + 2/16 = 11/16
    //
    // so eleven sixteenths of the way along the span is where `t = 1/2` lives.
    // With the vertical handles at nought and one,
    // `y(1/2) = 3·(1/2)·(1/4)·1 + 1/8 = 1/2`.
    //
    // Sixteen frames, asked at frame eleven: the answer must be exactly a
    // half. A linear ramp would say eleven sixteenths, so this fails if the
    // inversion is skipped, and it fails differently if the inversion is
    // wrong.
    let bent = Interpolation::Ease {
        out_x: value(1, 2),
        out_y: value(0, 1),
        in_x: value(1, 1),
        in_y: value(1, 1),
    };
    let curve = ramp(16, bent);
    assert_eq!(curve.value_at(at(11)).expect("a value"), value(1, 2));
    assert_ne!(curve.value_at(at(11)).expect("a value"), value(11, 16));
}

#[test]
fn a_handle_above_the_top_overshoots_on_purpose() {
    // An overshoot is a thing an editor asks for — a push that goes a little
    // past and settles — so the vertical handles are not clamped, only the
    // horizontal ones. With the incoming handle at two,
    // `y(t) = 6(1-t)t² + t³`, and at three quarters that is
    // `6·(1/4)·(9/16) + 27/64 = 54/64 + 27/64 = 81/64`, which is past one and
    // is meant to be.
    let over = Interpolation::Ease {
        out_x: value(1, 3),
        out_y: value(0, 1),
        in_x: value(2, 3),
        in_y: value(2, 1),
    };
    let curve = ramp(24, over);
    assert_eq!(curve.value_at(at(18)).expect("a value"), value(81, 64));
    assert!(curve.value_at(at(18)).expect("a value") > value(1, 1));

    // And it still arrives exactly where it was told to.
    assert_eq!(curve.value_at(at(24)).expect("a value"), value(1, 1));
}

#[test]
fn an_ease_never_goes_backwards() {
    // Monotone in, monotone out. A fade that stepped back at one frame and
    // nowhere else is the hardest kind of fault to find by looking, so it is
    // asserted rather than assumed — over a bent horizontal, where the
    // bisection is doing real work.
    let bent = Interpolation::Ease {
        out_x: value(1, 2),
        out_y: value(0, 1),
        in_x: value(1, 1),
        in_y: value(1, 1),
    };
    let curve = ramp(200, bent);
    let mut previous = curve.value_at(at(-5)).expect("a value");
    for frame in -5..=205 {
        let now = curve.value_at(at(frame)).expect("a value");
        assert!(
            now >= previous,
            "frame {frame} went backwards, from {previous:?} to {now:?}"
        );
        previous = now;
    }
    assert_eq!(previous, value(1, 1), "it did not arrive");
}

#[test]
fn a_curve_gives_the_same_answer_every_time() {
    // The bisection runs a fixed number of passes and compares by
    // cross-multiplication, so there is no place for two machines to
    // disagree — and no place for the same machine to disagree with itself
    // depending on what it was asked first.
    let bent = Interpolation::Ease {
        out_x: value(1, 5),
        out_y: value(1, 10),
        in_x: value(4, 5),
        in_y: value(9, 10),
    };
    let curve = ramp(97, bent);
    let forwards: std::vec::Vec<Rational> = (0..=97)
        .map(|f| curve.value_at(at(f)).expect("a value"))
        .collect();
    let backwards: std::vec::Vec<Rational> = (0..=97)
        .rev()
        .map(|f| curve.value_at(at(f)).expect("a value"))
        .collect();
    let mut reversed = backwards;
    reversed.reverse();
    assert_eq!(forwards, reversed);

    // And the same curve built again answers the same.
    assert_eq!(
        forwards,
        (0..=97)
            .map(|f| ramp(97, bent).value_at(at(f)).expect("a value"))
            .collect::<std::vec::Vec<_>>()
    );
}

#[test]
fn the_ease_fraction_lands_on_a_multiple_of_its_own_precision() {
    // The rounding at the end is stated, so it can be checked: every eased
    // fraction is a multiple of 2^-EASE_BITS. On a ramp from nought to one the
    // value *is* the fraction, so a denominator that does not divide the
    // precision means something rounded somewhere it did not say it would.
    let bent = Interpolation::Ease {
        out_x: value(1, 5),
        out_y: value(1, 10),
        in_x: value(4, 5),
        in_y: value(9, 10),
    };
    let curve = ramp(97, bent);
    let precision = 1_i64 << EASE_BITS;
    for frame in 0..=97 {
        let held = curve.value_at(at(frame)).expect("a value");
        assert_eq!(
            precision % held.denominator(),
            0,
            "frame {frame} came back as {held:?}, which is not a multiple of 2^-{EASE_BITS}"
        );
    }
}

#[test]
fn the_ease_rounds_to_the_nearest_and_not_towards_nought() {
    // This test exists because a mutation that rounded towards nought failed
    // nothing. The rounding rule was stated in the code and pinned by no test,
    // which is a rule that will change one day without anybody noticing.
    //
    // The case is chosen so both the parameter and the exact value are known.
    // With the horizontal handles at a third and two thirds, `x(t) = t`, so
    // frame 1 of 4 is `t = 1/4` on the nose. With the vertical handles at a
    // fifth and nought:
    //
    //   y(1/4) = 3·(3/4)²·(1/4)·(1/5) + 0 + (1/4)³
    //          = 27/320 + 5/320 = 32/320 = 1/10
    //
    // A tenth is not a multiple of 2^-20: it is 104857.6 of them. Nearest is
    // 104858 and towards nought is 104857, so the two rules give different
    // answers and this asserts which one is in force.
    let tenth = Interpolation::Ease {
        out_x: value(1, 3),
        out_y: value(1, 5),
        in_x: value(2, 3),
        in_y: value(0, 1),
    };
    let curve = ramp(4, tenth);
    assert_eq!(
        curve.value_at(at(1)).expect("a value"),
        value(104_858, 1 << EASE_BITS)
    );
    assert_ne!(
        curve.value_at(at(1)).expect("a value"),
        value(104_857, 1 << EASE_BITS)
    );
}

#[test]
fn keyframes_that_do_not_run_forward_are_refused() {
    // Sorting them would be deciding the caller meant something other than
    // what it said, and two at one instant have no order to sort into: a
    // parameter with two values at one moment has none.
    assert_eq!(
        Curve::new(std::vec![
            key(10, 0, 1, Interpolation::Linear),
            key(5, 1, 1, Interpolation::Linear),
        ])
        .map(|_| ()),
        Err(ModelStatus::KeyframesOutOfOrder)
    );
    assert_eq!(
        Curve::new(std::vec![
            key(5, 0, 1, Interpolation::Linear),
            key(5, 1, 1, Interpolation::Linear),
        ])
        .map(|_| ()),
        Err(ModelStatus::KeyframesOutOfOrder)
    );
}

#[test]
fn a_curve_with_nothing_in_it_is_refused() {
    assert_eq!(
        Curve::new(std::vec::Vec::new()).map(|_| ()),
        Err(ModelStatus::EmptyCurve)
    );
}

#[test]
fn keyframes_counted_two_ways_are_refused() {
    // Frame 12 at 24 and frame 12 at 25 are different moments, and a curve
    // that held both would be measuring its own span in two units.
    let other = Instant::new(12, Timebase::PAL_25);
    assert_eq!(
        Curve::new(std::vec![
            key(0, 0, 1, Interpolation::Linear),
            Keyframe::new(other, value(1, 1), Interpolation::Hold).expect("a keyframe"),
        ])
        .map(|_| ()),
        Err(ModelStatus::MixedTimebases)
    );

    // And asking a curve about an instant it does not count in is refused too,
    // rather than being read as a tick number that happens to be in range.
    let curve = ramp(24, Interpolation::Linear);
    assert_eq!(
        curve
            .value_at(Instant::new(12, Timebase::PAL_25))
            .map(|_| ()),
        Err(ModelStatus::WrongTimebase)
    );
}

#[test]
fn a_handle_outside_the_span_is_refused() {
    // A horizontal handle outside nought to one makes x(t) fold back on
    // itself, and "the value at this instant" stops having one answer.
    // Clamping would silently draw a different curve from the one that was
    // dragged.
    for (out_x, in_x) in [
        (value(-1, 10), value(1, 2)),
        (value(1, 2), value(11, 10)),
        (value(3, 1), value(1, 2)),
    ] {
        let bad = Interpolation::Ease {
            out_x,
            out_y: value(0, 1),
            in_x,
            in_y: value(1, 1),
        };
        assert_eq!(bad.check(), Err(ModelStatus::HandleOutOfSpan));
        assert_eq!(
            Keyframe::new(at(0), value(0, 1), bad).map(|_| ()),
            Err(ModelStatus::HandleOutOfSpan)
        );
    }

    // The ends themselves are inside, and a vertical outside is fine.
    let edge = Interpolation::Ease {
        out_x: value(0, 1),
        out_y: value(-1, 2),
        in_x: value(1, 1),
        in_y: value(3, 2),
    };
    assert_eq!(edge.check(), Ok(()));
}

#[test]
fn more_keyframes_than_the_policy_allows_are_refused() {
    let mut held = std::vec::Vec::new();
    for frame in 0..=i64::try_from(MAX_KEYFRAMES).expect("a bound") {
        held.push(key(frame, 0, 1, Interpolation::Linear));
    }
    assert_eq!(
        Curve::new(held).map(|_| ()),
        Err(ModelStatus::CapacityExhausted)
    );
}

#[test]
fn arithmetic_that_will_not_fit_is_refused_rather_than_wrapped() {
    // The ease works in 128-bit integers and still has a ceiling. A handle
    // written over two to the fortieth puts the common denominator past it,
    // and that is a refusal — because a curve that wrapped would draw a fade
    // that leapt backwards at one frame and nowhere else.
    let vast = Interpolation::Ease {
        out_x: value(1, 1 << 40),
        out_y: value(0, 1),
        in_x: value(1, 1 << 40),
        in_y: value(1, 1),
    };
    assert_eq!(vast.check(), Ok(()), "the handles are inside the span");
    let curve = ramp(24, vast);
    assert_eq!(
        curve.value_at(at(12)).map(|_| ()),
        Err(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))
    );
}

#[test]
fn a_curve_finds_the_right_pair_among_many() {
    // The search for which two keyframes an instant falls between is a
    // bisection too, and an off-by-one in it would read the value from the
    // wrong segment — which looks like a curve with the right shape in the
    // wrong place.
    let mut held = std::vec::Vec::new();
    for index in 0..50 {
        held.push(key(index * 3, index, 100, Interpolation::Hold));
    }
    let curve = Curve::new(held).expect("a curve");
    for index in 0..50 {
        for offset in 0..3 {
            let frame = index * 3 + offset;
            if frame > 49 * 3 {
                continue;
            }
            assert_eq!(
                curve.value_at(at(frame)).expect("a value"),
                value(index, 100),
                "frame {frame} read from the wrong segment"
            );
        }
    }
}
