// SPDX-License-Identifier: GPL-3.0-only
//! A title's colour, named in light rather than in code values.
//!
//! The claim under test is not that a colour is stored — anything can store
//! three numbers — but that the three numbers mean the same thing everywhere.
//! A code value does not: 128 is one brightness in sRGB and a different one in
//! a linear working space, so a model holding bytes would be holding an
//! encoding it never named (R-8.3). A fraction of full light is the same
//! colour in every encoding, and the renderer spells it in whichever one the
//! frame it is drawing uses.
//!
//! What that buys is checked in the renderer's own tests, where the *same*
//! ink comes out 255 in a full-range frame and 235 in a limited-range one.
//! What is checked here is the model's half: the range, the refusals, and the
//! fact that a colour is part of what a card *is*.

use media_editor_core::{Duration, Rational, Timebase};
use media_editor_model::{Alignment, Ink, MediaAsset, ModelStatus, Title};

const RATE: Timebase = Timebase::FILM_24;

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

fn card() -> Title {
    Title::line("MEDIAEDTO".into(), r(1, 6), r(1, 2), r(1, 2)).expect("a card")
}

#[test]
fn a_title_is_white_until_somebody_colours_it() {
    // White is the default for titles without an explicit ink.
    assert_eq!(card().ink(), Ink::WHITE);
    assert!(card().ink().is_white());
    assert_eq!(Ink::WHITE.channels(), [Rational::ONE; 3]);
}

#[test]
fn an_ink_is_three_fractions_of_full_light() {
    let amber = Ink::new(Rational::ONE, r(3, 4), Rational::ZERO).expect("an amber");
    assert_eq!(amber.channels(), [Rational::ONE, r(3, 4), Rational::ZERO]);
    assert!(!amber.is_white());
}

#[test]
fn no_light_and_full_light_are_both_colours() {
    // The bounds are inclusive at both ends, which is worth a test because a
    // guard written with the wrong comparison refuses exactly these two and
    // nothing else -- and black type on a white card is a real design.
    assert!(Ink::new(Rational::ZERO, Rational::ZERO, Rational::ZERO).is_ok());
    assert!(Ink::new(Rational::ONE, Rational::ONE, Rational::ONE).is_ok());
}

#[test]
fn a_channel_brighter_than_white_is_refused() {
    // Not clamped. A premultiplied frame holding a sample above its coverage
    // is one the compositor refuses on the way past, and by then nobody can
    // tell which of the three numbers was the wrong one.
    for channel in 0..3 {
        let mut channels = [r(1, 2); 3];
        channels[channel] = r(5, 4);
        let [red, green, blue] = channels;
        assert_eq!(
            Ink::new(red, green, blue),
            Err(ModelStatus::InkOutOfRange),
            "channel {channel} above full light"
        );
    }
}

#[test]
fn a_channel_below_nothing_is_refused() {
    // Darker than black is not a colour, it is a subtraction, and there is no
    // such ink. Every channel checked, because a loop that only reached the
    // first would pass a test that only set the first.
    for channel in 0..3 {
        let mut channels = [r(1, 2); 3];
        channels[channel] = r(-1, 100);
        let [red, green, blue] = channels;
        assert_eq!(
            Ink::new(red, green, blue),
            Err(ModelStatus::InkOutOfRange),
            "channel {channel} below no light at all"
        );
    }
}

#[test]
fn white_is_white_however_it_is_written() {
    // Two over two is one. A rational normalises when it is built, so this is
    // not a claim about `Ink` so much as a check that it did not undo one --
    // an ink that compared numerators and denominators would write forty-eight
    // bytes into the file to say the card was white.
    let written = Ink::new(r(2, 2), r(3, 3), r(4, 4)).expect("white, the long way");
    assert_eq!(written, Ink::WHITE);
    assert!(written.is_white());
}

#[test]
fn the_same_words_in_two_colours_are_two_pictures() {
    // Which means two assets, two digests and two cache entries. A digest that
    // ignored the ink would make a red card and a white card one asset, and
    // every clip of either would show whichever was drawn first -- the exact
    // fault the size and the alignment are in the digest to avoid.
    let white = card();
    let red = white.with_ink(Ink::new(Rational::ONE, Rational::ZERO, Rational::ZERO).expect("red"));
    assert_ne!(
        white.digest().expect("a digest"),
        red.digest().expect("a digest")
    );
    // And it is the *ink* that separates them, not merely that `with_ink`
    // returned something different: everything else is equal.
    assert_eq!(white.lines(), red.lines());
    assert_eq!(white.size(), red.size());
    assert_eq!(white.across(), red.across());
    assert_eq!(white.down(), red.down());
    assert_eq!(white.alignment(), red.alignment());
}

#[test]
fn two_inks_that_differ_in_one_channel_are_two_pictures() {
    // The digest absorbs three rationals, and a digest that absorbed only the
    // first would pass every test above.
    let mut seen = std::vec::Vec::new();
    for channel in 0..3 {
        let mut channels = [Rational::ONE; 3];
        channels[channel] = r(1, 2);
        let [red, green, blue] = channels;
        seen.push(
            card()
                .with_ink(Ink::new(red, green, blue).expect("an ink"))
                .digest()
                .expect("a digest"),
        );
    }
    assert_ne!(seen[0], seen[1]);
    assert_ne!(seen[1], seen[2]);
    assert_ne!(seen[0], seen[2]);
}

#[test]
fn a_coloured_card_is_still_an_asset_named_by_what_it_says() {
    // The asset takes its digest from the title, ink and all, so colouring a
    // card is the same kind of change as rewording it: a different asset,
    // reached by a different clip, rather than a quiet edit to what every
    // existing clip of it shows.
    let red =
        card().with_ink(Ink::new(Rational::ONE, Rational::ZERO, Rational::ZERO).expect("red"));
    let asset = MediaAsset::titled(
        red.clone(),
        RATE,
        Duration::new(240, RATE).expect("a length"),
    )
    .expect("an asset");
    assert_eq!(asset.digest(), red.digest().expect("a digest"));
}

#[test]
fn an_ink_travels_with_every_other_field_a_card_carries() {
    // `with_ink` must preserve every layout field while rebuilding the card.
    let block = Title::new(
        std::vec!["ONE".into(), "TWO".into()],
        r(1, 8),
        r(1, 4),
        r(3, 4),
        Alignment::Right,
    )
    .expect("a block");
    let coloured =
        block.with_ink(Ink::new(Rational::ZERO, r(1, 2), Rational::ONE).expect("a blue"));
    assert_eq!(coloured.alignment(), Alignment::Right);
    assert_eq!(coloured.lines().len(), 2);
    assert_eq!(coloured.size(), r(1, 8));
    assert_eq!(coloured.across(), r(1, 4));
    assert_eq!(coloured.down(), r(3, 4));
}
