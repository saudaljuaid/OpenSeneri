// SPDX-License-Identifier: GPL-3.0-only
//! Positions, lengths, and the conversions that are exact or refused.

use core::cmp::Ordering;

use media_editor_core::{CoreStatus, Duration, Instant, Rational, Timebase};

#[test]
fn a_duration_cannot_be_negative() {
    assert_eq!(
        Duration::new(-1, Timebase::PAL_25),
        Err(CoreStatus::NegativeDuration)
    );
    let one = Duration::new(1, Timebase::PAL_25).expect("one frame");
    let two = Duration::new(2, Timebase::PAL_25).expect("two frames");
    assert_eq!(one.checked_sub(two), Err(CoreStatus::NegativeDuration));
}

#[test]
fn quantities_in_different_timebases_do_not_combine() {
    let position = Instant::new(10, Timebase::PAL_25);
    let length = Duration::new(5, Timebase::FILM_24).expect("five frames");
    assert_eq!(position.advance(length), Err(CoreStatus::TimebaseMismatch));
    assert_eq!(position.retreat(length), Err(CoreStatus::TimebaseMismatch));
    assert_eq!(
        position.compare(Instant::new(10, Timebase::FILM_24)),
        Err(CoreStatus::TimebaseMismatch),
        "ten frames of PAL is not ten frames of film"
    );
}

#[test]
fn advancing_and_retreating_are_inverse() {
    let start = Instant::new(1000, Timebase::NTSC_30);
    let length = Duration::new(137, Timebase::NTSC_30).expect("a length");
    let moved = start.advance(length).expect("later");
    assert_eq!(moved.ticks(), 1137);
    assert_eq!(moved.retreat(length).expect("back"), start);
    assert_eq!(moved.since(start).expect("the length between"), length);
}

#[test]
fn an_earlier_instant_cannot_be_subtracted_from_a_later_one_backwards() {
    let early = Instant::new(5, Timebase::PAL_25);
    let late = Instant::new(9, Timebase::PAL_25);
    assert_eq!(early.since(late), Err(CoreStatus::NegativeDuration));
    assert_eq!(late.since(early).expect("four frames").ticks(), 4);
    assert_eq!(early.compare(late), Ok(Ordering::Less));
}

#[test]
fn overflow_is_refused_rather_than_wrapped() {
    let far = Instant::new(i64::MAX, Timebase::PAL_25);
    let one = Duration::new(1, Timebase::PAL_25).expect("one frame");
    assert_eq!(far.advance(one), Err(CoreStatus::Overflow));
}

#[test]
fn seconds_are_exact() {
    let one_second_of_pal = Instant::new(25, Timebase::PAL_25);
    assert_eq!(one_second_of_pal.seconds(), Ok(Rational::from_integer(1)));

    let one_ntsc_frame = Instant::new(1, Timebase::NTSC_30);
    assert_eq!(
        one_ntsc_frame.seconds(),
        Ok(Rational::new(1001, 30_000).expect("exact"))
    );
}

#[test]
fn conversion_between_timebases_is_exact_or_refused() {
    // One second of PAL is one second of film, exactly, at both ends.
    let one_second = Instant::new(25, Timebase::PAL_25);
    let as_film = one_second
        .convert(Timebase::FILM_24)
        .expect("one second is 24 film frames");
    assert_eq!(as_film.ticks(), 24);

    // The second PAL frame is 24/25 of a film frame in, which is not a film
    // frame at all, so there is no answer and none is invented.
    assert_eq!(
        Instant::new(1, Timebase::PAL_25).convert(Timebase::FILM_24),
        Err(CoreStatus::InexactConversion)
    );

    // The MPEG timebase divides both, which is why it exists.
    assert_eq!(
        Instant::new(1, Timebase::PAL_25)
            .convert(Timebase::MPEG_90K)
            .expect("exact")
            .ticks(),
        3600
    );
    assert_eq!(
        Instant::new(1, Timebase::FILM_24)
            .convert(Timebase::MPEG_90K)
            .expect("exact")
            .ticks(),
        3750
    );
}

#[test]
fn ntsc_conversion_keeps_the_thousand_and_first() {
    // One NTSC video frame is 1001/30000 of a second, which is exactly 3003
    // ticks of the 90 kHz transport timebase. This is the number that makes
    // NTSC work, and it is exact.
    let frame = Instant::new(1, Timebase::NTSC_30);
    assert_eq!(
        frame.convert(Timebase::MPEG_90K).expect("exact").ticks(),
        3003
    );

    // And the reverse.
    assert_eq!(
        Instant::new(3003, Timebase::MPEG_90K)
            .convert(Timebase::NTSC_30)
            .expect("exact")
            .ticks(),
        1
    );
    assert_eq!(
        Instant::new(3002, Timebase::MPEG_90K).convert(Timebase::NTSC_30),
        Err(CoreStatus::InexactConversion)
    );
}

#[test]
fn converting_to_the_same_timebase_is_the_identity() {
    let position = Instant::new(12_345, Timebase::NTSC_FILM);
    assert_eq!(position.convert(Timebase::NTSC_FILM), Ok(position));
}

#[test]
fn audio_and_picture_meet_at_whole_seconds() {
    let one_second_of_picture = Duration::new(24, Timebase::FILM_24).expect("a second");
    assert_eq!(
        one_second_of_picture
            .convert(Timebase::AUDIO_48K)
            .expect("exact")
            .ticks(),
        48_000
    );

    // A single film frame is exactly 2000 samples at 48 kHz.
    assert_eq!(
        Duration::new(1, Timebase::FILM_24)
            .expect("a frame")
            .convert(Timebase::AUDIO_48K)
            .expect("exact")
            .ticks(),
        2000
    );

    // A single NTSC video frame is 1601.6 samples, so it is not a whole
    // number of samples and the conversion says so.
    assert_eq!(
        Duration::new(1, Timebase::NTSC_30)
            .expect("a frame")
            .convert(Timebase::AUDIO_48K),
        Err(CoreStatus::InexactConversion)
    );

    // Five NTSC video frames are exactly 8008 samples, which is why NTSC
    // audio is edited in five-frame groups.
    assert_eq!(
        Duration::new(5, Timebase::NTSC_30)
            .expect("five frames")
            .convert(Timebase::AUDIO_48K)
            .expect("exact")
            .ticks(),
        8008
    );
}

#[test]
fn a_timebase_must_be_positive() {
    assert_eq!(
        Timebase::new(Rational::ZERO),
        Err(CoreStatus::NonPositiveRate)
    );
    assert_eq!(
        Timebase::from_fraction(-24, 1),
        Err(CoreStatus::NonPositiveRate)
    );
    assert_eq!(
        Timebase::from_fraction(24, 0),
        Err(CoreStatus::ZeroDenominator)
    );
}

#[test]
fn nominal_rates_round_to_broadcast_practice() {
    assert_eq!(Timebase::NTSC_FILM.nominal_rate(), Ok(24));
    assert_eq!(Timebase::NTSC_30.nominal_rate(), Ok(30));
    assert_eq!(Timebase::NTSC_60.nominal_rate(), Ok(60));
    assert_eq!(Timebase::PAL_25.nominal_rate(), Ok(25));
    assert_eq!(Timebase::FILM_24.nominal_rate(), Ok(24));
    assert!(Timebase::FILM_24.is_integral());
    assert!(!Timebase::NTSC_30.is_integral());
}
