// SPDX-License-Identifier: GPL-3.0-only
//! The colour derivation, checked against the numbers the standards print.
//!
//! These tests are the reason the pipeline is exact. Every constant asserted
//! here is one a standards document states, and every value compared against
//! it is derived from that same document's chromaticity coordinates by this
//! crate — so agreement means the derivation is right, and disagreement means
//! either the derivation or the transcription is wrong.

use media_editor_core::Rational;
use media_editor_media::Primaries;
use media_editor_render::{Chromaticity, Gamut, Matrix3, RenderStatus, Vector3, gamut_of};

/// A rational rounded to a number of decimal places, exactly.
fn rounded(value: Rational, places: u32) -> i64 {
    let scale = 10_i64.pow(places);
    let half = Rational::new(1, 2).expect("a half");
    value
        .scale(scale)
        .and_then(|scaled| scaled.checked_add(half))
        .and_then(Rational::floor)
        .expect("a value inside the representable range")
}

fn luminance_of(primaries: Primaries) -> Vector3 {
    gamut_of(primaries)
        .expect("a gamut")
        .derived_luminance()
        .expect("a derivation")
}

#[test]
fn bt709_luma_coefficients_fall_out_of_its_primaries() {
    // ITU-R BT.709-6 prints 0.2126, 0.7152 and 0.0722. Those numbers are not
    // arbitrary: they are what the primaries and D65 produce. If this crate's
    // derivation is right, they come back out.
    let luminance = luminance_of(Primaries::Bt709);
    assert_eq!(rounded(luminance.elements()[0], 4), 2126);
    assert_eq!(rounded(luminance.elements()[1], 4), 7152);
    assert_eq!(rounded(luminance.elements()[2], 4), 722);
}

#[test]
fn bt2020_luma_coefficients_fall_out_of_its_primaries() {
    // ITU-R BT.2020-2 prints 0.2627, 0.6780 and 0.0593.
    let luminance = luminance_of(Primaries::Bt2020);
    assert_eq!(rounded(luminance.elements()[0], 4), 2627);
    assert_eq!(rounded(luminance.elements()[1], 4), 6780);
    assert_eq!(rounded(luminance.elements()[2], 4), 593);
}

#[test]
fn bt601_luma_coefficients_do_not_fall_out_of_its_primaries() {
    // The one everybody gets wrong. BT.601's matrix uses 0.299, 0.587 and
    // 0.114, which came from the 1953 NTSC primaries and were kept when the
    // primaries changed to SMPTE 170M. Deriving them from the modern
    // primaries gives something else entirely, and an implementation that
    // derives its matrix from its primaries would silently produce a
    // different picture.
    //
    // So this test asserts the disagreement. Matrix coefficients and
    // primaries are different things, and this crate keeps them apart.
    let luminance = luminance_of(Primaries::Bt601Ntsc);
    assert_ne!(rounded(luminance.elements()[0], 3), 299);
    assert_ne!(rounded(luminance.elements()[1], 3), 587);
    assert_ne!(rounded(luminance.elements()[2], 3), 114);

    // What SMPTE 170M's primaries actually derive to, for the record: the
    // gap between these and the coefficients the matrix uses is the historical
    // artefact, and it is about nine parts in a thousand on blue.
    assert_eq!(rounded(luminance.elements()[0], 4), 2124);
    assert_eq!(rounded(luminance.elements()[1], 4), 7011);
    assert_eq!(rounded(luminance.elements()[2], 4), 866);
}

#[test]
fn every_gamuts_luminance_sums_to_exactly_one() {
    // White is by definition the colour with luminance one, and the three
    // coefficients are how much of it each primary supplies. In floating
    // point this sum is 0.9999999999999999 or 1.0000000000000002 depending on
    // the machine. Here it is one.
    for primaries in [
        Primaries::Bt709,
        Primaries::Bt601Ntsc,
        Primaries::Bt601Pal,
        Primaries::Bt2020,
        Primaries::DciP3,
        Primaries::DisplayP3,
        Primaries::AcesAp0,
        Primaries::AcesAp1,
    ] {
        let luminance = luminance_of(primaries);
        let sum = luminance.elements()[0]
            .checked_add(luminance.elements()[1])
            .and_then(|partial| partial.checked_add(luminance.elements()[2]))
            .expect("a sum");
        assert_eq!(sum, Rational::ONE, "{primaries:?}");
    }
}

#[test]
fn white_maps_to_the_white_point_exactly() {
    // Equal amounts of the three primaries must produce the gamut's white
    // point, exactly. This is the property the whole derivation exists to
    // establish, and it is the one a rounded matrix breaks first.
    for primaries in [
        Primaries::Bt709,
        Primaries::Bt2020,
        Primaries::DciP3,
        Primaries::AcesAp1,
    ] {
        let gamut = gamut_of(primaries).expect("a gamut");
        let matrix = gamut.rgb_to_xyz().expect("a matrix");
        let white = matrix
            .apply(Vector3::new(Rational::ONE, Rational::ONE, Rational::ONE))
            .expect("white");
        let expected = gamut.white.tristimulus().expect("the white point");
        assert_eq!(white, expected, "{primaries:?}");
    }
}

#[test]
fn a_gamut_round_trips_through_xyz_exactly() {
    for primaries in [Primaries::Bt709, Primaries::Bt2020, Primaries::AcesAp0] {
        let gamut = gamut_of(primaries).expect("a gamut");
        let there = gamut.rgb_to_xyz().expect("a matrix");
        let back = gamut.xyz_to_rgb().expect("a matrix");
        assert_eq!(
            back.multiply(&there).expect("a product"),
            Matrix3::identity(),
            "{primaries:?} does not round trip exactly"
        );
    }
}

#[test]
fn a_gamut_to_itself_is_the_identity() {
    // Not approximately the identity. Exactly it. A pipeline that converts
    // Rec. 709 to Rec. 709 must not change a single sample, and in floating
    // point it usually does.
    for primaries in [Primaries::Bt709, Primaries::Bt2020, Primaries::DisplayP3] {
        let gamut = gamut_of(primaries).expect("a gamut");
        assert_eq!(
            gamut.to_gamut(&gamut).expect("a matrix"),
            Matrix3::identity(),
            "{primaries:?}"
        );
    }
}

#[test]
fn converting_between_two_gamuts_and_back_is_the_identity() {
    let source = gamut_of(Primaries::Bt709).expect("a gamut");
    let target = gamut_of(Primaries::Bt2020).expect("a gamut");
    let there = source.to_gamut(&target).expect("a matrix");
    let back = target.to_gamut(&source).expect("a matrix");
    assert_eq!(
        back.multiply(&there).expect("a product"),
        Matrix3::identity(),
        "a round trip through a wider gamut must lose nothing"
    );
}

#[test]
fn bt709_to_bt2020_is_the_matrix_the_standard_publishes() {
    // ITU-R BT.2087 gives the 709-to-2020 conversion to four places:
    //   0.6274  0.3293  0.0433
    //   0.0691  0.9195  0.0114
    //   0.0164  0.0880  0.8956
    // Derived here from the two gamuts' chromaticities alone.
    let source = gamut_of(Primaries::Bt709).expect("a gamut");
    let target = gamut_of(Primaries::Bt2020).expect("a gamut");
    let matrix = source.to_gamut(&target).expect("a matrix");
    let expected = [[6274, 3293, 433], [691, 9195, 114], [164, 880, 8956]];
    for (row, values) in expected.iter().enumerate() {
        for (column, value) in values.iter().enumerate() {
            assert_eq!(
                rounded(matrix.get(row, column).expect("an element"), 4),
                *value,
                "element {row},{column}"
            );
        }
    }
}

#[test]
fn each_row_of_a_conversion_sums_to_one() {
    // White in must be white out, so every row of a gamut-to-gamut matrix
    // sums to exactly one. Rounded published matrices do not have this
    // property, which is why applying one twice drifts.
    let source = gamut_of(Primaries::Bt709).expect("a gamut");
    let target = gamut_of(Primaries::Bt2020).expect("a gamut");
    let matrix = source.to_gamut(&target).expect("a matrix");
    for row in 0..3 {
        let mut sum = Rational::ZERO;
        for column in 0..3 {
            let element = matrix.get(row, column).expect("an element");
            sum = sum.checked_add(element).expect("a sum");
        }
        assert_eq!(sum, Rational::ONE, "row {row}");
    }
}

#[test]
fn a_chromaticity_on_the_x_axis_is_refused() {
    assert_eq!(
        Chromaticity::new(Rational::ONE, Rational::ZERO),
        Err(RenderStatus::DegenerateChromaticity),
        "a zero y is a division by zero waiting to happen"
    );
}

#[test]
fn three_collinear_primaries_span_no_gamut() {
    // A gamut needs three points that are not on one line. Given three that
    // are, there is no scaling of them that reaches the white point, and the
    // matrix is singular.
    let collinear = Gamut {
        red: Chromaticity::decimal(100, 100, 1000).expect("a point"),
        green: Chromaticity::decimal(200, 200, 1000).expect("a point"),
        blue: Chromaticity::decimal(300, 300, 1000).expect("a point"),
        white: Chromaticity::decimal(3127, 3290, 10_000).expect("a point"),
    };
    assert_eq!(collinear.rgb_to_xyz(), Err(RenderStatus::DegenerateGamut));
}

#[test]
fn the_matrix_algebra_holds() {
    let matrix = Matrix3::from_rows([
        [
            Rational::new(2, 1).expect("a value"),
            Rational::new(-1, 3).expect("a value"),
            Rational::new(5, 7).expect("a value"),
        ],
        [
            Rational::new(1, 4).expect("a value"),
            Rational::new(3, 2).expect("a value"),
            Rational::new(-2, 5).expect("a value"),
        ],
        [
            Rational::new(-1, 6).expect("a value"),
            Rational::new(7, 8).expect("a value"),
            Rational::new(4, 3).expect("a value"),
        ],
    ]);
    let inverse = matrix.inverse().expect("an inverse");
    assert_eq!(
        matrix.multiply(&inverse).expect("a product"),
        Matrix3::identity()
    );
    assert_eq!(
        inverse.multiply(&matrix).expect("a product"),
        Matrix3::identity()
    );
    assert_eq!(matrix.transpose().transpose(), matrix);
    assert_eq!(
        Matrix3::identity().multiply(&matrix).expect("a product"),
        matrix
    );
}

#[test]
fn a_singular_matrix_has_no_inverse() {
    let singular = Matrix3::from_rows([
        [
            Rational::ONE,
            Rational::from_integer(2),
            Rational::from_integer(3),
        ],
        [
            Rational::from_integer(2),
            Rational::from_integer(4),
            Rational::from_integer(6),
        ],
        [
            Rational::from_integer(1),
            Rational::from_integer(0),
            Rational::from_integer(1),
        ],
    ]);
    assert_eq!(singular.determinant(), Ok(Rational::ZERO));
    assert_eq!(singular.inverse(), Err(RenderStatus::Singular));
}
