// SPDX-License-Identifier: GPL-3.0-only
//! What a `.cube` file is allowed to claim.
//!
//! Each of this format's traps gets a test that fails when the trap is sprung,
//! the same way the edit decision list's four do. A grading format's faults do
//! not look like faults: a transposed cube is a plausible picture with the
//! wrong look, and a domain ignored is the wrong look on every pixel. Neither
//! crashes, and neither is visible without the original to compare against.

use media_editor_core::{Fixed, Rational};
use media_editor_io::IoStatus;
use media_editor_io::cube;
use media_editor_render::RenderStatus;
use media_editor_render::lut::{Interpolation, Lut3D, MAX_SIZE, MIN_SIZE};

/// A fixed-point value from a fraction.
fn at(numerator: i64, denominator: i64) -> Fixed {
    Fixed::from_rational(Rational::new(numerator, denominator).expect("a ratio")).expect("a value")
}

/// A three-a-side table whose every axis does something different.
///
/// Deliberately asymmetric in all three: red scales, green is inverted, blue
/// steps in unequal jumps. A transposition of any pair of axes changes the
/// samples, which is the only way a transposition can be caught — a symmetric
/// fixture would survive one and say nothing.
fn lively() -> std::string::String {
    use std::fmt::Write as _;

    let mut text = std::string::String::from("# a fixture\nTITLE \"lively\"\nLUT_3D_SIZE 3\n");
    for blue in 0..3_i64 {
        for green in 0..3_i64 {
            for red in 0..3_i64 {
                let r = red * 100;
                let g = (2 - green) * 100;
                let b = [0_i64, 30, 200][usize::try_from(blue).expect("an index")];
                writeln!(text, "{r}.0 {g}.0 {b}.0").expect("a line");
            }
        }
    }
    text
}

#[test]
fn red_varies_fastest() {
    // The trap that costs the most and shows the least. A transposed cube is
    // not a crash and not a garish mess — it is a plausible picture with the
    // wrong look, which is the worst kind of wrong for a format to be.
    //
    // In this fixture red rises along its axis, green *falls* along its own,
    // and blue steps unevenly. So reading the axes in any other order gives
    // different samples, and the second lattice point is the one that says so:
    // it is one step along red and nowhere along the others.
    let table = cube::parse(&lively()).expect("a table");
    assert_eq!(table.size(), 3);

    // Sample (1,0,0): red one step in, green at its start — which is the top,
    // because green is inverted — and blue at nought.
    assert_eq!(
        table.sample(1, 0, 0).expect("a sample"),
        [at(100, 1), at(200, 1), at(0, 1)],
        "the second sample in the file is not one step along red"
    );
    // And along the other two axes, which a transposition would swap in.
    assert_eq!(
        table.sample(0, 1, 0).expect("a sample"),
        [at(0, 1), at(100, 1), at(0, 1)]
    );
    assert_eq!(
        table.sample(0, 0, 1).expect("a sample"),
        [at(0, 1), at(200, 1), at(30, 1)]
    );
}

#[test]
fn a_decimal_is_read_exactly() {
    // The numbers in the file are decimal text, so they can be read exactly:
    // 0.123456789 is 123456789/1000000000 and nothing is lost. A reader that
    // went through a binary floating-point type would lose that on the way in,
    // for no reason, in a project with no floating point anywhere else.
    let text = "LUT_3D_SIZE 2\n".to_string()
        + &std::iter::repeat_n("0.123456789 0.5 0.1\n", 8).collect::<std::string::String>();
    let table = cube::parse(&text).expect("a table");
    let sample = table.sample(0, 0, 0).expect("a sample");

    assert_eq!(
        sample[0],
        Fixed::from_rational(Rational::new(123_456_789, 1_000_000_000).expect("a ratio"))
            .expect("a value"),
        "the digits did not survive the way in"
    );
    // A half and a tenth are exact fractions and must arrive as such.
    assert_eq!(sample[1], at(1, 2));
    assert_eq!(sample[2], at(1, 10));
}

#[test]
fn a_domain_that_is_not_the_unit_interval_is_refused() {
    // A table authored for another input range, applied as though the range
    // were nought to one, is the wrong look on every pixel — silently. A
    // reader that skipped the lines it did not handle would do exactly that,
    // which is why the domain is read and checked rather than ignored.
    let body: std::string::String = std::iter::repeat_n("0.0 0.0 0.0\n", 8).collect();
    let good =
        std::format!("LUT_3D_SIZE 2\nDOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n{body}");
    assert!(cube::parse(&good).is_ok(), "the unit interval was refused");

    for line in [
        "DOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 4.0 4.0 4.0\n",
        "DOMAIN_MIN -1.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n",
        "DOMAIN_MAX 0.9 1.0 1.0\n",
    ] {
        assert_eq!(
            cube::parse(&std::format!("LUT_3D_SIZE 2\n{line}{body}")).map(|_| ()),
            Err(IoStatus::CubeDomainUnsupported),
            "a domain of {line:?} was accepted"
        );
    }
}

#[test]
fn a_value_past_white_is_not_clamped() {
    // A look can send a highlight above white on purpose, and a reader that
    // clamped on the way in would quietly flatten it. Below nought too — a
    // table can undershoot.
    let body: std::string::String = std::iter::repeat_n("1.5 -0.25 2.0\n", 8).collect();
    let table = cube::parse(&std::format!("LUT_3D_SIZE 2\n{body}")).expect("a table");
    assert_eq!(
        table.sample(0, 0, 0).expect("a sample"),
        [at(3, 2), at(-1, 4), at(2, 1)],
        "an over-range value was flattened on the way in"
    );
}

#[test]
fn a_one_dimensional_table_is_refused_by_name() {
    // The same file extension carries a per-channel curve, which is not a cube
    // at all. Reading one as the other would treat a curve's samples as a
    // cube's and produce nonsense that still parses.
    let text = "LUT_1D_SIZE 4\n0.0 0.0 0.0\n1.0 1.0 1.0\n";
    assert_eq!(
        cube::parse(text).map(|_| ()),
        Err(IoStatus::CubeNotThreeDimensional)
    );
}

#[test]
fn scientific_notation_is_refused_rather_than_guessed_at() {
    // Files in the wild carry it for very small values. Refusing by name says
    // plainly that this is unimplemented rather than mis-parsed — which is a
    // better place to be than having read `1e-3` as one.
    let body: std::string::String = std::iter::repeat_n("1e-3 0.0 0.0\n", 8).collect();
    assert_eq!(
        cube::parse(&std::format!("LUT_3D_SIZE 2\n{body}")).map(|_| ()),
        Err(IoStatus::CubeExponentUnsupported)
    );
}

#[test]
fn comments_and_blank_lines_and_a_title_are_skipped() {
    let body: std::string::String = std::iter::repeat_n("0.0 0.0 0.0\n", 8).collect();
    let text = std::format!(
        "# leading comment\n\n   \nTITLE \"a look with spaces\"\n\nLUT_3D_SIZE 2\n\n# another\n{body}"
    );
    assert!(cube::parse(&text).is_ok());
}

#[test]
fn a_file_that_does_not_say_how_big_it_is_is_refused() {
    let body: std::string::String = std::iter::repeat_n("0.0 0.0 0.0\n", 8).collect();
    assert_eq!(
        cube::parse(&body).map(|_| ()),
        Err(IoStatus::CubeSampleBeforeSize),
        "samples before a size were read as though the size were known"
    );
    assert_eq!(
        cube::parse("TITLE \"nothing\"\n").map(|_| ()),
        Err(IoStatus::CubeNoSize)
    );
    assert_eq!(
        cube::parse(&std::format!("LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n{body}")).map(|_| ()),
        Err(IoStatus::CubeSizeRepeated),
        "a file that states its size twice was believed"
    );
}

#[test]
fn a_file_with_the_wrong_number_of_samples_is_refused() {
    // The size and the sample count are two statements of one fact, and unlike
    // the summary file this format has no choice about that — so the reader
    // checks them against each other rather than trusting either.
    let short: std::string::String = std::iter::repeat_n("0.0 0.0 0.0\n", 7).collect();
    assert_eq!(
        cube::parse(&std::format!("LUT_3D_SIZE 2\n{short}")).map(|_| ()),
        Err(IoStatus::Render(RenderStatus::LutNotACube))
    );
    let long: std::string::String = std::iter::repeat_n("0.0 0.0 0.0\n", 9).collect();
    assert_eq!(
        cube::parse(&std::format!("LUT_3D_SIZE 2\n{long}")).map(|_| ()),
        Err(IoStatus::Render(RenderStatus::LutNotACube))
    );
}

#[test]
fn a_sample_line_that_is_not_three_numbers_is_refused() {
    let body: std::string::String = std::iter::repeat_n("0.0 0.0 0.0\n", 7).collect();
    for bad in [
        "0.0 0.0\n",
        "0.0 0.0 0.0 0.0\n",
        "0.0 0.0 red\n",
        "0..1 0.0 0.0\n",
    ] {
        assert_eq!(
            cube::parse(&std::format!("LUT_3D_SIZE 2\n{bad}{body}")).map(|_| ()),
            Err(IoStatus::CubeMalformed),
            "a sample line of {bad:?} was accepted"
        );
    }
}

#[test]
fn a_side_this_build_does_not_carry_is_refused() {
    for size in [0, 1, MAX_SIZE + 1, 99_999] {
        assert_eq!(
            cube::parse(&std::format!("LUT_3D_SIZE {size}\n")).map(|_| ()),
            Err(IoStatus::CubeSizeUnsupported),
            "a side of {size} was accepted"
        );
    }
}

#[test]
fn a_line_past_the_bound_is_refused() {
    let long = "0.0 ".repeat(100);
    assert_eq!(
        cube::parse(&std::format!("LUT_3D_SIZE 2\n{long}\n")).map(|_| ()),
        Err(IoStatus::CubeLineTooLong)
    );
}

#[test]
fn a_table_survives_being_written_and_read_back() {
    // Nine decimal places out, and the parser reads nine in, so a table that
    // arrived with nine or fewer places leaves with the same digits — which is
    // what makes this an equality rather than a tolerance.
    let table = cube::parse(&lively()).expect("a table");
    let written = cube::write(&table).expect("a file");
    let again = cube::parse(&written).expect("a table");
    assert_eq!(again, table);

    // And writing what was read gives the same bytes, which is what a cache
    // and a diff both need.
    assert_eq!(cube::write(&again).expect("a file"), written);
}

#[test]
fn an_identity_table_survives_at_every_size_this_build_carries() {
    for size in [MIN_SIZE, 3, 17, 33, MAX_SIZE] {
        let table = Lut3D::identity(size).expect("a table");
        let written = cube::write(&table).expect("a file");
        let again = cube::parse(&written).expect("a table");
        assert_eq!(again, table, "an identity of side {size} did not survive");

        // And it still does nothing after the trip, which is the property that
        // matters rather than the bytes.
        let probe = [at(1, 3), at(2, 7), at(5, 11)];
        let held = again
            .look_up(probe, Interpolation::Tetrahedral)
            .expect("a colour");
        for channel in 0..3 {
            assert!(
                (held[channel].raw() - probe[channel].raw()).abs() <= 2,
                "identity of side {size} moved channel {channel} after a round trip"
            );
        }
    }
}

#[test]
fn a_number_with_more_places_than_this_build_carries_is_refused() {
    // Ten places rather than nine. Refused by name rather than truncated,
    // because truncating a colour is a decision and this is not the place to
    // make one.
    let body: std::string::String = std::iter::repeat_n("0.1234567891 0.0 0.0\n", 8).collect();
    assert_eq!(
        cube::parse(&std::format!("LUT_3D_SIZE 2\n{body}")).map(|_| ()),
        Err(IoStatus::CubeTooPrecise)
    );
}

#[test]
fn a_truncation_is_detected_everywhere_but_inside_the_last_number() {
    // A parser is a hostile-input surface (R-11.1), and this format had no
    // sweep at all when it was written — the three binary formats each have
    // two and the edit decision list has one. That was a gap against this
    // project's own rule, found by going and looking rather than by anything
    // failing. Writing the sweep then found something about the *format*.
    //
    // Every prefix that stops before the last sample line is refused: the cube
    // comes up short and the sample count says so. But a prefix that stops
    // *inside* the last line can still spell three numbers — `200.0 0.0 200.0`
    // cut to `200.0 0.0 2` is three numbers, and cut to `200.0 0.0 200.` is
    // three numbers with the same values. Those files are accepted, and no
    // reader could do otherwise.
    //
    // That is a property of the format rather than of this reader. `.cube`
    // carries no length and no digest, so a file truncated inside its final
    // number is indistinguishable from a valid file somebody authored
    // differently. Every format this project writes itself carries both, which
    // is exactly why: `SPRJ`, `SPRW` and `SPPK` all refuse every prefix, and
    // this one cannot.
    //
    // So the assertion is the sharp version of what is true: a truncation is
    // detected everywhere except inside the final record.
    let file = lively();
    let last_line = file
        .trim_end_matches('\n')
        .rfind('\n')
        .expect("a last line")
        + 1;

    let mut accepted = 0;
    for length in 0..file.len() {
        if cube::parse(&file[..length]).is_ok() {
            assert!(
                length >= last_line,
                "a file truncated to {length} bytes produced a table, and that is \
                 before the last sample line begins at {last_line}"
            );
            accepted += 1;
        }
    }
    assert!(
        accepted > 1,
        "only {accepted} prefixes were accepted, so this test is not exercising \
         the case it describes"
    );
    assert!(cube::parse(&file).is_ok(), "the whole file was refused");

    // And the trailing newline is not required, which is a real property
    // rather than an off-by-one: a file whose last line has no terminator is
    // still a file, and a reader that refused one would refuse most of what
    // editors write.
    assert_eq!(file.as_bytes()[file.len() - 1], b'\n');
    assert!(cube::parse(&file[..file.len() - 1]).is_ok());
}

#[test]
fn every_extension_of_a_file_is_refused() {
    // The other end. A file with a sample after its last sample has more
    // samples than its size declares, and a file with a second size line does
    // not know how big it is.
    let file = lively();
    for tail in [
        "0.0 0.0 0.0\n",
        "\n0.0 0.0 0.0\n",
        "LUT_3D_SIZE 3\n",
        "garbage\n",
        "0.0\n",
    ] {
        assert!(
            cube::parse(&std::format!("{file}{tail}")).is_err(),
            "a file with {tail:?} appended was accepted"
        );
    }

    // Whitespace, comments and a title are accepted after the samples, and
    // that is a decision rather than an oversight. A title is metadata about
    // the file rather than part of its data, so where it sits carries nothing
    // — and refusing a late one would reject files over a position no reader
    // needs and no writer promises. This test asserted the opposite first, and
    // the assertion was wrong rather than the parser.
    for tail in [
        "",
        "\n",
        "\n\n   \n",
        "# a trailing note\n",
        "TITLE \"late\"\n",
    ] {
        assert!(
            cube::parse(&std::format!("{file}{tail}")).is_ok(),
            "a file with {tail:?} appended was refused"
        );
    }
}

#[test]
fn no_digit_of_a_sample_is_silently_ignored() {
    // The text analogue of the binary formats' byte sweep, and it has to be a
    // different assertion twice over.
    //
    // First, changing a byte in a text file usually produces another *valid*
    // file, so "every change is refused" would be false and useless. What must
    // hold is that no digit of a sample carries no meaning: every change is
    // either refused or produces a different table. A byte that can be changed
    // with no effect is a byte the reader dropped, which is how a field goes
    // missing without anything failing.
    //
    // Second — and this is what the first attempt got wrong — the mutation has
    // to stay inside the alphabet of the field. Replacing the `0` of `0.0`
    // with a space gives ` .0`, which splits to `.0`, which is still nought:
    // different text, same number, and the sweep read that as a byte carrying
    // nothing. It was the sweep that was wrong. Mutating a digit to a
    // *different digit* always changes the number it spells, so the claim is
    // sharp again.
    //
    // The general form: a text sweep that mutates outside a field's alphabet
    // measures the lexer's leniency rather than the parser's completeness.
    let file = lively();
    let marker = "LUT_3D_SIZE 3\n";
    let payload = file.find(marker).expect("a size line") + marker.len();
    let original = cube::parse(&file).expect("a table");

    let mut checked = 0;
    for index in payload..file.len() {
        let byte = file.as_bytes()[index];
        if !byte.is_ascii_digit() {
            continue;
        }
        for replacement in b"0123456789" {
            if *replacement == byte {
                continue;
            }
            let mut mutated = file.clone().into_bytes();
            mutated[index] = *replacement;
            let text = std::string::String::from_utf8(mutated).expect("still text");
            match cube::parse(&text) {
                Err(_) => {}
                Ok(table) => assert_ne!(
                    table, original,
                    "digit {index} changed to {:?} was read as the same table, so that \
                     digit carries nothing",
                    *replacement as char
                ),
            }
            checked += 1;
        }
    }
    assert!(checked > 500, "the sweep covered only {checked} mutations");
}

#[test]
fn a_hostile_file_is_bounded_rather_than_trusted() {
    // Nothing may make this reader reserve or read without a bound first.
    let long = std::format!("LUT_3D_SIZE 2\n{}\n", "0".repeat(cube::MAX_LINE_BYTES + 1));
    assert_eq!(cube::parse(&long), Err(IoStatus::CubeLineTooLong));

    // A size line naming a cube far larger than this build carries is refused
    // before anything reserves room for it.
    assert_eq!(
        cube::parse("LUT_3D_SIZE 4294967295\n").map(|_| ()),
        Err(IoStatus::CubeSizeUnsupported)
    );
    // And one that would overflow the multiplication that counts its samples.
    assert_eq!(
        cube::parse("LUT_3D_SIZE 99999999999999999999999999\n").map(|_| ()),
        Err(IoStatus::CubeSizeUnsupported)
    );

    // A number with more digits than an integer holds is refused rather than
    // wrapped.
    let body: std::string::String = std::iter::repeat_n("0.0 0.0 0.0\n", 7).collect();
    assert_eq!(
        cube::parse(&std::format!(
            "LUT_3D_SIZE 2\n{}999 0.0 0.0\n{body}",
            "9".repeat(30)
        ))
        .map(|_| ()),
        Err(IoStatus::CubeOutOfRange)
    );
}
