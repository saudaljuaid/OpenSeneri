// SPDX-License-Identifier: GPL-3.0-only
//! Captions burned into the picture.
//!
//! The last step of the caption story, and it is deliberately a *step*: a
//! viewer draws captions as an overlay it can switch off, an export may burn
//! them in and then they are there for good. Those are different things done
//! by different callers, so burning is asked for rather than had.
//!
//! It is also what M8.42 unblocked. A `Legend` and a `Type` had no row form
//! until then, so a captioned programme could be rendered whole and not
//! scanned — and an export scans.

use media_editor_app::{SlateStatus, timeline};
use media_editor_core::{Digest, Duration, Instant, Timebase};
use media_editor_media::colour::{AlphaState, ColourDescription};
use media_editor_media::{Frame, FrameDescription, FramePool, Geometry, PixelFormat};
use media_editor_model::caption::{self, Caption};
use media_editor_model::{Clip, Edit, Item, MediaAsset, MediaId, Project, SequenceId, TrackKind};
use media_editor_render::{Library, Look, RenderStatus};

const RATE: Timebase = Timebase::FILM_24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a duration")
}

fn at(ticks: i64) -> Instant {
    Instant::new(ticks, RATE)
}

/// Large enough for the letters to fit, which the legend's own tests settled
/// at a hundred and twenty by sixty.
fn described() -> FrameDescription {
    FrameDescription::square(
        Geometry::new(120, 60).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description")
}

struct Flat {
    colour: [u8; 4],
}

impl Library for Flat {
    fn frame(
        &mut self,
        _media: Digest,
        _tick: i64,
        description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        let mut bytes = std::vec::Vec::new();
        for _ in 0..description.geometry().width() * description.geometry().height() {
            bytes.extend_from_slice(&self.colour);
        }
        Frame::from_packed(description, &bytes).map_err(RenderStatus::Media)
    }

    fn look(&mut self, _look: Digest) -> Result<Look, RenderStatus> {
        Err(RenderStatus::UnknownNode)
    }

    fn row(
        &mut self,
        _media: Digest,
        _tick: i64,
        description: FrameDescription,
        row: usize,
    ) -> Result<Frame, RenderStatus> {
        let one = media_editor_render::row_description(description, row)?;
        let mut bytes = std::vec::Vec::new();
        for _ in 0..one.geometry().width() {
            bytes.extend_from_slice(&self.colour);
        }
        Frame::from_packed(one, &bytes).map_err(RenderStatus::Media)
    }
}

/// One captioned shot, in at source nought, twenty frames long.
fn programme(captions: std::vec::Vec<Caption>) -> (Project, SequenceId, MediaId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let asset = MediaAsset::new(Digest::of(b"an interview"), RATE, frames(1000))
        .expect("an asset")
        .with_captions(captions)
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
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 0, frames(20)).expect("a clip")),
            },
        )
        .expect("an insert");
    (project, sequence, media)
}

/// A reading over one frame, which is what asking about an instant now means.
fn reading(project: &Project, sequence: SequenceId, instant: Instant) -> caption::Reading {
    let one = media_editor_core::TimeRange::new(
        instant,
        media_editor_core::Duration::new(1, instant.timebase()).expect("a duration"),
    )
    .expect("a span");
    let mut held = caption::Held::new(project);
    caption::Reading::over(project, sequence, one, &mut held).expect("a reading")
}

fn drawn(project: &Project, sequence: SequenceId, instant: Instant, library: &mut Flat) -> Frame {
    let (mut graph, root) =
        timeline::plan(project, sequence, instant, described(), library).expect("a plan");
    let read = reading(project, sequence, instant);
    let top = timeline::burn(&mut graph, root, &read, instant, described()).expect("a burn");
    graph
        .evaluate(top, &mut FramePool::new(64, 1 << 20), library)
        .expect("a render")
}

#[test]
fn a_caption_on_screen_changes_the_picture_and_one_off_it_does_not() {
    // The whole of it in one test: at an instant the words cover, the picture
    // has letters on it; at an instant they do not, it is the bare programme
    // and *the same bytes* -- because nothing was composited at all.
    let (project, sequence, _) = programme(std::vec![
        Caption::new(0, 5, 0, "so I said to him").expect("a caption"),
    ]);
    let mut library = Flat {
        colour: [20, 40, 60, 255],
    };
    let bare = timeline::render(
        &project,
        sequence,
        at(10),
        described(),
        &mut FramePool::new(64, 1 << 20),
        &mut library,
    )
    .expect("a render");

    let captioned = drawn(&project, sequence, at(2), &mut library);
    assert_ne!(captioned, bare, "the caption drew nothing");
    // Off screen at frame ten, and byte for byte the programme.
    assert_eq!(drawn(&project, sequence, at(10), &mut library), bare);

    // And nothing was *composited*, which the pixels alone cannot say: a card
    // of no lines draws nothing, so `over` it gives the same picture while
    // still passing over every pixel of the frame. So the graph is what is
    // asked -- the root comes straight back and not one node is added.
    let (mut graph, root) =
        timeline::plan(&project, sequence, at(10), described(), &mut library).expect("a plan");
    let before = graph.len();
    let read = reading(&project, sequence, at(10));
    let top = timeline::burn(&mut graph, root, &read, at(10), described()).expect("a burn");
    assert_eq!(top, root, "an empty card was composited over the programme");
    assert_eq!(
        graph.len(),
        before,
        "the graph grew for a caption nobody said"
    );
}

#[test]
fn a_burned_caption_is_at_the_bottom_of_the_picture() {
    // Seven eighths down and centred, which is where a caption goes: above the
    // bottom edge rather than on it, because the frame edge is the first thing
    // a projector's overscan eats.
    let (project, sequence, _) = programme(std::vec![
        Caption::new(0, 5, 0, "so I said to him").expect("a caption"),
    ]);
    let mut library = Flat {
        colour: [0, 0, 0, 255],
    };
    let picture = drawn(&project, sequence, at(2), &mut library);
    let bytes = picture.to_packed().expect("bytes");
    let stride = 120 * 4;
    let lit = |row: usize| {
        bytes[row * stride..(row + 1) * stride]
            .iter()
            .step_by(4)
            .filter(|value| **value > 0)
            .count()
    };
    // The top third has nothing on it and the band around seven eighths does.
    assert_eq!((0..20).map(lit).sum::<usize>(), 0, "letters at the top");
    assert!(
        (46..56).map(lit).sum::<usize>() > 0,
        "no letters where a caption goes"
    );
}

#[test]
fn two_speakers_at_once_are_two_lines_of_one_block() {
    // Not two cards over each other, which is what a naive burn does and what
    // makes two speakers unreadable: one block of several lines, ordered by
    // voice so it cannot reshuffle itself between frames.
    let (project, sequence, _) = programme(std::vec![
        Caption::new(0, 10, 1, "and what did he say").expect("a caption"),
        Caption::new(0, 10, 0, "so I said to him").expect("a caption"),
    ]);
    let mut library = Flat {
        colour: [0, 0, 0, 255],
    };
    let (mut graph, root) =
        timeline::plan(&project, sequence, at(2), described(), &mut library).expect("a plan");
    let before = graph.len();
    let read = reading(&project, sequence, at(2));
    let top = timeline::burn(&mut graph, root, &read, at(2), described()).expect("a burn");
    // One card and one composite, whatever the number of speakers.
    assert_eq!(
        graph.len(),
        before + 2,
        "two speakers made more than one card"
    );
    let media_editor_render::Node::Over { layers } = graph.node(top).expect("a node") else {
        panic!("the burn did not composite")
    };
    let media_editor_render::Node::Type { lines, .. } = graph.node(layers[1]).expect("a node")
    else {
        panic!("the burn did not set a card")
    };
    // Voice nought first, then voice one -- by voice rather than by whichever
    // track the walk happened to reach first.
    assert_eq!(
        lines,
        &std::vec![
            std::string::String::from("so I said to him"),
            std::string::String::from("and what did he say"),
        ]
    );
}

#[test]
fn more_lines_than_fit_on_screen_are_refused() {
    // R-1.4: a caption silently dropped is worse than a caption refused,
    // because the person it belongs to is the one who cannot tell.
    let mut captions = std::vec::Vec::new();
    for voice in 0..5_u8 {
        captions.push(Caption::new(0, 10, voice, "everybody talking at once").expect("a caption"));
    }
    let (project, sequence, _) = programme(captions);
    let mut library = Flat {
        colour: [0, 0, 0, 255],
    };
    let (mut graph, root) =
        timeline::plan(&project, sequence, at(2), described(), &mut library).expect("a plan");
    let read = reading(&project, sequence, at(2));
    assert_eq!(
        timeline::burn(&mut graph, root, &read, at(2), described()).err(),
        Some(SlateStatus::Model(
            media_editor_model::ModelStatus::TooManyCaptions
        ))
    );
    assert_eq!(timeline::MAX_LINES_ON_SCREEN, 4);
}

#[test]
fn a_captioned_programme_can_still_be_scanned() {
    // What M8.42 unblocked, and the reason it mattered: a `Type` had no row
    // form, so burning a caption in would have made a programme that could be
    // rendered whole and not exported. The rows are the frame, letters and all.
    let (project, sequence, _) = programme(std::vec![
        Caption::new(0, 5, 0, "so I said to him").expect("a caption"),
    ]);
    let mut library = Flat {
        colour: [20, 40, 60, 255],
    };
    let (mut graph, root) =
        timeline::plan(&project, sequence, at(2), described(), &mut library).expect("a plan");
    let read = reading(&project, sequence, at(2));
    let top = timeline::burn(&mut graph, root, &read, at(2), described()).expect("a burn");
    graph.row_local(top).expect("a captioned programme scans");

    let whole = graph
        .evaluate(top, &mut FramePool::new(64, 1 << 20), &mut library)
        .expect("a render");
    let stride = 120 * 4;
    let mut gathered = std::vec::Vec::new();
    for row in 0..60 {
        let line = graph.row(top, row, &mut library).expect("a row");
        gathered.extend_from_slice(&line.to_packed().expect("bytes"));
    }
    assert_eq!(gathered, whole.to_packed().expect("bytes"));
    assert_eq!(gathered.len(), stride * 60);
}

/// A transcript that counts how often it is walked.
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
fn a_span_is_read_once_and_burned_frame_by_frame() {
    // What the span form is for, at the level a caller sees it. Twenty frames
    // of a captioned programme, every one of them burned -- and the project
    // walked **once**, not twenty times. Burning is still per frame, because
    // each frame is its own graph; what stopped being per frame is the
    // question "what is being said".
    let (project, sequence, _) = programme(std::vec![
        Caption::new(0, 8, 0, "the first thing").expect("a caption"),
        Caption::new(8, 16, 1, "the second thing").expect("a caption"),
    ]);
    let mut library = Flat {
        colour: [20, 40, 60, 255],
    };
    let mut counted = Counted {
        inner: caption::Held::new(&project),
        asked: 0,
    };
    let over = media_editor_core::TimeRange::new(at(0), frames(20)).expect("a span");
    let reading =
        caption::Reading::over(&project, sequence, over, &mut counted).expect("a reading");
    assert_eq!(counted.asked, 1, "one clip, one walk over the whole span");

    // Three stretches: 0..8 the first speaker, 8..16 the second, 16..20 with
    // nobody talking. The frames are twenty; the cards are three.
    assert_eq!(reading.len(), 3);

    let mut burned = 0_usize;
    let mut plain = 0_usize;
    for tick in 0..20 {
        let (mut graph, root) =
            timeline::plan(&project, sequence, at(tick), described(), &mut library)
                .expect("a plan");
        let top =
            timeline::burn(&mut graph, root, &reading, at(tick), described()).expect("a burn");
        if top == root {
            plain += 1;
        } else {
            burned += 1;
        }
        // And the burned frame is a frame: it renders, and it scans.
        graph.row_local(top).expect("it scans");
        graph
            .evaluate(top, &mut FramePool::new(64, 1 << 20), &mut library)
            .expect("a render");
    }
    assert_eq!(burned, 16, "sixteen frames have words on them");
    assert_eq!(plain, 4, "and four do not");
    // The reading was not asked again for any of them.
    assert_eq!(counted.asked, 1, "the project was walked a second time");
}

#[test]
fn a_stretch_holds_the_same_card_for_all_of_its_frames() {
    // Why the stretches are worth having as *stretches* and not merely as a
    // faster lookup: within one, every frame draws the same card, so a caller
    // that wanted to build the card once already knows which frames it serves.
    let (project, sequence, _) = programme(std::vec![
        Caption::new(3, 9, 0, "a sentence").expect("a caption"),
    ]);
    let mut held = caption::Held::new(&project);
    let over = media_editor_core::TimeRange::new(at(0), frames(12)).expect("a span");
    let reading = caption::Reading::over(&project, sequence, over, &mut held).expect("a reading");

    // Edges at 0 (the span), 3 and 9 (the caption), 12 (the span): three
    // stretches, 0..3 silent, 3..9 speaking, 9..12 silent.
    assert_eq!(reading.len(), 3);
    let mut library = Flat {
        colour: [20, 40, 60, 255],
    };
    for stretch in 0..reading.len() {
        let over = reading.span(stretch).expect("a span");
        let expected = reading.lines(stretch).expect("lines").len();
        for tick in over.start().ticks()..over.start().ticks() + over.duration().ticks() {
            assert_eq!(
                reading.stretch(at(tick)).expect("a stretch"),
                stretch,
                "frame {tick} is not in the stretch that covers it"
            );
            assert_eq!(reading.lines(stretch).expect("lines").len(), expected);
        }
    }
    assert_eq!(
        (0..reading.len())
            .map(|stretch| reading.lines(stretch).expect("lines").len())
            .collect::<std::vec::Vec<_>>(),
        std::vec![0, 1, 0],
        "the middle stretch is the one with words in it"
    );
    // And the pictures agree with that: silent frames are the bare programme.
    let bare = drawn(&project, sequence, at(0), &mut library);
    let spoken = drawn(&project, sequence, at(5), &mut library);
    assert_ne!(bare, spoken, "the card drew nothing");
    assert_eq!(bare, drawn(&project, sequence, at(11), &mut library));
}
