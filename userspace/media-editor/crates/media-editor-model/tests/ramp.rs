// SPDX-License-Identifier: GPL-3.0-only
//! A speed that changes over a clip: a ramp.
//!
//! A fixed speed multiplies. A ramp **integrates** — the media position at an
//! offset is the *area* under the speed curve up to that offset, not a
//! multiple of anything. That is the whole of the feature and the whole of its
//! difficulty, and it is why every number in this file is worked out by hand
//! in the comment above the assertion rather than read back out of the code.
//!
//! The area is exact because the shapes it integrates are the two a rational
//! can hold: a hold is a rectangle and a straight run is a trapezium. An ease
//! is refused, and the reason is worth stating twice: the area under a cubic
//! Bézier is exactly rational over a *whole* segment, and finding the
//! parameter at a tick *inside* one means solving a cubic. A clip is asked
//! where it has got to at every tick, so the case that would be exact is never
//! the case that is asked.

use media_editor_core::{Digest, Duration, Instant, Rational, Timebase};
use media_editor_model::curve::{Curve, Interpolation, Keyframe};
use media_editor_model::{
    Clip, Edit, Item, MediaAsset, ModelStatus, Playback, Project, SequenceId, TrackKind,
};

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

/// A curve from `(at, value)` pairs, every one running straight to the next.
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

/// A clip of `length` frames, in at [`IN_POINT`], running on `ramp`.
fn ramped(ramp: Curve, length: i64) -> Clip {
    Clip::new(media_id(), IN_POINT, frames(length))
        .expect("a clip")
        .with_ramp(ramp)
        .expect("a ramp")
}

/// Any media identifier: nothing in this file resolves one.
fn media_id() -> media_editor_model::MediaId {
    let mut project = Project::new();
    project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room")
}

#[test]
fn the_area_under_a_straight_run_is_the_trapezium() {
    // One to a half over ten ticks. A trapezium is the mean of its two ends
    // times its width: (1 + 1/2)/2 x 10 = 3/4 x 10 = 15/2.
    let curve = straight(&[(0, Rational::ONE), (10, r(1, 2))]);
    assert_eq!(curve.area_to(at(10)).expect("an area"), r(15, 2));

    // And part way along it, at four ticks in. The speed there is
    // 1 + (1/2 - 1) x 4/10 = 1 - 1/5 = 4/5, so the area is
    // (1 + 4/5)/2 x 4 = 9/10 x 4 = 18/5.
    assert_eq!(curve.area_to(at(4)).expect("an area"), r(18, 5));

    // Past the last keyframe the curve is held, so the area past it is a
    // rectangle: 15/2 + 1/2 x 10 = 15/2 + 5 = 25/2.
    assert_eq!(curve.area_to(at(20)).expect("an area"), r(25, 2));

    // And at nought there is no area at all, whatever the curve says.
    assert_eq!(curve.area_to(at(0)).expect("an area"), Rational::ZERO);
}

#[test]
fn the_area_under_a_hold_is_the_rectangle() {
    let curve = Curve::new(std::vec![
        Keyframe::new(at(0), r(2, 1), Interpolation::Hold).expect("a keyframe"),
        Keyframe::new(at(10), Rational::ONE, Interpolation::Linear).expect("a keyframe"),
    ])
    .expect("a curve");
    // Held at two for the whole ten ticks: 2 x 5 = 10 at the halfway point,
    // 2 x 10 = 20 at the end of the segment -- the second keyframe's value
    // never gets a say, which is what a hold means.
    assert_eq!(curve.area_to(at(5)).expect("an area"), r(10, 1));
    assert_eq!(curve.area_to(at(10)).expect("an area"), r(20, 1));
    // And then held at one past the end: 20 + 1 x 5 = 25.
    assert_eq!(curve.area_to(at(15)).expect("an area"), r(25, 1));
}

#[test]
fn the_area_before_the_first_keyframe_is_held_too() {
    // The gesture an editor actually makes: full speed, a ramp down, a
    // plateau. The curve says nothing before tick ten, so the clip runs at the
    // first keyframe's value there.
    let curve = straight(&[(10, Rational::ONE), (30, r(1, 4))]);
    // Ten ticks at one: 10.
    assert_eq!(curve.area_to(at(10)).expect("an area"), r(10, 1));
    // Then the trapezium: 10 + (1 + 1/4)/2 x 20 = 10 + 5/8 x 20 = 10 + 25/2.
    assert_eq!(curve.area_to(at(30)).expect("an area"), r(45, 2));
    // Then thirty ticks at a quarter: 45/2 + 30/4 = 45/2 + 15/2 = 30.
    assert_eq!(curve.area_to(at(60)).expect("an area"), r(30, 1));
}

#[test]
fn a_ramp_reads_the_media_the_area_says() {
    // Sixty frames of programme consuming exactly thirty of media, from the
    // curve above. Each answer is the in point plus the floor of the area.
    let held = ramped(straight(&[(10, Rational::ONE), (30, r(1, 4))]), 60);

    // Tick five is inside the held head, so five ticks at one: floor(5) = 5.
    assert_eq!(held.source_at(5).expect("a tick"), IN_POINT + 5);
    // Tick twenty is halfway down the ramp. The speed there is
    // 1 + (1/4 - 1) x 10/20 = 1 - 3/8 = 5/8, so the area is
    // 10 + (1 + 5/8)/2 x 10 = 10 + 13/16 x 10 = 10 + 65/8 = 145/8 = 18.125.
    assert_eq!(held.source_at(20).expect("a tick"), IN_POINT + 18);
    // And the end: thirty ticks of media, exactly, for sixty of programme.
    assert_eq!(held.source_end().expect("a tick"), IN_POINT + 30);
}

#[test]
fn a_ramp_of_one_speed_is_that_speed() {
    // Two ways to say a half, and they must agree at every tick. A curve of
    // one keyframe is held everywhere, so its area to `n` is n/2 -- which is
    // exactly what the fixed mapping floors.
    let source = Clip::new(media_id(), IN_POINT, frames(24)).expect("a clip");
    let fixed = source.with_speed(r(1, 2)).expect("a speed");
    let curved = source
        .with_ramp(Curve::constant(at(0), r(1, 2)).expect("a curve"))
        .expect("a ramp");
    for offset in 0..24 {
        assert_eq!(
            curved.source_at(offset).expect("a tick"),
            fixed.source_at(offset).expect("a tick"),
            "the two ways of saying one speed disagree at {offset}"
        );
    }
    assert_eq!(
        curved.source_end().expect("a tick"),
        fixed.source_end().expect("a tick")
    );
    // They are not the same clip, though, and must not compare as one: a ramp
    // that happens to be flat is still a ramp, and joining the two would keep
    // one and discard the other.
    assert_ne!(curved, fixed);
}

#[test]
fn a_ramp_has_no_one_speed() {
    let held = ramped(straight(&[(0, Rational::ONE), (10, r(1, 2))]), 20);
    assert_eq!(held.speed(), None, "a ramp is not a speed");
    assert!(!held.is_frozen(), "and it is not a freeze either");
    assert!(!held.is_real_time());
    assert!(matches!(held.playback(), Playback::Ramp(_)));
}

#[test]
fn a_ramp_runs_backwards_if_every_keyframe_does() {
    // In at a hundred, running back at one and easing to a half. The area is
    // negative, so the media position falls: at ten ticks the area is
    // -(1 + 1/2)/2 x 10 = -15/2, and the size of that is what gets floored --
    // seven, subtracted from the in point.
    let held = ramped(straight(&[(0, r(-1, 1)), (10, r(-1, 2))]), 11);
    assert_eq!(held.source_at(10).expect("a tick"), IN_POINT - 7);
    // Both ends of what it reads, in order, whichever way it runs.
    assert_eq!(
        held.source_span().expect("a span"),
        (IN_POINT - 7, IN_POINT)
    );
}

#[test]
fn a_ramp_that_turns_around_is_refused() {
    let turning = straight(&[(0, Rational::ONE), (10, r(-1, 1))]);
    assert_eq!(
        Clip::new(media_id(), IN_POINT, frames(20))
            .expect("a clip")
            .with_ramp(turning),
        Err(ModelStatus::SpeedRampChangesDirection)
    );
}

#[test]
fn a_ramp_that_stops_is_refused() {
    // A speed of nought is a freeze, which is a different edit with a
    // different name -- and a ramp that touched nought would stop consuming
    // media without saying so.
    let stopping = straight(&[(0, Rational::ONE), (10, Rational::ZERO)]);
    assert_eq!(
        Clip::new(media_id(), IN_POINT, frames(20))
            .expect("a clip")
            .with_ramp(stopping),
        Err(ModelStatus::SpeedNotUsable)
    );
}

#[test]
fn a_ramp_that_eases_is_refused() {
    let eased = Curve::new(std::vec![
        Keyframe::new(
            at(0),
            Rational::ONE,
            Interpolation::ease_in_out().expect("an ease")
        )
        .expect("a keyframe"),
        Keyframe::new(at(10), r(1, 2), Interpolation::Linear).expect("a keyframe"),
    ])
    .expect("a curve");
    assert_eq!(
        Clip::new(media_id(), IN_POINT, frames(20))
            .expect("a clip")
            .with_ramp(eased.clone()),
        Err(ModelStatus::EaseHasNoExactArea)
    );
    // And the curve itself refuses to be integrated through the ease, which is
    // the refusal the clip's is protecting callers from ever reaching.
    assert_eq!(
        eased.area_to(at(5)),
        Err(ModelStatus::EaseHasNoExactArea),
        "the area under an ease is not a rational"
    );
    // But an ease the walk stops short of is no obstacle: the area up to the
    // first keyframe is a held rectangle and nothing has been eased through.
    assert_eq!(eased.area_to(at(0)), Ok(Rational::ZERO));
}

#[test]
fn a_ramp_that_reads_before_its_media_is_refused() {
    // Twenty frames running backwards at one from an in point of five reaches
    // tick minus fourteen, which is not a tick.
    let backwards = Curve::constant(at(0), r(-1, 1)).expect("a curve");
    assert_eq!(
        Clip::new(media_id(), 5, frames(20))
            .expect("a clip")
            .with_ramp(backwards),
        Err(ModelStatus::SourceBeforeStart)
    );
}

#[test]
fn a_ramp_counted_another_way_is_refused() {
    let elsewhere =
        Curve::constant(Instant::new(0, Timebase::NTSC_30), Rational::ONE).expect("a curve");
    assert_eq!(
        Clip::new(media_id(), IN_POINT, frames(20))
            .expect("a clip")
            .with_ramp(elsewhere),
        Err(ModelStatus::WrongTimebase)
    );
}

#[test]
fn a_cut_through_a_ramp_leaves_the_tail_where_the_head_stopped() {
    // The property a cut promises, and the one a ramp makes hard: the tail
    // does not begin `offset` ticks into the media, it begins wherever the
    // head had got to -- which only the area knows.
    let uncut = Item::Clip(ramped(straight(&[(10, Rational::ONE), (30, r(1, 4))]), 60));
    let (head, tail) = uncut.split(25).expect("a cut");
    let Item::Clip(head) = &head else {
        panic!("a clip");
    };
    let Item::Clip(tail) = &tail else {
        panic!("a clip");
    };
    // Twenty-five ticks in: ten at one, then fifteen of the twenty-tick ramp.
    // The speed at fifteen is 1 - (3/4) x 15/20 = 1 - 9/16 = 7/16, so the area
    // is 10 + (1 + 7/16)/2 x 15 = 10 + 23/32 x 15 = 10 + 345/32, and
    // 345/32 = 10.78125, so the floor of the whole is 20.
    assert_eq!(head.source_end().expect("a tick"), IN_POINT + 20);
    assert_eq!(tail.source_start(), IN_POINT + 20);
    // And the tail's own ramp is the head's, re-based -- so it starts at the
    // speed the head ended at rather than back at the top.
    assert!(matches!(tail.playback(), Playback::Ramp(_)));
}

#[test]
fn a_cut_through_a_ramp_is_undone_by_a_join() {
    let uncut = Item::Clip(ramped(straight(&[(10, Rational::ONE), (30, r(1, 4))]), 60));
    for cut in [1, 12, 25, 41, 59] {
        let (head, tail) = uncut.split(cut).expect("a cut");
        assert!(
            head.continues_into(&tail),
            "a ramp cut at {cut} does not join back"
        );
        assert_eq!(head.join(&tail).expect("a join"), uncut, "cut at {cut}");
    }
}

#[test]
fn two_differently_ramped_halves_do_not_join() {
    // The condition that keeps join the exact inverse of split. Two clips of
    // one shot, adjacent in its media, ramped differently are two shots --
    // joining them would keep the first's ramp and discard the second's.
    let first = ramped(straight(&[(0, Rational::ONE), (30, r(1, 4))]), 30);
    let second = Clip::new(media_id(), first.source_end().expect("a tick"), frames(30))
        .expect("a clip")
        .with_ramp(straight(&[(0, Rational::ONE), (30, r(1, 4))]))
        .expect("a ramp");
    assert!(
        !Item::Clip(first).continues_into(&Item::Clip(second)),
        "a ramp that restarts at the cut is not the one a split would have made"
    );
}

#[test]
fn a_ramped_clip_and_a_fixed_one_do_not_join() {
    let first = ramped(Curve::constant(at(0), Rational::ONE).expect("a curve"), 12);
    let second = Clip::new(media_id(), IN_POINT + 12, frames(12))
        .expect("a clip")
        .with_speed(Rational::ONE)
        .expect("a speed");
    assert!(!Item::Clip(first).continues_into(&Item::Clip(second)));
}

/// A project with one ramped clip on a track of `kind`.
fn project(kind: TrackKind, ramp: Curve, length: i64) -> (Project, SequenceId, Clip) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"footage"), RATE, frames(9_000)).expect("an asset"))
        .expect("room");
    project
        .apply(sequence, Edit::AddTrack { index: 0, kind })
        .expect("a track");
    let clip = Clip::new(media, IN_POINT, frames(length))
        .expect("a clip")
        .with_ramp(ramp)
        .expect("a ramp");
    (project, sequence, clip)
}

#[test]
fn a_ramp_goes_on_through_an_edit_and_comes_back_off() {
    let ramp = straight(&[(10, Rational::ONE), (30, r(1, 4))]);
    let (mut project, sequence, clip) = project(TrackKind::Video, ramp.clone(), 60);
    let plain = Clip::new(clip.media(), IN_POINT, frames(60)).expect("a clip");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(plain),
            },
        )
        .expect("a clip");
    project
        .apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::Ramp(ramp),
            },
        )
        .expect("a ramp");
    let Item::Clip(held) = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    assert_eq!(held.source_end().expect("a tick"), IN_POINT + 30);
    // And undo puts back the real time it was playing at, which is what the
    // edit handed over as its inverse.
    project.undo(sequence).expect("an undo");
    let Item::Clip(back) = project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .item(0)
        .expect("an item")
    else {
        panic!("a clip");
    };
    assert!(back.is_real_time(), "undo did not put the speed back");
}

#[test]
fn sound_cannot_be_ramped() {
    // For the reason it cannot be retimed: a resampler needs a filter somebody
    // chose and a decision about pitch, and playing the samples at the wrong
    // rate in the meantime would be an answer nobody asked for.
    let ramp = straight(&[(0, Rational::ONE), (10, r(1, 2))]);
    let (mut project, sequence, clip) = project(TrackKind::Audio, ramp.clone(), 20);
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(clip.media(), IN_POINT, frames(20)).expect("a clip")),
            },
        )
        .expect("a clip");
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::Ramp(ramp),
            },
        ),
        Err(ModelStatus::SoundCannotBeRetimed)
    );
}

#[test]
fn a_ramp_that_reads_past_the_end_of_its_media_is_refused() {
    // The upper bound is the library's, because only the library knows how
    // long the asset is. A clip of sixty frames ramping from one up to four
    // reads (1 + 4)/2 x 60 = 150 frames of a hundred-frame asset.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("room");
    let media = project
        .add_media(MediaAsset::new(Digest::of(b"short"), RATE, frames(100)).expect("an asset"))
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
                item: Item::Clip(Clip::new(media, 0, frames(60)).expect("a clip")),
            },
        )
        .expect("a clip");
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetClipPlayback {
                track: 0,
                index: 0,
                playback: Playback::Ramp(straight(&[(0, Rational::ONE), (60, r(4, 1))])),
            },
        ),
        Err(ModelStatus::SourceAfterEnd)
    );
}

#[test]
fn a_ramp_whose_ease_lies_past_the_clip_is_still_refused() {
    // The half of the ease refusal that the obvious test cannot see. This
    // clip is twenty frames long and the ease is on the keyframe at fifty, so
    // no tick of it is ever integrated through one -- `area_to` would answer
    // happily at every offset the clip has. Accepting it would leave a clip
    // that reads correctly until somebody lengthens it, and then refuses at a
    // distance from the edit that caused it.
    let eased = Curve::new(std::vec![
        Keyframe::new(at(0), Rational::ONE, Interpolation::Linear).expect("a keyframe"),
        Keyframe::new(
            at(50),
            r(1, 2),
            Interpolation::ease_in_out().expect("an ease")
        )
        .expect("a keyframe"),
        Keyframe::new(at(100), r(1, 4), Interpolation::Linear).expect("a keyframe"),
    ])
    .expect("a curve");
    // The area at the clip's last tick walks only the first segment, which is
    // straight -- so the curve itself has no complaint to make.
    assert!(
        eased.area_to(at(19)).is_ok(),
        "the ease is past where this clip ever reaches"
    );
    assert_eq!(
        Clip::new(media_id(), IN_POINT, frames(20))
            .expect("a clip")
            .with_ramp(eased),
        Err(ModelStatus::EaseHasNoExactArea)
    );
}
