// SPDX-License-Identifier: GPL-3.0-only
//! Landing a timeline position on a sample, and why it is a floor.

use media_editor_core::{CoreStatus, Instant, Timebase};

#[test]
fn a_whole_rate_lands_exactly_and_agrees_with_the_exact_conversion() {
    // At 24 frames a second and 48 kHz, a frame is 2000 samples exactly. Where
    // the exact conversion succeeds, the floor must give the same answer — if
    // the two ever disagreed, one of them would be wrong.
    for frame in 0..500 {
        let instant = Instant::new(frame, Timebase::FILM_24);
        let floored = instant
            .floor_into(Timebase::AUDIO_48K)
            .expect("a conversion");
        assert_eq!(floored.ticks(), frame * 2000);
        assert_eq!(
            floored,
            instant.convert(Timebase::AUDIO_48K).expect("a conversion")
        );
    }
}

#[test]
fn no_frame_at_ntsc_is_a_whole_number_of_samples() {
    // 48000 x 1001 / 30000 is 1601.6. This is the fact that makes a floor
    // necessary: the exact conversion is *right* to refuse, and a mixer still
    // has to be able to say where frame one begins.
    let frame = Instant::new(1, Timebase::NTSC_30);
    assert_eq!(
        frame.convert(Timebase::AUDIO_48K),
        Err(CoreStatus::InexactConversion),
        "an exact conversion must refuse, because there is no exact answer"
    );
    assert_eq!(
        frame
            .floor_into(Timebase::AUDIO_48K)
            .expect("a conversion")
            .ticks(),
        1601,
        "and the floor says which sample it is inside"
    );
}

#[test]
fn consecutive_frames_tile_the_samples_with_no_gap_and_no_overlap() {
    // Each frame's start is the previous frame's end, so summing the per-frame
    // sample counts over any span gives exactly the samples in that span — no
    // sample is dropped, none is played twice, and there is no drift however
    // long the programme runs.
    //
    // This property does *not* distinguish a floor from a round. A negative
    // control that replaced the floor with a round left this test passing,
    // because rounding is monotone too and consecutive rounds tile just as
    // well. What the floor gives that the round does not is in
    // `no_frame_at_ntsc_is_a_whole_number_of_samples`: the answer to "which
    // sample is this position inside", rather than "which sample is nearest".
    // Both tests are needed and neither is redundant.
    for rate in [
        Timebase::NTSC_30,
        Timebase::NTSC_FILM,
        Timebase::NTSC_60,
        Timebase::FILM_24,
        Timebase::PAL_25,
    ] {
        for audio in [Timebase::AUDIO_48K, Timebase::AUDIO_44K1] {
            let mut previous = Instant::new(0, rate)
                .floor_into(audio)
                .expect("a conversion")
                .ticks();
            assert_eq!(previous, 0, "the origin is the origin");
            let mut total = 0_i64;
            for frame in 1..=2000 {
                let now = Instant::new(frame, rate)
                    .floor_into(audio)
                    .expect("a conversion")
                    .ticks();
                let span = now - previous;
                assert!(span > 0, "a frame holds at least one sample");
                total += span;
                previous = now;
            }
            assert_eq!(
                total, previous,
                "the per-frame counts sum to the whole span at {rate:?} into {audio:?}"
            );
        }
    }
}

#[test]
fn a_frame_holds_one_of_two_sample_counts_and_they_average_out() {
    // At 29.97 into 48 kHz a frame is 1601 samples or 1602, never anything
    // else, and over 30000 frames they average to exactly 1601.6 — which is
    // what "no drift" means when stated as a number.
    let mut short = 0_i64;
    let mut long = 0_i64;
    let mut previous = 0_i64;
    for frame in 1..=30_000 {
        let now = Instant::new(frame, Timebase::NTSC_30)
            .floor_into(Timebase::AUDIO_48K)
            .expect("a conversion")
            .ticks();
        match now - previous {
            1601 => short += 1,
            1602 => long += 1,
            other => panic!("a frame of {other} samples at frame {frame}"),
        }
        previous = now;
    }
    assert_eq!(short + long, 30_000);
    // 30000 frames is exactly 1001 seconds, which is 48_048_000 samples.
    assert_eq!(previous, 48_048_000);
    assert_eq!(short * 1601 + long * 1602, 48_048_000);
}

#[test]
fn a_position_before_the_origin_rounds_downwards_rather_than_towards_it() {
    // Integer division truncates towards zero, which for a negative position
    // rounds *up*. A clip that begins before the origin would then start one
    // sample late, and the fault would appear only on material with handles
    // before its own zero — which is most of it.
    let before = Instant::new(-1, Timebase::NTSC_30);
    assert_eq!(
        before
            .floor_into(Timebase::AUDIO_48K)
            .expect("a conversion")
            .ticks(),
        -1602,
        "minus 1601.6 floors to minus 1602, not minus 1601"
    );

    // And the tiling still holds across the origin, which is the real check.
    let mut previous = Instant::new(-100, Timebase::NTSC_30)
        .floor_into(Timebase::AUDIO_48K)
        .expect("a conversion")
        .ticks();
    for frame in -99..=100 {
        let now = Instant::new(frame, Timebase::NTSC_30)
            .floor_into(Timebase::AUDIO_48K)
            .expect("a conversion")
            .ticks();
        let span = now - previous;
        assert!(span == 1601 || span == 1602, "{span} samples at {frame}");
        previous = now;
    }
}

#[test]
fn the_floor_is_the_same_floor_every_time() {
    for frame in [0_i64, 1, 29, 30, 1000, 107_892] {
        let instant = Instant::new(frame, Timebase::NTSC_30);
        assert_eq!(
            instant.floor_into(Timebase::AUDIO_48K),
            instant.floor_into(Timebase::AUDIO_48K)
        );
    }
}
