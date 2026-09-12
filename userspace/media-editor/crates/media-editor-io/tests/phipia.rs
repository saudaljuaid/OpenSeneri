// SPDX-License-Identifier: GPL-3.0-only
//! Phipia's filesystem contract, checked against Phipia's own rules.
//!
//! Every case here is read out of `src/rust/fat32.rs` and `docs/FAT32.md` at
//! Phipia 2.1.0, commit `8fe1817`. A name this accepts is a name Phipia's
//! `parse_component` accepts, and a name this refuses is one Phipia refuses —
//! the point being that a person finds out at the moment they type it rather
//! than at the moment they save.

use media_editor_io::IoStatus;
use media_editor_io::phipia::{
    MAX_DIRECTORY_ENTRIES, MAX_FILE_BYTES, MAX_PATH_BYTES, MAX_PATH_COMPONENTS, Name, PUNCTUATION,
    Path, is_canonical,
};

#[test]
fn an_ordinary_name_survives_uppercased() {
    // Lowercase is accepted on input and uppercased on disk, so the two
    // spellings of one file are one file rather than two.
    let name = Name::new("clip.bmp").expect("a name");
    assert_eq!(name.as_str(), "CLIP.BMP");
    assert_eq!(Name::new("CLIP.BMP").expect("a name"), name);
    assert!(name.has_extension("BMP"));
    assert!(!name.has_extension("PHIP"));
}

#[test]
fn eight_and_three_is_eight_and_three() {
    assert!(Name::new("ABCDEFGH.XYZ").is_ok());
    assert_eq!(Name::new("ABCDEFGHI.XYZ"), Err(IoStatus::NameTooLong));
    assert_eq!(Name::new("ABCDEFGH.WXYZ"), Err(IoStatus::NameTooLong));
    // Thirteen bytes is past the twelve a component may be, and is refused on
    // the component bound before either half is looked at.
    assert_eq!(Name::new("ABCDEFGHIJKLM"), Err(IoStatus::NameTooLong));
    // And the case that separates "eight and three" from "eleven between
    // them": nine and two is twelve bytes and eleven characters, so a bound on
    // the total accepts it and Phipia does not.
    assert_eq!(Name::new("ABCDEFGHI.XY"), Err(IoStatus::NameTooLong));
    assert!(
        Name::new("ABCDEFGH.XY").is_ok(),
        "eight and two is both bounds"
    );
    // A name with no extension at all is ordinary.
    assert!(Name::new("VAULT").is_ok());
}

#[test]
fn the_punctuation_is_exactly_phipias() {
    // Sixteen marks, and the test writes them out rather than reading the
    // constant back: `$%'-_@~`!(){}^#&`.
    for mark in "$%'-_@~`!(){}^#&".bytes() {
        assert!(
            is_canonical(mark),
            "Phipia accepts {} and this does not",
            mark as char
        );
    }
    assert_eq!(PUNCTUATION.len(), 16);
    // And what is not in it. A space is the one that catches people, because
    // every other filesystem they have used takes one.
    for mark in " ,;=+[]\\/\"*?<>|:".bytes() {
        assert!(
            !is_canonical(mark),
            "Phipia refuses {} and this does not",
            mark as char
        );
    }
}

#[test]
fn a_name_outside_the_subset_is_refused() {
    assert_eq!(Name::new("MY FILE.BMP"), Err(IoStatus::NameNotCanonical));
    assert_eq!(
        Name::new("CAFE\u{301}.BMP"),
        Err(IoStatus::NameNotCanonical)
    );
    assert_eq!(Name::new("A+B.BMP"), Err(IoStatus::NameNotCanonical));
    assert_eq!(Name::new(""), Err(IoStatus::NameEmpty));
}

#[test]
fn the_dot_is_a_separator_and_there_is_one() {
    assert_eq!(Name::new(".BMP"), Err(IoStatus::NameDotMisplaced));
    assert_eq!(Name::new("CLIP."), Err(IoStatus::NameDotMisplaced));
    assert_eq!(Name::new("A.B.C"), Err(IoStatus::NameDotMisplaced));
}

#[test]
fn a_path_is_relative_and_bounded() {
    let path = Path::new("media/clips/take3.bmp").expect("a path");
    assert_eq!(path.components().len(), 3);
    assert_eq!(path.name().as_str(), "TAKE3.BMP");

    assert_eq!(Path::new("/media/x.bmp"), Err(IoStatus::PathAbsolute));
    assert_eq!(Path::new("media\\x.bmp"), Err(IoStatus::PathMalformed));
    assert_eq!(Path::new("media//x.bmp"), Err(IoStatus::PathMalformed));
    assert_eq!(Path::new("media/"), Err(IoStatus::PathMalformed));
    assert_eq!(Path::new(""), Err(IoStatus::PathEmpty));
    assert_eq!(Path::new(".."), Err(IoStatus::PathAboveRoot));
    assert_eq!(Path::new("media/../.."), Err(IoStatus::PathAboveRoot));
    // A path that resolves to the root names a directory, and every caller
    // here wants a file.
    assert_eq!(Path::new("media/.."), Err(IoStatus::PathEmpty));
}

#[test]
fn a_path_climbs_and_lands_where_it_should() {
    let path = Path::new("a/b/../c/./d.bmp").expect("a path");
    let names: Vec<&str> = path.components().iter().map(Name::as_str).collect();
    assert_eq!(names, vec!["A", "C", "D.BMP"]);
}

#[test]
fn the_path_bounds_are_phipias() {
    // A hundred and twenty-seven bytes, which is `FAT32_PATH_BYTES`. The
    // fixture is fifteen components of "AB/" -- forty-five bytes -- and then a
    // final component long enough to cross the bound.
    assert_eq!(MAX_PATH_BYTES, 127);
    assert_eq!(MAX_PATH_COMPONENTS, 16);
    let mut long = String::new();
    for _ in 0..15 {
        long.push_str("AB/");
    }
    long.push_str("ABCDEFGH.BMP");
    assert_eq!(long.len(), 57);
    assert!(Path::new(&long).is_ok(), "sixteen components is the bound");

    let mut past = String::new();
    for _ in 0..16 {
        past.push_str("AB/");
    }
    past.push_str("X.BMP");
    assert_eq!(Path::new(&past), Err(IoStatus::PathTooLong));

    let mut wide = String::new();
    while wide.len() < MAX_PATH_BYTES {
        wide.push_str("ABCDEFGH/");
    }
    assert!(wide.len() > MAX_PATH_BYTES);
    assert_eq!(Path::new(&wide), Err(IoStatus::PathTooLong));
}

#[test]
fn the_bounds_are_the_ones_phipia_documents() {
    // Written out rather than computed, because the whole value of this module
    // is that these are somebody else's numbers and a reader can check them
    // against `FAT32.md` without running anything.
    assert_eq!(MAX_FILE_BYTES, 16_777_216, "PHIPFS_MAX_FILE_BYTES");
    assert_eq!(MAX_DIRECTORY_ENTRIES, 64, "PHIPFS_MAX_LIST_ENTRIES");
    // And the fact that makes a vault necessary: one 1920x1080 frame in
    // eight-bit RGBA is 1920 x 1080 x 4 = 8,294,400 bytes. **Two** fit in one
    // of Phipia's files, with 188,416 bytes to spare; three do not.
    //
    // The first version of this test asserted that two did not fit, which is
    // the sort of number that sounds right and is not. It is written out here
    // in full so the next reader can do the arithmetic rather than trust it.
    let frame: usize = 1920 * 1080 * 4;
    assert_eq!(frame, 8_294_400);
    assert_eq!(MAX_FILE_BYTES.checked_sub(2 * frame), Some(188_416));
    assert_eq!(MAX_FILE_BYTES.checked_sub(3 * frame), None);
}
