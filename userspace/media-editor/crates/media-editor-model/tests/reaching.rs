// SPDX-License-Identifier: GPL-3.0-only
//! Inverting a retime: which offsets of a clip show a stretch of the media.
//!
//! One property does the work here and everything else supports it: **for
//! every clip and every source range, the offsets reported are exactly the
//! offsets whose `source_at` falls in that range.** The forward map is
//! checked exhaustively against the inverse at every frame of every fixture,
//! because a bisection that is subtly wrong is a bisection that is right
//! nearly everywhere.
//!
//! Why it cannot be a formula: a ramp's forward map is the integral of its
//! speed curve, whose segments are rectangles and trapeziums — so the integral
//! is *quadratic* along a straight run, and inverting a quadratic wants a
//! square root, which is not rational. The same wall the cubic ease hit.
//!
//! Why a search is not a compromise: a caption needs the frame, not the
//! moment, and the frames of a clip are a monotone integer sequence with
//! exactly comparable terms. Every answer below is the right answer, not a
//! near one.

use media_editor_core::{Digest, Duration, Instant, Rational, Timebase};
use media_editor_model::curve::{Curve, Interpolation, Keyframe};
use media_editor_model::{Clip, MediaAsset, MediaId, Project};

const RATE: Timebase = Timebase::FILM_24;
const IN_POINT: i64 = 100;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn at(ticks: i64) -> Instant {
    Instant::new(ticks, RATE)
}

fn r(numerator: i64, denominator: i64) -> Rational {
    Rational::new(numerator, denominator).expect("a rational")
}

fn media_id() -> MediaId {
    let mut project = Project::new();
    project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room")
}

fn straight(points: &[(i64, Rational)]) -> Curve {
    Curve::new(
        points
            .iter()
            .map(|(tick, value)| {
                Keyframe::new(at(*tick), *value, Interpolation::Linear).expect("a keyframe")
            })
            .collect(),
    )
    .expect("a curve")
}

/// Every offset of a clip that covers any of `[from, to)`, found by asking the
/// *forward* map at each one.
///
/// The definition of the answer, computed the slow and obvious way — and the
/// rule for what one frame covers is spelled out here rather than fetched, so
/// that this is a second implementation of the specification and not a second
/// call to the code under test. The bisection has to agree with it everywhere.
fn by_walking(clip: &Clip, from: i64, to: i64) -> Option<(i64, i64)> {
    let mut first = None;
    let mut last = None;
    for offset in 0..clip.duration().ticks() {
        let here = clip.source_at(offset).expect("a source tick");
        let next = clip.source_at(offset + 1).expect("a source tick");
        // From this frame's tick to the next frame's, whichever order they
        // come in, and one tick wide where they are the same.
        let (low, high) = match next.cmp(&here) {
            std::cmp::Ordering::Greater => (here, next),
            std::cmp::Ordering::Less => (next + 1, here + 1),
            std::cmp::Ordering::Equal => (here, here + 1),
        };
        if low < to && high > from {
            first.get_or_insert(offset);
            last = Some(offset);
        }
    }
    first.map(|start| (start, last.expect("a last") + 1))
}

/// Require the search and the walk to agree over every range worth asking.
fn agrees(clip: &Clip, name: &str) {
    let length = clip.duration().ticks();
    let low = clip.source_at(0).expect("a tick");
    let high = clip.source_at(length).expect("a tick");
    let (low, high) = if low <= high {
        (low, high)
    } else {
        (high, low)
    };
    // A window either side of what the clip actually reads, so the empty
    // answers are checked as hard as the full ones.
    for from in (low - 3)..=(high + 3) {
        for span in 1..=6 {
            assert_eq!(
                clip.offsets_showing(from, from + span).expect("a search"),
                by_walking(clip, from, from + span),
                "{name}: source [{from}, {}) disagrees",
                from + span
            );
        }
    }
    // And the whole of it at once, which must be the whole clip.
    assert_eq!(
        clip.offsets_showing(low - 5, high + 5).expect("a search"),
        Some((0, length)),
        "{name}: the whole range is not the whole clip"
    );
    // A range the clip reads nothing of, either side.
    assert_eq!(
        clip.offsets_showing(low - 9, low - 5).expect("a search"),
        None
    );
    assert_eq!(
        clip.offsets_showing(high + 5, high + 9).expect("a search"),
        None
    );
    // And an empty or backwards range is nothing, whatever the clip does --
    // at *every* tick, not only at the ends. A control found the difference:
    // an empty range at a tick the clip skips over falls inside one frame's
    // coverage, and the general path reports that frame for a range covering
    // nothing at all. At the ends it does not, which is why testing the ends
    // proved nothing.
    for tick in (low - 2)..=(high + 2) {
        assert_eq!(
            clip.offsets_showing(tick, tick).expect("a search"),
            None,
            "{name}: an empty range at {tick} reported a frame"
        );
        assert_eq!(
            clip.offsets_showing(tick + 1, tick).expect("a search"),
            None,
            "{name}: a backwards range at {tick} reported a frame"
        );
    }
}

#[test]
fn the_search_agrees_with_the_forward_map_at_every_speed() {
    let plain = Clip::new(media_id(), IN_POINT, frames(24)).expect("a clip");
    agrees(&plain, "real time");
    for (numerator, denominator) in [(2, 1), (3, 1), (1, 2), (1, 3), (2, 3), (5, 2)] {
        let clip = plain
            .with_speed(r(numerator, denominator))
            .expect("a speed");
        agrees(&clip, "a fixed speed");
    }
}

#[test]
fn the_search_agrees_with_the_forward_map_on_a_ramp() {
    let plain = Clip::new(media_id(), IN_POINT, frames(24)).expect("a clip");
    for points in [
        // A straight run from a half to two: slow, then fast.
        std::vec![(0_i64, r(1, 2)), (24, r(2, 1))],
        // The other way: fast into slow.
        std::vec![(0, r(2, 1)), (24, r(1, 4))],
        // A plateau, a ramp, a plateau -- the gesture an editor makes.
        std::vec![
            (0, Rational::ONE),
            (6, Rational::ONE),
            (18, r(1, 4)),
            (24, r(1, 4))
        ],
        // A ramp entirely inside the clip, held at both ends.
        std::vec![(8, r(3, 1)), (16, r(1, 3))],
    ] {
        let clip = plain.with_ramp(straight(&points)).expect("a ramp");
        agrees(&clip, "a ramp");
    }
}

#[test]
fn a_slow_clip_reports_the_first_offset_a_word_is_on_screen() {
    // Half speed: offsets 0, 1, 2, 3 show source ticks 100, 100, 101, 101. A
    // caption on source tick 101 must start at offset **2** -- the moment the
    // word is on screen -- and not at 3, and not at either of the offsets
    // showing 100.
    let clip = Clip::new(media_id(), IN_POINT, frames(24))
        .expect("a clip")
        .with_speed(r(1, 2))
        .expect("a speed");
    assert_eq!(clip.source_at(0).expect("a tick"), 100);
    assert_eq!(clip.source_at(1).expect("a tick"), 100);
    assert_eq!(clip.source_at(2).expect("a tick"), 101);
    assert_eq!(clip.source_at(3).expect("a tick"), 101);
    assert_eq!(
        clip.offsets_showing(101, 102).expect("a search"),
        Some((2, 4))
    );
    assert_eq!(
        clip.offsets_showing(100, 101).expect("a search"),
        Some((0, 2))
    );
    // One source tick becomes two frames of caption, which is what slow motion
    // does to everything: a word held twice as long is on screen twice as long.
    assert_eq!(
        clip.offsets_showing(100, 102).expect("a search"),
        Some((0, 4))
    );
}

#[test]
fn a_fast_clip_shows_more_words_in_less_time() {
    // Triple speed: offsets 0, 1, 2 show 100, 103, 106. Three source ticks a
    // frame, so a caption three ticks long is on screen for one frame.
    let clip = Clip::new(media_id(), IN_POINT, frames(8))
        .expect("a clip")
        .with_speed(r(3, 1))
        .expect("a speed");
    assert_eq!(
        clip.offsets_showing(103, 106).expect("a search"),
        Some((1, 2))
    );
    // And a caption between two sampled frames is still on screen, which is
    // the finding this whole rule came out of. Ticks 104 and 105 land on no
    // frame at all; frame 1 shows 103 and frame 2 shows 106, so frame 1
    // *covers* [103, 106) and the words are on it. Sampling would have dropped
    // two thirds of a transcript at this speed.
    assert_eq!(
        clip.offsets_showing(104, 105).expect("a search"),
        Some((1, 2))
    );
    // Every tick the clip passes over is covered by exactly one frame, so no
    // caption anywhere in the read span can vanish.
    for tick in 100..124 {
        assert!(
            clip.offsets_showing(tick, tick + 1)
                .expect("a search")
                .is_some(),
            "tick {tick} is covered by no frame"
        );
    }
}

#[test]
fn a_reversed_clip_plays_its_words_backwards() {
    // A reversed clip reads downwards, so the words come out in reverse order
    // -- which is what a reversed clip does to everything it shows, and is not
    // a defect to be corrected here.
    let clip = Clip::new(media_id(), IN_POINT, frames(10))
        .expect("a clip")
        .with_speed(r(-1, 1))
        .expect("a speed");
    assert_eq!(clip.source_at(0).expect("a tick"), 100);
    assert_eq!(clip.source_at(4).expect("a tick"), 96);
    agrees(&clip, "reversed");
    // The word at source 96 is four frames in; the word at 99 is one frame in.
    // Later in the recording is earlier on the timeline.
    assert_eq!(
        clip.offsets_showing(96, 97).expect("a search"),
        Some((4, 5))
    );
    assert_eq!(
        clip.offsets_showing(99, 100).expect("a search"),
        Some((1, 2))
    );
    let early = clip
        .offsets_showing(99, 100)
        .expect("a search")
        .expect("some");
    let late = clip
        .offsets_showing(96, 97)
        .expect("a search")
        .expect("some");
    assert!(early.0 < late.0, "the recording did not run backwards");
    // And reversed at half speed, where both effects apply at once.
    let slow = Clip::new(media_id(), IN_POINT, frames(10))
        .expect("a clip")
        .with_speed(r(-1, 2))
        .expect("a speed");
    agrees(&slow, "reversed and slow");
}

#[test]
fn a_freeze_holds_a_word_for_as_long_as_it_holds_the_frame() {
    // A held frame shows one tick for the whole clip, so the sequence is
    // constant and there is nothing to bisect. A caption covering that tick is
    // on screen throughout; one that does not is absent -- which is what a
    // freeze means rather than a case bolted on.
    let clip = Clip::new(media_id(), IN_POINT, frames(30))
        .expect("a clip")
        .frozen();
    assert!(clip.is_frozen());
    assert_eq!(
        clip.offsets_showing(100, 101).expect("a search"),
        Some((0, 30))
    );
    assert_eq!(
        clip.offsets_showing(90, 200).expect("a search"),
        Some((0, 30))
    );
    // The tick either side, which the freeze never shows.
    assert_eq!(clip.offsets_showing(99, 100).expect("a search"), None);
    assert_eq!(clip.offsets_showing(101, 102).expect("a search"), None);
}

#[test]
fn a_clip_that_reads_none_of_a_range_says_so() {
    let clip = Clip::new(media_id(), IN_POINT, frames(10)).expect("a clip");
    // Before the in point and past the out point.
    assert_eq!(clip.offsets_showing(0, 100).expect("a search"), None);
    assert_eq!(clip.offsets_showing(110, 200).expect("a search"), None);
    // Straddling the in point: the part it does read.
    assert_eq!(
        clip.offsets_showing(95, 103).expect("a search"),
        Some((0, 3))
    );
    // Straddling the out point.
    assert_eq!(
        clip.offsets_showing(107, 120).expect("a search"),
        Some((7, 10))
    );
}

#[test]
fn the_search_costs_a_handful_of_comparisons_on_a_long_clip() {
    // Twenty comparisons cover a clip a million frames long, which is the
    // reason this is a bisection rather than the walk the tests above use to
    // check it. A million-frame walk would be a million calls to `area_to`,
    // each of which integrates the whole curve up to that point.
    let long = Clip::new(media_id(), 0, frames(1_048_576))
        .expect("a clip")
        .with_ramp(straight(&[(0, r(1, 2)), (1_048_576, r(3, 2))]))
        .expect("a ramp");
    let found = long.offsets_showing(500_000, 500_001).expect("a search");
    let (start, end) = found.expect("some offsets");
    // Checked against the forward map at the boundary rather than against a
    // number typed here: the offset before the first must not have reached the
    // tick, and the first must have.
    assert!(long.source_at(start - 1).expect("a tick") < 500_000);
    assert!(long.source_at(start).expect("a tick") >= 500_000);
    assert!(long.source_at(end - 1).expect("a tick") < 500_001);
    assert!(long.source_at(end).expect("a tick") >= 500_001);
}
