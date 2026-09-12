// SPDX-License-Identifier: GPL-3.0-only
//! Waveform overview tests using deterministic, uneven sample data.
//!
//! The fixtures cover isolated peaks, asymmetric signals, and partial blocks.

use media_editor_audio::MAX_SAMPLES;
use media_editor_audio::overview::{FANOUT, MAX_BUCKET, MIN_BUCKET, Overview};
use media_editor_audio::{AudioBuffer, AudioStatus, Bucket, SampleRate};

/// A block size small enough to make several levels out of a short buffer.
const BASE: usize = MIN_BUCKET;

/// A repeatable pseudo-random channel, in 24-bit range.
///
/// A linear congruential generator, which is not a good source of randomness
/// and does not need to be: what is wanted is material that differs block to
/// block and is the same on every run.
fn noise(samples: usize, seed: u64) -> std::vec::Vec<i32> {
    let mut state = seed | 1;
    let mut held = std::vec::Vec::with_capacity(samples);
    for _ in 0..samples {
        state = state
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        // The top bits of an LCG are the ones that behave; the bottom ones
        // cycle with short periods.
        let value = i64::from((state >> 40) as u32 & 0x00FF_FFFF) - 8_388_608;
        held.push(i32::try_from(value).expect("a sample"));
    }
    held
}

/// A buffer from channels of noise.
fn noisy(samples: usize, channels: usize) -> AudioBuffer {
    let held: std::vec::Vec<std::vec::Vec<i32>> = (0..channels)
        .map(|index| noise(samples, index as u64 * 7919 + 1))
        .collect();
    AudioBuffer::new(SampleRate::Hz48000, held).expect("a buffer")
}

/// The true mean square of a slice, floored, computed the slow honest way.
fn true_mean_square(samples: &[i32]) -> i64 {
    let mut sum = 0_i128;
    for sample in samples {
        sum += i128::from(*sample) * i128::from(*sample);
    }
    i64::try_from(sum / i128::try_from(samples.len()).expect("a count")).expect("a mean")
}

#[test]
fn a_block_reports_samples_that_are_actually_there() {
    // The lowest and highest of a block are not summary statistics that happen
    // to bound the samples — they are samples. Some sample in the block had
    // exactly that value. A drawing that showed an envelope wider than the
    // signal would show headroom that was not used, and one that showed it
    // narrower would hide the sample nearest the rails.
    let buffer = noisy(BASE * 10 + 7, 2);
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    for channel in 0..2 {
        let samples = buffer.channel(channel).expect("a channel");
        for (index, bucket) in overview
            .buckets(0, channel)
            .expect("a level")
            .iter()
            .enumerate()
        {
            let start = index * BASE;
            let block = &samples[start..(start + BASE).min(samples.len())];
            assert!(
                block.contains(&bucket.minimum()),
                "block {index} of channel {channel} reports a lowest sample it does not hold"
            );
            assert!(
                block.contains(&bucket.maximum()),
                "block {index} of channel {channel} reports a highest sample it does not hold"
            );
        }
    }
}

#[test]
fn a_click_is_visible_at_every_zoom() {
    // The property the whole pyramid rests on. One sample at the rails in
    // half a second of near silence is exactly the thing an editor scrolls out
    // to find, and it must not disappear as the view widens: the highest of
    // two highests is the highest, so the peak is the maximum of its block, of
    // the pair containing it, and of every pair above.
    //
    // Without that, the fix for a click you cannot see when zoomed out is to
    // zoom in and scrub the whole clip, which is the job the waveform exists
    // to save.
    let mut quiet = std::vec![0_i32; 24_000];
    let at = 9_001;
    quiet[at] = 8_388_607;
    let buffer = AudioBuffer::new(SampleRate::Hz48000, std::vec![quiet]).expect("a buffer");
    let overview = Overview::of(&buffer, BASE).expect("a summary");

    assert!(overview.level_count() > 1, "one level is not a pyramid");
    for level in 0..overview.level_count() {
        let width = overview.samples_per_bucket(level).expect("a width");
        let bucket = overview.buckets(level, 0).expect("a level")[at / width];
        assert_eq!(
            bucket.maximum(),
            8_388_607,
            "the click is missing from level {level}"
        );
        assert_eq!(
            bucket.peak(),
            8_388_607,
            "level {level} understates the peak"
        );
    }
}

#[test]
fn zooming_out_does_not_invent_a_peak_either() {
    // The other half, and the one a max-of-max cannot get wrong but a
    // resampled or interpolated summary could: no level may report an
    // excursion larger than the samples actually made. The top level covers
    // the whole channel, so its block must be the whole channel's extremes.
    let buffer = noisy(BASE * 37, 2);
    let top = overview_top(&buffer);
    for (channel, bucket) in top.iter().enumerate() {
        let samples = buffer.channel(channel).expect("a channel");
        assert_eq!(
            bucket.maximum(),
            *samples.iter().max().expect("a sample"),
            "the widest zoom disagrees with the samples about the highest"
        );
        assert_eq!(
            bucket.minimum(),
            *samples.iter().min().expect("a sample"),
            "the widest zoom disagrees with the samples about the lowest"
        );
    }
}

/// The single block per channel at the coarsest level.
fn overview_top(buffer: &AudioBuffer) -> std::vec::Vec<Bucket> {
    let overview = Overview::of(buffer, BASE).expect("a summary");
    let last = overview.level_count() - 1;
    (0..buffer.channel_count())
        .map(|channel| {
            let held = overview.buckets(last, channel).expect("a level");
            assert_eq!(held.len(), 1, "the coarsest level is not one block wide");
            held[0]
        })
        .collect()
}

#[test]
fn the_two_sides_of_a_lopsided_signal_stay_apart() {
    // The reason a block holds two numbers rather than one magnitude. This
    // signal reaches a quarter of full scale upward and all of it downward,
    // which is what brass, speech and a kick drum do to varying degrees. A
    // summary that stored `peak` and drew it mirrored would show a signal
    // four times too loud on the way up, and would hide that only one side is
    // near the rails.
    let mut lopsided = std::vec::Vec::with_capacity(BASE * 8);
    for index in 0..BASE * 8 {
        lopsided.push(if index % 4 == 0 {
            -8_388_608
        } else {
            2_097_152
        });
    }
    let buffer = AudioBuffer::new(SampleRate::Hz48000, std::vec![lopsided]).expect("a buffer");
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    for level in 0..overview.level_count() {
        for bucket in overview.buckets(level, 0).expect("a level") {
            assert_eq!(
                bucket.minimum(),
                -8_388_608,
                "level {level} lost the low side"
            );
            assert_eq!(
                bucket.maximum(),
                2_097_152,
                "level {level} lost the high side"
            );
            assert_ne!(
                bucket.minimum().saturating_abs(),
                bucket.maximum(),
                "level {level} made a lopsided signal symmetric"
            );
        }
    }
}

#[test]
fn the_mean_square_is_within_one_of_the_truth_at_every_zoom() {
    // The stored mean is a division, so it is not exact — but the error must
    // not compound with zoom. The sums are folded upward in wide integers and
    // divided once at the end, so a block at the widest zoom is one floor away
    // from the true mean of its samples, not one floor per level.
    //
    // A floor-of-floors implementation would drift by roughly one per level
    // and would pass a test written only against level zero. This one checks
    // every level, and the bound it asserts is one, not "small".
    let buffer = noisy(BASE * 33 + 11, 2);
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    for level in 0..overview.level_count() {
        let width = overview.samples_per_bucket(level).expect("a width");
        for channel in 0..2 {
            let samples = buffer.channel(channel).expect("a channel");
            for (index, bucket) in overview
                .buckets(level, channel)
                .expect("a level")
                .iter()
                .enumerate()
            {
                let start = index * width;
                let block = &samples[start..(start + width).min(samples.len())];
                let truth = true_mean_square(block);
                let apart = (bucket.mean_square() - truth).abs();
                assert!(
                    apart <= 1,
                    "level {level} block {index} is {apart} from the true mean square"
                );
            }
        }
    }
}

#[test]
fn a_window_of_blocks_is_within_two() {
    // A drawing rarely lands one pixel on one block, so blocks get combined —
    // and combining stored means is a second division on top of the first.
    // Two is the bound that follows, and it is asserted rather than assumed.
    let buffer = noisy(BASE * 20, 1);
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    let samples = buffer.channel(0).expect("a channel");

    let whole = overview.window(0, 0, 0, samples.len()).expect("a window");
    assert_eq!(whole.maximum(), *samples.iter().max().expect("a sample"));
    assert_eq!(whole.minimum(), *samples.iter().min().expect("a sample"));
    let apart = (whole.mean_square() - true_mean_square(samples)).abs();
    assert!(apart <= 2, "a window of the whole channel is {apart} out");

    // A window that starts and ends inside blocks takes the blocks it touches,
    // because a pixel showing part of a block must show that block rather than
    // nothing.
    let part = overview
        .window(0, 0, BASE + 5, BASE * 3 - 5)
        .expect("a window");
    let covered = &samples[BASE..BASE * 3];
    assert_eq!(part.maximum(), *covered.iter().max().expect("a sample"));
    let apart = (part.mean_square() - true_mean_square(covered)).abs();
    assert!(apart <= 2, "a partial window is {apart} out");
}

#[test]
fn a_short_last_block_is_not_drawn_quiet() {
    // The last block of a clip holds whatever is left, which is usually not a
    // whole block. Dividing its energy by a full block's width would draw the
    // end of every clip fading out — a fade nobody applied, at the exact place
    // an editor is looking for the end of the sound.
    let length = BASE * 4 + 3;
    let loud = std::vec![4_000_000_i32; length];
    let buffer = AudioBuffer::new(SampleRate::Hz48000, std::vec![loud]).expect("a buffer");
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    let blocks = overview.buckets(0, 0).expect("a level");
    assert_eq!(blocks.len(), 5, "the tail did not get a block of its own");

    let full = blocks[0].mean_square();
    let tail = blocks[4].mean_square();
    assert_eq!(
        tail, full,
        "three samples at the same level read differently from a full block"
    );
    assert_eq!(full, 4_000_000_i64 * 4_000_000);
}

#[test]
fn channels_are_summarised_apart() {
    // The last block of one channel and the first of the next are adjacent in
    // memory and not in time. Folding across that boundary would draw one
    // channel's ending into the other's beginning — and with a silent channel
    // beside a loud one, it would put sound on a track that has none.
    //
    // The length matters and is not arbitrary. Eleven blocks halve to six,
    // three, two, one, so two levels have an odd count and therefore a block
    // with no partner. That is the only place a fold can reach across a
    // channel, and a fixture with a power-of-two block count cannot catch it —
    // which the first attempt at this test did not, and the mutation went
    // through.
    //
    // Silence on both sides of the sound, because the boundary is crossed
    // forwards: a silent channel before a loud one is what shows it.
    let sound = noise(BASE * 11, 5);
    let quiet = std::vec![0_i32; BASE * 11];
    let channels = std::vec![quiet.clone(), sound, quiet];
    let buffer = AudioBuffer::new(SampleRate::Hz48000, channels).expect("a buffer");
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    assert!(
        (0..overview.level_count())
            .any(|level| overview.buckets(level, 0).expect("a level").len() % 2 == 1),
        "no level has an odd block count, so nothing here crosses a boundary"
    );

    for level in 0..overview.level_count() {
        for silent in [0, 2] {
            for bucket in overview.buckets(level, silent).expect("a level") {
                assert!(
                    bucket.is_silent(),
                    "level {level} put sound on silent channel {silent}"
                );
                assert_eq!(bucket.mean_square(), 0);
            }
        }
        assert!(
            overview
                .buckets(level, 1)
                .expect("a level")
                .iter()
                .any(|bucket| !bucket.is_silent()),
            "level {level} lost the sound from the channel that has it"
        );
    }
}

#[test]
fn the_levels_halve_until_one_block_covers_everything() {
    // The shape of the pyramid, stated as arithmetic rather than trusted. Two
    // things matter: each level is half the one below it rounded up, and the
    // last level is exactly one block per channel — a pyramid that stopped
    // early would leave the widest zoom unable to draw, and one that went on
    // would hold blocks describing samples that do not exist.
    let buffer = noisy(BASE * 100 + 1, 3);
    let overview = Overview::of(&buffer, BASE).expect("a summary");

    let mut expected = (BASE * 100 + 1).div_ceil(BASE);
    for level in 0..overview.level_count() {
        assert_eq!(
            overview.buckets(level, 0).expect("a level").len(),
            expected,
            "level {level} is not the width the halving implies"
        );
        assert_eq!(
            overview.samples_per_bucket(level).expect("a width"),
            BASE << level
        );
        expected = expected.div_ceil(FANOUT);
    }
    assert_eq!(
        overview
            .buckets(overview.level_count() - 1, 0)
            .expect("a level")
            .len(),
        1,
        "the pyramid does not reach a single block"
    );
    assert_eq!(
        overview.samples_per_bucket(overview.level_count()),
        Err(AudioStatus::NoSuchLevel)
    );
}

#[test]
fn the_whole_pyramid_costs_about_one_more_level_than_its_base() {
    // Why doubling is worth doing rather than storing one fine level and
    // combining at draw time. The geometric series above level zero sums to
    // less than level zero itself, so the entire zoom range costs under twice
    // the finest level — and drawing at any zoom is then a read rather than a
    // reduction over a million samples.
    let buffer = noisy(BASE * 512, 2);
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    let zero = overview.buckets(0, 0).expect("a level").len();
    let total: usize = (0..overview.level_count())
        .map(|level| overview.buckets(level, 0).expect("a level").len())
        .sum();
    assert!(
        total < zero * 2,
        "the pyramid costs {total} blocks against {zero} at level zero"
    );
    assert_eq!(overview.level_count(), 10, "512 blocks halve nine times");
}

#[test]
fn a_zoom_is_chosen_by_what_a_pixel_covers() {
    // What a drawing actually asks. Given how many samples one pixel stands
    // for, the useful level is the coarsest whose blocks still fit inside a
    // pixel: a finer one means reducing in the drawing code, a coarser one
    // means a block spanning pixels that then show the same thing.
    let buffer = noisy(BASE * 64, 1);
    let overview = Overview::of(&buffer, BASE).expect("a summary");

    assert_eq!(overview.level_for(BASE), 0);
    assert_eq!(overview.level_for(BASE * 2), 1);
    assert_eq!(overview.level_for(BASE * 2 - 1), 0);
    assert_eq!(overview.level_for(BASE * 8), 3);

    // Zoomed in past the finest detail the summary kept, the best it has is
    // level zero — which is a true answer, and better than a refusal, because
    // a caller at that zoom can go and read the samples themselves.
    assert_eq!(overview.level_for(1), 0);
    assert_eq!(overview.level_for(0), 0);

    // Zoomed out past the whole clip, the widest level there is.
    assert_eq!(
        overview.level_for(usize::MAX),
        overview.level_count() - 1,
        "asking for more than the clip should give the widest zoom, not overflow"
    );
}

#[test]
fn a_width_cannot_run_off_the_end_of_a_usize() {
    // `samples_per_bucket` shifts rather than checking, and this is why it is
    // allowed to. The pyramid stops when one block covers a whole channel, so
    // the widest block is under twice the sample count — never a function of
    // how many levels there happen to be. The worst case is therefore the
    // largest buffer this crate accepts, and it is nowhere near the end of a
    // `usize`.
    //
    // Keep the policy bounds representable by `usize` at every overview level.
    let deepest = MAX_SAMPLES.div_ceil(MIN_BUCKET);
    let mut levels = 1;
    let mut count = deepest;
    while count > 1 {
        count = count.div_ceil(FANOUT);
        levels += 1;
    }
    let widest = MAX_BUCKET
        .checked_shl(u32::try_from(levels).expect("a level count"))
        .expect("a width");
    assert!(
        widest < usize::MAX / 2,
        "the widest conceivable block is {widest}, which is close enough to the \
         end of a usize to want checking"
    );

    // And the widest a real summary reaches is smaller still, because a coarse
    // base gives few levels and a fine one gives narrow blocks.
    let buffer = noisy(BASE * 100, 1);
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    let top = overview
        .samples_per_bucket(overview.level_count() - 1)
        .expect("a width");
    assert!(
        top < buffer.len() * 2,
        "the coarsest block covers {top} samples of a {} sample channel",
        buffer.len()
    );
}

#[test]
fn a_summary_is_the_same_summary_every_time() {
    // The digest is what a cache is keyed on and what a peak file is checked
    // against, so it must depend on the samples and on nothing else.
    let buffer = noisy(BASE * 9, 2);
    let first = Overview::of(&buffer, BASE).expect("a summary");
    let second = Overview::of(&buffer, BASE).expect("a summary");
    assert_eq!(first.digest(), second.digest());
    assert_eq!(first, second);

    // A different block size is a different summary of the same sound.
    let coarser = Overview::of(&buffer, BASE * 2).expect("a summary");
    assert_ne!(first.digest(), coarser.digest());
    assert_eq!(first.source(), coarser.source(), "the source is the source");

    // A different sound is a different summary.
    let other = noisy(BASE * 9, 2);
    let changed = {
        let mut held: std::vec::Vec<std::vec::Vec<i32>> = (0..2)
            .map(|index| other.channel(index).expect("a channel").to_vec())
            .collect();
        held[0][0] = held[0][0].wrapping_add(1);
        AudioBuffer::new(SampleRate::Hz48000, held).expect("a buffer")
    };
    let after = Overview::of(&changed, BASE).expect("a summary");
    assert_ne!(
        after.source(),
        Overview::of(&other, BASE).expect("s").source()
    );
}

#[test]
fn a_summary_names_the_sound_it_summarises() {
    // The whole point of storing a peak file beside a media file: staleness is
    // something you can see rather than infer from a modification time, which
    // a copy, a restore or a clock change will happily lie about.
    let buffer = noisy(BASE * 6, 1);
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    assert_eq!(overview.source(), buffer.digest());
    assert_ne!(
        overview.digest(),
        buffer.digest(),
        "a summary is not the thing it summarises"
    );
}

#[test]
fn a_summary_survives_being_taken_apart_and_put_back() {
    // What reading a peak file does. Assembling from stored parts must give
    // back a summary that hashes the same, or the file is not a record of the
    // thing that was computed.
    let buffer = noisy(BASE * 21 + 4, 2);
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    let levels: std::vec::Vec<std::vec::Vec<Bucket>> = (0..overview.level_count())
        .map(|level| {
            (0..2)
                .flat_map(|channel| {
                    overview
                        .buckets(level, channel)
                        .expect("a level")
                        .iter()
                        .copied()
                })
                .collect()
        })
        .collect();
    let rebuilt = Overview::assemble(
        overview.rate(),
        overview.channel_count(),
        overview.len(),
        overview.base(),
        overview.source(),
        levels,
    )
    .expect("a summary");
    assert_eq!(rebuilt, overview);
    assert_eq!(rebuilt.digest(), overview.digest());
}

#[test]
fn a_block_size_that_would_not_tile_is_refused() {
    // A base that is not a power of two leaves a level whose blocks straddle
    // two of the level below, which is not a summary of anything.
    let buffer = noisy(BASE * 4, 1);
    for base in [0, 1, 100, MIN_BUCKET - 1, MAX_BUCKET * 2, MAX_BUCKET + 1] {
        assert_eq!(
            Overview::of(&buffer, base).map(|_| ()),
            Err(AudioStatus::BucketSizeUnsupported),
            "a base of {base} was accepted"
        );
    }
    assert!(Overview::of(&buffer, MIN_BUCKET).is_ok());
    assert!(Overview::of(&buffer, MAX_BUCKET).is_ok());
}

#[test]
fn a_summary_of_no_sound_is_refused_rather_than_returned_empty() {
    // An empty summary is a thing to index into by mistake and has nothing to
    // draw. Refusing by name puts the decision at the one place that knows
    // there is no sound.
    let nothing = AudioBuffer::silence(SampleRate::Hz48000, 2, 0).expect("a buffer");
    assert_eq!(
        Overview::of(&nothing, BASE).map(|_| ()),
        Err(AudioStatus::BufferTooShort)
    );
}

#[test]
fn a_file_that_is_not_a_pyramid_is_refused() {
    // A peak file is somebody else's bytes (R-11.1), so every relationship the
    // builder guarantees is checked again on the way in.
    let buffer = noisy(BASE * 8, 1);
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    let good: std::vec::Vec<std::vec::Vec<Bucket>> = (0..overview.level_count())
        .map(|level| overview.buckets(level, 0).expect("a level").to_vec())
        .collect();

    let assemble = |levels: std::vec::Vec<std::vec::Vec<Bucket>>| {
        Overview::assemble(
            SampleRate::Hz48000,
            1,
            BASE * 8,
            BASE,
            buffer.digest(),
            levels,
        )
        .map(|_| ())
    };
    assert!(
        assemble(good.clone()).is_ok(),
        "the real pyramid is refused"
    );

    // A level of the wrong width.
    let mut wide = good.clone();
    wide[1].push(Bucket::default());
    assert_eq!(assemble(wide), Err(AudioStatus::OverviewNotShaped));

    // Stopping before one block covers the channel: the widest zoom would have
    // nothing to draw.
    let mut short = good.clone();
    short.pop();
    assert_eq!(assemble(short), Err(AudioStatus::OverviewNotShaped));

    // Going on past it: blocks describing samples that do not exist.
    let mut long = good.clone();
    long.push(std::vec![Bucket::default()]);
    assert_eq!(assemble(long), Err(AudioStatus::OverviewNotShaped));

    // No levels at all.
    assert_eq!(
        assemble(std::vec::Vec::new()),
        Err(AudioStatus::OverviewNotShaped)
    );
}

#[test]
fn a_block_that_describes_no_samples_is_refused() {
    // A block whose lowest sample is above its highest, and one holding more
    // energy than samples can carry, are both things a corrupt file can say
    // and no buffer can produce.
    assert_eq!(
        Bucket::new(100, -100, 0).map(|_| ()),
        Err(AudioStatus::BucketNotOrdered)
    );
    assert_eq!(
        Bucket::new(-100, 100, -1).map(|_| ()),
        Err(AudioStatus::BucketNotPossible)
    );
    let beyond = 8_388_608_i64 * 8_388_608 + 1;
    assert_eq!(
        Bucket::new(-100, 100, beyond).map(|_| ()),
        Err(AudioStatus::BucketNotPossible)
    );
    // The square of the most negative sample is reachable, and is the largest
    // that is: a block of nothing but minus full scale.
    assert!(Bucket::new(-8_388_608, -8_388_608, 8_388_608_i64 * 8_388_608).is_ok());
}

#[test]
fn asking_for_what_is_not_there_is_refused_by_name() {
    let buffer = noisy(BASE * 8, 2);
    let overview = Overview::of(&buffer, BASE).expect("a summary");
    assert_eq!(
        overview.buckets(overview.level_count(), 0).map(|_| ()),
        Err(AudioStatus::NoSuchLevel)
    );
    assert_eq!(
        overview.buckets(0, 2).map(|_| ()),
        Err(AudioStatus::ChannelCountUnsupported)
    );
    assert_eq!(
        overview.window(0, 0, 10, 10).map(|_| ()),
        Err(AudioStatus::EmptyWindow),
        "a span of no samples is not a window"
    );
    assert_eq!(
        overview.window(0, 0, 20, 10).map(|_| ()),
        Err(AudioStatus::EmptyWindow),
        "a span that runs backwards is not a window"
    );
    assert_eq!(
        overview.window(0, 0, BASE * 8, BASE * 9).map(|_| ()),
        Err(AudioStatus::EmptyWindow),
        "a span past the end touches no block"
    );
}
