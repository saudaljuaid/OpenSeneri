// SPDX-License-Identifier: GPL-3.0-only
//! Tracks, items, and the invariants the types make unrepresentable.

use media_editor_core::{CoreStatus, Duration, Instant, Timebase};
use media_editor_model::media::Digest;
use media_editor_model::{Clip, Item, MediaAsset, MediaId, ModelStatus, Track, TrackKind};

const RATE: Timebase = Timebase::PAL_25;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a non-negative length")
}

fn media_id() -> MediaId {
    // A track knows nothing about the library, so any identifier will do here;
    // the project's own tests exercise the checks that need a real asset.
    let mut project = media_editor_model::Project::new();
    let asset = MediaAsset::new(Digest::new([7; 32]), RATE, frames(10_000)).expect("an asset");
    project.add_media(asset).expect("room in the library")
}

fn clip(source_start: i64, length: i64, media: MediaId) -> Item {
    Item::Clip(Clip::new(media, source_start, frames(length)).expect("a clip"))
}

#[test]
fn item_positions_are_the_sum_of_what_came_before() {
    let media = media_id();
    let mut track = Track::new(TrackKind::Video, RATE);
    track.insert(0, clip(0, 10, media)).expect("an insert");
    track.insert(1, clip(100, 5, media)).expect("an insert");
    track
        .insert(2, Item::gap(frames(3)).expect("a gap"))
        .expect("an insert");
    track.insert(3, clip(200, 7, media)).expect("an insert");

    assert_eq!(track.item_start(0).expect("a start"), Instant::new(0, RATE));
    assert_eq!(
        track.item_start(1).expect("a start"),
        Instant::new(10, RATE)
    );
    assert_eq!(
        track.item_start(2).expect("a start"),
        Instant::new(15, RATE)
    );
    assert_eq!(
        track.item_start(3).expect("a start"),
        Instant::new(18, RATE)
    );
    assert_eq!(track.duration().expect("a length"), frames(25));
    assert_eq!(
        track.item_start(4).expect("the end of the track"),
        Instant::new(25, RATE),
        "one past the last item names where an append would land"
    );
    assert_eq!(track.item_start(5), Err(ModelStatus::UnknownItem));
}

#[test]
fn inserting_ripples_everything_after_it() {
    let media = media_id();
    let mut track = Track::new(TrackKind::Video, RATE);
    track.insert(0, clip(0, 10, media)).expect("an insert");
    track.insert(1, clip(100, 10, media)).expect("an insert");
    assert_eq!(track.item_start(1).expect("a start").ticks(), 10);

    track
        .insert(0, clip(50, 4, media))
        .expect("an insert at the head");
    assert_eq!(track.item_start(1).expect("a start").ticks(), 4);
    assert_eq!(track.item_start(2).expect("a start").ticks(), 14);
    assert_eq!(track.duration().expect("a length"), frames(24));
}

#[test]
fn an_empty_item_is_refused() {
    let media = media_id();
    let mut track = Track::new(TrackKind::Video, RATE);
    assert_eq!(
        Clip::new(media, 0, frames(0)).map(Item::Clip),
        Err(ModelStatus::EmptyItem)
    );
    assert_eq!(Item::gap(frames(0)), Err(ModelStatus::EmptyItem));
    assert_eq!(
        track.insert(1, clip(0, 1, media)),
        Err(ModelStatus::UnknownItem)
    );
}

#[test]
fn an_item_from_another_timebase_is_refused() {
    let media = media_id();
    let mut track = Track::new(TrackKind::Video, RATE);
    let film = Item::Clip(
        Clip::new(
            media,
            0,
            Duration::new(10, Timebase::FILM_24).expect("a length"),
        )
        .expect("a clip"),
    );
    assert_eq!(
        track.insert(0, film),
        Err(ModelStatus::Time(CoreStatus::TimebaseMismatch))
    );
}

#[test]
fn the_item_at_an_instant_is_found_with_its_offset() {
    let media = media_id();
    let mut track = Track::new(TrackKind::Video, RATE);
    track.insert(0, clip(0, 10, media)).expect("an insert");
    track.insert(1, clip(100, 5, media)).expect("an insert");

    assert_eq!(track.item_at(Instant::new(0, RATE)), Ok(Some((0, 0))));
    assert_eq!(track.item_at(Instant::new(9, RATE)), Ok(Some((0, 9))));
    assert_eq!(
        track.item_at(Instant::new(10, RATE)),
        Ok(Some((1, 0))),
        "the boundary belongs to the later item, because ranges are half-open"
    );
    assert_eq!(track.item_at(Instant::new(14, RATE)), Ok(Some((1, 4))));
    assert_eq!(
        track.item_at(Instant::new(15, RATE)),
        Ok(None),
        "the track simply does not reach that far"
    );
    assert_eq!(
        track.item_at(Instant::new(0, Timebase::FILM_24)),
        Err(ModelStatus::Time(CoreStatus::TimebaseMismatch))
    );
}

#[test]
fn splitting_produces_two_pieces_contiguous_in_their_source() {
    let media = media_id();
    let mut track = Track::new(TrackKind::Video, RATE);
    track.insert(0, clip(100, 10, media)).expect("an insert");
    track.split(0, 4).expect("a cut");

    assert_eq!(track.len(), 2);
    let Item::Clip(head) = track.item(0).expect("the head") else {
        panic!("the head is a clip");
    };
    let Item::Clip(tail) = track.item(1).expect("the tail") else {
        panic!("the tail is a clip");
    };
    assert_eq!(head.source_start(), 100);
    assert_eq!(head.duration(), frames(4));
    assert_eq!(tail.source_start(), 104, "the tail continues the head");
    assert_eq!(tail.duration(), frames(6));
    assert_eq!(
        track.duration().expect("a length"),
        frames(10),
        "a cut changes nothing about the track's length"
    );
}

#[test]
fn a_split_outside_the_item_is_refused() {
    let media = media_id();
    let mut track = Track::new(TrackKind::Video, RATE);
    track.insert(0, clip(0, 10, media)).expect("an insert");
    assert_eq!(track.split(0, 0), Err(ModelStatus::SplitOutsideItem));
    assert_eq!(track.split(0, 10), Err(ModelStatus::SplitOutsideItem));
    assert_eq!(track.split(0, -1), Err(ModelStatus::SplitOutsideItem));
    assert_eq!(track.len(), 1, "a refused split changed nothing");
}

#[test]
fn joining_is_the_exact_inverse_of_splitting() {
    let media = media_id();
    let mut track = Track::new(TrackKind::Video, RATE);
    track.insert(0, clip(100, 10, media)).expect("an insert");
    let before = track.clone();
    track.split(0, 4).expect("a cut");
    track.join(0).expect("a join");
    assert_eq!(track, before, "split then join is the identity");
}

#[test]
fn items_that_are_not_contiguous_cannot_be_joined() {
    let media = media_id();
    let mut track = Track::new(TrackKind::Video, RATE);
    // Two clips of the same media, but with a jump in the source between them.
    track.insert(0, clip(0, 5, media)).expect("an insert");
    track.insert(1, clip(100, 5, media)).expect("an insert");
    assert_eq!(track.join(0), Err(ModelStatus::ItemsNotContiguous));

    // A clip and a gap are never contiguous, whatever their lengths.
    let mut mixed = Track::new(TrackKind::Video, RATE);
    mixed.insert(0, clip(0, 5, media)).expect("an insert");
    mixed
        .insert(1, Item::gap(frames(5)).expect("a gap"))
        .expect("an insert");
    assert_eq!(mixed.join(0), Err(ModelStatus::ItemsNotContiguous));
}

#[test]
fn adjacent_gaps_join() {
    let mut track = Track::new(TrackKind::Video, RATE);
    track
        .insert(0, Item::gap(frames(3)).expect("a gap"))
        .expect("an insert");
    track
        .insert(1, Item::gap(frames(4)).expect("a gap"))
        .expect("an insert");
    track.join(0).expect("a join");
    assert_eq!(track.len(), 1);
    assert_eq!(track.item(0).expect("the gap").duration(), frames(7));
}

#[test]
fn removing_ripples_and_returns_what_was_removed() {
    let media = media_id();
    let mut track = Track::new(TrackKind::Video, RATE);
    track.insert(0, clip(0, 10, media)).expect("an insert");
    track.insert(1, clip(100, 5, media)).expect("an insert");
    let removed = track.remove(0).expect("a removal");
    assert_eq!(removed.duration(), frames(10));
    assert_eq!(track.len(), 1);
    assert_eq!(track.item_start(0).expect("a start").ticks(), 0);
    assert_eq!(track.remove(5), Err(ModelStatus::UnknownItem));
}
