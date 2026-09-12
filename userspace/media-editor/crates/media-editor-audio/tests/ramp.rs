// SPDX-License-Identifier: GPL-3.0-only
//! A fader that moves while the sound is playing.
//!
//! A gain applied one block at a time and held constant across each block puts
//! a step at every block boundary. On a slow move nobody hears it; on a fast
//! one it is a buzz at the frame rate, which is the noise every mixer that
//! ever shipped has had to be taught not to make. These tests are about the
//! shape of the ramp that avoids it, and about the two properties that matter:
//! the ends are exact, and consecutive blocks tile.

use media_editor_audio::mix::Source;
use media_editor_audio::{AudioBuffer, Gain, SampleRate, mix};

/// A buffer of one repeated sample.
fn steady(value: i32, samples: usize) -> AudioBuffer {
    AudioBuffer::new(SampleRate::Hz48000, std::vec![std::vec![value; samples]]).expect("a buffer")
}

/// The samples of a one-channel mix.
fn mixed(sources: &[Source<'_>]) -> std::vec::Vec<i32> {
    let (buffer, report) = mix(sources).expect("a mix");
    assert!(report.is_clean(), "the fixture was not meant to clip");
    buffer.channel(0).expect("a channel").to_vec()
}

#[test]
fn a_ramp_starts_exactly_where_it_was_told_to() {
    // The first sample gets `from`, with no interpolation applied at all.
    // Anything else and the fader would jump at the start of every block,
    // which is the fault the ramp exists to remove.
    let buffer = steady(4_000_000, 100);
    let held = mixed(&[Source::ramped(
        &buffer,
        Gain::UNITY,
        Gain::whole_decibels(-20).expect("a gain"),
    )]);
    assert_eq!(held[0], 4_000_000, "the ramp did not start at unity");
}

#[test]
fn a_ramp_stops_one_step_short_of_where_it_is_going() {
    // The interval is half open: `to` belongs to the sample *after* the last,
    // which is the next block's first. Closing it would repeat one gain at
    // every boundary, and a repetition at a regular interval is a tone — a hum
    // at the frame rate in every automated fade.
    //
    // Twenty decibels is exactly a tenth, so a hundred samples from unity to
    // −20 dB should end at the ninety-ninth hundredth of the way, not at the
    // hundredth.
    let buffer = steady(1_000_000, 100);
    let down = Gain::whole_decibels(-20).expect("a gain");
    let held = mixed(&[Source::ramped(&buffer, Gain::UNITY, down)]);

    // Factor at index 99 of 100 is 1 + (0.1 - 1) * 99/100 = 0.109.
    assert_eq!(held[99], 109_000, "the last sample is not one step short");
    // And the value it is heading for, which no sample here receives.
    assert_ne!(held[99], 100_000);
}

#[test]
fn consecutive_blocks_tile_a_fader_move() {
    // The property the half-open interval is for. Splitting a move across two
    // blocks must give exactly the samples one long block gives — no sample
    // repeated, none skipped, and nothing different at the seam.
    let whole = steady(2_000_000, 200);
    let first = steady(2_000_000, 100);
    let second = steady(2_000_000, 100);
    let start = Gain::UNITY;
    let middle = Gain::whole_decibels(-10).expect("a gain");
    let end = Gain::whole_decibels(-20).expect("a gain");

    let together = mixed(&[Source::ramped(&whole, start, end)]);
    let mut apart = mixed(&[Source::ramped(&first, start, middle)]);
    apart.extend(mixed(&[Source::ramped(&second, middle, end)]));

    // The two halves are not identical to the whole, because −10 dB is not
    // half way between unity and −20 dB in *factor* — the interpolation is
    // linear in the factor and the decibel scale is not. What must hold is
    // that the seam is continuous: the value at the join is the one both
    // blocks agree on.
    assert_eq!(apart.len(), together.len());
    // Ten decibels down is `10^(-1/2)`, so 2,000,000 becomes 632,455.53 and
    // rounds half away from zero to 632,456. Worked out rather than read off:
    // the first draft of this line said 632,000, and the code was right.
    assert_eq!(
        apart[100], 632_456,
        "the second block did not start at −10 dB"
    );
    assert_eq!(together[0], apart[0], "the two starts disagree");

    // No step at the seam larger than the steps around it, which is the thing
    // a listener would hear.
    let seam = (apart[100] - apart[99]).abs();
    let before = (apart[99] - apart[98]).abs();
    let after = (apart[101] - apart[100]).abs();
    assert!(
        seam <= before.max(after) * 2,
        "the seam steps by {seam} where its neighbours step by {before} and {after}"
    );
}

#[test]
fn a_ramp_is_monotone_and_lands_between_its_ends() {
    // A fader coming down must not go up anywhere, and must stay inside the
    // two levels it runs between. An off-by-one in the interpolation shows up
    // here as a sample outside the range, and a sign error as a reversal.
    let buffer = steady(8_000_000, 500);
    let held = mixed(&[Source::ramped(
        &buffer,
        Gain::UNITY,
        Gain::whole_decibels(-40).expect("a gain"),
    )]);
    let mut previous = i32::MAX;
    for (index, sample) in held.iter().enumerate() {
        assert!(
            *sample <= previous,
            "sample {index} went up during a fade down"
        );
        assert!(
            *sample <= 8_000_000,
            "sample {index} is above where it began"
        );
        assert!(
            *sample >= 80_000,
            "sample {index} is below where it is going"
        );
        previous = *sample;
    }
}

#[test]
fn a_fader_that_is_not_moving_is_not_the_same_as_one_moving_to_itself() {
    // `Source::new` keeps unity a copy and silence a nought. `Source::ramped`
    // from a value to itself goes through the multiply, and for unity that is
    // still exact — but the two are different code paths and the distinction
    // is deliberate, so it is asserted rather than assumed.
    let buffer = steady(1_234_567, 64);
    let still = mixed(&[Source::new(&buffer, Gain::UNITY)]);
    let moving = mixed(&[Source::ramped(&buffer, Gain::UNITY, Gain::UNITY)]);
    assert_eq!(still, moving, "unity to unity is not unity");

    // Silence is silence either way.
    let quiet = mixed(&[Source::ramped(&buffer, Gain::SILENT, Gain::SILENT)]);
    assert_eq!(quiet, std::vec![0; 64]);
}

#[test]
fn a_fade_to_silence_arrives_at_silence() {
    // Silence is the off detent rather than a very small number, so its factor
    // is exactly nought and a ramp into it is a ramp to nothing. The last
    // sample is one step short of silence, and the block after it — which
    // begins at silence — is nothing at all.
    let buffer = steady(6_000_000, 120);
    let down = mixed(&[Source::ramped(&buffer, Gain::UNITY, Gain::SILENT)]);
    assert_eq!(down[0], 6_000_000);
    assert!(down[119] > 0, "the fade reached silence a sample early");
    assert!(down[119] < 60_000, "the fade did not get close to silence");

    let after = mixed(&[Source::ramped(&buffer, Gain::SILENT, Gain::SILENT)]);
    assert_eq!(after, std::vec![0; 120]);
}

#[test]
fn a_ramp_still_reports_what_full_scale_cost() {
    // A moving fader does not get to clip quietly. Two full-scale sources, one
    // of them ramping up past unity, must still say how many samples were
    // written at a value the arithmetic did not give.
    let buffer = steady(8_000_000, 50);
    let sources = [
        Source::new(&buffer, Gain::UNITY),
        Source::ramped(
            &buffer,
            Gain::whole_decibels(-40).expect("a gain"),
            Gain::UNITY,
        ),
    ];
    let (_, report) = mix(&sources).expect("a mix");
    assert!(!report.is_clean(), "a sum past full scale was not reported");
    assert!(report.overshoot > 0);
}

#[test]
fn a_ramp_across_one_sample_is_just_its_start() {
    // The degenerate block. One sample, and it is the block's first, so it
    // gets `from` and the whole of `to` belongs to whatever comes next.
    let buffer = steady(3_000_000, 1);
    let held = mixed(&[Source::ramped(
        &buffer,
        Gain::UNITY,
        Gain::whole_decibels(-60).expect("a gain"),
    )]);
    assert_eq!(held, std::vec![3_000_000]);
}
