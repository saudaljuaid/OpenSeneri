// SPDX-License-Identifier: GPL-3.0-only
//! The media library: one asset per digest, and where its bytes were seen.
//!
//! The first of those is not a tidiness rule. Two identifiers naming one
//! digest quietly falsified the conform round trip — an export writes the
//! digest, an import looks it up, and with two candidates it finds the first,
//! so a sequence cutting the same footage under two identifiers came back
//! pointing at one of them with nothing reported as lost.

use media_editor_core::{Digest, Duration, Timebase};
use media_editor_model::{Location, MAX_LOCATION_BYTES, MediaAsset, ModelStatus, Project};

const RATE: Timebase = Timebase::FILM_24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a non-negative length")
}

fn asset(tag: &[u8], length: i64) -> MediaAsset {
    MediaAsset::new(Digest::of(tag), RATE, frames(length)).expect("an asset")
}

#[test]
fn the_same_content_added_twice_is_one_asset() {
    let mut project = Project::new();
    let one = project.add_media(asset(b"same", 100)).expect("room");
    let two = project.add_media(asset(b"same", 100)).expect("room");
    assert_eq!(one, two, "content addressing means one asset per digest");
    assert_eq!(project.media().len(), 1);
}

#[test]
fn different_content_is_different_assets() {
    let mut project = Project::new();
    let one = project.add_media(asset(b"one", 100)).expect("room");
    let two = project.add_media(asset(b"two", 100)).expect("room");
    assert_ne!(one, two);
    assert_eq!(project.media().len(), 2);
}

#[test]
fn the_same_content_at_two_lengths_is_refused() {
    // The same bytes cannot be two durations. Silently keeping the first would
    // mean a project whose record of an asset depends on which order two files
    // were opened in.
    let mut project = Project::new();
    project.add_media(asset(b"same", 100)).expect("room");
    assert_eq!(
        project.add_media(asset(b"same", 200)),
        Err(ModelStatus::MediaContradiction)
    );
}

#[test]
fn the_same_content_in_two_timebases_is_refused() {
    let mut project = Project::new();
    project.add_media(asset(b"same", 100)).expect("room");
    let other = MediaAsset::new(
        Digest::of(b"same"),
        Timebase::PAL_25,
        Duration::new(100, Timebase::PAL_25).expect("a length"),
    )
    .expect("an asset");
    assert_eq!(
        project.add_media(other),
        Err(ModelStatus::MediaContradiction)
    );
}

#[test]
fn a_hint_is_not_part_of_what_makes_an_asset_the_same() {
    // The same content found in a second place is the same content. Adding it
    // again with a different hint gives back the identifier it already had,
    // and does *not* rewrite the record — moving a hint is its own operation,
    // and doing it as a side effect of opening a file would edit a project
    // nobody asked to edit.
    let mut project = Project::new();
    let first = Location::new(b"/reels/a.sprw").expect("a hint");
    let one = project
        .add_media(
            asset(b"same", 100)
                .with_location(Some(first.clone()))
                .expect("a hint"),
        )
        .expect("room");
    let two = project
        .add_media(
            asset(b"same", 100)
                .with_location(Some(Location::new(b"/elsewhere/a.sprw").expect("a hint")))
                .expect("a hint"),
        )
        .expect("room");
    assert_eq!(one, two);
    assert_eq!(
        project.media().get(one).expect("the asset").location(),
        Some(&first),
        "the record kept the hint it had"
    );
}

#[test]
fn a_hint_moves_and_gives_back_the_one_it_replaced() {
    let mut project = Project::new();
    let id = project.add_media(asset(b"same", 100)).expect("room");
    assert_eq!(project.media().get(id).expect("the asset").location(), None);

    let first = Location::new(b"/reels/a.sprw").expect("a hint");
    let previous = project
        .set_media_location(id, Some(first.clone()))
        .expect("an asset");
    assert_eq!(previous, None);
    assert_eq!(
        project.media().get(id).expect("the asset").location(),
        Some(&first)
    );

    let second = Location::new(b"/archive/a.sprw").expect("a hint");
    let previous = project
        .set_media_location(id, Some(second))
        .expect("an asset");
    assert_eq!(
        previous,
        Some(first),
        "the old hint comes back, which is the whole of what undo would do"
    );
}

#[test]
fn moving_a_hint_does_not_move_the_identity() {
    // Relinking is a hint moving. Pointing a clip at different bytes is
    // different media, and there is no operation here that swaps one for the
    // other while keeping the name.
    let mut project = Project::new();
    let id = project.add_media(asset(b"same", 100)).expect("room");
    let before = project.media().get(id).expect("the asset").digest();
    project
        .set_media_location(id, Some(Location::new(b"/moved").expect("a hint")))
        .expect("an asset");
    assert_eq!(
        project.media().get(id).expect("the asset").digest(),
        before,
        "the digest is what the media is"
    );
    assert_eq!(project.find_media(before), Some(id));
}

#[test]
fn a_hint_that_says_nothing_is_refused() {
    assert_eq!(Location::new(b""), Err(ModelStatus::EmptyLocation));
}

#[test]
fn a_hint_longer_than_the_bound_is_refused() {
    assert!(Location::new(&vec![b'a'; MAX_LOCATION_BYTES]).is_ok());
    assert_eq!(
        Location::new(&vec![b'a'; MAX_LOCATION_BYTES + 1]),
        Err(ModelStatus::CapacityExhausted)
    );
}

#[test]
fn a_hint_is_bytes_rather_than_text() {
    // A path is whatever the platform says it is, and Phipia's is not decided.
    // Refusing to interpret it is what keeps this crate free of the operating
    // system -- so a hint that is not valid text is still a hint.
    let held = Location::new(&[0xFF, 0xFE, b'/', 0x80]).expect("a hint");
    assert_eq!(held.bytes(), &[0xFF, 0xFE, b'/', 0x80]);
}

#[test]
fn setting_a_hint_on_an_asset_that_is_gone_is_refused() {
    // The store refuses a *stale* identifier rather than an unknown one, and
    // the distinction is worth keeping: the identifier was real, the asset was
    // removed, and saying "unknown" would suggest the caller made it up.
    let mut project = Project::new();
    let id = project.add_media(asset(b"same", 100)).expect("room");
    project.remove_media(id).expect("a removal");
    assert_eq!(
        project.set_media_location(id, None),
        Err(ModelStatus::Time(
            media_editor_core::CoreStatus::StaleIdentifier
        ))
    );
}
