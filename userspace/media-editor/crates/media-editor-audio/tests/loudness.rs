// SPDX-License-Identifier: GPL-3.0-only
//! Loudness, against the standard's own compliance cases.
//!
//! EBU Tech 3341 states what a conforming meter must read for a handful of
//! signals, to a stated tolerance. Those are the tests here, generated rather
//! than shipped as files — a 1 kHz tone at a stated level is exactly
//! reproducible, and generating it with the integer sine means the fixture is
//! the same on every machine too.

use media_editor_audio::loudness::{self, ABSOLUTE_GATE, BLOCK_SAMPLES, MAX_WEIGHTED_CHANNELS};
use media_editor_audio::{AudioBuffer, AudioStatus, SampleRate};
use media_editor_core::{Fixed, Rational};

/// What a conforming meter is allowed to be out by, in loudness units.
///
/// A tenth, which is what EBU Tech 3341 states for its own cases.
const TOLERANCE: (i64, i64) = (1, 10);

/// A level in thousandths, for a failure message.
///
/// Integers, in a file whose whole subject is arithmetic without floating
/// point. Printing a reading as a float here would be a small hypocrisy and a
/// lossy one.
fn thousandths(level: Fixed) -> i64 {
    let scaled = i128::from(level.raw()) * 1000;
    let half = 1_i128 << 31;
    let rounded = if scaled >= 0 {
        (scaled + half) >> 32
    } else {
        -((-scaled + half) >> 32)
    };
    i64::try_from(rounded).unwrap_or(0)
}

/// The tolerance as a value.
fn tolerance() -> Fixed {
    Fixed::from_rational(Rational::new(TOLERANCE.0, TOLERANCE.1).expect("a ratio"))
        .expect("a value")
}

/// A tone at a level, for a number of seconds.
///
/// Generated with the integer sine, so the fixture is bit-identical on every
/// machine. One kilohertz at 48 kHz is a phase advance of exactly 1/48 of a
/// turn per sample — an exact rational, so the tone neither drifts nor
/// accumulates error however long it runs.
fn tone(decibels: i64, seconds: usize, channels: usize) -> AudioBuffer {
    tone_at(1000, decibels, seconds, channels)
}

/// A tone at a frequency and a level.
///
/// The phase advance per sample is `hertz / 48000` turns — an exact rational,
/// so the tone neither drifts nor accumulates error however long it runs.
fn tone_at(hertz: i64, decibels: i64, seconds: usize, channels: usize) -> AudioBuffer {
    let samples = seconds * 48_000;
    let amplitude = Fixed::from_integer(10)
        .expect("a value")
        .pow(Fixed::from_rational(Rational::new(decibels, 20).expect("a ratio")).expect("a value"))
        .expect("an amplitude");
    let full_scale = Fixed::from_integer(8_388_608).expect("a value");
    let peak = amplitude.checked_mul(full_scale).expect("a peak");

    let mut channel = std::vec::Vec::with_capacity(samples);
    for index in 0..samples {
        let turns = Fixed::from_rational(
            Rational::new(i64::try_from(index).expect("an index") * hertz, 48_000)
                .expect("a ratio"),
        )
        .expect("a phase");
        let value = turns
            .sin_turns()
            .expect("a sine")
            .checked_mul(peak)
            .expect("a sample");
        channel.push(i32::try_from(value.raw() >> 32).unwrap_or(0));
    }

    let held: std::vec::Vec<std::vec::Vec<i32>> = core::iter::repeat_n(channel, channels).collect();
    AudioBuffer::new(SampleRate::Hz48000, held).expect("a buffer")
}

/// Assert a measured level against what the standard says it must be.
fn reads(measured: Option<Fixed>, expected: i64, what: &str) {
    let level = measured.expect("a measurement");
    let target =
        Fixed::from_rational(Rational::new(expected, 1).expect("a ratio")).expect("a value");
    let allowed = tolerance();
    let apart = (level.raw() - target.raw()).abs();
    assert!(
        apart <= allowed.raw(),
        "{what}: read {} thousandths of a unit against {expected}000",
        thousandths(level)
    );
}

#[test]
fn a_tone_at_minus_twenty_three_reads_minus_twenty_three() {
    // EBU Tech 3341 case 1. A 1 kHz sine at −23 dBFS in both channels must
    // measure −23.0 LUFS, and it is the case that ties the whole chain
    // together: the K-weighting has about 0.698 dB of gain at 1 kHz and the
    // standard's offset is −0.691, so the two very nearly cancel and the tone
    // reads at the level it was recorded at. Get either wrong and this misses.
    let signal = tone(-23, 2, 2);
    reads(
        loudness::integrated(&signal).expect("a measurement"),
        -23,
        "a −23 dBFS tone",
    );
}

#[test]
fn a_tone_at_minus_thirty_three_reads_minus_thirty_three() {
    // EBU Tech 3341 case 2. The same signal ten units quieter, which checks
    // that the scale is a scale rather than one calibrated point.
    let signal = tone(-33, 2, 2);
    reads(
        loudness::integrated(&signal).expect("a measurement"),
        -33,
        "a −33 dBFS tone",
    );
}

#[test]
fn the_scale_moves_one_for_one_with_the_signal() {
    // Ten decibels down is ten units down, every time. A meter whose scale
    // compressed or expanded would still pass a single calibration point.
    let mut previous: Option<Fixed> = None;
    for level in [-13_i64, -23, -33, -43] {
        let measured = loudness::integrated(&tone(level, 1, 2))
            .expect("a measurement")
            .expect("a level");
        if let Some(before) = previous {
            let step = before.checked_sub(measured).expect("a difference");
            let ten = Fixed::from_integer(10).expect("a value");
            let apart = (step.raw() - ten.raw()).abs();
            assert!(
                apart <= tolerance().raw(),
                "a ten decibel step measured as {} thousandths of a unit",
                thousandths(step)
            );
        }
        previous = Some(measured);
    }
}

#[test]
fn two_identical_channels_are_three_units_louder_than_one() {
    // BS.1770 sums the channels rather than averaging them, so the same signal
    // in two channels is twice the power — 3.01 units. A meter that averaged
    // would read the same for both and understate every stereo mix.
    let mono = loudness::integrated(&tone(-23, 1, 1))
        .expect("a measurement")
        .expect("a level");
    let stereo = loudness::integrated(&tone(-23, 1, 2))
        .expect("a measurement")
        .expect("a level");
    let step = stereo.checked_sub(mono).expect("a difference");
    let expected =
        Fixed::from_rational(Rational::new(301, 100).expect("a ratio")).expect("a value");
    let apart = (step.raw() - expected.raw()).abs();
    assert!(
        apart <= tolerance().raw(),
        "doubling the channels moved the reading by {} thousandths, not 3010",
        thousandths(step)
    );
}

#[test]
fn silence_has_no_loudness_rather_than_a_very_small_one() {
    // The loudness of nothing is not a number on this scale. Returning some
    // very negative value instead would let it be averaged in with real
    // measurements, and a programme with a long silence would measure quieter
    // than it sounds — which is the whole reason gating exists.
    let quiet = AudioBuffer::silence(SampleRate::Hz48000, 2, 48_000).expect("silence");
    assert_eq!(loudness::integrated(&quiet).expect("a measurement"), None);
}

#[test]
fn a_long_silence_does_not_drag_a_programme_down() {
    // The gate, doing the job it exists for. One second of tone followed by
    // four of silence, and the whole expectation is worked out from the block
    // structure rather than from what the code says.
    //
    // Five seconds is 47 blocks. Seven of them are entirely tone. Three
    // straddle the point the tone stops and hold three quarters, a half and a
    // quarter of it — those are real blocks with real energy and they count,
    // which is why the answer is not a flat -23. The other 37 are silence and
    // the absolute gate discards them.
    //
    //   mean of the ten kept blocks = (7 + 0.75 + 0.5 + 0.25) / 10 = 0.85
    //   level = -23 + 10 log10(0.85) = -23.706
    //
    // Ungated it would be 10 log10(8.5/47) below the tone, which is -30.4 —
    // nearly seven units quieter for no reason a mixer could act on, and the
    // fix would be to trim the silence rather than to mix it better. That gap
    // is what the gate is worth, and it is asserted rather than described.
    let loud = tone(-23, 1, 2);
    let mut channels: std::vec::Vec<std::vec::Vec<i32>> = std::vec::Vec::new();
    for index in 0..2 {
        let mut held = loud.channel(index).expect("a channel").to_vec();
        held.extend(core::iter::repeat_n(0_i32, 48_000 * 4));
        channels.push(held);
    }
    let padded = AudioBuffer::new(SampleRate::Hz48000, channels).expect("a buffer");

    let measured = loudness::integrated(&padded)
        .expect("a measurement")
        .expect("a level");
    let expected =
        Fixed::from_rational(Rational::new(-23_706, 1000).expect("a ratio")).expect("a value");
    let apart = (measured.raw() - expected.raw()).abs();
    assert!(
        apart <= tolerance().raw(),
        "read {} thousandths against the -23706 the block structure gives",
        thousandths(measured)
    );

    // And it is nowhere near the ungated answer, which is the point.
    let ungated =
        Fixed::from_rational(Rational::new(-30_427, 1000).expect("a ratio")).expect("a value");
    assert!(
        measured.raw() - ungated.raw() > Fixed::from_integer(6).expect("a value").raw(),
        "the gate saved less than six units, so it is barely doing anything"
    );
}

#[test]
fn the_absolute_gate_is_where_the_standard_puts_it() {
    // A tone far below the gate leaves nothing to measure. One just above it
    // does not. The two together pin the threshold rather than merely
    // asserting that a gate exists.
    let below = tone(ABSOLUTE_GATE - 5, 1, 2);
    assert_eq!(loudness::integrated(&below).expect("a measurement"), None);

    let above = tone(ABSOLUTE_GATE + 10, 1, 2);
    assert!(
        loudness::integrated(&above)
            .expect("a measurement")
            .is_some(),
        "a tone ten units above the gate must still measure"
    );
}

#[test]
fn a_rate_this_build_cannot_weight_is_refused() {
    // BS.1770 prints its coefficients for 48 kHz and says to re-derive the
    // filter at other rates. Reusing them would measure the wrong thing
    // quietly, which is worse than refusing (R-1.3).
    let other = AudioBuffer::silence(SampleRate::Hz44100, 2, 48_000).expect("silence");
    assert_eq!(
        loudness::integrated(&other),
        Err(AudioStatus::UnsupportedSampleRate)
    );
}

#[test]
fn more_channels_than_there_is_a_layout_for_are_refused() {
    // The surround channels weigh more than the front ones, which needs to
    // know which channel is which — and a buffer carries a count, not a
    // layout. Inventing an order would be guessing at what the material is.
    let wide = AudioBuffer::silence(SampleRate::Hz48000, MAX_WEIGHTED_CHANNELS + 1, 48_000)
        .expect("silence");
    assert_eq!(
        loudness::integrated(&wide),
        Err(AudioStatus::ChannelCountUnsupported)
    );
}

#[test]
fn a_programme_shorter_than_one_block_has_no_integrated_loudness() {
    // Four hundred milliseconds is the shortest thing this measurement is
    // defined over. Less than that is not a quiet programme, it is not a
    // programme.
    let brief = AudioBuffer::silence(SampleRate::Hz48000, 2, BLOCK_SAMPLES - 1).expect("silence");
    assert_eq!(loudness::integrated(&brief).expect("a measurement"), None);
}

#[test]
fn a_measurement_is_the_same_measurement_every_time() {
    let signal = tone(-23, 1, 2);
    assert_eq!(
        loudness::integrated(&signal).expect("a measurement"),
        loudness::integrated(&signal).expect("a measurement")
    );
}

#[test]
fn the_high_pass_takes_the_rumble_out() {
    // The second filter stage, which every other test in this file is blind
    // to — a control proved it: removing the high-pass entirely failed
    // nothing, because a 1 kHz tone barely touches it. At a kilohertz the
    // stage costs 0.03 dB. That is the whole point of it.
    //
    // Down where it does act, the numbers are worked out from the transfer
    // function rather than from a run:
    //
    //   at 50 Hz the two stages together are -3.934 dB,
    //   the shelf alone is 0.000 dB,
    //   so a -23 dBFS tone at 50 Hz reads -27.625 LUFS,
    //   and without the high-pass it would read -23.691.
    //
    // Four units apart, which no tolerance can hide.
    let low = tone_at(50, -23, 2, 2);
    let measured = loudness::integrated(&low)
        .expect("a measurement")
        .expect("a level");
    let expected =
        Fixed::from_rational(Rational::new(-27_625, 1000).expect("a ratio")).expect("a value");
    let apart = (measured.raw() - expected.raw()).abs();
    assert!(
        apart <= tolerance().raw(),
        "a 50 Hz tone read {} thousandths against the -27625 the filter gives",
        thousandths(measured)
    );

    // And it is nowhere near what the shelf alone would give.
    let unfiltered =
        Fixed::from_rational(Rational::new(-23_691, 1000).expect("a ratio")).expect("a value");
    assert!(
        unfiltered.raw() - measured.raw() > Fixed::from_integer(3).expect("a value").raw(),
        "the high-pass took off less than three units at 50 Hz"
    );
}

#[test]
fn the_weighting_prefers_the_frequencies_the_ear_does() {
    // The shape of the curve, not one point on it. A tone of the same
    // amplitude reads quieter the lower it goes — which is what weighting
    // *is*, and what makes a loudness meter disagree with a peak meter about
    // a bass-heavy mix.
    let mut previous: Option<Fixed> = None;
    for hertz in [40_i64, 60, 100, 300, 1000] {
        let measured = loudness::integrated(&tone_at(hertz, -23, 1, 2))
            .expect("a measurement")
            .expect("a level");
        if let Some(before) = previous {
            assert!(
                measured.raw() > before.raw(),
                "{hertz} Hz did not read louder than the frequency below it"
            );
        }
        previous = Some(measured);
    }
}

#[test]
fn a_momentary_window_shorter_than_the_window_is_refused_by_name() {
    // A momentary reading requires a full 400 ms window. A shorter buffer is
    // invalid, while None means a valid window with no measurable programme.
    let brief = AudioBuffer::silence(SampleRate::Hz48000, 2, BLOCK_SAMPLES - 1).expect("silence");
    assert_eq!(
        loudness::momentary(&brief),
        Err(AudioStatus::BufferTooShort)
    );

    // Exactly one block's worth is enough, and reads as silence rather than
    // refusing.
    let exact = AudioBuffer::silence(SampleRate::Hz48000, 2, BLOCK_SAMPLES).expect("silence");
    assert_eq!(loudness::momentary(&exact).expect("a measurement"), None);
}

#[test]
fn a_momentary_reading_is_the_first_window_and_not_the_whole_thing() {
    // A momentary meter shows what is happening now, ungated. A tone followed
    // by silence must read as the tone at the start and as silence at the end
    // — where the integrated measurement, which is the whole programme, sits
    // between them.
    let loud = tone(-23, 1, 2);
    let mut channels: std::vec::Vec<std::vec::Vec<i32>> = std::vec::Vec::new();
    for index in 0..2 {
        let mut held = loud.channel(index).expect("a channel").to_vec();
        held.extend(core::iter::repeat_n(0_i32, 48_000));
        channels.push(held);
    }
    let padded = AudioBuffer::new(SampleRate::Hz48000, channels).expect("a buffer");

    reads(
        loudness::momentary(&padded).expect("a measurement"),
        -23,
        "the first window of a tone",
    );

    // The tail on its own is silence, and a momentary meter says so.
    let tail: std::vec::Vec<std::vec::Vec<i32>> = (0..2)
        .map(|index| padded.channel(index).expect("a channel")[48_000..].to_vec())
        .collect();
    let quiet = AudioBuffer::new(SampleRate::Hz48000, tail).expect("a buffer");
    assert_eq!(loudness::momentary(&quiet).expect("a measurement"), None);
}
