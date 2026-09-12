// SPDX-License-Identifier: GPL-3.0-only
//! Transfer functions, against the values their standards define.
//!
//! Each reference below is the standard's own formula evaluated to nine
//! places. The tolerance is what the fixed-point arithmetic can deliver, not a
//! number chosen to make a test pass — and it is four orders of magnitude
//! finer than a sixteen-bit sample, so nothing here rounds differently from an
//! exact implementation once it reaches a pixel.

use media_editor_core::Rational;
use media_editor_media::TransferFunction;
use media_editor_render::{Fixed, RenderStatus, decode, encode};

/// How close a result must be, in units of the last fixed-point bit.
///
/// A thousand parts in 2³², which is 2.3 × 10⁻⁷: below what a twenty-bit
/// sample can express, and far below the nine places the references are given
/// to. The powers in these curves compound the logarithm's error, which is why
/// this is looser than the tolerance on [`Fixed`] itself.
const TOLERANCE: i64 = 1000;

fn value(numerator: i64, denominator: i64) -> Fixed {
    Fixed::from_rational(Rational::new(numerator, denominator).expect("a ratio"))
        .expect("a fixed-point value")
}

/// A reference given to nine decimal places.
fn reference(billionths: i64) -> Fixed {
    value(billionths, 1_000_000_000)
}

fn close(actual: Fixed, expected: Fixed, note: &str) {
    let difference = (actual.raw() - expected.raw()).abs();
    assert!(
        difference <= TOLERANCE,
        "{note}: {} vs {}, off by {difference} bits",
        actual.raw(),
        expected.raw()
    );
}

const ALL: [TransferFunction; 8] = [
    TransferFunction::Linear,
    TransferFunction::Srgb,
    TransferFunction::Bt709,
    TransferFunction::Bt2020Ten,
    TransferFunction::Gamma22,
    TransferFunction::Gamma26,
    TransferFunction::PerceptualQuantiser,
    TransferFunction::HybridLogGamma,
];

#[test]
fn srgb_matches_iec_61966() {
    let cases = [
        (0, 0),
        (reference(1_000_000).raw(), 12_920_000),
        (value(18, 1000).raw(), 142_825_681),
        (value(1, 10).raw(), 349_190_213),
        (value(1, 2).raw(), 735_356_983),
        (Fixed::ONE.raw(), 1_000_000_000),
    ];
    for (input, expected) in cases {
        close(
            encode(TransferFunction::Srgb, Fixed::from_raw(input)).expect("an encoding"),
            reference(expected),
            "sRGB",
        );
    }
}

#[test]
fn bt709_matches_the_recommendation() {
    let cases = [
        (0, 0),
        (reference(1_000_000).raw(), 4_500_000),
        (value(18, 1000).raw(), 81_247_944),
        (value(1, 10).raw(), 290_939_915),
        (value(1, 2).raw(), 705_515_090),
        (Fixed::ONE.raw(), 1_000_000_000),
    ];
    for (input, expected) in cases {
        close(
            encode(TransferFunction::Bt709, Fixed::from_raw(input)).expect("an encoding"),
            reference(expected),
            "BT.709",
        );
    }
}

#[test]
fn bt2020_at_ten_bits_is_bt709() {
    // The recommendation says so, and an implementation that quietly used
    // different constants would be a difference nobody could see until an
    // export was graded against a reference monitor.
    for step in 0..=20_i64 {
        let input = value(step, 20);
        assert_eq!(
            encode(TransferFunction::Bt2020Ten, input),
            encode(TransferFunction::Bt709, input),
            "at {step}/20"
        );
    }
}

#[test]
fn the_perceptual_quantiser_matches_st_2084() {
    let cases = [
        (value(1, 1000).raw(), 299_699_092),
        (value(18, 1000).raw(), 568_156_710),
        (value(1, 10).raw(), 751_827_096),
        (value(1, 2).raw(), 926_546_704),
        (Fixed::ONE.raw(), 1_000_000_000),
    ];
    for (input, expected) in cases {
        close(
            encode(
                TransferFunction::PerceptualQuantiser,
                Fixed::from_raw(input),
            )
            .expect("an encoding"),
            reference(expected),
            "PQ",
        );
    }

    // The number a colourist actually uses: a hundred candelas, which is one
    // hundredth of the ten thousand the curve is normalised to, encodes to a
    // little over half.
    close(
        encode(TransferFunction::PerceptualQuantiser, value(1, 100)).expect("an encoding"),
        reference(508_078_422),
        "PQ at 100 nits",
    );
}

#[test]
fn hybrid_log_gamma_matches_bt2100() {
    let cases = [
        (0, 0),
        (value(1, 1000).raw(), 54_772_256),
        (value(18, 1000).raw(), 232_379_001),
        (value(1, 10).raw(), 544_089_494),
        (value(1, 2).raw(), 871_643_471),
    ];
    for (input, expected) in cases {
        close(
            encode(TransferFunction::HybridLogGamma, Fixed::from_raw(input)).expect("an encoding"),
            reference(expected),
            "HLG",
        );
    }
    // The curve reaches one at one, to the precision its published constants
    // define it to.
    close(
        encode(TransferFunction::HybridLogGamma, Fixed::ONE).expect("an encoding"),
        Fixed::ONE,
        "HLG at one",
    );
}

#[test]
fn a_pure_gamma_is_a_pure_gamma() {
    close(
        encode(TransferFunction::Gamma22, value(1, 2)).expect("an encoding"),
        reference(729_740_053),
        "gamma 2.2",
    );
    close(
        decode(TransferFunction::Gamma22, reference(729_740_053)).expect("a decoding"),
        value(1, 2),
        "gamma 2.2 back",
    );
}

#[test]
fn black_stays_black_and_white_stays_white() {
    for transfer in ALL {
        if transfer == TransferFunction::PerceptualQuantiser {
            // The perceptual quantiser does not reach zero at zero: its
            // published formula gives about seven parts in ten million, which
            // is a property of the standard rather than of this
            // implementation.
            continue;
        }
        assert_eq!(
            encode(transfer, Fixed::ZERO),
            Ok(Fixed::ZERO),
            "{transfer:?} at zero"
        );
        close(
            encode(transfer, Fixed::ONE).expect("an encoding"),
            Fixed::ONE,
            "at one",
        );
    }
}

#[test]
fn every_curve_undoes_itself() {
    for transfer in ALL {
        for step in 1..=40_i64 {
            let light = value(step, 40);
            let encoded = encode(transfer, light).expect("an encoding");
            let back = decode(transfer, encoded).expect("a decoding");
            close(back, light, "a round trip");
        }
    }
}

#[test]
fn every_curve_rises() {
    // A transfer function that went backwards anywhere would make a gradient
    // band, and the band would be blamed on the codec.
    for transfer in ALL {
        let mut previous = encode(transfer, Fixed::ZERO).expect("an encoding");
        for step in 1..=200_i64 {
            let current = encode(transfer, value(step, 200)).expect("an encoding");
            assert!(
                current >= previous,
                "{transfer:?} fell between {} and {step} two-hundredths",
                step - 1
            );
            previous = current;
        }
    }
}

#[test]
fn negative_light_is_refused_rather_than_clamped() {
    for transfer in ALL {
        assert_eq!(
            encode(transfer, Fixed::from_raw(-1)),
            Err(RenderStatus::OutsideDomain),
            "{transfer:?}"
        );
        assert_eq!(
            decode(transfer, Fixed::from_raw(-1)),
            Err(RenderStatus::OutsideDomain),
            "{transfer:?}"
        );
    }
}

#[test]
fn the_perceptual_quantiser_refuses_where_its_inverse_is_undefined() {
    // Above a code value of one its denominator passes through zero. The
    // standard does not define the curve there, so neither does this.
    assert!(decode(TransferFunction::PerceptualQuantiser, Fixed::ONE).is_ok());
    assert_eq!(
        decode(
            TransferFunction::PerceptualQuantiser,
            Fixed::from_integer(2).expect("two")
        ),
        Err(RenderStatus::Singular)
    );
}

#[test]
fn every_result_is_the_same_every_time() {
    for transfer in ALL {
        for step in 1..=20_i64 {
            let light = value(step, 20);
            assert_eq!(encode(transfer, light), encode(transfer, light));
            assert_eq!(decode(transfer, light), decode(transfer, light));
        }
    }
}
