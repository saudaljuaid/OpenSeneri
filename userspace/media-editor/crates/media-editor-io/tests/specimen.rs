// SPDX-License-Identifier: GPL-3.0-only
//! The specimen: the whole face, kept as a picture so it can be looked at.
//!
//! Every other test of the face measures a number — an area, a coverage, an
//! intersection that must be nought. All of them would pass on a face whose
//! letters were the wrong letters. A `G` drawn as a `C`, a `5` drawn as an
//! `S`, a crossbar at the wrong height: each is a perfectly disjoint, exactly
//! rasterised, entirely wrong glyph.
//!
//! So the face is also committed as a picture, compared byte for byte, and on
//! a mismatch this writes what it actually drew beside the reference and says
//! where both are. That is the same argument the reference capture makes and
//! it is a stronger one here, because a face is the one thing in this project
//! whose correctness is a *judgement by eye* and cannot be anything else.

use std::path::PathBuf;

use media_editor_core::Rational;
use media_editor_media::{
    AlphaState, ColourDescription, Frame, FrameDescription, Geometry, PixelFormat,
};
use media_editor_render::font::{self, Face};

/// How big the specimen is.
const WIDTH: usize = 344;
const HEIGHT: usize = 212;

/// The em, in pixels.
const SIZE: i64 = 24;

/// How far apart the lines sit.
///
/// More than the em, because this face descends: a `g` reaches twenty-one
/// half-units below the cap line and the em is sixteen, so lines set at the em
/// would have every descender in the row above touching every cap below it.
const LEADING: i64 = 34;

/// What the specimen sets: every character the face has, in four lines.
const LINES: [&str; 6] = [
    "ABCDEFGHIJKLM",
    "NOPQRSTUVWXYZ",
    "abcdefghijklm",
    "nopqrstuvwxyz",
    "0123456789 -:",
    "(Media/Editor).,",
];

fn golden() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tests/golden/specimen.png")
}

/// The specimen, as white type on black.
fn drawn() -> Vec<u8> {
    let face = Face::stencil();
    let mut plane = std::vec![0_u8; WIDTH * HEIGHT];
    for (row, line) in LINES.iter().enumerate() {
        let down = 6 + LEADING * i64::try_from(row).expect("a row");
        let run = font::place(
            face,
            line,
            Rational::new(SIZE, 1).expect("a size"),
            (
                Rational::new(6, 1).expect("a margin"),
                Rational::new(down, 1).expect("a line"),
            ),
        )
        .expect("a run");
        let line = run.plane(WIDTH, HEIGHT).expect("a plane");
        for (into, from) in plane.iter_mut().zip(line) {
            // The lines do not overlap, so this only ever adds to nought --
            // and `saturating_add` rather than `+` so that a leading somebody
            // tightens later produces a bright join rather than a panic.
            *into = (*into).saturating_add(from);
        }
    }
    plane
}

fn encoded(plane: &[u8]) -> Vec<u8> {
    let mut packed = Vec::new();
    for value in plane {
        packed.extend_from_slice(&[*value, *value, *value, 255]);
    }
    let description = FrameDescription::square(
        Geometry::new(
            u32::try_from(WIDTH).expect("a width"),
            u32::try_from(HEIGHT).expect("a height"),
        )
        .expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description");
    let frame = Frame::from_packed(description, &packed).expect("a frame");
    media_editor_io::png::encode(&frame).expect("an encoding")
}

#[test]
fn the_specimen_is_the_picture_committed_beside_it() {
    let png = encoded(&drawn());
    let reference = golden();
    let expected = std::fs::read(&reference).unwrap_or_else(|error| {
        panic!(
            "the specimen is missing at {}: {error}",
            reference.display()
        )
    });
    if png != expected {
        let actual = reference.with_file_name("specimen-actual.png");
        std::fs::write(&actual, &png).expect("the specimen is written");
        panic!(
            "the face has changed.\n  reference: {}\n  actual:    {}",
            reference.display(),
            actual.display()
        );
    }
}

#[test]
fn the_specimen_is_deterministic() {
    assert_eq!(drawn(), drawn(), "two drawings of one face are one picture");
}

#[test]
fn the_specimen_shows_every_character_the_face_has() {
    // So that adding a glyph and forgetting to set it here is a failure rather
    // than a glyph nobody ever looks at.
    let face = Face::stencil();
    let shown: String = LINES.concat();
    for character in face.repertoire() {
        assert!(
            shown.contains(character),
            "the specimen does not show {character:?}"
        );
    }
}

#[test]
fn a_single_changed_pixel_fails_the_comparison() {
    // The comparison is only evidence if it can fail. One byte of one glyph,
    // and the encoding has to come out different.
    let mut plane = drawn();
    let at = plane
        .iter()
        .position(|value| *value == 255)
        .expect("some ink");
    plane[at] = 254;
    assert_ne!(encoded(&plane), encoded(&drawn()));
}
