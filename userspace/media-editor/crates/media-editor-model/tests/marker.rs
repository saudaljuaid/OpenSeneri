// SPDX-License-Identifier: GPL-3.0-only
//! Timeline marker behavior. Markers are editing metadata: they do not render,
//! composite, or alter clips.

use media_editor_core::{Digest, Duration, Instant, Timebase};
use media_editor_model::{
    Clip, Edit, Item, MAX_MARKER_TEXT, MediaAsset, ModelStatus, Project, SequenceId, TrackKind,
};

const RATE: Timebase = Timebase::FILM_24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn at(tick: i64) -> Instant {
    Instant::new(tick, RATE)
}

fn note(text: &str) -> std::string::String {
    std::string::String::from(text)
}

fn project() -> (Project, SequenceId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
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
                item: Item::Clip(Clip::new(media, 100, frames(200)).expect("a clip")),
            },
        )
        .expect("a clip");
    project.forget_history();
    (project, sequence)
}

fn texts(project: &Project, sequence: SequenceId) -> std::vec::Vec<(i64, std::string::String)> {
    project
        .sequence(sequence)
        .expect("a sequence")
        .markers()
        .iter()
        .map(|held| (held.at().ticks(), note(held.text())))
        .collect()
}

#[test]
fn a_note_goes_where_it_was_put_and_says_what_it_said() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddMarker {
                at: at(40),
                text: note("the sync drifts here"),
            },
        )
        .expect("a marker");
    assert_eq!(
        texts(&project, sequence),
        std::vec![(40, note("the sync drifts here"))]
    );
    assert_eq!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .marker_at(at(40))
            .expect("a marker")
            .text(),
        "the sync drifts here"
    );
    assert!(
        project
            .sequence(sequence)
            .expect("a sequence")
            .marker_at(at(41))
            .is_none()
    );
}

#[test]
fn a_note_with_nothing_written_on_it_is_a_note() {
    // The commonest kind: somebody pressed the key to mark a spot and will come
    // back to it. Refusing that would refuse the gesture the feature is for.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddMarker {
                at: at(10),
                text: note(""),
            },
        )
        .expect("a marker");
    assert_eq!(texts(&project, sequence), std::vec![(10, note(""))]);
}

#[test]
fn the_notes_come_back_in_time_order_whatever_order_they_were_written_in() {
    // Which means the list's order carries no information a caller has to
    // preserve, and two projects with the same notes have the same list
    // (R-4.5).
    let (mut project, sequence) = project();
    for tick in [90_i64, 10, 50, 30] {
        project
            .apply(
                sequence,
                Edit::AddMarker {
                    at: at(tick),
                    text: note("x"),
                },
            )
            .expect("a marker");
    }
    assert_eq!(
        texts(&project, sequence)
            .iter()
            .map(|(tick, _)| *tick)
            .collect::<std::vec::Vec<_>>(),
        std::vec![10, 30, 50, 90]
    );
}

#[test]
fn two_notes_at_one_instant_are_refused() {
    // Two markers at an instant is the same nothing as none: neither can be
    // named, moved or removed without saying which, and "which" is exactly
    // what an instant was going to answer. The same decision a curve makes
    // about keyframes.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddMarker {
                at: at(40),
                text: note("first"),
            },
        )
        .expect("a marker");
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::AddMarker {
                    at: at(40),
                    text: note("second"),
                },
            )
            .expect_err("a refusal"),
        ModelStatus::MarkerExists
    );
    assert_eq!(
        texts(&project, sequence),
        std::vec![(40, note("first"))],
        "the refused marker overwrote the one that was there"
    );
}

#[test]
fn a_note_before_the_programme_starts_is_refused() {
    let (mut project, sequence) = project();
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::AddMarker {
                    at: at(-1),
                    text: note("x"),
                },
            )
            .expect_err("a refusal"),
        ModelStatus::MarkerBeforeStart
    );
}

#[test]
fn a_note_longer_than_the_bound_is_refused() {
    let (mut project, sequence) = project();
    let long: std::string::String = core::iter::repeat_n('x', MAX_MARKER_TEXT + 1).collect();
    assert_eq!(
        project
            .apply(
                sequence,
                Edit::AddMarker {
                    at: at(0),
                    text: long,
                },
            )
            .expect_err("a refusal"),
        ModelStatus::MarkerTextTooLong
    );
    // And exactly the bound is inside it, which is the mutation that turns the
    // longest legal note into the shortest refused one.
    let longest: std::string::String = core::iter::repeat_n('x', MAX_MARKER_TEXT).collect();
    project
        .apply(
            sequence,
            Edit::AddMarker {
                at: at(0),
                text: longest,
            },
        )
        .expect("a marker exactly as long as the bound allows");
}

#[test]
fn the_bound_counts_characters_rather_than_bytes() {
    // A bound in bytes is a bound that means something different in every
    // language. This one is the same note either way round.
    let (mut project, sequence) = project();
    let wide: std::string::String = core::iter::repeat_n('é', MAX_MARKER_TEXT).collect();
    assert!(
        wide.len() > MAX_MARKER_TEXT,
        "the fixture is not wider in bytes than in characters, so it cannot \
         tell the two bounds apart"
    );
    project
        .apply(
            sequence,
            Edit::AddMarker {
                at: at(0),
                text: wide,
            },
        )
        .expect("a marker of the same length in a language that spells wider");
}

#[test]
fn taking_a_note_off_gives_back_what_it_said() {
    // The inverse carries the text, because nothing else remembers it once the
    // sequence no longer has it.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddMarker {
                at: at(40),
                text: note("check the grade"),
            },
        )
        .expect("a marker");
    project.forget_history();

    project
        .apply(sequence, Edit::RemoveMarker { at: at(40) })
        .expect("removed");
    assert!(texts(&project, sequence).is_empty());

    project.undo(sequence).expect("undone");
    assert_eq!(
        texts(&project, sequence),
        std::vec![(40, note("check the grade"))],
        "undoing a removal put back a marker that had forgotten what it said"
    );
}

#[test]
fn taking_a_note_off_an_instant_that_has_none_is_refused() {
    let (mut project, sequence) = project();
    assert_eq!(
        project
            .apply(sequence, Edit::RemoveMarker { at: at(40) })
            .expect_err("a refusal"),
        ModelStatus::NoSuchMarker
    );
}

#[test]
fn a_note_moves_in_one_edit_and_moves_back() {
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddMarker {
                at: at(40),
                text: note("here"),
            },
        )
        .expect("a marker");
    project.forget_history();

    project
        .apply(
            sequence,
            Edit::MoveMarker {
                from: at(40),
                to: at(90),
            },
        )
        .expect("moved");
    assert_eq!(texts(&project, sequence), std::vec![(90, note("here"))]);

    project.undo(sequence).expect("undone");
    assert_eq!(texts(&project, sequence), std::vec![(40, note("here"))]);
}

#[test]
fn a_move_onto_an_occupied_instant_is_refused_and_writes_nothing() {
    // R-1.4. A move is a removal and an addition, and the addition can refuse
    // -- so the removal has to be put back rather than left as a marker
    // quietly deleted by a move that did not happen.
    let (mut project, sequence) = project();
    for (tick, text) in [(40_i64, "first"), (90, "second")] {
        project
            .apply(
                sequence,
                Edit::AddMarker {
                    at: at(tick),
                    text: note(text),
                },
            )
            .expect("a marker");
    }
    project.forget_history();
    let before = project.clone();

    assert_eq!(
        project
            .apply(
                sequence,
                Edit::MoveMarker {
                    from: at(40),
                    to: at(90),
                },
            )
            .expect_err("a refusal"),
        ModelStatus::MarkerExists
    );
    assert_eq!(
        project.sequence(sequence).expect("a sequence"),
        before.sequence(sequence).expect("a sequence"),
        "a refused move deleted the marker it was moving"
    );
    assert_eq!(
        project.undo(sequence).expect_err("a refusal"),
        ModelStatus::NothingToDo,
        "a refused edit took a place in the history"
    );
}

#[test]
fn a_note_does_not_move_when_an_item_ripples() {
    // The decision, asserted. A marker names a moment of the finished piece,
    // and an unrelated shot getting longer must not move it away from the
    // thing it is about.
    //
    // The opposite decision -- a marker that belongs to a clip and travels
    // with it -- is a different feature with a different name.
    let (mut project, sequence) = project();
    project
        .apply(
            sequence,
            Edit::AddMarker {
                at: at(150),
                text: note("the cut lands here"),
            },
        )
        .expect("a marker");
    project
        .apply(
            sequence,
            Edit::SetItemDuration {
                track: 0,
                index: 0,
                duration: frames(80),
            },
        )
        .expect("a ripple trim");

    assert_eq!(
        texts(&project, sequence),
        std::vec![(150, note("the cut lands here"))],
        "a note moved because something else on the timeline did"
    );
}

#[test]
fn a_note_belongs_to_its_own_sequence() {
    // Two sequences, one note each, and neither sees the other's.
    let (mut project, first) = project();
    let second = project.add_sequence(RATE).expect("room");
    project
        .apply(
            first,
            Edit::AddMarker {
                at: at(40),
                text: note("on the first"),
            },
        )
        .expect("a marker");
    project
        .apply(
            second,
            Edit::AddMarker {
                at: at(40),
                text: note("on the second"),
            },
        )
        .expect("a marker");

    assert_eq!(
        texts(&project, first),
        std::vec![(40, note("on the first"))]
    );
    assert_eq!(
        texts(&project, second),
        std::vec![(40, note("on the second"))]
    );
}
