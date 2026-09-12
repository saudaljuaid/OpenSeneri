// SPDX-License-Identifier: GPL-3.0-only
//! Titles: media the program makes out of words.
//!
//! The claim this file is mostly about is not that a title draws — that is the
//! renderer's — but that **a title is media**. Not a new kind of item, not a
//! property of a clip: an asset, which a clip cuts from like any other. If
//! that is true then trimming, rolling, sliding, splitting, grading, masking,
//! moving and animating a title all work already and none of them had to be
//! told what a title is.
//!
//! So most of what follows asks exactly that, one editing operation at a time,
//! of a clip that happens to be a card. A title that were its own kind of item
//! would need every one of those written again and would get one of them
//! subtly wrong.

use media_editor_core::{Digest, Duration, Rational, Timebase};
use media_editor_model::{
    Clip, Edit, Item, Location, MediaAsset, MediaSource, ModelStatus, Project, SequenceId, Title,
    TrackKind, title::MAX_TITLE_TEXT,
};

const RATE: Timebase = Timebase::FILM_24;
const LENGTH: i64 = 96;

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn card(words: &str) -> Title {
    Title::line(words.into(), r(1, 6), r(1, 2), r(1, 2)).expect("a title")
}

fn asset(words: &str) -> MediaAsset {
    MediaAsset::titled(card(words), RATE, frames(240)).expect("an asset")
}

/// A project with one title clip on one track.
fn project() -> (Project, SequenceId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let media = project.add_media(asset("MEDIAEDTO")).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 0, frames(LENGTH)).expect("a clip")),
            },
        )
        .expect("a clip");
    project.forget_history();
    (project, sequence)
}

fn clip_at(project: &Project, sequence: SequenceId, index: usize) -> Clip {
    let Item::Clip(clip) = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(index)
        .expect("an item")
    else {
        panic!("a clip");
    };
    clip.clone()
}

#[test]
fn a_title_is_named_by_what_it_says() {
    // What content addressing already meant. The same card in two projects is
    // the same card, two clips of it share a cached frame, and changing a word
    // makes a *different* asset rather than quietly changing what every clip
    // of it shows.
    assert_eq!(
        asset("MEDIAEDTO").digest(),
        asset("MEDIAEDTO").digest(),
        "the same words are the same asset"
    );
    assert_ne!(asset("MEDIAEDTO").digest(), asset("MEDIAEDTD").digest());
}

#[test]
fn every_part_of_a_title_is_in_its_name() {
    // A title that hashed only its words would make two cards at two sizes one
    // asset, and every clip of either would show whichever was drawn first.
    let base = Title::line("TITLE".into(), r(1, 6), r(1, 2), r(1, 2)).expect("a title");
    let others = [
        Title::line("TITLE".into(), r(1, 5), r(1, 2), r(1, 2)).expect("a title"),
        Title::line("TITLE".into(), r(1, 6), r(1, 3), r(1, 2)).expect("a title"),
        Title::line("TITLE".into(), r(1, 6), r(1, 2), r(1, 3)).expect("a title"),
        Title::line("TITLES".into(), r(1, 6), r(1, 2), r(1, 2)).expect("a title"),
    ];
    let named = base.digest().expect("a digest");
    for other in &others {
        assert_ne!(named, other.digest().expect("a digest"));
    }
}

#[test]
fn a_title_with_nothing_to_say_is_refused() {
    // A card that says nothing is a gap, and saying so is better than holding
    // a picture that draws nothing and claims to be a title.
    assert_eq!(
        Title::line(String::new(), r(1, 6), r(1, 2), r(1, 2)),
        Err(ModelStatus::EmptyTitle)
    );
}

#[test]
fn a_title_longer_than_this_describes_is_refused() {
    let long: String = core::iter::repeat_n('A', MAX_TITLE_TEXT + 1).collect();
    assert_eq!(
        Title::line(long, r(1, 6), r(1, 2), r(1, 2)),
        Err(ModelStatus::TitleTooLong)
    );
    let fits: String = core::iter::repeat_n('A', MAX_TITLE_TEXT).collect();
    assert!(Title::line(fits, r(1, 6), r(1, 2), r(1, 2)).is_ok());
}

#[test]
fn type_has_to_be_some_size() {
    for size in [Rational::ZERO, r(-1, 6)] {
        assert_eq!(
            Title::line("TITLE".into(), size, r(1, 2), r(1, 2)),
            Err(ModelStatus::TypeNotPositive)
        );
    }
}

#[test]
fn a_title_may_be_placed_off_the_frame() {
    // Not a refusal. A card that flies in from the side is placed outside the
    // frame for most of its length, and a model that refused that would make
    // the ordinary case unrepresentable to prevent a mistake nobody makes.
    assert!(Title::line("TITLE".into(), r(1, 6), r(-1, 2), r(3, 2)).is_ok());
}

#[test]
fn a_title_has_nowhere_to_be() {
    // There is nothing to find, so there is nothing to hint at -- and a hint
    // on a title would be inviting somebody to relink it to a file, which
    // would be a different asset the moment it was opened.
    let hint = Location::new(b"/somewhere/title.mov").expect("a hint");
    assert_eq!(
        asset("TITLE").with_location(Some(hint)),
        Err(ModelStatus::NotRecordedMedia)
    );
    assert!(asset("TITLE").location().is_none());
}

#[test]
fn a_title_cannot_be_relinked_through_the_project_either() {
    // The same invariant by the other door, which is where it would have been
    // missed: the library's own relink call goes through `with_location`, and
    // a guard on the asset that the project routed around would be no guard.
    let (mut project, _) = project();
    let id = project.media().iter().next().expect("an asset").0;
    let hint = Location::new(b"/somewhere/title.mov").expect("a hint");
    assert_eq!(
        project.set_media_location(id, Some(hint)),
        Err(ModelStatus::NotRecordedMedia)
    );
    assert!(
        project
            .media()
            .get(id)
            .expect("an asset")
            .location()
            .is_none(),
        "and the refusal changed nothing"
    );
}

#[test]
fn a_recording_is_not_a_title_and_says_so() {
    let recorded = MediaAsset::new(Digest::of(b"footage"), RATE, frames(240)).expect("an asset");
    assert!(recorded.title().is_none());
    assert_eq!(*recorded.source(), MediaSource::Recorded);
    assert!(asset("TITLE").title().is_some());
    assert!(matches!(asset("TITLE").source(), MediaSource::Title(_)));
}

#[test]
fn a_title_clip_trims_and_splits_and_joins_like_any_other() {
    // The claim the whole design rests on. None of these operations was told
    // what a title is, and none of them should need to be.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 40,
            },
        )
        .expect("a cut");
    assert_eq!(clip_at(&project, sequence, 0).duration().ticks(), 40);
    assert_eq!(clip_at(&project, sequence, 1).source_start(), 40);
    project
        .apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: 10,
            },
        )
        .expect("a roll");
    assert_eq!(clip_at(&project, sequence, 0).duration().ticks(), 50);
    project
        .apply(
            sequence,
            Edit::RollCut {
                track: 0,
                boundary: 1,
                by: -10,
            },
        )
        .expect("a roll back");
    project
        .apply(sequence, Edit::JoinItems { track: 0, index: 0 })
        .expect("a join");
    assert_eq!(clip_at(&project, sequence, 0).duration().ticks(), LENGTH);
}

#[test]
fn a_title_clip_carries_everything_a_clip_carries() {
    // A grade, in particular, which is how a title gets a colour: the model
    // has never held one, and a lookup table applied in the encoding it was
    // authored for is a colour with a name on it.
    let (mut project, sequence) = project();
    let grade = Digest::of(b"warm");
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(grade),
            },
        )
        .expect("a grade");
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(
                    media_editor_model::Transform::scaled(
                        r(1, 2),
                        r(1, 2),
                        (Rational::ZERO, Rational::ZERO),
                        media_editor_model::Resampling::Area,
                    )
                    .expect("a transform"),
                ),
            },
        )
        .expect("a transform");
    let clip = clip_at(&project, sequence, 0);
    assert_eq!(clip.grade(), Some(grade));
    assert!(clip.transform().is_some());
}

#[test]
fn a_title_can_be_removed_when_nothing_cuts_from_it() {
    // And not before -- the same rule as any other asset, reached without a
    // line of code that mentions titles.
    let (mut project, sequence) = project();
    let id = project.media().iter().next().expect("an asset").0;
    assert_eq!(project.remove_media(id), Err(ModelStatus::MediaInUse));
    project
        .apply(sequence, Edit::RemoveItem { track: 0, index: 0 })
        .expect("a removal");
    assert!(project.remove_media(id).is_ok());
}

#[test]
fn two_clips_of_one_card_are_two_clips_of_one_asset() {
    // Which is the point of naming a title by what it says. Adding the same
    // card twice is adding it once, so a project with a lower third at the top
    // and the bottom of a programme holds one asset and caches one picture.
    let (mut project, _) = project();
    let again = project.add_media(asset("MEDIAEDTO")).expect("room");
    assert_eq!(project.media().iter().count(), 1, "one asset");
    assert_eq!(
        project.media().get(again).expect("an asset").digest(),
        asset("MEDIAEDTO").digest()
    );
}

#[test]
fn a_card_may_say_more_than_one_thing() {
    let card = Title::new(
        std::vec!["Media Editor".into(), "a cut that keeps".into()],
        r(1, 8),
        r(1, 2),
        r(1, 2),
        media_editor_model::Alignment::Left,
    )
    .expect("a title");
    assert_eq!(card.lines().len(), 2);
    assert_eq!(card.alignment(), media_editor_model::Alignment::Left);
}

#[test]
fn a_blank_line_among_others_is_how_a_card_puts_air_in() {
    // Refusing it would make the ordinary thing unrepresentable to prevent a
    // mistake nobody makes: a card with a gap between two stanzas is a card
    // with a blank line in it.
    assert!(
        Title::new(
            std::vec!["Media Editor".into(), String::new(), "MMXXVI".into()],
            r(1, 8),
            r(1, 2),
            r(1, 2),
            media_editor_model::Alignment::Centre,
        )
        .is_ok()
    );
}

#[test]
fn a_card_where_every_line_is_blank_is_refused() {
    for lines in [
        std::vec![String::new()],
        std::vec![String::new(), String::new()],
        std::vec![],
    ] {
        assert_eq!(
            Title::new(
                lines,
                r(1, 8),
                r(1, 2),
                r(1, 2),
                media_editor_model::Alignment::Centre
            ),
            Err(ModelStatus::EmptyTitle)
        );
    }
}

#[test]
fn a_card_of_more_lines_than_this_describes_is_refused() {
    use media_editor_model::MAX_TITLE_LINES;

    let many: std::vec::Vec<String> =
        core::iter::repeat_n(String::from("A"), MAX_TITLE_LINES + 1).collect();
    assert_eq!(
        Title::new(
            many,
            r(1, 8),
            r(1, 2),
            r(1, 2),
            media_editor_model::Alignment::Centre
        ),
        Err(ModelStatus::TitleTooLong)
    );
    let fits: std::vec::Vec<String> =
        core::iter::repeat_n(String::from("A"), MAX_TITLE_LINES).collect();
    assert!(
        Title::new(
            fits,
            r(1, 8),
            r(1, 2),
            r(1, 2),
            media_editor_model::Alignment::Centre
        )
        .is_ok()
    );
}

#[test]
fn the_lines_and_the_alignment_are_in_the_name() {
    // Two cards that differ only in how their lines sit are two cards, and
    // two cards whose lines *concatenate* the same are two cards -- which is
    // what the per-line length in the digest is for.
    use media_editor_model::Alignment;

    let card = |lines: std::vec::Vec<String>, alignment| {
        Title::new(lines, r(1, 8), r(1, 2), r(1, 2), alignment)
            .expect("a title")
            .digest()
            .expect("a digest")
    };
    let base = card(std::vec!["AB".into(), "C".into()], Alignment::Centre);
    for other in [
        card(std::vec!["AB".into(), "C".into()], Alignment::Left),
        card(std::vec!["AB".into(), "C".into()], Alignment::Right),
        card(std::vec!["A".into(), "BC".into()], Alignment::Centre),
        card(std::vec!["ABC".into()], Alignment::Centre),
        card(
            std::vec!["AB".into(), "C".into(), String::new()],
            Alignment::Centre,
        ),
    ] {
        assert_ne!(base, other);
    }
}
