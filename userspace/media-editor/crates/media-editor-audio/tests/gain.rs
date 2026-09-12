// SPDX-License-Identifier: GPL-3.0-only
//! Gain, against the definition of a decibel rather than against a table.

use media_editor_audio::{AudioStatus, Gain, MAXIMUM_DECIBELS, MINIMUM_DECIBELS};
use media_editor_core::{Fixed, Rational};

/// How close two factors must be, in units of the last bit of a [`Fixed`].
///
/// Four. The logarithm and the exponential inside `pow` work at forty-eight
/// fractional bits and round once, so a factor is right to within a bit or
/// two, and a product of two of them to within a few.
const TOLERANCE: i64 = 4;

fn close(left: Fixed, right: Fixed, what: &str) {
    let apart = (left.raw() - right.raw()).abs();
    assert!(
        apart <= TOLERANCE,
        "{what}: {} and {} are {apart} bits apart",
        left.raw(),
        right.raw()
    );
}

fn decibels(numerator: i64, denominator: i64) -> Gain {
    Gain::decibels(Rational::new(numerator, denominator).expect("a ratio")).expect("a gain")
}

#[test]
fn unity_is_exactly_one() {
    // Not nearly one. A fader at unity must pass its input through untouched,
    // and a mixer whose unity is a bit away from one colours every channel it
    // touches — compounding down a bus, where the error is applied again at
    // every stage.
    assert_eq!(Gain::UNITY.factor().expect("a factor"), Fixed::ONE);
    assert_eq!(
        Gain::whole_decibels(0)
            .expect("a gain")
            .factor()
            .expect("a factor"),
        Fixed::ONE
    );
    assert_eq!(
        Gain::whole_decibels(0).expect("a gain"),
        Gain::UNITY,
        "zero decibels is the unity detent, not a value near it"
    );
}

#[test]
fn the_general_path_reaches_unity_too() {
    // `Gain::factor` short-circuits at zero decibels rather than computing a
    // logarithm nobody needs. That shortcut must not be the reason unity is
    // exact, so this asserts the long way round: a gain a hair either side of
    // zero must approach one from both directions, and the smallest
    // representable step away from zero must still be within a bit of it.
    //
    // Removing the shortcut altogether was tried, and every test still passed
    // — which is the point. The arithmetic gets this right on its own, and
    // this test is what says so.
    // A billionth of a decibel is a factor of 10^(1e-9/20), which is
    // 1 + 1.1513e-10 — half of the last bit of a Fixed. So it must land on
    // unity or within a bit of it, from either side.
    close(
        decibels(1, 1_000_000_000).factor().expect("a factor"),
        Fixed::ONE,
        "a billionth of a decibel above unity",
    );
    close(
        decibels(-1, 1_000_000_000).factor().expect("a factor"),
        Fixed::ONE,
        "a billionth of a decibel below unity",
    );

    // And a millionth of a decibel is a thousand times further out, which by
    // the same derivation is 1 + 1.1513e-7, or 494.5 of the last bit. It has
    // to land there — close enough to unity that nothing hears it, far enough
    // that the exponential is demonstrably doing arithmetic rather than
    // returning one for everything small.
    let above = decibels(1, 1_000_000).factor().expect("a factor");
    let below = decibels(-1, 1_000_000).factor().expect("a factor");
    for (value, sign) in [(above, 1_i64), (below, -1)] {
        let offset = (value.raw() - Fixed::ONE.raw()) * sign;
        assert!(
            (490..=499).contains(&offset),
            "a millionth of a decibel should sit about 494 bits off unity; \
             it sits {offset}"
        );
    }
    assert!(
        above.raw() > Fixed::ONE.raw() && below.raw() < Fixed::ONE.raw(),
        "and on the right sides of it"
    );
}

#[test]
fn twenty_decibels_is_exactly_ten() {
    // The definition: a decibel of amplitude is 20 log10(factor), so twenty
    // decibels is a factor of ten. It is the cleanest available check that the
    // logarithm and the exponential are inverses of each other, and the quiet
    // side goes three decades down, which is well past where any rounding
    // would have had a chance to accumulate.
    close(
        Gain::whole_decibels(20)
            .expect("a gain")
            .factor()
            .expect("a factor"),
        Fixed::from_integer(10).expect("a value"),
        "plus twenty decibels",
    );
    for (db, divisor) in [(-20_i64, 10_i64), (-40, 100), (-60, 1000)] {
        close(
            Gain::whole_decibels(db)
                .expect("a gain")
                .factor()
                .expect("a factor"),
            Fixed::from_rational(Rational::new(1, divisor).expect("a ratio")).expect("a value"),
            "minus decibels",
        );
    }
}

#[test]
fn a_doubling_is_not_six_decibels_and_the_difference_is_measurable() {
    // A doubling is 20 log10(2) = 6.020599913... dB. Writing it as 6 is the
    // approximation everyone makes, and this test says what it costs rather
    // than repeating the folklore.
    //
    // Every number below is arrived at from the definition, not from running
    // the code. At nine decimals the decibel value is within 2.8e-10 dB of the
    // real one, which puts its factor within 0.28 of the last bit of a Fixed —
    // so it must land inside the ordinary arithmetic tolerance. At four
    // decimals it is 85.8 bits out, and at zero decimals it is out by a factor
    // of 1.0024, which is a fifth of a decibel per stage.
    let two = Fixed::from_integer(2).expect("a value");
    close(
        decibels(6_020_599_913, 1_000_000_000)
            .factor()
            .expect("a factor"),
        two,
        "6.020599913 dB",
    );

    let four_decimals = decibels(60_206, 10_000).factor().expect("a factor");
    let apart = (four_decimals.raw() - two.raw()).abs();
    assert!(
        (80..=92).contains(&apart),
        "6.0206 dB should sit about 86 bits from a doubling; it sits {apart}"
    );

    let whole = Gain::whole_decibels(6)
        .expect("a gain")
        .factor()
        .expect("a factor");
    let apart = (whole.raw() - two.raw()).abs();
    assert!(
        apart > 4_000_000,
        "6 dB is not a doubling by any tolerance this crate uses: {apart} bits"
    );
}

#[test]
fn faders_in_series_add_their_decibels_exactly() {
    // The whole reason the scale is logarithmic. A channel fader and a bus
    // fader are their *sum*, computed in exact rationals — rather than the
    // product of two factors that have each been through a logarithm and
    // picked up a bit of rounding on the way.
    let channel = decibels(-15, 2);
    let bus = decibels(9, 2);
    let combined = channel.then(bus).expect("a gain");
    assert_eq!(
        combined.value().expect("a value"),
        Rational::new(-3, 1).expect("a ratio"),
        "minus seven and a half plus four and a half is minus three, exactly"
    );

    // And the exact sum agrees with multiplying the factors, to within the
    // rounding that multiplying introduces and adding does not.
    let multiplied = channel
        .factor()
        .expect("a factor")
        .checked_mul(bus.factor().expect("a factor"))
        .expect("a product");
    close(
        combined.factor().expect("a factor"),
        multiplied,
        "in series",
    );
}

#[test]
fn silence_is_a_detent_rather_than_a_decibel_value() {
    // The logarithm of zero is not a number, so "off" cannot be written on the
    // decibel scale. Every real fader has a separate detent below its lowest
    // marking, and so does this — which also means silence has no inverse,
    // because nothing multiplied by zero comes back.
    assert!(Gain::SILENT.is_silent());
    assert_eq!(Gain::SILENT.value(), None);
    assert_eq!(Gain::SILENT.factor().expect("a factor"), Fixed::ZERO);
    assert_eq!(Gain::SILENT.inverse(), Err(AudioStatus::NoInverse));

    // Anything in series with silence is silence, whichever side it is on.
    let loud = Gain::whole_decibels(24).expect("a gain");
    assert!(Gain::SILENT.then(loud).expect("a gain").is_silent());
    assert!(loud.then(Gain::SILENT).expect("a gain").is_silent());

    assert!(
        !Gain::whole_decibels(MINIMUM_DECIBELS)
            .expect("a gain")
            .is_silent(),
        "the quietest marking on the scale is still a level, not off"
    );
}

#[test]
fn the_fader_has_ends_and_says_so() {
    assert!(Gain::whole_decibels(MINIMUM_DECIBELS).is_ok());
    assert!(Gain::whole_decibels(MAXIMUM_DECIBELS).is_ok());
    assert_eq!(
        Gain::whole_decibels(MINIMUM_DECIBELS - 1),
        Err(AudioStatus::GainOutOfRange)
    );
    assert_eq!(
        Gain::whole_decibels(MAXIMUM_DECIBELS + 1),
        Err(AudioStatus::GainOutOfRange)
    );

    // Two faders in series can leave the range, and that is refused rather
    // than saturated: a bus that quietly stopped adding gain would be lying
    // about where its fader is.
    let loud = Gain::whole_decibels(20).expect("a gain");
    assert_eq!(loud.then(loud), Err(AudioStatus::GainOutOfRange));
}

#[test]
fn a_gain_is_the_same_gain_every_time() {
    // Determinism, stated for the arithmetic that most invites drift.
    for db in [-96_i64, -60, -18, -3, 0, 3, 18, 24] {
        let first = Gain::whole_decibels(db)
            .expect("a gain")
            .factor()
            .expect("a factor");
        let second = Gain::whole_decibels(db)
            .expect("a gain")
            .factor()
            .expect("a factor");
        assert_eq!(first, second, "{db} dB");
    }
}

#[test]
fn the_scale_rises_everywhere() {
    // A fader that did not rise monotonically would have a dead spot, or worse
    // a place where turning it up turns it down. Checked in tenths of a
    // decibel across the whole travel.
    let mut previous = Gain::decibels(Rational::new(MINIMUM_DECIBELS, 1).expect("a ratio"))
        .expect("a gain")
        .factor()
        .expect("a factor");
    let mut step = MINIMUM_DECIBELS * 10 + 1;
    while step <= MAXIMUM_DECIBELS * 10 {
        let now = decibels(step, 10).factor().expect("a factor");
        assert!(
            now.raw() > previous.raw(),
            "the fader does not rise at {step} tenths of a decibel"
        );
        previous = now;
        step += 1;
    }
}
