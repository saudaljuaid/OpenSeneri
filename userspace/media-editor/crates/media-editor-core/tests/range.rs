// SPDX-License-Identifier: GPL-3.0-only
//! Half-open ranges: the convention, and the edges it makes unambiguous.

use media_editor_core::{CoreStatus, Duration, Instant, TimeRange, Timebase};

fn pal(ticks: i64) -> Instant {
    Instant::new(ticks, Timebase::PAL_25)
}

fn frames(count: i64) -> Duration {
    Duration::new(count, Timebase::PAL_25).expect("a non-negative length")
}

#[test]
fn a_range_excludes_its_end() {
    let range = TimeRange::new(pal(10), frames(5)).expect("a range");
    assert_eq!(range.end().expect("an end"), pal(15));
    assert_eq!(range.contains(pal(10)), Ok(true), "the start is inside");
    assert_eq!(
        range.contains(pal(14)),
        Ok(true),
        "the last frame is inside"
    );
    assert_eq!(range.contains(pal(15)), Ok(false), "the end is outside");
    assert_eq!(range.contains(pal(9)), Ok(false));
}

#[test]
fn an_empty_range_contains_nothing() {
    let empty = TimeRange::empty_at(pal(7));
    assert!(empty.is_empty());
    assert_eq!(empty.contains(pal(7)), Ok(false));
    assert_eq!(empty.end().expect("an end"), pal(7));
}

#[test]
fn a_range_built_between_two_instants_spans_them_half_open() {
    let range = TimeRange::between(pal(4), pal(9)).expect("a range");
    assert_eq!(range.duration().ticks(), 5);
    assert_eq!(range.contains(pal(8)), Ok(true));
    assert_eq!(range.contains(pal(9)), Ok(false));
}

#[test]
fn a_backwards_range_is_refused() {
    assert_eq!(
        TimeRange::between(pal(9), pal(4)),
        Err(CoreStatus::RangeMalformed)
    );
}

#[test]
fn ranges_in_different_timebases_do_not_combine() {
    let pal_range = TimeRange::new(pal(0), frames(10)).expect("a range");
    let film_range = TimeRange::new(
        Instant::new(0, Timebase::FILM_24),
        Duration::new(10, Timebase::FILM_24).expect("a length"),
    )
    .expect("a range");
    assert_eq!(
        pal_range.intersection(film_range),
        Err(CoreStatus::TimebaseMismatch)
    );
    assert_eq!(
        TimeRange::new(
            pal(0),
            Duration::new(1, Timebase::FILM_24).expect("a length")
        ),
        Err(CoreStatus::TimebaseMismatch)
    );
}

#[test]
fn touching_ranges_do_not_overlap() {
    // This is the whole reason for half-open ranges: two adjacent clips share
    // an instant in a closed convention and would be judged to overlap.
    let first = TimeRange::new(pal(0), frames(10)).expect("a range");
    let second = TimeRange::new(pal(10), frames(10)).expect("a range");
    assert_eq!(first.intersects(second), Ok(false));
    assert!(
        first
            .intersection(second)
            .expect("a result")
            .expect("an empty range")
            .is_empty()
    );
}

#[test]
fn overlapping_ranges_intersect_where_they_overlap() {
    let first = TimeRange::new(pal(0), frames(10)).expect("a range");
    let second = TimeRange::new(pal(7), frames(10)).expect("a range");
    let overlap = first
        .intersection(second)
        .expect("a result")
        .expect("an overlap");
    assert_eq!(overlap.start(), pal(7));
    assert_eq!(overlap.duration().ticks(), 3);
    assert_eq!(first.intersects(second), Ok(true));
    assert_eq!(
        second.intersection(first).expect("a result"),
        Some(overlap),
        "intersection does not depend on the order of its arguments"
    );
}

#[test]
fn disjoint_ranges_have_no_intersection() {
    let first = TimeRange::new(pal(0), frames(5)).expect("a range");
    let second = TimeRange::new(pal(20), frames(5)).expect("a range");
    assert_eq!(first.intersection(second), Ok(None));
    assert_eq!(first.intersects(second), Ok(false));
}

#[test]
fn a_range_converts_when_both_edges_are_exact() {
    let second_of_pal = TimeRange::new(pal(25), frames(25)).expect("a range");
    let as_film = second_of_pal
        .convert(Timebase::FILM_24)
        .expect("both edges land on film frames");
    assert_eq!(as_film.start().ticks(), 24);
    assert_eq!(as_film.duration().ticks(), 24);

    let ragged = TimeRange::new(pal(1), frames(25)).expect("a range");
    assert_eq!(
        ragged.convert(Timebase::FILM_24),
        Err(CoreStatus::InexactConversion)
    );
}
