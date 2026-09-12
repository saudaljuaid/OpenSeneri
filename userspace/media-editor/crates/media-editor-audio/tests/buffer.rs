// SPDX-License-Identifier: GPL-3.0-only
//! Buffers, and the fade that runs across one.
//!
//! A fade on a clip is a fraction of the material rather than a position on a
//! fader, so it scales the samples and the fader scales the source. They are
//! two different things multiplied together, and converting one into the
//! other's units would mean a logarithm at every sample for a number that only
//! ever takes two values.

use media_editor_audio::{AudioBuffer, SampleRate};

#[test]
fn a_faded_buffer_ramps_exactly_across_it() {
    // Exact integer arithmetic, rounded once, half away from zero -- the same
    // rounding the compositor uses on a coverage, so a picture fade and a
    // sound fade of the same length agree about where they are.
    let buffer =
        AudioBuffer::new(SampleRate::Hz48000, std::vec![std::vec![1000_i32; 4]]).expect("a buffer");
    let faded = buffer
        .faded(
            media_editor_core::Rational::ZERO,
            media_editor_core::Rational::ONE,
        )
        .expect("a fade");
    // Nought at the first sample, and the *fourth* quarter belongs to the next
    // block's first sample rather than to this one's last -- which is what
    // makes consecutive blocks tile a fade instead of repeating a value at
    // every seam.
    assert_eq!(faded.channel(0).expect("a channel"), [0, 250, 500, 750]);
}

#[test]
fn a_fade_of_one_to_one_is_the_buffer_it_was_given() {
    // A multiply by one is exact, so a clip whose fade has finished is not
    // quietly re-quantised on its way through.
    let buffer = AudioBuffer::new(
        SampleRate::Hz48000,
        std::vec![std::vec![-32_768, -1, 0, 1, 32_767]],
    )
    .expect("a buffer");
    let faded = buffer
        .faded(
            media_editor_core::Rational::ONE,
            media_editor_core::Rational::ONE,
        )
        .expect("a fade");
    assert_eq!(
        faded.channel(0).expect("a channel"),
        buffer.channel(0).expect("a channel")
    );
}

#[test]
fn a_fade_rounds_away_from_zero_in_both_directions() {
    // A negative sample and a positive one of the same magnitude have to come
    // back the same magnitude, or a fade would put a tiny asymmetry into every
    // waveform it touched -- which is a direct-current offset, and a
    // direct-current offset is a thump at every cut.
    let buffer =
        AudioBuffer::new(SampleRate::Hz48000, std::vec![std::vec![-3, 3]]).expect("a buffer");
    let half = media_editor_core::Rational::new(1, 2).expect("a half");
    let faded = buffer.faded(half, half).expect("a fade");
    assert_eq!(faded.channel(0).expect("a channel"), [-2, 2]);
}

#[test]
fn a_fade_keeps_every_channel() {
    let buffer = AudioBuffer::new(
        SampleRate::Hz48000,
        std::vec![std::vec![100_i32; 3], std::vec![-100_i32; 3]],
    )
    .expect("a buffer");
    let half = media_editor_core::Rational::new(1, 2).expect("a half");
    let faded = buffer.faded(half, half).expect("a fade");
    assert_eq!(faded.channel_count(), 2);
    assert_eq!(faded.channel(0).expect("a channel"), [50, 50, 50]);
    assert_eq!(faded.channel(1).expect("a channel"), [-50, -50, -50]);
}
