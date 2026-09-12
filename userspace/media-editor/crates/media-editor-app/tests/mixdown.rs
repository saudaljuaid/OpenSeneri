// SPDX-License-Identifier: GPL-3.0-only
//! Mixing a sequence's sound, and the sample counting behind it.

use media_editor_app::SlateStatus;
use media_editor_app::mixdown::{self, SampleSource};
use media_editor_audio::{AudioBuffer, SampleRate};
use media_editor_core::{Digest, Duration, Instant, Rational, TimeRange, Timebase};
use media_editor_model::curve::{Curve, Interpolation, Keyframe};
use media_editor_model::{Clip, Edit, Item, MediaAsset, MediaId, Project, SequenceId, TrackKind};

/// A source whose every sample is a constant, keyed by media.
///
/// Constant so that a mix is trivially predictable, and recorded so that the
/// test can check *which* samples were asked for, which is the part that goes
/// wrong.
struct Flat {
    levels: std::vec::Vec<(Digest, i32)>,
    rate: SampleRate,
    channels: usize,
    asked: std::vec::Vec<(Digest, i64, usize)>,
}

impl SampleSource for Flat {
    fn samples(
        &mut self,
        media: Digest,
        start: i64,
        count: usize,
    ) -> Result<AudioBuffer, media_editor_audio::AudioStatus> {
        self.asked.push((media, start, count));
        let level = self
            .levels
            .iter()
            .find(|(id, _)| *id == media)
            .map_or(0, |(_, level)| *level);
        let held: std::vec::Vec<std::vec::Vec<i32>> = (0..self.channels)
            .map(|_| std::vec![level; count])
            .collect();
        AudioBuffer::new(self.rate, held)
    }
}

/// What the vault would call this asset: its content digest.
///
/// A sound source is asked for a digest now rather than for a position in this
/// project's table, because a vault is shared between projects and a position
/// means something different in each. These fixtures follow.
fn digest_of(project: &Project, id: MediaId) -> Digest {
    project.media().get(id).expect("an asset").digest()
}

fn media(project: &mut Project, tag: u8, rate: Timebase) -> MediaId {
    let mut bytes = [0_u8; 32];
    bytes[0] = tag;
    let asset = MediaAsset::new(
        media_editor_model::media::Digest::new(bytes),
        rate,
        Duration::new(100_000, rate).expect("a duration"),
    )
    .expect("an asset");
    project.add_media(asset).expect("an identifier")
}

fn lay(project: &mut Project, sequence: SequenceId, track: usize, items: &[Item]) {
    while project
        .sequence(sequence)
        .expect("a sequence")
        .track_count()
        <= track
    {
        let index = project
            .sequence(sequence)
            .expect("a sequence")
            .track_count();
        project
            .apply(
                sequence,
                Edit::AddTrack {
                    index,
                    kind: TrackKind::Audio,
                },
            )
            .expect("a track");
    }
    for (index, item) in items.iter().enumerate() {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track,
                    index,
                    item: item.clone(),
                },
            )
            .expect("an insert");
    }
}

fn span(rate: Timebase, from: i64, frames: i64) -> TimeRange {
    TimeRange::new(
        Instant::new(from, rate),
        Duration::new(frames, rate).expect("a duration"),
    )
    .expect("a range")
}

#[test]
fn a_whole_rate_gives_a_whole_number_of_samples_per_frame() {
    // At 24 into 48 kHz a frame is 2000 samples exactly, so ten frames is
    // twenty thousand and every block is the same size.
    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let id = media(&mut project, 1, Timebase::FILM_24);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(
                id,
                0,
                Duration::new(10, Timebase::FILM_24).expect("a duration"),
            )
            .expect("a clip"),
        )],
    );
    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 1000)],
        rate: SampleRate::Hz48000,
        channels: 2,
        asked: std::vec::Vec::new(),
    };
    let (mixed, report) = mixdown::mix(
        &project,
        sequence,
        span(Timebase::FILM_24, 0, 10),
        SampleRate::Hz48000,
        2,
        &mut source,
    )
    .expect("a mix");

    assert_eq!(mixed.len(), 20_000);
    assert_eq!(mixed.channel_count(), 2);
    assert!(report.is_clean());
    assert!(
        mixed
            .channel(0)
            .expect("a channel")
            .iter()
            .all(|sample| *sample == 1000)
    );
    assert!(
        source.asked.iter().all(|(_, _, count)| *count == 2000),
        "every block is two thousand samples"
    );
}

#[test]
fn no_frame_at_ntsc_is_a_whole_number_of_samples_and_none_are_lost() {
    // The real case, and the one an implementation that multiplied frames by a
    // constant would get wrong. At 29.97 a frame is 1601.6 samples: some
    // blocks are 1601 and some are 1602, and over any span they sum to exactly
    // the samples that span holds. Nothing drifts.
    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::NTSC_30).expect("a sequence");
    let id = media(&mut project, 1, Timebase::NTSC_30);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(
                id,
                0,
                Duration::new(300, Timebase::NTSC_30).expect("a duration"),
            )
            .expect("a clip"),
        )],
    );
    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 7)],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    let (mixed, _) = mixdown::mix(
        &project,
        sequence,
        span(Timebase::NTSC_30, 0, 300),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");

    // Three hundred frames at 30000/1001 is 10.01 seconds, which is 480,480
    // samples exactly — a number arrived at by arithmetic, not by running the
    // code: 300 x 1001/30000 x 48000 = 480480.
    assert_eq!(mixed.len(), 480_480);
    assert_eq!(source.asked.len(), 300, "one block per frame");
    let counts: std::vec::Vec<usize> = source.asked.iter().map(|(_, _, c)| *c).collect();
    assert!(
        counts.iter().all(|count| *count == 1601 || *count == 1602),
        "every block is one of the two sizes and nothing else"
    );
    assert_eq!(
        counts.iter().sum::<usize>(),
        480_480,
        "and they tile the span exactly"
    );
    assert!(
        counts.contains(&1601) && counts.contains(&1602),
        "both sizes actually occur, or this test is checking nothing"
    );
}

#[test]
fn the_blocks_are_contiguous_with_no_gap_and_no_overlap() {
    // Each block must begin where the last one ended. A gap is a dropout and
    // an overlap is a stutter, and both are inaudible on a sine and obvious on
    // speech.
    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::NTSC_30).expect("a sequence");
    let id = media(&mut project, 1, Timebase::NTSC_30);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(
                id,
                0,
                Duration::new(120, Timebase::NTSC_30).expect("a duration"),
            )
            .expect("a clip"),
        )],
    );
    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 1)],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    mixdown::mix(
        &project,
        sequence,
        span(Timebase::NTSC_30, 0, 120),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");

    let mut expected = 0_i64;
    for (_, start, count) in &source.asked {
        assert_eq!(*start, expected, "a block began somewhere else");
        expected += i64::try_from(*count).expect("a count");
    }
}

#[test]
fn two_tracks_sum() {
    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let one = media(&mut project, 1, Timebase::FILM_24);
    let two = media(&mut project, 2, Timebase::FILM_24);
    for (track, id) in [(0, one), (1, two)] {
        lay(
            &mut project,
            sequence,
            track,
            &[Item::Clip(
                Clip::new(
                    id,
                    0,
                    Duration::new(2, Timebase::FILM_24).expect("a duration"),
                )
                .expect("a clip"),
            )],
        );
    }
    let mut source = Flat {
        levels: std::vec![
            (digest_of(&project, one), 100),
            (digest_of(&project, two), 250)
        ],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    let (mixed, report) = mixdown::mix(
        &project,
        sequence,
        span(Timebase::FILM_24, 0, 2),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");
    assert!(report.is_clean());
    assert!(
        mixed
            .channel(0)
            .expect("a channel")
            .iter()
            .all(|sample| *sample == 350)
    );
}

#[test]
fn a_gap_and_an_empty_sequence_are_silence_rather_than_a_hole() {
    // A programme with no sound at an instant has silence there, and an export
    // writes it. The buffer is still exactly the right length.
    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let id = media(&mut project, 1, Timebase::FILM_24);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::gap(Duration::new(2, Timebase::FILM_24).expect("a duration")).expect("a gap"),
            Item::Clip(
                Clip::new(
                    id,
                    0,
                    Duration::new(2, Timebase::FILM_24).expect("a duration"),
                )
                .expect("a clip"),
            ),
        ],
    );
    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 500)],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    let (mixed, _) = mixdown::mix(
        &project,
        sequence,
        span(Timebase::FILM_24, 0, 4),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");

    assert_eq!(mixed.len(), 8000);
    let samples = mixed.channel(0).expect("a channel");
    assert!(
        samples[..4000].iter().all(|sample| *sample == 0),
        "the gap is silence"
    );
    assert!(
        samples[4000..].iter().all(|sample| *sample == 500),
        "and the clip is not"
    );
    assert_eq!(source.asked.len(), 2, "nothing was decoded for the gap");

    // An empty sequence is the same statement, at full length.
    let empty = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let (silence, _) = mixdown::mix(
        &project,
        empty,
        span(Timebase::FILM_24, 0, 3),
        SampleRate::Hz48000,
        2,
        &mut source,
    )
    .expect("a mix");
    assert_eq!(
        silence,
        AudioBuffer::silence(SampleRate::Hz48000, 2, 6000).expect("silence")
    );
}

#[test]
fn the_source_position_is_the_clips_start_plus_how_far_in() {
    // The sound counterpart of the playhead arithmetic, and the same failure:
    // wrong for the whole clip rather than for one block of it.
    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let id = media(&mut project, 1, Timebase::FILM_24);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(
                id,
                48,
                Duration::new(3, Timebase::FILM_24).expect("a duration"),
            )
            .expect("a clip"),
        )],
    );
    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 1)],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    mixdown::mix(
        &project,
        sequence,
        span(Timebase::FILM_24, 0, 3),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");

    // Source frame 48 at 24 fps is two seconds in, which at 48 kHz is sample
    // 96,000; each following frame advances by 2000.
    assert_eq!(
        source.asked,
        std::vec![
            (digest_of(&project, id), 96_000, 2000),
            (digest_of(&project, id), 98_000, 2000),
            (digest_of(&project, id), 100_000, 2000)
        ]
    );
}

#[test]
fn a_source_that_answers_with_a_different_span_is_refused() {
    // Padding or resampling it here would be a decision made in the wrong
    // place: the source was told how many samples to produce.
    struct Short;
    impl SampleSource for Short {
        fn samples(
            &mut self,
            _media: Digest,
            _start: i64,
            count: usize,
        ) -> Result<AudioBuffer, media_editor_audio::AudioStatus> {
            AudioBuffer::silence(SampleRate::Hz48000, 1, count.saturating_sub(1))
        }
    }

    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let id = media(&mut project, 1, Timebase::FILM_24);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(
                id,
                0,
                Duration::new(1, Timebase::FILM_24).expect("a duration"),
            )
            .expect("a clip"),
        )],
    );
    assert_eq!(
        mixdown::mix(
            &project,
            sequence,
            span(Timebase::FILM_24, 0, 1),
            SampleRate::Hz48000,
            1,
            &mut Short,
        ),
        Err(SlateStatus::Audio(
            media_editor_audio::AudioStatus::NotMixable
        ))
    );
}

#[test]
fn a_mixdown_is_the_same_mixdown_every_time() {
    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::NTSC_30).expect("a sequence");
    let id = media(&mut project, 1, Timebase::NTSC_30);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(
                id,
                5,
                Duration::new(30, Timebase::NTSC_30).expect("a duration"),
            )
            .expect("a clip"),
        )],
    );
    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 123)],
        rate: SampleRate::Hz48000,
        channels: 2,
        asked: std::vec::Vec::new(),
    };
    let first = mixdown::mix(
        &project,
        sequence,
        span(Timebase::NTSC_30, 0, 30),
        SampleRate::Hz48000,
        2,
        &mut source,
    )
    .expect("a mix");
    let second = mixdown::mix(
        &project,
        sequence,
        span(Timebase::NTSC_30, 0, 30),
        SampleRate::Hz48000,
        2,
        &mut source,
    )
    .expect("a mix");
    assert_eq!(first.0.digest(), second.0.digest());
}

#[test]
fn the_two_fader_bounds_agree() {
    // The model may not depend on the sound crate — they are siblings — so the
    // fader's travel is written down in both. This is the only place that sees
    // both, so it is where the two are checked against each other. If they
    // ever drift, a level the model accepts would be one the mixer refuses,
    // and a project would fail to play rather than fail to load.
    assert_eq!(
        media_editor_model::MINIMUM_DECIBELS,
        media_editor_audio::MINIMUM_DECIBELS
    );
    assert_eq!(
        media_editor_model::MAXIMUM_DECIBELS,
        media_editor_audio::MAXIMUM_DECIBELS
    );
}

#[test]
fn a_fader_at_unity_changes_nothing_at_all() {
    // Not "changes almost nothing". A track nobody has touched must arrive at
    // the bus bit for bit, or every bounce of an untouched session differs
    // from its source.
    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let id = media(&mut project, 1, Timebase::FILM_24);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(
                id,
                0,
                Duration::new(2, Timebase::FILM_24).expect("a duration"),
            )
            .expect("a clip"),
        )],
    );
    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 12_345)],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    let (mixed, _) = mixdown::mix(
        &project,
        sequence,
        span(Timebase::FILM_24, 0, 2),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");
    assert!(
        mixed
            .channel(0)
            .expect("a channel")
            .iter()
            .all(|sample| *sample == 12_345)
    );
}

#[test]
fn a_fader_move_reaches_the_mix() {
    // Twenty decibels down is a factor of ten, exactly, which is why it is the
    // level this checks at: the expected sample is arrived at by dividing
    // rather than by running the code.
    use media_editor_core::Rational;
    use media_editor_model::Fader;

    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let id = media(&mut project, 1, Timebase::FILM_24);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(
                id,
                0,
                Duration::new(1, Timebase::FILM_24).expect("a duration"),
            )
            .expect("a clip"),
        )],
    );
    project
        .apply(
            sequence,
            Edit::SetTrackFader {
                track: 0,
                fader: Fader::at(Rational::new(-20, 1).expect("a ratio")).expect("a level"),
            },
        )
        .expect("a fader move");

    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 10_000)],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    let (mixed, _) = mixdown::mix(
        &project,
        sequence,
        span(Timebase::FILM_24, 0, 1),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");
    assert!(
        mixed
            .channel(0)
            .expect("a channel")
            .iter()
            .all(|sample| *sample == 1000),
        "ten thousand at minus twenty decibels is one thousand"
    );
}

#[test]
fn a_muted_track_is_neither_heard_nor_decoded() {
    // Two claims. A muted track contributes nothing, and — because there is
    // nothing it could contribute — its media is never read. Reading media for
    // a track nobody will hear is the difference between a mix that keeps up
    // and one that does not.
    use media_editor_model::Fader;

    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let heard = media(&mut project, 1, Timebase::FILM_24);
    let silenced = media(&mut project, 2, Timebase::FILM_24);
    for (track, id) in [(0, heard), (1, silenced)] {
        lay(
            &mut project,
            sequence,
            track,
            &[Item::Clip(
                Clip::new(
                    id,
                    0,
                    Duration::new(1, Timebase::FILM_24).expect("a duration"),
                )
                .expect("a clip"),
            )],
        );
    }
    project
        .apply(
            sequence,
            Edit::SetTrackFader {
                track: 1,
                fader: Fader::MUTED,
            },
        )
        .expect("a mute");

    let mut source = Flat {
        levels: std::vec![
            (digest_of(&project, heard), 500),
            (digest_of(&project, silenced), 9_000)
        ],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    let (mixed, _) = mixdown::mix(
        &project,
        sequence,
        span(Timebase::FILM_24, 0, 1),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");
    assert!(
        mixed
            .channel(0)
            .expect("a channel")
            .iter()
            .all(|sample| *sample == 500),
        "only the unmuted track is heard"
    );
    assert!(
        source
            .asked
            .iter()
            .all(|(id, _, _)| *id == digest_of(&project, heard)),
        "and the muted one was never read"
    );
}

#[test]
fn muting_every_track_is_silence_of_the_right_length() {
    use media_editor_model::Fader;

    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let id = media(&mut project, 1, Timebase::FILM_24);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(
                id,
                0,
                Duration::new(3, Timebase::FILM_24).expect("a duration"),
            )
            .expect("a clip"),
        )],
    );
    project
        .apply(
            sequence,
            Edit::SetTrackFader {
                track: 0,
                fader: Fader::MUTED,
            },
        )
        .expect("a mute");

    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 8_000)],
        rate: SampleRate::Hz48000,
        channels: 2,
        asked: std::vec::Vec::new(),
    };
    let (mixed, _) = mixdown::mix(
        &project,
        sequence,
        span(Timebase::FILM_24, 0, 3),
        SampleRate::Hz48000,
        2,
        &mut source,
    )
    .expect("a mix");
    assert_eq!(
        mixed,
        AudioBuffer::silence(SampleRate::Hz48000, 2, 6000).expect("silence")
    );
    assert!(source.asked.is_empty());
}

/// A curve in decibels over a sound track's own timebase.
fn level(rate: Timebase, points: &[(i64, i64)]) -> Curve {
    let mut keyframes = std::vec::Vec::new();
    for (index, (frame, decibels)) in points.iter().enumerate() {
        let how = if index + 1 == points.len() {
            Interpolation::Hold
        } else {
            Interpolation::Linear
        };
        keyframes.push(
            Keyframe::new(
                Instant::new(*frame, rate),
                Rational::new(*decibels, 1).expect("a value"),
                how,
            )
            .expect("a keyframe"),
        );
    }
    Curve::new(keyframes).expect("a curve")
}

/// A one-track sequence of one clip, mixed over `frames` frames.
fn mixed_with(
    curve: Option<Curve>,
    frames: i64,
    sample: i32,
) -> (AudioBuffer, media_editor_audio::MixReport) {
    let rate = Timebase::FILM_24;
    let mut project = Project::new();
    let sequence = project.add_sequence(rate).expect("a sequence");
    let id = media(&mut project, 1, rate);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(id, 0, Duration::new(frames, rate).expect("a duration")).expect("a clip"),
        )],
    );
    if let Some(curve) = curve {
        project
            .apply(
                sequence,
                Edit::SetTrackLevel {
                    track: 0,
                    level: Some(curve),
                },
            )
            .expect("an automation");
    }
    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), sample)],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    mixdown::mix(
        &project,
        sequence,
        span(rate, 0, frames),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix")
}

#[test]
fn an_automated_fader_moves_within_a_frame_and_not_only_between_them() {
    // The fault this exists to prevent. A gain applied one frame at a time and
    // held flat across each frame puts a step at every frame boundary — a
    // buzz at twenty-four hertz on any fast move, which is the noise every
    // mixer that ever shipped has had to be taught not to make.
    //
    // A fade from unity to −40 dB over four frames at 24 into 48 kHz is 8000
    // samples. If the fader only moved between frames there would be exactly
    // four distinct values in the whole buffer.
    let (buffer, _) = mixed_with(
        Some(level(Timebase::FILM_24, &[(0, 0), (4, -40)])),
        4,
        4_000_000,
    );
    let held = buffer.channel(0).expect("a channel");
    assert_eq!(held.len(), 8_000);

    let mut distinct = std::collections::BTreeSet::new();
    for sample in held {
        distinct.insert(*sample);
    }
    assert!(
        distinct.len() > 1_000,
        "a fade over 8000 samples took only {} distinct values, which is a \
         fader moving between frames rather than within them",
        distinct.len()
    );
}

#[test]
fn an_automated_fade_has_no_step_at_a_frame_boundary() {
    // The property that makes it inaudible. A frame at 24 into 48 kHz is 2000
    // samples, so the boundaries are at 2000, 4000 and 6000. The step across
    // each must be no larger than the steps on either side of it: the fader
    // arrives at the next frame's position from *inside* this frame, because
    // the ramp's interval is half open and both blocks are handed the same
    // number for the instant they share.
    let (buffer, _) = mixed_with(
        Some(level(Timebase::FILM_24, &[(0, 0), (4, -40)])),
        4,
        8_000_000,
    );
    let held = buffer.channel(0).expect("a channel");
    for boundary in [2_000_usize, 4_000, 6_000] {
        let across = (held[boundary] - held[boundary - 1]).abs();
        let before = (held[boundary - 1] - held[boundary - 2]).abs();
        let after = (held[boundary + 1] - held[boundary]).abs();
        assert!(
            across <= before.max(after) + 1,
            "frame boundary at {boundary} steps by {across} where its \
             neighbours step by {before} and {after}"
        );
    }
}

#[test]
fn an_automated_fader_starts_and_ends_where_the_curve_says() {
    // The ends are exact, whatever happens in between. Unity is a copy, and
    // −20 dB is exactly a tenth.
    let (buffer, _) = mixed_with(
        Some(level(Timebase::FILM_24, &[(0, 0), (2, -20)])),
        3,
        1_000_000,
    );
    let held = buffer.channel(0).expect("a channel");
    assert_eq!(held[0], 1_000_000, "it did not start at unity");
    // Frame 2 onwards the curve holds at −20 dB, so every sample from 4000 is
    // exactly a tenth.
    assert_eq!(held[4_000], 100_000);
    assert_eq!(held[5_999], 100_000);
}

#[test]
fn a_track_with_no_automation_still_reads_its_static_fader() {
    // Automation replaces the fader; its absence hands the track back to the
    // static one, which is not the same as a curve sitting at that value.
    let rate = Timebase::FILM_24;
    let mut project = Project::new();
    let sequence = project.add_sequence(rate).expect("a sequence");
    let id = media(&mut project, 1, rate);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(id, 0, Duration::new(2, rate).expect("a duration")).expect("a clip"),
        )],
    );
    project
        .apply(
            sequence,
            Edit::SetTrackFader {
                track: 0,
                fader: media_editor_model::Fader::at(Rational::new(-20, 1).expect("a value"))
                    .expect("a fader"),
            },
        )
        .expect("a fader");
    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 1_000_000)],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    let (buffer, _) = mixdown::mix(
        &project,
        sequence,
        span(rate, 0, 2),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");
    let held = buffer.channel(0).expect("a channel");
    assert!(
        held.iter().all(|sample| *sample == 100_000),
        "the static fader was not read"
    );
}

#[test]
fn a_muted_track_stays_muted_under_automation() {
    // Mute is a switch, not a fader position, and automation drives the
    // fader. A curve holds decibels and cannot reach the off detent at all —
    // its floor is very quiet and is still a level. So a track turned off at
    // the surface must not come back on because somebody drew a fade on it.
    //
    // This test exists because the first version did exactly that: `fader_at`
    // read the curve and never looked at the static fader, so automation
    // silently overrode mute. The name of a test written honestly is what
    // found it.
    let rate = Timebase::FILM_24;
    let mut project = Project::new();
    let sequence = project.add_sequence(rate).expect("a sequence");
    let id = media(&mut project, 1, rate);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(id, 0, Duration::new(2, rate).expect("a duration")).expect("a clip"),
        )],
    );
    project
        .apply(
            sequence,
            Edit::SetTrackLevel {
                track: 0,
                level: Some(level(rate, &[(0, 0), (2, 0)])),
            },
        )
        .expect("an automation");
    project
        .apply(
            sequence,
            Edit::SetTrackFader {
                track: 0,
                fader: media_editor_model::Fader::MUTED,
            },
        )
        .expect("a mute");

    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 8_000_000)],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    let (buffer, _) = mixdown::mix(
        &project,
        sequence,
        span(rate, 0, 2),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");
    assert!(
        buffer
            .channel(0)
            .expect("a channel")
            .iter()
            .all(|sample| *sample == 0),
        "automation spoke over a mute"
    );
    // And nothing was read for it, which is the other half of muting.
    assert!(source.asked.is_empty(), "a muted track was decoded anyway");
}

#[test]
fn a_fade_up_from_the_floor_arrives() {
    // The quietest a curve can go is the fader's floor, which is very quiet
    // and is *not* the mute detent — so the track is decoded and every sample
    // of the fade is heard. A fade from the floor to unity over two frames
    // must arrive: the last sample is near full where the first is near
    // nothing.
    let (buffer, _) = mixed_with(
        Some(level(Timebase::FILM_24, &[(0, -120), (2, 0)])),
        2,
        8_000_000,
    );
    let held = buffer.channel(0).expect("a channel");
    assert_eq!(held.len(), 4_000);
    assert!(
        held[3_999] > 4_000_000,
        "the fade never arrived: the last sample is {}",
        held[3_999]
    );
    assert!(
        held.iter().any(|sample| *sample != 0),
        "the whole fade-in was silent"
    );
}

#[test]
fn a_fader_level_on_a_picture_track_is_refused() {
    // A picture track's level is its opacity. A fader curve on one would be a
    // second level with different units that nothing reads.
    let rate = Timebase::FILM_24;
    let mut project = Project::new();
    let sequence = project.add_sequence(rate).expect("a sequence");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    assert_eq!(
        project.apply(
            sequence,
            Edit::SetTrackLevel {
                track: 0,
                level: Some(level(rate, &[(0, 0), (10, -6)])),
            },
        ),
        Err(media_editor_model::ModelStatus::LevelOnPicture)
    );
}

#[test]
fn a_sound_clip_fades_up_from_silence() {
    // The gesture a cut cannot make, on the sound side. A fade on the clip is
    // a fraction of the *material*; the fader is a position on a console. They
    // are two different things multiplied together, so the fade scales the
    // samples and the fader scales the source, rather than one of them being
    // converted into the other's units — which for a fraction and a decibel
    // would mean a logarithm at every sample.
    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let id = media(&mut project, 1, Timebase::FILM_24);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(
                id,
                0,
                Duration::new(4, Timebase::FILM_24).expect("a duration"),
            )
            .expect("a clip"),
        )],
    );
    project
        .apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: Duration::new(2, Timebase::FILM_24).expect("a duration"),
                fade_out: Duration::new(0, Timebase::FILM_24).expect("a duration"),
            },
        )
        .expect("a fade in");
    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 10_000)],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    let (mixed, _) = mixdown::mix(
        &project,
        sequence,
        span(Timebase::FILM_24, 0, 4),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");
    let samples = mixed.channel(0).expect("a channel");
    assert_eq!(samples[0], 0, "the first sample is silence");
    assert!(
        samples[1] > 0 && samples[1] < 10_000,
        "and the second is on its way up: {}",
        samples[1]
    );
    let per_frame = samples.len() / 4;
    assert_eq!(
        samples[2 * per_frame],
        10_000,
        "two frames in, the fade has finished"
    );
    assert_eq!(
        samples[samples.len() - 1],
        10_000,
        "and it stays finished to the end"
    );
    // Rising, sample by sample, with no step at the block boundary -- which is
    // what the half-open pair is for. A fade that arrived at its value on the
    // last sample of each block instead would repeat one value at every seam,
    // and a repetition at a regular interval is a tone.
    for pair in samples[..2 * per_frame].windows(2) {
        assert!(pair[1] >= pair[0], "{pair:?} goes backwards");
    }
}

#[test]
fn a_clip_nobody_faded_arrives_at_the_bus_bit_for_bit() {
    // The fixture that would have caught a fade being applied where there is
    // none -- and it did: reading the *next frame's* value off the track
    // rather than off the clip made every unfaded clip duck to silence over
    // its last block, and this said so within a minute.
    let mut project = Project::new();
    let sequence = project.add_sequence(Timebase::FILM_24).expect("a sequence");
    let id = media(&mut project, 1, Timebase::FILM_24);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(
                id,
                0,
                Duration::new(3, Timebase::FILM_24).expect("a duration"),
            )
            .expect("a clip"),
        )],
    );
    let mut source = Flat {
        levels: std::vec![(digest_of(&project, id), 12_345)],
        rate: SampleRate::Hz48000,
        channels: 1,
        asked: std::vec::Vec::new(),
    };
    let (mixed, _) = mixdown::mix(
        &project,
        sequence,
        span(Timebase::FILM_24, 0, 3),
        SampleRate::Hz48000,
        1,
        &mut source,
    )
    .expect("a mix");
    assert!(
        mixed
            .channel(0)
            .expect("a channel")
            .iter()
            .all(|sample| *sample == 12_345),
        "including the last block, which is where an end-of-clip fade would show"
    );
}
