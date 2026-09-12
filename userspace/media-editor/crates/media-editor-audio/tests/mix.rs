// SPDX-License-Identifier: GPL-3.0-only
//! The mixer: what summing must preserve, and what full scale costs.

use media_editor_audio::mix::Source;
use media_editor_audio::{
    AudioBuffer, AudioStatus, FULL_SCALE, Gain, NEGATIVE_FULL_SCALE, Pan, SampleRate, mix,
    pan_to_stereo,
};
use media_editor_core::Rational;

fn buffer(channels: &[&[i32]]) -> AudioBuffer {
    let held: std::vec::Vec<std::vec::Vec<i32>> =
        channels.iter().map(|channel| channel.to_vec()).collect();
    AudioBuffer::new(SampleRate::Hz48000, held).expect("a buffer")
}

/// A deterministic spread of samples, well inside full scale.
fn spread(seed: u64, length: usize) -> std::vec::Vec<i32> {
    let mut state = seed;
    (0..length)
        .map(|_| {
            state = state
                .wrapping_mul(6_364_136_223_846_793_005)
                .wrapping_add(1);
            // A quarter of full scale, so four of these can be summed without
            // reaching the rails.
            i32::try_from((state >> 40) % 4_194_304).unwrap_or(0) - 2_097_152
        })
        .collect()
}

fn one(buffer: &AudioBuffer, gain: Gain) -> (AudioBuffer, media_editor_audio::MixReport) {
    mix(&[Source::new(buffer, gain)]).expect("a mix")
}

#[test]
fn a_source_at_unity_passes_through_bit_for_bit() {
    // The claim a mixer has to be able to make. A single channel at unity must
    // arrive at the bus unchanged — not to within a bit, unchanged — or every
    // bounce of an unedited session differs from its source.
    let source = buffer(&[&spread(1, 512), &spread(2, 512)]);
    let (mixed, report) = one(&source, Gain::UNITY);
    assert_eq!(mixed, source);
    assert_eq!(mixed.digest(), source.digest());
    assert!(report.is_clean());
}

#[test]
fn silence_changes_nothing_and_nothing_changes_silence() {
    // Two statements, both exact, and both about a case that a mixer meets
    // constantly: a muted channel. A mix that drifted by a bit when one was
    // present would not be reproducible from one bounce to the next.
    let sound = buffer(&[&spread(3, 256)]);
    let quiet = AudioBuffer::silence(SampleRate::Hz48000, 1, 256).expect("silence");

    let (with_silence, _) = mix(&[
        Source::new(&sound, Gain::UNITY),
        Source::new(&quiet, Gain::UNITY),
    ])
    .expect("a mix");
    assert_eq!(with_silence, sound, "adding silence changes nothing");

    let (muted, _) = mix(&[
        Source::new(&sound, Gain::SILENT),
        Source::new(&quiet, Gain::UNITY),
    ])
    .expect("a mix");
    assert_eq!(muted, quiet, "a muted source contributes nothing");
}

#[test]
fn a_signal_against_its_own_inverse_is_exact_silence() {
    // Phase cancellation, which is the sharpest available check that the sum
    // is exact: every sample must land on precisely zero. A mixer that
    // accumulated in anything lossy would leave a residue here, and a residue
    // is audible on a null test long before it is audible on music.
    let samples = spread(4, 1024);
    let inverted: std::vec::Vec<i32> = samples.iter().map(|sample| -sample).collect();

    let (nulled, report) = mix(&[
        Source::new(&buffer(&[&samples]), Gain::UNITY),
        Source::new(&buffer(&[&inverted]), Gain::UNITY),
    ])
    .expect("a mix");

    assert!(report.is_clean());
    assert!(
        nulled
            .channel(0)
            .expect("a channel")
            .iter()
            .all(|s| *s == 0),
        "a null test must null"
    );
    assert_eq!(
        nulled,
        AudioBuffer::silence(SampleRate::Hz48000, 1, 1024).expect("silence")
    );
}

#[test]
fn summing_does_not_depend_on_the_order_the_channels_arrive_in() {
    // Integer addition is associative and commutative, so a bus is the same
    // bus however its channels are scheduled. Proving that now is what lets
    // anything ever run them in parallel (R-6.2) — the same argument the
    // render graph makes for pictures.
    let one = buffer(&[&spread(5, 128)]);
    let two = buffer(&[&spread(6, 128)]);
    let three = buffer(&[&spread(7, 128)]);
    let quiet = Gain::whole_decibels(-6).expect("a gain");

    let orders = [
        [(&one, Gain::UNITY), (&two, quiet), (&three, Gain::UNITY)],
        [(&three, Gain::UNITY), (&one, Gain::UNITY), (&two, quiet)],
        [(&two, quiet), (&three, Gain::UNITY), (&one, Gain::UNITY)],
    ];
    let mut digests = std::vec::Vec::new();
    for order in orders {
        let sources: std::vec::Vec<Source<'_>> = order
            .iter()
            .map(|(buffer, gain)| Source::new(buffer, *gain))
            .collect();
        digests.push(mix(&sources).expect("a mix").0.digest());
    }
    assert_eq!(digests[0], digests[1]);
    assert_eq!(digests[1], digests[2]);
}

#[test]
fn full_scale_is_reported_rather_than_reached_quietly() {
    // The headroom question, and the only honest answer to it. Two sources at
    // full scale sum to twice full scale; the mix says how many samples it had
    // to write differently from how it computed them, and by how much.
    //
    // A mixer that clipped in silence is one whose user finds out on a
    // listening copy. A mixer that refused would be one that cannot open a
    // session a fader move would fix.
    let loud = buffer(&[&[FULL_SCALE; 16]]);
    let (mixed, report) = mix(&[
        Source::new(&loud, Gain::UNITY),
        Source::new(&loud, Gain::UNITY),
    ])
    .expect("a mix");

    assert!(!report.is_clean());
    assert_eq!(report.clipped, 16, "every sample of the block");
    assert_eq!(
        report.overshoot,
        i64::from(FULL_SCALE),
        "twice full scale is one whole full scale past the rail"
    );
    assert!(
        mixed
            .channel(0)
            .expect("a channel")
            .iter()
            .all(|sample| *sample == FULL_SCALE)
    );

    // The negative rail is one further out than the positive one, which is a
    // property of two's complement rather than an oversight — so it is checked
    // separately rather than assumed symmetric.
    let quiet = buffer(&[&[NEGATIVE_FULL_SCALE; 4]]);
    let (mixed, report) = mix(&[
        Source::new(&quiet, Gain::UNITY),
        Source::new(&quiet, Gain::UNITY),
    ])
    .expect("a mix");
    assert_eq!(report.clipped, 4);
    assert!(
        mixed
            .channel(0)
            .expect("a channel")
            .iter()
            .all(|sample| *sample == NEGATIVE_FULL_SCALE)
    );

    // And a mix that fits reports nothing at all.
    let (_, report) = mix(&[
        Source::new(&buffer(&[&spread(8, 64)]), Gain::UNITY),
        Source::new(&buffer(&[&spread(9, 64)]), Gain::UNITY),
    ])
    .expect("a mix");
    assert!(report.is_clean());
    assert_eq!(report.overshoot, 0);
}

#[test]
fn a_fader_move_can_rescue_a_mix_that_clipped() {
    // The reason clipping is reported rather than refused: the session is not
    // broken, it is loud. Pulling the sources down six decibels each has to
    // make the same mix fit.
    let loud = buffer(&[&[FULL_SCALE; 8]]);
    let (_, before) = mix(&[
        Source::new(&loud, Gain::UNITY),
        Source::new(&loud, Gain::UNITY),
    ])
    .expect("a mix");
    assert!(!before.is_clean());

    // Six decibels down each is *not* enough, and this is where the difference
    // between 6 and 6.020599913 stops being pedantry. A factor of 10^(-6/20)
    // is 0.50119, so two of them still sum to 1.0024 of full scale — the mix
    // clips by a quarter of a percent, which is a handful of samples at the
    // loudest moment and exactly the kind of thing nobody can reproduce.
    let almost = Gain::whole_decibels(-6).expect("a gain");
    let (_, still_clipping) =
        mix(&[Source::new(&loud, almost), Source::new(&loud, almost)]).expect("a mix");
    assert!(
        !still_clipping.is_clean(),
        "six decibels is not a halving, so this must still clip"
    );

    // A true halving overshoots by exactly one, and the reason is arithmetic
    // rather than gain: positive full scale is 8_388_607, an *odd* number, so
    // half of it is 4_194_303.5. Rounding half away from zero — which is what
    // keeps quiet passages from drifting towards silence — takes it up to
    // 4_194_304, and two of those are one past the rail.
    //
    // One sample unit out of eight million is inaudible. It is asserted anyway,
    // because a mixer that reported "clean" here would be rounding its report
    // as well as its samples, and then the report would be worth nothing.
    let halved = Gain::decibels(Rational::new(-6_020_599_913, 1_000_000_000).expect("a ratio"))
        .expect("a gain");
    let (_, exactly_half) =
        mix(&[Source::new(&loud, halved), Source::new(&loud, halved)]).expect("a mix");
    assert_eq!(
        exactly_half.overshoot, 1,
        "half of an odd number is not one"
    );

    // A hair below a halving fits with nothing to spare, which is what a
    // trim control is for.
    let trimmed = Gain::decibels(Rational::new(-60_206, 10_000).expect("a ratio")).expect("a gain");
    let (_, after) =
        mix(&[Source::new(&loud, trimmed), Source::new(&loud, trimmed)]).expect("a mix");
    assert!(
        after.is_clean(),
        "a hair under a halving each is room enough"
    );
}

#[test]
fn buffers_that_do_not_describe_the_same_sound_are_not_mixed() {
    // Different rates, different lengths, different channel counts. Each is a
    // decision somebody has to make first — resampling, padding, upmixing —
    // and none of them is the mixer's to make silently (R-1.3).
    let base = buffer(&[&spread(10, 64)]);

    let other_rate =
        AudioBuffer::new(SampleRate::Hz44100, std::vec![spread(10, 64)]).expect("a buffer");
    assert_eq!(
        mix(&[
            Source::new(&base, Gain::UNITY),
            Source::new(&other_rate, Gain::UNITY)
        ]),
        Err(AudioStatus::NotMixable)
    );

    let shorter = buffer(&[&spread(10, 32)]);
    assert_eq!(
        mix(&[
            Source::new(&base, Gain::UNITY),
            Source::new(&shorter, Gain::UNITY)
        ]),
        Err(AudioStatus::NotMixable)
    );

    let stereo = buffer(&[&spread(10, 64), &spread(11, 64)]);
    assert_eq!(
        mix(&[
            Source::new(&base, Gain::UNITY),
            Source::new(&stereo, Gain::UNITY)
        ]),
        Err(AudioStatus::NotMixable)
    );

    assert_eq!(mix(&[]), Err(AudioStatus::ChannelCountUnsupported));
}

#[test]
fn a_ragged_buffer_is_not_a_buffer() {
    // Channels of different lengths do not describe a span of sound, and
    // padding the short ones would be inventing silence nobody recorded.
    assert_eq!(
        AudioBuffer::new(
            SampleRate::Hz48000,
            std::vec![std::vec![0_i32; 8], std::vec![0_i32; 7]]
        ),
        Err(AudioStatus::RaggedBuffer)
    );
    assert_eq!(
        AudioBuffer::new(SampleRate::Hz48000, std::vec![]),
        Err(AudioStatus::ChannelCountUnsupported)
    );
}

#[test]
fn panning_a_mono_source_keeps_its_energy() {
    // A source panned hard left, centre, and hard right must be the same
    // loudness in all three — that is what the constant-power law is for, and
    // it has to survive the trip through actual samples rather than holding
    // only for the gains.
    let mono = buffer(&[&[1_000_000_i32; 64]]);
    let mut powers = std::vec::Vec::new();
    for pan in [
        Pan::left().expect("a position"),
        Pan::CENTRE,
        Pan::new(Rational::new(1, 2).expect("a ratio")).expect("a position"),
        Pan::right().expect("a position"),
    ] {
        let stereo = pan_to_stereo(&mono, pan, Gain::UNITY).expect("a pan");
        assert_eq!(stereo.channel_count(), 2);
        let left = i64::from(stereo.channel(0).expect("a channel")[0]);
        let right = i64::from(stereo.channel(1).expect("a channel")[0]);
        powers.push(left * left + right * right);
    }
    let reference = powers[0];
    for power in &powers {
        // Within a thousandth: the samples are quantised to whole integers, so
        // the energy cannot be exact once it reaches them, and this says how
        // far from exact it is allowed to be.
        let apart = (power - reference).abs();
        assert!(
            apart * 1000 < reference,
            "the energy moved by {apart} out of {reference} across the image"
        );
    }
}

#[test]
fn hard_left_puts_nothing_in_the_right_speaker() {
    let mono = buffer(&[&spread(12, 32)]);
    let stereo =
        pan_to_stereo(&mono, Pan::left().expect("a position"), Gain::UNITY).expect("a pan");
    assert_eq!(
        stereo.channel(0).expect("a channel"),
        mono.channel(0).expect("a channel")
    );
    assert!(
        stereo
            .channel(1)
            .expect("a channel")
            .iter()
            .all(|s| *s == 0)
    );
}

#[test]
fn only_a_mono_source_can_be_panned() {
    // Panning a stereo source is a balance control or a width control, and
    // those are different operations with different names.
    let stereo = buffer(&[&spread(13, 16), &spread(14, 16)]);
    assert_eq!(
        pan_to_stereo(&stereo, Pan::CENTRE, Gain::UNITY),
        Err(AudioStatus::ChannelCountUnsupported)
    );
}

#[test]
fn a_peak_meter_reads_the_loudest_sample_in_each_channel() {
    let mixed = buffer(&[&[10, -20, 5], &[1, 2, -3]]);
    assert_eq!(mixed.peaks(), std::vec![20, 3]);

    // The rails are not symmetric: two's complement gives one more value below
    // zero than above it, so the most negative sample has a magnitude of
    // 8_388_608 where positive full scale is 8_388_607. The meter reports the
    // true magnitude rather than clamping, because "one past the positive
    // rail" is precisely what a listener needs told — and a meter that clamped
    // would make the loudest possible sample indistinguishable from one that
    // merely reached full scale.
    let rail = buffer(&[&[NEGATIVE_FULL_SCALE, 0]]);
    assert_eq!(rail.peaks(), std::vec![8_388_608_i32]);
    assert!(
        rail.peaks()[0] > FULL_SCALE,
        "the negative rail is one further out, and the meter says so"
    );
}

#[test]
fn a_mix_is_the_same_mix_every_time() {
    let one = buffer(&[&spread(15, 256)]);
    let two = buffer(&[&spread(16, 256)]);
    let gain = Gain::whole_decibels(-3).expect("a gain");
    let first = mix(&[Source::new(&one, gain), Source::new(&two, Gain::UNITY)]).expect("a mix");
    let second = mix(&[Source::new(&one, gain), Source::new(&two, Gain::UNITY)]).expect("a mix");
    assert_eq!(first.0.digest(), second.0.digest());
    assert_eq!(first.1, second.1);
}

#[test]
fn two_buffers_are_the_same_buffer_exactly_when_they_hold_the_same_sound() {
    let samples = spread(17, 64);
    assert_eq!(buffer(&[&samples]).digest(), buffer(&[&samples]).digest());

    let mut changed = samples.clone();
    changed[31] += 1;
    assert_ne!(
        buffer(&[&samples]).digest(),
        buffer(&[&changed]).digest(),
        "one sample out of sixty-four is a different buffer"
    );

    // The same samples at a different rate are a different buffer, because
    // they are a different sound.
    let at_44 =
        AudioBuffer::new(SampleRate::Hz44100, std::vec![samples.clone()]).expect("a buffer");
    assert_ne!(buffer(&[&samples]).digest(), at_44.digest());
}
