// SPDX-License-Identifier: GPL-3.0-only
//! Timecode, including the drop-frame arithmetic editors get wrong.
//!
//! The landmark values below are the ones a colourist can check by hand, and
//! the exhaustive sweeps prove there is no frame in a day where the label and
//! the count disagree.

use media_editor_core::{CoreStatus, Instant, Timebase, Timecode};

/// Frames in twenty-four hours at a nominal rate and counting style.
fn frames_per_day(nominal_rate: u32, drop_frame: bool) -> i64 {
    let dropped = if drop_frame {
        i64::from(nominal_rate / 15)
    } else {
        0
    };
    (i64::from(nominal_rate) * 600 - 9 * dropped) * 6 * 24
}

#[test]
fn a_day_is_the_expected_number_of_frames() {
    assert_eq!(frames_per_day(30, true), 2_589_408, "29.97 drop-frame");
    assert_eq!(frames_per_day(30, false), 2_592_000, "30 non-drop");
    assert_eq!(frames_per_day(24, false), 2_073_600);
    assert_eq!(frames_per_day(25, false), 2_160_000);
    assert_eq!(frames_per_day(60, true), 5_178_816, "59.94 drop-frame");
}

#[test]
fn the_drop_frame_landmarks_are_exact() {
    // The frame that carries 01:00:00;00 at 29.97 is 107,892, not 108,000.
    // Every hour of drop-frame counting skips 108 labels: two a minute for
    // fifty-four of the sixty minutes.
    let hour = Timecode::new(1, 0, 0, 0, 30, true).expect("a legal label");
    assert_eq!(hour.to_frame_number(), Ok(107_892));

    // The first minute is whole; the second begins at ;02.
    let minute = Timecode::from_frame_number(1800, 30, true).expect("a frame");
    assert_eq!(minute.to_string(), "00:01:00;02");

    // Every tenth minute is whole again, so ;00 exists there.
    let tenth = Timecode::from_frame_number(17_982, 30, true).expect("a frame");
    assert_eq!(tenth.to_string(), "00:10:00;00");
    assert_eq!(tenth.to_frame_number(), Ok(17_982));

    // The frame before it is the last of the ninth minute.
    let before = Timecode::from_frame_number(17_981, 30, true).expect("a frame");
    assert_eq!(before.to_string(), "00:09:59;29");
}

#[test]
fn the_non_drop_landmarks_are_exact() {
    let hour = Timecode::new(1, 0, 0, 0, 30, false).expect("a legal label");
    assert_eq!(hour.to_frame_number(), Ok(108_000));
    assert_eq!(hour.to_string(), "01:00:00:00", "a colon, not a semicolon");

    let film_hour = Timecode::new(1, 0, 0, 0, 24, false).expect("a legal label");
    assert_eq!(film_hour.to_frame_number(), Ok(86_400));
}

#[test]
fn a_skipped_label_is_refused_rather_than_moved() {
    // No frame carries 00:01:00;00 at 29.97 drop-frame. Returning a
    // neighbouring frame would be repair; refusing it is the rule (R-1.3).
    assert_eq!(
        Timecode::new(0, 1, 0, 0, 30, true),
        Err(CoreStatus::TimecodeMalformed)
    );
    assert_eq!(
        Timecode::new(0, 1, 0, 1, 30, true),
        Err(CoreStatus::TimecodeMalformed)
    );
    // The second of that minute exists.
    assert!(Timecode::new(0, 1, 0, 2, 30, true).is_ok());
    // And the tenth minute keeps its first two labels.
    assert!(Timecode::new(0, 10, 0, 0, 30, true).is_ok());
    assert!(Timecode::new(0, 20, 0, 1, 30, true).is_ok());
}

#[test]
fn malformed_fields_are_refused() {
    assert_eq!(
        Timecode::new(24, 0, 0, 0, 25, false),
        Err(CoreStatus::TimecodeMalformed)
    );
    assert_eq!(
        Timecode::new(0, 60, 0, 0, 25, false),
        Err(CoreStatus::TimecodeMalformed)
    );
    assert_eq!(
        Timecode::new(0, 0, 60, 0, 25, false),
        Err(CoreStatus::TimecodeMalformed)
    );
    assert_eq!(
        Timecode::new(0, 0, 0, 25, 25, false),
        Err(CoreStatus::TimecodeMalformed),
        "frame 25 does not exist at a nominal 25"
    );
}

#[test]
fn unsupported_rates_and_styles_are_refused() {
    assert_eq!(
        Timecode::new(0, 0, 0, 0, 23, false),
        Err(CoreStatus::UnsupportedTimecodeRate)
    );
    assert_eq!(
        Timecode::new(0, 0, 0, 0, 25, true),
        Err(CoreStatus::DropFrameUnavailable),
        "PAL has no drop-frame counting because it does not need one"
    );
    assert_eq!(
        Timecode::new(0, 0, 0, 0, 24, true),
        Err(CoreStatus::DropFrameUnavailable)
    );
}

#[test]
fn frames_outside_the_day_are_refused() {
    assert_eq!(
        Timecode::from_frame_number(-1, 25, false),
        Err(CoreStatus::TimecodeOutOfRange)
    );
    assert_eq!(
        Timecode::from_frame_number(frames_per_day(25, false), 25, false),
        Err(CoreStatus::TimecodeOutOfRange)
    );
    assert!(Timecode::from_frame_number(frames_per_day(25, false) - 1, 25, false).is_ok());
    assert_eq!(
        Timecode::from_frame_number(frames_per_day(30, true), 30, true),
        Err(CoreStatus::TimecodeOutOfRange)
    );
}

/// Every frame of a whole day round-trips, and no two frames share a label.
fn sweep_a_whole_day(nominal_rate: u32, drop_frame: bool) {
    let total = frames_per_day(nominal_rate, drop_frame);
    let mut previous_label: Option<Timecode> = None;
    for frame in 0..total {
        let label = Timecode::from_frame_number(frame, nominal_rate, drop_frame)
            .expect("every frame in a day has a label");
        assert_eq!(
            label.to_frame_number(),
            Ok(frame),
            "the label of frame {frame} at {nominal_rate} must name it back"
        );
        if let Some(previous) = previous_label {
            assert_ne!(previous, label, "two frames share one label at {frame}");
        }
        previous_label = Some(label);
    }
    // The last frame of a day is the last label of the twenty-third hour.
    let last =
        Timecode::from_frame_number(total - 1, nominal_rate, drop_frame).expect("the last frame");
    assert_eq!(last.hours(), 23);
    assert_eq!(last.minutes(), 59);
    assert_eq!(last.seconds(), 59);
    assert_eq!(u32::from(last.frames()), nominal_rate - 1);
}

#[test]
fn every_drop_frame_label_in_a_day_round_trips() {
    sweep_a_whole_day(30, true);
    sweep_a_whole_day(60, true);
}

#[test]
fn every_non_drop_label_in_a_day_round_trips() {
    sweep_a_whole_day(24, false);
    sweep_a_whole_day(25, false);
    sweep_a_whole_day(30, false);
    sweep_a_whole_day(48, false);
    sweep_a_whole_day(50, false);
    sweep_a_whole_day(60, false);
}

#[test]
fn drop_frame_labels_track_wall_clock_time() {
    // The point of drop-frame: after an hour of 30000/1001 frames, the label
    // reads one hour. The count is 107,892 frames, which is 107,892 * 1001 /
    // 30,000 seconds - within half a frame of 3,600 seconds, where non-drop
    // counting would be 3.6 seconds adrift.
    let hour_label = Timecode::new(1, 0, 0, 0, 30, true).expect("a legal label");
    let frame = hour_label.to_frame_number().expect("a frame number");
    let seconds = Instant::new(frame, Timebase::NTSC_30)
        .seconds()
        .expect("exact seconds");
    let one_hour = media_editor_core::Rational::from_integer(3600);
    let drift = seconds.checked_sub(one_hour).expect("a difference");
    // |drift| < 1/30 of a second.
    let bound = media_editor_core::Rational::new(1, 30).expect("a frame of time");
    let negated = bound.checked_neg().expect("a negative bound");
    assert!(drift < bound && drift > negated, "drift was {drift}");
}

#[test]
fn instants_label_themselves_by_their_timebase() {
    let ntsc = Instant::new(107_892, Timebase::NTSC_30);
    assert_eq!(
        Timecode::from_instant(ntsc).expect("a label").to_string(),
        "01:00:00;00",
        "30000/1001 is labelled drop-frame"
    );

    let pal = Instant::new(90_000, Timebase::PAL_25);
    assert_eq!(
        Timecode::from_instant(pal).expect("a label").to_string(),
        "01:00:00:00"
    );

    let film = Instant::new(86_400, Timebase::NTSC_FILM);
    assert_eq!(
        Timecode::from_instant(film).expect("a label").to_string(),
        "01:00:00:00",
        "24000/1001 is labelled non-drop, as broadcast practice has it"
    );
}

#[test]
fn a_label_returns_to_the_instant_it_names() {
    let label = Timecode::new(12, 34, 56, 7, 25, false).expect("a legal label");
    let instant = label.to_instant(Timebase::PAL_25).expect("an instant");
    assert_eq!(
        Timecode::from_instant(instant).expect("a label"),
        label,
        "the round trip is exact"
    );
    assert_eq!(
        label.to_instant(Timebase::FILM_24),
        Err(CoreStatus::UnsupportedTimecodeRate),
        "a label counts at one rate and does not silently move to another"
    );
}
