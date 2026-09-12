// SPDX-License-Identifier: GPL-3.0-only
//! Captions that track the edit.
//!
//! One claim does the work here: **not one line of `Edit` knows captions
//! exist, and every edit moves them correctly anyway.** Every test below is
//! that claim against a different operation — a cut, a trim, a slip, a ripple,
//! a retime, an undo — and each one applies a real edit through
//! `Project::apply` rather than building the after-state by hand, because
//! building it by hand would be testing the fixture.
//!
//! The mechanism is that a caption is anchored in the *source*, so the
//! timeline never has to be told anything. What it costs is inverting the map
//! a clip applies to time, which is `Clip::offsets_showing` and has its own
//! file.

use media_editor_core::{Digest, Duration, Instant, Rational, TimeRange, Timebase};
use media_editor_model::caption::{self, Caption};
use media_editor_model::{
    Clip, Edit, Item, MediaAsset, MediaId, ModelStatus, Project, SequenceId, TrackKind,
};

const RATE: Timebase = Timebase::FILM_24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a length")
}

fn at(ticks: i64) -> Instant {
    Instant::new(ticks, RATE)
}

fn span(from: i64, count: i64) -> TimeRange {
    TimeRange::new(at(from), frames(count)).expect("a span")
}

/// One line of what is on screen, as the projection reports it.
fn line(from: i64, to: i64, text: &str) -> (i64, i64, std::string::String) {
    (from, to, text.into())
}

fn said(from: i64, to: i64, text: &str) -> Caption {
    Caption::new(from, to, 0, text).expect("a caption")
}

/// A project with one captioned recording and one sequence.
///
/// The transcript is three sentences over source ticks 0..30, so a clip that
/// reads part of the recording reads part of the transcript.
fn interview() -> (Project, SequenceId, MediaId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let asset = MediaAsset::new(Digest::of(b"an interview"), RATE, frames(1000))
        .expect("an asset")
        .with_captions(std::vec![
            said(0, 10, "the first thing he said"),
            said(10, 20, "the second thing"),
            said(20, 30, "and the third"),
        ])
        .expect("a transcript");
    let media = project.add_media(asset).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    (project, sequence, media)
}

fn lay(project: &mut Project, sequence: SequenceId, index: usize, item: Item) {
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index,
                item,
            },
        )
        .expect("an insert");
}

/// What is on screen over a span, as `(from, to, text)`, in timeline order.
fn shown(
    project: &Project,
    sequence: SequenceId,
    over: TimeRange,
) -> std::vec::Vec<(i64, i64, std::string::String)> {
    let mut found: std::vec::Vec<(i64, i64, std::string::String)> =
        caption::captions_over(project, sequence, over, &mut caption::Held::new(project))
            .expect("a projection")
            .into_iter()
            .map(|shown| {
                (
                    shown.from.ticks(),
                    shown.to.ticks(),
                    shown.caption.text().into(),
                )
            })
            .collect();
    found.sort_by_key(|(from, to, _)| (*from, *to));
    found
}

#[test]
fn a_clip_shows_the_words_it_reads_and_no_others() {
    // The starting point everything below is a change to. A clip in at source
    // tick 10 for ten frames reads the second sentence and nothing else.
    let (mut project, sequence, media) = interview();
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 10, frames(10)).expect("a clip")),
    );
    assert_eq!(
        shown(&project, sequence, span(0, 10)),
        std::vec![line(0, 10, "the second thing")]
    );
}

#[test]
fn a_sentence_across_a_cut_appears_on_both_halves() {
    // The case that decides the design. A razor through the middle of a clip
    // leaves two clips reading two halves of one sentence, so the sentence is
    // reported twice -- once for each half, each clipped to what it shows.
    // Merging them would be wrong: an editor can now move the halves apart.
    let (mut project, sequence, media) = interview();
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 10, frames(10)).expect("a clip")),
    );
    project
        .apply(
            sequence,
            Edit::SplitItem {
                track: 0,
                index: 0,
                offset: 4,
            },
        )
        .expect("a cut");
    let after = shown(&project, sequence, span(0, 10));
    assert_eq!(
        after,
        std::vec![
            line(0, 4, "the second thing"),
            line(4, 10, "the second thing"),
        ],
        "a sentence cut in two was not reported for both halves"
    );
    // And undoing the cut puts it back as one, with nothing about captions
    // having been undone -- because nothing about captions was ever done.
    project.undo(sequence).expect("an undo");
    assert_eq!(
        shown(&project, sequence, span(0, 10)),
        std::vec![line(0, 10, "the second thing")]
    );
}

#[test]
fn rippling_an_earlier_shot_moves_the_words_with_the_picture() {
    // Nothing in this edit touches the captioned clip, and nothing in the
    // model moves a caption. The words move because the picture did.
    let (mut project, sequence, media) = interview();
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 10, frames(10)).expect("a clip")),
    );
    assert_eq!(
        shown(&project, sequence, span(0, 40)),
        std::vec![line(0, 10, "the second thing")]
    );
    // Six frames of black in front of it.
    lay(
        &mut project,
        sequence,
        0,
        Item::gap(frames(6)).expect("a gap"),
    );
    assert_eq!(
        shown(&project, sequence, span(0, 40)),
        std::vec![line(6, 16, "the second thing")],
        "the words did not ripple with the shot"
    );
}

#[test]
fn slipping_a_shot_changes_which_words_are_on_it() {
    // Slipping changes *which* of the recording is shown without moving the
    // clip, and the transcript is of the recording -- so the words change and
    // their position does not. This is the case a timeline-anchored caption
    // gets exactly backwards.
    let (mut project, sequence, media) = interview();
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 0, frames(10)).expect("a clip")),
    );
    assert_eq!(
        shown(&project, sequence, span(0, 10)),
        std::vec![line(0, 10, "the first thing he said")]
    );
    project
        .apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 10,
            },
        )
        .expect("a slip");
    assert_eq!(
        shown(&project, sequence, span(0, 10)),
        std::vec![line(0, 10, "the second thing")],
        "slipping the shot did not slip the words"
    );
}

#[test]
fn trimming_a_shot_trims_the_words_it_shows() {
    let (mut project, sequence, media) = interview();
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 5, frames(20)).expect("a clip")),
    );
    // Source 5..25 covers the tail of the first sentence, all of the second,
    // and the head of the third.
    assert_eq!(
        shown(&project, sequence, span(0, 20)),
        std::vec![
            line(0, 5, "the first thing he said"),
            line(5, 15, "the second thing"),
            line(15, 20, "and the third"),
        ]
    );
    // Trim five frames off the front: the first sentence goes entirely.
    project
        .apply(
            sequence,
            Edit::SetClipSource {
                track: 0,
                index: 0,
                source_start: 10,
            },
        )
        .expect("a slip");
    project
        .apply(
            sequence,
            Edit::SetItemDuration {
                track: 0,
                index: 0,
                duration: frames(15),
            },
        )
        .expect("a trim");
    assert_eq!(
        shown(&project, sequence, span(0, 20)),
        std::vec![
            line(0, 10, "the second thing"),
            line(10, 15, "and the third")
        ]
    );
}

#[test]
fn retiming_a_shot_stretches_the_words_with_it() {
    // The one that needs the inverse. At half speed a ten-frame sentence takes
    // twenty frames of programme; at double speed it takes five. Neither
    // number is stored anywhere.
    let (mut project, sequence, media) = interview();
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(
            Clip::new(media, 0, frames(60))
                .expect("a clip")
                .with_speed(Rational::new(1, 2).expect("a speed"))
                .expect("a speed"),
        ),
    );
    // Half speed: source 0..10 is programme 0..20, 10..20 is 20..40, and
    // 20..30 is 40..60. Worked out from the speed, not read back.
    assert_eq!(
        shown(&project, sequence, span(0, 60)),
        std::vec![
            line(0, 20, "the first thing he said"),
            line(20, 40, "the second thing"),
            line(40, 60, "and the third"),
        ]
    );
    project.undo(sequence).expect("an undo");
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(
            Clip::new(media, 0, frames(15))
                .expect("a clip")
                .with_speed(Rational::new(2, 1).expect("a speed"))
                .expect("a speed"),
        ),
    );
    // Double speed: each ten-tick sentence is five frames.
    assert_eq!(
        shown(&project, sequence, span(0, 15)),
        std::vec![
            line(0, 5, "the first thing he said"),
            line(5, 10, "the second thing"),
            line(10, 15, "and the third"),
        ]
    );
}

#[test]
fn a_span_reports_only_what_is_on_screen_in_it() {
    let (mut project, sequence, media) = interview();
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 0, frames(30)).expect("a clip")),
    );
    // A window over the middle sentence only, and clipped to the window.
    assert_eq!(
        shown(&project, sequence, span(12, 5)),
        std::vec![line(12, 17, "the second thing")]
    );
    // One instant, which is what a viewer asks.
    let now = caption::captions_at(
        &project,
        sequence,
        at(25),
        &mut caption::Held::new(&project),
    )
    .expect("a query");
    assert_eq!(now.len(), 1);
    assert_eq!(now[0].caption.text(), "and the third");
    assert_eq!((now[0].from.ticks(), now[0].to.ticks()), (25, 26));
    // And an instant with nothing on it.
    assert!(
        caption::captions_at(
            &project,
            sequence,
            at(40),
            &mut caption::Held::new(&project)
        )
        .expect("a query")
        .is_empty()
    );
}

#[test]
fn two_voices_may_speak_at_once_and_one_may_not() {
    // A conversation is two captions covering the same ticks, which a single
    // sorted list of captions gets wrong -- so it is the case the check is
    // about.
    let both = std::vec![
        Caption::new(0, 10, 0, "did you get that").expect("a caption"),
        Caption::new(5, 15, 1, "most of it").expect("a caption"),
    ];
    caption::checked(&both).expect("two voices may overlap");
    let one = std::vec![
        Caption::new(0, 10, 0, "did you get that").expect("a caption"),
        Caption::new(9, 15, 0, "most of it").expect("a caption"),
    ];
    assert_eq!(caption::checked(&one), Err(ModelStatus::CaptionsOverlap));
    // Meeting is not overlapping: half-open at both ends.
    let meeting = std::vec![
        Caption::new(0, 10, 0, "did you get that").expect("a caption"),
        Caption::new(10, 15, 0, "most of it").expect("a caption"),
    ];
    caption::checked(&meeting).expect("captions that meet do not overlap");
    // And both voices reach the timeline.
    let (mut project, sequence, _) = interview();
    let asset = MediaAsset::new(Digest::of(b"a conversation"), RATE, frames(100))
        .expect("an asset")
        .with_captions(both)
        .expect("a transcript");
    let media = project.add_media(asset).expect("room");
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 0, frames(20)).expect("a clip")),
    );
    let at_seven =
        caption::captions_at(&project, sequence, at(7), &mut caption::Held::new(&project))
            .expect("a query");
    assert_eq!(at_seven.len(), 2, "only one speaker was reported");
    let mut voices: std::vec::Vec<u8> = at_seven.iter().map(|s| s.caption.voice()).collect();
    voices.sort_unstable();
    assert_eq!(voices, std::vec![0, 1]);
}

#[test]
fn a_caption_inside_a_nested_sequence_reaches_the_programme() {
    // A nest's timeline *is* the media its clip reads, so lifting a caption
    // out of one is the same question asked again. Nothing new was written for
    // this beyond composing the answer.
    let (mut project, inner, media) = interview();
    lay(
        &mut project,
        inner,
        0,
        Item::Clip(Clip::new(media, 10, frames(10)).expect("a clip")),
    );
    let outer = project.add_sequence(RATE).expect("a sequence");
    let nest = project
        .add_media(MediaAsset::nesting(inner, RATE, frames(10)).expect("a nest"))
        .expect("room");
    project
        .apply(
            outer,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    // The nest laid six frames in, reading from its own tick 2.
    lay(&mut project, outer, 0, Item::gap(frames(6)).expect("a gap"));
    lay(
        &mut project,
        outer,
        1,
        Item::Clip(Clip::new(nest, 2, frames(8)).expect("a clip")),
    );
    // Inside the nest the sentence is at 0..10; the outer clip reads the
    // nest's 2..10 and sits at 6, so it lands at 6..14.
    assert_eq!(
        shown(&project, outer, span(0, 30)),
        std::vec![line(6, 14, "the second thing")]
    );
    // And the inner sequence still answers for itself, unchanged.
    assert_eq!(
        shown(&project, inner, span(0, 10)),
        std::vec![line(0, 10, "the second thing")]
    );
}

#[test]
fn a_query_that_would_report_more_than_the_bound_is_refused() {
    // R-1.4: a partial transcript that looks whole is worse than none, so the
    // bound is a refusal rather than a truncation.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let asset = MediaAsset::new(
        Digest::of(b"a very talkative subject"),
        RATE,
        frames(10_000),
    )
    .expect("an asset")
    .with_captions(
        (0..caption::MAX_CAPTIONS_PER_ASSET)
            .map(|index| {
                let from = i64::try_from(index).expect("a tick") * 2;
                Caption::new(from, from + 2, 0, "a word").expect("a caption")
            })
            .collect(),
    )
    .expect("a transcript");
    let media = project.add_media(asset).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    // Sixty-four captions a clip, and nine clips is 576 -- past the bound of
    // 512, which eight clips at 512 exactly is not.
    for index in 0..8 {
        lay(
            &mut project,
            sequence,
            index,
            Item::Clip(Clip::new(media, 0, frames(128)).expect("a clip")),
        );
    }
    assert_eq!(
        caption::captions_over(
            &project,
            sequence,
            span(0, 8 * 128),
            &mut caption::Held::new(&project)
        )
        .expect("a query")
        .len(),
        caption::MAX_CAPTIONS_SHOWN
    );
    lay(
        &mut project,
        sequence,
        8,
        Item::Clip(Clip::new(media, 0, frames(128)).expect("a clip")),
    );
    assert_eq!(
        caption::captions_over(
            &project,
            sequence,
            span(0, 9 * 128),
            &mut caption::Held::new(&project)
        ),
        Err(ModelStatus::TooManyCaptions)
    );
}

#[test]
fn a_caption_a_person_could_not_have_made_is_refused() {
    // Every bound the model puts on a transcript, checked at the door rather
    // than discovered by whatever draws it. A file cannot talk its way past
    // these either: the reader comes through the same constructor.
    assert_eq!(
        Caption::new(10, 5, 0, "backwards"),
        Err(ModelStatus::EmptyCaption)
    );
    assert_eq!(
        Caption::new(10, 10, 0, "nothing"),
        Err(ModelStatus::EmptyCaption)
    );
    Caption::new(10, 11, 0, "one tick").expect("a caption of one tick is a caption");

    let long: std::string::String =
        core::iter::repeat_n('a', caption::MAX_CAPTION_TEXT + 1).collect();
    assert_eq!(
        Caption::new(0, 1, 0, &long),
        Err(ModelStatus::CaptionTextTooLong)
    );
    let longest: std::string::String =
        core::iter::repeat_n('a', caption::MAX_CAPTION_TEXT).collect();
    Caption::new(0, 1, 0, &longest).expect("the bound itself is allowed");
    // Characters rather than bytes, so a transcript in a script that needs
    // three bytes a character is not a third the length.
    let wide: std::string::String =
        core::iter::repeat_n('\u{4e00}', caption::MAX_CAPTION_TEXT).collect();
    assert_eq!(wide.len(), caption::MAX_CAPTION_TEXT * 3);
    Caption::new(0, 1, 0, &wide).expect("a bound in characters");

    assert_eq!(
        Caption::new(0, 1, caption::MAX_VOICES, "who said that"),
        Err(ModelStatus::UnknownVoice)
    );
    Caption::new(0, 1, caption::MAX_VOICES - 1, "the last voice").expect("the last voice");
}

#[test]
fn an_asset_carries_no_more_captions_than_the_bound() {
    // A transcript that does not fit is refused rather than truncated: a
    // project file is read in one piece, so a bound that could be talked past
    // is a bound that is not doing anything.
    let asset =
        MediaAsset::new(Digest::of(b"a long interview"), RATE, frames(10_000)).expect("an asset");
    let transcript = |count: usize| {
        (0..count)
            .map(|index| {
                let from = i64::try_from(index).expect("a tick") * 2;
                Caption::new(from, from + 1, 0, "a word").expect("a caption")
            })
            .collect::<std::vec::Vec<_>>()
    };
    asset
        .with_captions(transcript(caption::MAX_CAPTIONS_PER_ASSET))
        .expect("the bound itself is allowed");
    assert_eq!(
        asset
            .with_captions(transcript(caption::MAX_CAPTIONS_PER_ASSET + 1))
            .err(),
        Some(ModelStatus::TooManyCaptions)
    );
}

/// A transcript that records the ranges it was asked for.
struct Asked {
    project_captions: std::vec::Vec<Caption>,
    ranges: std::vec::Vec<(i64, i64)>,
}

impl caption::Transcript for Asked {
    fn captions(
        &mut self,
        _media: Digest,
        from: i64,
        to: i64,
    ) -> Result<std::vec::Vec<Caption>, ModelStatus> {
        self.ranges.push((from, to));
        Ok(self
            .project_captions
            .iter()
            .filter(|caption| caption.from() < to && from < caption.to())
            .cloned()
            .collect())
    }
}

#[test]
fn a_projection_asks_only_for_the_stretch_its_clip_reads() {
    // The reason a transcript is asked for a *range*: a source with a thousand
    // captions hands back the handful that overlap, and the rest is never
    // built. A projection that asked for everything would work and would make
    // the trait pointless.
    let (mut project, sequence, media) = interview();
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 10, frames(10)).expect("a clip")),
    );
    let mut asked = Asked {
        project_captions: std::vec![
            said(0, 10, "the first thing he said"),
            said(10, 20, "the second thing"),
            said(20, 30, "and the third"),
        ],
        ranges: std::vec::Vec::new(),
    };
    let found =
        caption::captions_over(&project, sequence, span(0, 10), &mut asked).expect("a projection");
    assert_eq!(found.len(), 1);
    // The clip reads source 10..19 inclusive, so the range asked for is
    // 10..20 -- not the whole recording, and not the span of the programme.
    assert_eq!(asked.ranges, std::vec![(10, 20)]);
}

#[test]
fn a_held_transcript_answers_only_what_overlaps() {
    // The project's own captions, asked the same question a reel is. A source
    // that ignored the range would still give the right *projection*, because
    // the projection filters again -- so this asks the source directly.
    let (project, _, _) = interview();
    let media = project.media().iter().next().expect("an asset").1.digest();
    let mut held = caption::Held::new(&project);
    let over = |held: &mut caption::Held<'_>, from, to| {
        caption::Transcript::captions(held, media, from, to)
            .expect("a read")
            .into_iter()
            .map(|caption| std::string::String::from(caption.text()))
            .collect::<std::vec::Vec<_>>()
    };
    assert_eq!(over(&mut held, 0, 3), std::vec!["the first thing he said"]);
    assert_eq!(over(&mut held, 12, 14), std::vec!["the second thing"]);
    assert_eq!(over(&mut held, 25, 40), std::vec!["and the third"]);
    assert!(over(&mut held, 40, 50).is_empty());
    // Half-open at both ends: a caption ending at 10 is not on tick 10.
    assert_eq!(over(&mut held, 10, 11), std::vec!["the second thing"]);
    // And media the project does not hold answers with nothing.
    assert!(
        caption::Transcript::captions(&mut held, Digest::of(b"elsewhere"), 0, 100)
            .expect("a read")
            .is_empty()
    );
}

/// A transcript that counts how often it is asked, so "one walk" is a number
/// rather than a claim.
struct Counted<'a> {
    inner: caption::Held<'a>,
    asked: usize,
}

impl caption::Transcript for Counted<'_> {
    fn captions(
        &mut self,
        media: Digest,
        from: i64,
        to: i64,
    ) -> Result<std::vec::Vec<Caption>, media_editor_model::ModelStatus> {
        self.asked += 1;
        self.inner.captions(media, from, to)
    }
}

#[test]
fn a_reading_agrees_with_the_instant_projection_at_every_frame() {
    // The property that makes the span form usable at all: it is not an
    // approximation of the per-frame answer, it *is* the per-frame answer,
    // arrived at once. Every frame of the span is checked, and the reading is
    // asked without being told which frame is a boundary.
    //
    // What is compared is **which words are on screen**, by voice and text,
    // because that is what the two agree about. Their *ranges* differ by
    // design: `captions_over` clips each answer to the window it was asked
    // about, so an instant projection reports a sentence as one frame long
    // and a reading reports it as the stretch of the span it covers. The
    // reading's is the more informative of the two, and it is what lets
    // `burn` order two speakers by when each of them actually started rather
    // than by which track they came off.
    let (mut project, sequence, media) = interview();
    // Two clips, so the words arrive from two directions, and the second at
    // half speed so its words stretch and its boundaries are not the first's.
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 0, frames(15)).expect("a clip")),
    );
    lay(
        &mut project,
        sequence,
        1,
        Item::Clip(
            Clip::new(media, 10, frames(20))
                .expect("a clip")
                .with_speed(Rational::new(1, 2).expect("a speed"))
                .expect("a speed"),
        ),
    );
    let over = span(0, 35);
    let mut held = caption::Held::new(&project);
    let reading = caption::Reading::over(&project, sequence, over, &mut held).expect("a reading");

    for tick in 0..35 {
        let stretch = reading.stretch(at(tick)).expect("a stretch");
        let mut from_span: std::vec::Vec<(u8, std::string::String)> = reading
            .lines(stretch)
            .expect("lines")
            .into_iter()
            .map(|held| (held.caption.voice(), held.caption.text().into()))
            .collect();
        from_span.sort();

        let mut from_instant: std::vec::Vec<(u8, std::string::String)> = caption::captions_at(
            &project,
            sequence,
            at(tick),
            &mut caption::Held::new(&project),
        )
        .expect("a projection")
        .into_iter()
        .map(|held| (held.caption.voice(), held.caption.text().into()))
        .collect();
        from_instant.sort();

        assert_eq!(from_span, from_instant, "frame {tick}");

        // And each line the reading gives does cover this frame, which is
        // what a stretch being maximal-constant is supposed to mean.
        for held in reading.lines(stretch).expect("lines") {
            assert!(
                held.from.ticks() <= tick && tick < held.to.ticks(),
                "frame {tick} is outside a line the reading gave for it"
            );
        }
    }
    // The span holds words at all, so the agreement above is not vacuous.
    assert!(
        (0..35).any(|tick| !reading
            .lines(reading.stretch(at(tick)).expect("a stretch"))
            .expect("lines")
            .is_empty()),
        "nothing was on screen anywhere in the span"
    );
}

#[test]
fn a_reading_walks_the_project_once_however_long_the_span_is() {
    // The whole reason the span form exists, measured. A per-frame projection
    // asks the transcript once a clip a frame; a reading asks once a clip.
    // The ratio is the length of the span, and on a real reel that is four
    // orders of magnitude.
    let (mut project, sequence, media) = interview();
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 0, frames(30)).expect("a clip")),
    );
    let over = span(0, 30);

    let mut once = Counted {
        inner: caption::Held::new(&project),
        asked: 0,
    };
    caption::Reading::over(&project, sequence, over, &mut once).expect("a reading");

    let mut each = Counted {
        inner: caption::Held::new(&project),
        asked: 0,
    };
    for tick in 0..30 {
        caption::captions_at(&project, sequence, at(tick), &mut each).expect("a projection");
    }

    assert_eq!(once.asked, 1, "one clip, one walk");
    assert_eq!(each.asked, 30, "thirty frames, thirty walks");
}

#[test]
fn a_reading_is_cut_where_the_words_change_and_nowhere_else() {
    // The step function, stated as a count. Three captions over source 0..30,
    // read straight, put their edges at programme 0, 10, 20 and 30 -- and the
    // span's own ends are 0 and 30, so the edges are 0, 10, 20, 30 and there
    // are **three** stretches. A reading with a stretch a frame would be a
    // reading that had learned nothing.
    let (mut project, sequence, media) = interview();
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 0, frames(30)).expect("a clip")),
    );
    let mut held = caption::Held::new(&project);
    let reading =
        caption::Reading::over(&project, sequence, span(0, 30), &mut held).expect("a reading");
    assert_eq!(reading.len(), 3, "one stretch a caption, not one a frame");
    assert!(!reading.is_empty());
    for (index, (from, count)) in [(0, 10), (10, 10), (20, 10)].into_iter().enumerate() {
        let over = reading.span(index).expect("a span");
        assert_eq!(
            (over.start().ticks(), over.duration().ticks()),
            (from, count)
        );
        assert_eq!(reading.lines(index).expect("lines").len(), 1);
    }
    // A span nobody speaks in is one stretch, not none: a span always has
    // itself, and a caller that asked for the stretch at a frame would
    // otherwise have nothing to be given.
    let quiet =
        caption::Reading::over(&project, sequence, span(40, 10), &mut held).expect("a reading");
    assert_eq!(quiet.len(), 1);
    assert!(quiet.lines(0).expect("lines").is_empty());
}

#[test]
fn a_reading_answers_only_for_the_span_it_was_projected_over() {
    // An instant outside the span is not a question this can answer, and the
    // dangerous answer would be the nearest stretch -- which would be words
    // from another part of the programme, drawn confidently.
    let (mut project, sequence, media) = interview();
    lay(
        &mut project,
        sequence,
        0,
        Item::Clip(Clip::new(media, 0, frames(30)).expect("a clip")),
    );
    let mut held = caption::Held::new(&project);
    let reading =
        caption::Reading::over(&project, sequence, span(10, 10), &mut held).expect("a reading");
    assert!(reading.stretch(at(10)).is_ok());
    assert!(reading.stretch(at(19)).is_ok());
    for outside in [9, 20, 100, -1] {
        assert_eq!(
            reading.stretch(at(outside)).err(),
            Some(media_editor_model::ModelStatus::OutsideTheReading),
            "instant {outside}"
        );
    }
    // And an instant counted in another rate is a different question, not a
    // nearby one.
    assert!(
        reading.stretch(Instant::new(12, Timebase::PAL_25)).is_err(),
        "a position in another timebase was answered for"
    );
    // A stretch this reading has not got is refused rather than clamped.
    assert_eq!(
        reading.span(reading.len()).err(),
        Some(media_editor_model::ModelStatus::OutsideTheReading)
    );
}
