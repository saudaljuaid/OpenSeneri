// SPDX-License-Identifier: GPL-3.0-only
//! A sequence, rendered: the timeline and the compositor meeting.

use media_editor_app::SlateStatus;
use media_editor_app::timeline;
use media_editor_core::Digest;
use media_editor_core::{Duration, Instant, Timebase};
use media_editor_media::{
    AlphaState, ColourDescription, Frame, FrameDescription, FramePool, Geometry, PixelFormat,
    TestPattern,
};
use media_editor_model::{Clip, Edit, Item, MediaAsset, MediaId, Project, SequenceId, TrackKind};
use media_editor_render::{Library, Look, RenderStatus};

const RATE: Timebase = Timebase::FILM_24;

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a duration")
}

fn at(frame: i64) -> Instant {
    Instant::new(frame, RATE)
}

/// The description everything in this file is rendered in.
fn described() -> FrameDescription {
    FrameDescription::square(
        Geometry::new(4, 4).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description")
}

/// A frame source that hands back flat colours, keyed by media.
///
/// Premultiplied by construction: the colour never exceeds the coverage, so
/// every frame it produces is one the compositor will accept.
struct Flat {
    colours: std::vec::Vec<(Digest, [u8; 4])>,
    description: FrameDescription,
    asked: std::vec::Vec<(Digest, i64)>,
    looks: std::vec::Vec<(Digest, Look)>,
    /// Answer with `description` rather than with what was asked for.
    ///
    /// A field rather than the default behaviour, which it used to be. A
    /// source that ignores the description it is given is a *fault*, and one
    /// test exists to prove the graph refuses it — but every other test then
    /// depended on the fault by accident, and a graded layer, which is fetched
    /// straight, could not render at all. The lie is deliberate now and only
    /// where it is the subject.
    answers_wrongly: bool,
}

impl Library for Flat {
    fn frame(
        &mut self,
        media: Digest,
        tick: i64,
        description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        self.asked.push((media, tick));
        let colour = self
            .colours
            .iter()
            .find(|(id, _)| *id == media)
            .map_or([0, 0, 0, 255], |(_, colour)| *colour);
        // Answer the description that was asked for, unless this fixture is
        // deliberately being the source that does not. It used to always
        // ignore it, which was harmless only while every layer was fetched the
        // same way — a graded layer is fetched straight, and a source that
        // hands back something else has answered a different question.
        let mut bytes = std::vec::Vec::new();
        for _ in 0..16 {
            bytes.extend_from_slice(&colour);
        }
        let answer = if self.answers_wrongly {
            self.description
        } else {
            description
        };
        Frame::from_packed(answer, &bytes).map_err(RenderStatus::Media)
    }

    fn look(&mut self, look: media_editor_core::Digest) -> Result<Look, RenderStatus> {
        self.looks
            .iter()
            .find(|(digest, _)| *digest == look)
            .map(|(_, held)| held.clone())
            .ok_or(RenderStatus::UnknownNode)
    }

    /// One row of the flat colour, which is what a flat colour's row is.
    ///
    /// Written rather than left to the refusing default, and that is the whole
    /// distinction the default exists to draw: a library that can serve rows
    /// says so by writing this, and `Whole` below says it cannot by not. The
    /// row is built from the width it was asked for rather than from a
    /// constant, so a description of another size would still be answered
    /// honestly instead of by an accident of `described` being four wide.
    fn row(
        &mut self,
        media: Digest,
        tick: i64,
        description: FrameDescription,
        row: usize,
    ) -> Result<Frame, RenderStatus> {
        self.asked.push((media, tick));
        let colour = self
            .colours
            .iter()
            .find(|(id, _)| *id == media)
            .map_or([0, 0, 0, 255], |(_, colour)| *colour);
        let one = media_editor_render::row_description(description, row)?;
        let mut bytes = std::vec::Vec::new();
        for _ in 0..one.geometry().width() {
            bytes.extend_from_slice(&colour);
        }
        Frame::from_packed(one, &bytes).map_err(RenderStatus::Media)
    }
}

/// A library that serves whole frames and no rows.
///
/// Everything `Flat` is, minus the one method — which is how a library says it
/// has no row form, and so the only way to build the fixture that proves where
/// that refusal comes from. A BMP decoder is this: it reads a file into a
/// frame and has no notion of part of one.
struct Whole {
    inner: Flat,
}

impl Library for Whole {
    fn frame(
        &mut self,
        media: Digest,
        tick: i64,
        description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        self.inner.frame(media, tick, description)
    }

    fn look(&mut self, look: Digest) -> Result<Look, RenderStatus> {
        self.inner.look(look)
    }
}

/// The content digest of a media asset, which is how the graph names it.
fn digest_of(project: &Project, id: MediaId) -> Digest {
    project.media().get(id).expect("an asset").digest()
}

/// A fresh pool for each render.
///
/// Deliberately not shared: most of these tests are about what a render
/// produces, and a pool that outlived one call would let a stale frame answer
/// for a changed graph. The caching test builds its own and keeps it.
fn pool() -> FramePool {
    FramePool::new(64, 1 << 20)
}

/// One pixel of a rendered frame.
fn pixel(frame: &Frame) -> (u8, u8, u8, u8) {
    let bytes = frame.to_packed().expect("bytes");
    (bytes[0], bytes[1], bytes[2], bytes[3])
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
                    kind: TrackKind::Video,
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

fn media(project: &mut Project, tag: u8) -> MediaId {
    let mut bytes = [0_u8; 32];
    bytes[0] = tag;
    let asset = MediaAsset::new(
        media_editor_model::media::Digest::new(bytes),
        RATE,
        frames(1000),
    )
    .expect("an asset");
    project.add_media(asset).expect("an identifier")
}

#[test]
fn an_empty_sequence_shows_opaque_black() {
    // Not a hole. A viewer shows black leader and an export writes black
    // frames; neither shows whatever was behind the window. So the programme
    // is opaque even where nothing is on it.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let mut source = Flat {
        colours: std::vec::Vec::new(),
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(pixel(&rendered), (0, 0, 0, 255));
    assert!(source.asked.is_empty(), "nothing was decoded for nothing");
}

#[test]
fn one_opaque_layer_is_what_you_see() {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let red = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(red, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, red), [200, 30, 40, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(pixel(&rendered), (200, 30, 40, 255));
    assert_eq!(
        source.asked,
        std::vec![(digest_of(&project, red), 5)],
        "one frame, the right one"
    );
}

#[test]
fn an_upper_track_covers_a_lower_one_and_a_gap_lets_it_through() {
    // The two halves of the same decision, in the same fixture: V2 covers V1
    // where it has material, and shows it where it has a gap. A gap that
    // contributed black would make the first half pass and the second fail.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let under = media(&mut project, 1);
    let over = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(under, 0, frames(20)).expect("a clip"))],
    );
    lay(
        &mut project,
        sequence,
        1,
        &[
            Item::gap(frames(10)).expect("a gap"),
            Item::Clip(Clip::new(over, 0, frames(10)).expect("a clip")),
        ],
    );
    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, under), [10, 20, 30, 255]),
            (digest_of(&project, over), [90, 80, 70, 255])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };

    let inside_the_gap = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(
        pixel(&inside_the_gap),
        (10, 20, 30, 255),
        "V1 shows through V2's gap"
    );

    let covered = timeline::render(
        &project,
        sequence,
        at(15),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(
        pixel(&covered),
        (90, 80, 70, 255),
        "and V2 covers it where it has material"
    );
}

#[test]
fn a_half_covered_upper_track_shows_the_lower_one_through_it() {
    // The whole point of doing this with `over` rather than with a copy. A
    // half-covered layer must let the one beneath show through it, in linear
    // light, so a dissolve or a soft-edged title lands without a fringe.
    //
    // The numbers: coverage 128 of white over opaque black is code value 205,
    // which is the pixel the compositor's own tests compute by hand. Here it
    // arrives through the timeline instead, which is the point — the same
    // arithmetic, reached the way a session reaches it.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let under = media(&mut project, 1);
    let over = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(under, 0, frames(10)).expect("a clip"))],
    );
    lay(
        &mut project,
        sequence,
        1,
        &[Item::Clip(Clip::new(over, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        // 188 at coverage 128 is premultiplied white: the compositor refuses
        // anything brighter, which is what keeps this honest.
        colours: std::vec![
            (digest_of(&project, under), [128, 128, 128, 255]),
            (digest_of(&project, over), [188, 188, 188, 128])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(
        pixel(&rendered),
        (205, 205, 205, 255),
        "half-covered white over mid-grey, in light"
    );
}

#[test]
fn three_layers_stack_bottom_first() {
    // Order matters and the stack has to get it right, so this uses three
    // opaque layers where only the top one can win.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let one = media(&mut project, 1);
    let two = media(&mut project, 2);
    let three = media(&mut project, 3);
    for (track, id) in [(0, one), (1, two), (2, three)] {
        lay(
            &mut project,
            sequence,
            track,
            &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
        );
    }
    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, one), [1, 1, 1, 255]),
            (digest_of(&project, two), [2, 2, 2, 255]),
            (digest_of(&project, three), [3, 3, 3, 255])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(pixel(&rendered), (3, 3, 3, 255), "V3 is on top");
    assert_eq!(
        source.asked,
        std::vec![
            (digest_of(&project, one), 0),
            (digest_of(&project, two), 0),
            (digest_of(&project, three), 0)
        ],
        "and they were fetched bottom first"
    );
}

#[test]
fn the_playhead_asks_for_the_right_source_frame() {
    // The arithmetic a whole clip depends on, checked through the render path
    // rather than only in the model.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::gap(frames(4)).expect("a gap"),
            Item::Clip(Clip::new(id, 100, frames(6)).expect("a clip")),
        ],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [7, 7, 7, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    for frame in 0..12 {
        timeline::render(
            &project,
            sequence,
            at(frame),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render");
    }
    assert_eq!(
        source.asked,
        std::vec![
            (digest_of(&project, id), 100),
            (digest_of(&project, id), 101),
            (digest_of(&project, id), 102),
            (digest_of(&project, id), 103),
            (digest_of(&project, id), 104),
            (digest_of(&project, id), 105)
        ],
        "six frames, starting at source 100, and nothing outside the clip"
    );
}

#[test]
fn a_source_that_answers_with_the_wrong_frame_is_refused() {
    // Converting it here would be a decision made in the wrong place: the
    // source was told what to produce, and quietly fixing its answer is how a
    // pipeline ends up with a conversion nobody chose. The refusal comes from
    // the graph now rather than from this layer, because that is where the
    // node that asked for the frame lives — `Convert` is a node, with a name.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
    );
    let wrong = FrameDescription::square(
        Geometry::new(4, 4).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description");
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [10, 10, 10, 255])],
        description: wrong,
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: true,
    };
    assert_eq!(
        timeline::render(
            &project,
            sequence,
            at(0),
            described(),
            &mut pool(),
            &mut source
        ),
        Err(SlateStatus::Render(RenderStatus::SourceDescriptionMismatch))
    );
}

#[test]
fn a_base_that_is_not_premultiplied_is_refused() {
    // `over` is only correct on premultiplied values, so the bottom of the
    // stack has to be one. Refusing rather than converting keeps the decision
    // with the caller who chose the description.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let straight = FrameDescription::square(
        Geometry::new(4, 4).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description");
    let mut source = Flat {
        colours: std::vec::Vec::new(),
        description: straight,
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    assert_eq!(
        timeline::render(
            &project,
            sequence,
            at(0),
            straight,
            &mut pool(),
            &mut source
        ),
        Err(SlateStatus::BaseNotPremultiplied)
    );
}

#[test]
fn rendering_one_instant_twice_gives_one_answer() {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let one = media(&mut project, 1);
    let two = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(one, 0, frames(10)).expect("a clip"))],
    );
    lay(
        &mut project,
        sequence,
        1,
        &[Item::Clip(Clip::new(two, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, one), [40, 50, 60, 255]),
            (digest_of(&project, two), [90, 40, 10, 100])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let first = timeline::render(
        &project,
        sequence,
        at(3),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    let second = timeline::render(
        &project,
        sequence,
        at(3),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(first.digest(), second.digest());
}

#[test]
fn a_dissolve_cross_fades_in_linear_light() {
    // The whole point of representing a dissolve as an opacity: `over` already
    // computes `in x t + out x (1 - t)`, so a cross-fade needs no second
    // operator — and it happens in light, like everything else.
    //
    // White dissolving to black over four frames. Every number below is worked
    // out from the definitions rather than read off a run:
    //
    //   the incoming clip's coverage at n/5 is round(255 x n/5),
    //   the result's light is dec(255) x (1 - coverage/255) = 1 - n/5,
    //   and the sRGB code nearest that light is the answer.
    //
    //   1/5 -> alpha  51 -> light 0.8 -> 231
    //   2/5 -> alpha 102 -> light 0.6 -> 203
    //   3/5 -> alpha 153 -> light 0.4 -> 170
    //   4/5 -> alpha 204 -> light 0.2 -> 124
    //
    // A dissolve done in code values instead would step 255, 204, 153, 102,
    // 51, 0 — evenly spaced numbers, and a visibly wrong fade that goes dark
    // too fast in the middle. That is what these four values catch.
    //
    // What they do *not* catch is scaling a layer's coverage without scaling
    // its colour, because the incoming clip here is black and black has no
    // colour to scale. `a_dissolve_is_opaque_all_the_way_through` uses
    // coloured clips and catches exactly that — a control confirmed the
    // division of labour, so neither test is redundant.
    use media_editor_model::Transition;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let white = media(&mut project, 1);
    let black = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::Clip(Clip::new(white, 0, frames(20)).expect("a clip")),
            Item::Clip(Clip::new(black, 100, frames(20)).expect("a clip")),
        ],
    );
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(4)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, white), [255, 255, 255, 255]),
            (digest_of(&project, black), [0, 0, 0, 255])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };

    assert_eq!(
        pixel(
            &timeline::render(
                &project,
                sequence,
                at(17),
                described(),
                &mut pool(),
                &mut source
            )
            .expect("a render")
        ),
        (255, 255, 255, 255),
        "the frame before the dissolve is all outgoing"
    );
    for (frame, expected) in [(18, 231_u8), (19, 203), (20, 170), (21, 124)] {
        let rendered = timeline::render(
            &project,
            sequence,
            at(frame),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render");
        assert_eq!(
            pixel(&rendered),
            (expected, expected, expected, 255),
            "frame {frame} of the dissolve"
        );
    }
    assert_eq!(
        pixel(
            &timeline::render(
                &project,
                sequence,
                at(22),
                described(),
                &mut pool(),
                &mut source
            )
            .expect("a render")
        ),
        (0, 0, 0, 255),
        "and the frame after it is all incoming"
    );
}

#[test]
fn a_dissolve_is_opaque_all_the_way_through() {
    // A cross-fade between two opaque clips must not show the background at
    // any point in it. If the two opacities did not sum to one, the middle of
    // every dissolve would be see-through — which over black looks like a dip
    // to dark, and is the classic wrong dissolve.
    use media_editor_model::Transition;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let one = media(&mut project, 1);
    let two = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::Clip(Clip::new(one, 0, frames(20)).expect("a clip")),
            Item::Clip(Clip::new(two, 100, frames(20)).expect("a clip")),
        ],
    );
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(12)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, one), [200, 100, 50, 255]),
            (digest_of(&project, two), [50, 100, 200, 255])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    for frame in 14..26 {
        let rendered = timeline::render(
            &project,
            sequence,
            at(frame),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render");
        assert_eq!(pixel(&rendered).3, 255, "frame {frame} is see-through");
    }
}

#[test]
fn a_dissolve_moves_in_one_direction_all_the_way_across() {
    // No dip, no plateau, no reversal. A fade that went back on itself at any
    // frame would be visible and nobody could say why.
    use media_editor_model::Transition;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let bright = media(&mut project, 1);
    let dark = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::Clip(Clip::new(bright, 0, frames(30)).expect("a clip")),
            Item::Clip(Clip::new(dark, 100, frames(30)).expect("a clip")),
        ],
    );
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::new(1, frames(20)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");

    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, bright), [255, 255, 255, 255]),
            (digest_of(&project, dark), [0, 0, 0, 255])
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let mut previous = 256_i32;
    for frame in 20..40 {
        let value = i32::from(
            pixel(
                &timeline::render(
                    &project,
                    sequence,
                    at(frame),
                    described(),
                    &mut pool(),
                    &mut source,
                )
                .expect("a render"),
            )
            .0,
        );
        assert!(
            value < previous,
            "frame {frame} went the wrong way: {value}"
        );
        previous = value;
    }
}

#[test]
fn a_layer_at_full_opacity_is_not_touched() {
    // Every frame outside a dissolve goes through the same code path, so the
    // path has to be a copy rather than a multiply by one — or every ordinary
    // frame in the programme would be a rounding away from its source.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [3, 5, 7, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(4),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(pixel(&rendered), (3, 5, 7, 255));
}

#[test]
fn a_pool_kept_across_renders_stops_the_same_frame_being_fetched_twice() {
    // The reason the render is a graph rather than a loop. A node's key is a
    // digest over its kind, its parameters and its inputs' identities, so a
    // pool that outlives one render answers for anything it has already seen —
    // and for a `Source` node the cost avoided is a decode.
    //
    // Scrubbing back and forth over one instant is the case a user creates
    // constantly, and it must not decode again each time.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [10, 20, 30, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let mut kept = pool();

    let first = timeline::render(
        &project,
        sequence,
        at(3),
        described(),
        &mut kept,
        &mut source,
    )
    .expect("a render");
    assert_eq!(source.asked.len(), 1);

    let second = timeline::render(
        &project,
        sequence,
        at(3),
        described(),
        &mut kept,
        &mut source,
    )
    .expect("a render");
    assert_eq!(first, second, "and the answer is the same one");
    assert_eq!(
        source.asked.len(),
        1,
        "the second render decoded nothing at all"
    );

    // A different instant of the same clip is a different frame, so it is
    // fetched — the cache must not be answering by luck.
    timeline::render(
        &project,
        sequence,
        at(4),
        described(),
        &mut kept,
        &mut source,
    )
    .expect("a render");
    assert_eq!(source.asked.len(), 2);
}

#[test]
fn two_sequences_using_one_asset_share_its_cached_frames() {
    // The graph names media by what it *is* rather than by this project's
    // index for it. Two sequences cutting the same footage therefore hit the
    // same cache entry — and a source that recorded a second fetch would show
    // that the naming had gone project-local somewhere.
    let mut project = Project::new();
    let id = media(&mut project, 1);
    let one = project.add_sequence(RATE).expect("a sequence");
    let two = project.add_sequence(RATE).expect("a sequence");
    for sequence in [one, two] {
        lay(
            &mut project,
            sequence,
            0,
            &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
        );
    }
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [1, 2, 3, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let mut kept = pool();

    let from_one = timeline::render(&project, one, at(2), described(), &mut kept, &mut source)
        .expect("a render");
    let from_two = timeline::render(&project, two, at(2), described(), &mut kept, &mut source)
        .expect("a render");
    assert_eq!(from_one, from_two);
    assert_eq!(source.asked.len(), 1, "one decode served both sequences");
}

#[test]
fn a_plan_can_be_read_without_fetching_anything() {
    // Building the graph and evaluating it are separate questions, and a
    // caller may want only the first: an export deciding whether an instant
    // needs decoding at all has no frames to fetch.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let one = media(&mut project, 1);
    let two = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(one, 0, frames(10)).expect("a clip"))],
    );
    lay(
        &mut project,
        sequence,
        1,
        &[Item::Clip(Clip::new(two, 0, frames(10)).expect("a clip"))],
    );

    let mut source = Flat {
        colours: std::vec::Vec::new(),
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let (graph, root) =
        timeline::plan(&project, sequence, at(5), described(), &mut source).expect("a plan");
    // A blank, two sources, and two `over` nodes: five.
    assert_eq!(graph.len(), 5);
    assert_eq!(
        graph.description(root).expect("a description"),
        described(),
        "and the root produces exactly what was asked for"
    );

    // An empty instant plans to a blank and nothing else.
    let (graph, _) =
        timeline::plan(&project, sequence, at(50), described(), &mut source).expect("a plan");
    assert_eq!(graph.len(), 1);
}

/// A table that swaps red and blue, neutral on its own diagonal.
fn swap_look() -> Look {
    use media_editor_render::lut::Lut3D;
    let size = 5_usize;
    let last = 4_i64;
    let mut samples = std::vec::Vec::new();
    for blue in 0..size {
        for green in 0..size {
            for red in 0..size {
                let value = |axis: usize| {
                    media_editor_core::Fixed::from_rational(
                        media_editor_core::Rational::new(
                            i64::try_from(axis).expect("an index"),
                            last,
                        )
                        .expect("a ratio"),
                    )
                    .expect("a value")
                };
                samples.push([value(blue), value(green), value(red)]);
            }
        }
    }
    Look::new(
        Lut3D::new(size, samples).expect("a table"),
        // The straight description the plan fetches a graded layer in.
        described()
            .with_alpha(AlphaState::Straight)
            .expect("a description")
            .colour(),
        media_editor_render::lut::Interpolation::Tetrahedral,
    )
}

#[test]
fn a_graded_clip_is_graded_when_it_renders() {
    // The end-to-end case, and the one a negative control found missing: the
    // model carried a grade, the node existed, and nothing tested that `plan`
    // put one in front of the other. Removing the wiring broke no test at all
    // until this one existed.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
    );

    let look = swap_look();
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look.digest().expect("a digest")),
            },
        )
        .expect("a grade");

    let mut source = Flat {
        colours: std::vec![(digest_of(&project, id), [200, 40, 10, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec![(look.digest().expect("a digest"), look)],
        answers_wrongly: false,
    };
    let frame = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a frame");

    // Red and blue swapped, opaque, over black.
    assert_eq!(
        pixel(&frame),
        (10, 40, 200, 255),
        "the grade did not reach the picture"
    );
}

#[test]
fn a_graded_layer_is_fetched_straight_and_associated_afterwards() {
    // A look is a non-linear function and on premultiplied samples computes
    // `f(ac)` where `a·f(c)` was wanted; `over` is only correct on
    // premultiplied ones. The two want opposite things, so a graded layer
    // arrives the way the look needs it and is associated afterwards — which
    // loses nothing, since it was never premultiplied to begin with.
    //
    // Without that, a graded clip cannot render at all: the look refuses the
    // frame the compositor wants. This asserts the *fetch*, because the
    // rendering test above would pass for a pipeline that unpremultiplied
    // silently, and unpremultiplying is lossy.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
    );
    let look = swap_look();
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look.digest().expect("a digest")),
            },
        )
        .expect("a grade");

    let mut library = Flat {
        colours: std::vec::Vec::new(),
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let (graph, root) =
        timeline::plan(&project, sequence, at(0), described(), &mut library).expect("a plan");

    // Walk from the root down its own edges, rather than over the node list:
    // a graph exposes no way to name a node by index, and adding one so a test
    // could enumerate would be public surface bought for a test.
    let mut straight = 0;
    let mut looks = 0;
    let mut associations = 0;
    let mut pending = std::vec![root];
    while let Some(id) = pending.pop() {
        let node = graph.node(id).expect("a node");
        pending.extend_from_slice(node.inputs());
        match node {
            media_editor_render::Node::Source { description, .. } => {
                assert_eq!(
                    description.alpha(),
                    Some(AlphaState::Straight),
                    "a graded layer was fetched premultiplied, which the look refuses"
                );
                straight += 1;
            }
            media_editor_render::Node::Look { .. } => looks += 1,
            media_editor_render::Node::Associate { target, .. } => {
                assert_eq!(
                    target.alpha(),
                    Some(AlphaState::Premultiplied),
                    "the association did not put it back for the compositor"
                );
                associations += 1;
            }
            _ => {}
        }
    }
    assert_eq!(straight, 1, "the source was not fetched straight");
    assert_eq!(looks, 1, "no look reached the graph");
    assert_eq!(associations, 1, "the layer was never re-associated");
    assert_eq!(
        graph.description(root).expect("a description").alpha(),
        Some(AlphaState::Premultiplied),
        "the render does not end premultiplied"
    );
}

#[test]
fn a_wipe_puts_a_hard_edge_across_the_picture() {
    // The end of the chain: a wipe in the model becomes a coverage plane in
    // the renderer and an edge in the picture. On the swept side the incoming
    // clip is whole; on the other, the outgoing one is; and the frame is
    // opaque all the way across, because the programme is.
    use media_editor_model::{Transition, Wipe};

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let outgoing = media(&mut project, 1);
    let incoming = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::Clip(Clip::new(outgoing, 0, frames(10)).expect("a clip")),
            Item::Clip(Clip::new(incoming, 100, frames(10)).expect("a clip")),
        ],
    );
    // Four frames, centred on the cut at frame 10: 8, 9, 10, 11. At frame 9
    // the fraction is 2/5, so on a four-pixel-wide frame the edge sits at 1.6.
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: Transition::wiping(1, frames(4), Wipe::RIGHTWARD).expect("a wipe"),
            },
        )
        .expect("a wipe");
    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, outgoing), [255, 255, 255, 255]),
            (digest_of(&project, incoming), [0, 0, 0, 255]),
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(9),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    let bytes = rendered.to_packed().expect("bytes");
    let row: std::vec::Vec<u8> = (0..4).map(|column| bytes[column * 4]).collect();

    // The incoming clip is black and fully covers the first pixel, so it is
    // black; the last two are past the edge and stay white. The second is the
    // one the edge crosses, and it is neither.
    assert_eq!(row[0], 0, "wholly the incoming clip");
    assert_eq!(row[3], 255, "wholly the outgoing clip");
    assert!(
        row[1] > 0 && row[1] < 255,
        "the pixel the edge crosses is neither, got {}",
        row[1]
    );
    assert_eq!(row[2], 255, "still past the edge at 1.6");
    for column in 0..4 {
        assert_eq!(
            bytes[column * 4 + 3],
            255,
            "the programme is opaque across the edge, at {column}"
        );
    }
}

#[test]
fn a_wipe_and_a_dissolve_are_different_pictures() {
    // They are timed identically and stack identically, so nothing before the
    // renderer can tell them apart. This is the test that the difference
    // survives all the way to the pixels -- without it, the whole transition
    // kind could be ignored and every test above would still pass.
    use media_editor_model::{Transition, Wipe};

    let build = |wiping: bool| {
        let mut project = Project::new();
        let sequence = project.add_sequence(RATE).expect("a sequence");
        let outgoing = media(&mut project, 1);
        let incoming = media(&mut project, 2);
        lay(
            &mut project,
            sequence,
            0,
            &[
                Item::Clip(Clip::new(outgoing, 0, frames(10)).expect("a clip")),
                Item::Clip(Clip::new(incoming, 100, frames(10)).expect("a clip")),
            ],
        );
        let transition = if wiping {
            Transition::wiping(1, frames(4), Wipe::RIGHTWARD).expect("a wipe")
        } else {
            Transition::new(1, frames(4)).expect("a dissolve")
        };
        project
            .apply(
                sequence,
                Edit::AddTransition {
                    track: 0,
                    transition,
                },
            )
            .expect("a transition");
        let mut source = Flat {
            colours: std::vec![
                (digest_of(&project, outgoing), [255, 255, 255, 255]),
                (digest_of(&project, incoming), [0, 0, 0, 255]),
            ],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        };
        timeline::render(
            &project,
            sequence,
            at(9),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render")
        .to_packed()
        .expect("bytes")
    };

    let wiped = build(true);
    let dissolved = build(false);
    assert_ne!(wiped, dissolved);
    // And the difference is the shape of it: a dissolve is the same value at
    // every pixel of the row, a wipe is not.
    let row = |bytes: &[u8]| (0..4).map(|c| bytes[c * 4]).collect::<std::vec::Vec<u8>>();
    let flat = row(&dissolved);
    assert!(
        flat.iter().all(|value| *value == flat[0]),
        "a dissolve is uniform across the frame, got {flat:?}"
    );
    let edged = row(&wiped);
    assert!(
        edged.iter().any(|value| *value != edged[0]),
        "a wipe is not, got {edged:?}"
    );
}

#[test]
fn a_soft_wipe_has_more_than_one_pixel_between_the_two_clips() {
    // The difference a soft edge makes, at the only place it can be seen: a
    // hard wipe has exactly one partial pixel across a row, because a straight
    // line crosses one pixel per row. A soft one has a band of them.
    use media_editor_core::Rational;
    use media_editor_model::{Transition, Wipe};

    let build = |softness: Rational| {
        let mut project = Project::new();
        let sequence = project.add_sequence(RATE).expect("a sequence");
        let outgoing = media(&mut project, 1);
        let incoming = media(&mut project, 2);
        lay(
            &mut project,
            sequence,
            0,
            &[
                Item::Clip(Clip::new(outgoing, 0, frames(10)).expect("a clip")),
                Item::Clip(Clip::new(incoming, 100, frames(10)).expect("a clip")),
            ],
        );
        let wipe = Wipe::soft(Rational::ONE, Rational::ZERO, softness).expect("a wipe");
        project
            .apply(
                sequence,
                Edit::AddTransition {
                    track: 0,
                    transition: Transition::wiping(1, frames(4), wipe).expect("a wipe"),
                },
            )
            .expect("a wipe");
        let mut source = Flat {
            colours: std::vec![
                (digest_of(&project, outgoing), [255, 255, 255, 255]),
                (digest_of(&project, incoming), [0, 0, 0, 255]),
            ],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        };
        let bytes = timeline::render(
            &project,
            sequence,
            at(9),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render")
        .to_packed()
        .expect("bytes");
        (0..4)
            .map(|column| bytes[column * 4])
            .collect::<std::vec::Vec<u8>>()
    };

    let hard = build(Rational::ZERO);
    let soft = build(Rational::new(3, 4).expect("a softness"));
    assert_ne!(hard, soft);
    let between = |row: &[u8]| {
        row.iter()
            .filter(|value| **value > 0 && **value < 255)
            .count()
    };
    assert_eq!(
        between(&hard),
        1,
        "a line crosses one pixel per row: {hard:?}"
    );
    assert!(between(&soft) > 1, "a ramp crosses several, got {soft:?}");
    // Still monotone across the row, and still opaque -- a soft edge changes
    // how the two clips meet, not which is on which side.
    assert!(
        soft.windows(2).all(|pair| pair[0] <= pair[1]),
        "the row runs from the incoming clip to the outgoing one: {soft:?}"
    );
}

#[test]
fn a_mask_takes_away_everything_outside_its_shape() {
    // The end of the chain for a mask: a shape in the model becomes coverage
    // in the renderer and a hole in the picture. Outside the shape the layer
    // is gone and the black underneath shows; inside it, the clip is whole.
    use media_editor_core::Rational;
    use media_editor_model::Mask;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let white = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(white, 0, frames(10)).expect("a clip"))],
    );
    // The left half of a four-wide frame.
    let mask = Mask::rectangle(
        Rational::ZERO,
        Rational::ZERO,
        Rational::new(1, 2).expect("a half"),
        Rational::ONE,
    )
    .expect("a rectangle");
    project
        .apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: Some(mask),
            },
        )
        .expect("a mask");
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, white), [255, 255, 255, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    let bytes = rendered.to_packed().expect("bytes");
    let row: std::vec::Vec<u8> = (0..4).map(|column| bytes[column * 4]).collect();
    assert_eq!(row, std::vec![255, 255, 0, 0], "the left half survives");
    for column in 0..4 {
        assert_eq!(
            bytes[column * 4 + 3],
            255,
            "the programme is opaque wherever the mask cut, at {column}"
        );
    }
}

#[test]
fn an_inverted_mask_keeps_the_other_half() {
    use media_editor_core::Rational;
    use media_editor_model::Mask;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let white = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(white, 0, frames(10)).expect("a clip"))],
    );
    let mask = Mask::rectangle(
        Rational::ZERO,
        Rational::ZERO,
        Rational::new(1, 2).expect("a half"),
        Rational::ONE,
    )
    .expect("a rectangle")
    .inverted();
    project
        .apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: Some(mask),
            },
        )
        .expect("a mask");
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, white), [255, 255, 255, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let bytes = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render")
    .to_packed()
    .expect("bytes");
    let row: std::vec::Vec<u8> = (0..4).map(|column| bytes[column * 4]).collect();
    assert_eq!(row, std::vec![0, 0, 255, 255], "the other half survives");
}

/// A library that can be told which media it cannot reach.
struct Sometimes {
    inner: Flat,
    missing: std::vec::Vec<Digest>,
}

impl Library for Sometimes {
    fn frame(
        &mut self,
        media: Digest,
        tick: i64,
        description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        assert!(
            !self.missing.contains(&media),
            "the planner asked for media it had been told was not there"
        );
        self.inner.frame(media, tick, description)
    }

    fn available(&mut self, media: Digest) -> bool {
        !self.missing.contains(&media)
    }

    fn look(&mut self, look: Digest) -> Result<Look, RenderStatus> {
        self.inner.look(look)
    }
}

/// One clip of one media, and a library that may or may not have it.
fn one_clip() -> (Project, SequenceId, MediaId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let only = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(only, 0, frames(10)).expect("a clip"))],
    );
    (project, sequence, only)
}

#[test]
fn a_clip_whose_media_is_missing_still_renders() {
    // A project opens when the drive is not mounted. Failing the whole render
    // because one source is unreachable is what makes an editor unusable on
    // the day it matters most.
    let (project, sequence, only) = one_clip();
    let digest = digest_of(&project, only);
    let mut library = Sometimes {
        inner: Flat {
            colours: std::vec![(digest, [200, 30, 40, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec![digest],
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut library,
    )
    .expect("a render even though the media is gone");
    assert_eq!(
        rendered.description(),
        &described(),
        "and it is still exactly the description that was asked for"
    );
}

#[test]
fn offline_is_not_black_and_is_not_a_colour_a_camera_makes() {
    // Black is what an empty timeline shows and a solid colour is something a
    // programme might legitimately contain, so either one would let "the drive
    // is not mounted" look like footage.
    let (project, sequence, only) = one_clip();
    let digest = digest_of(&project, only);
    let mut library = Sometimes {
        inner: Flat {
            colours: std::vec![(digest, [200, 30, 40, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec![digest],
    };
    let bytes = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut library,
    )
    .expect("a render")
    .to_packed()
    .expect("bytes");
    let reds: std::collections::BTreeSet<u8> = (0..4).map(|column| bytes[column * 4]).collect();
    assert!(
        reds.len() > 1,
        "the offline slate varies across the frame, got {reds:?}"
    );
    assert!(
        bytes.chunks_exact(4).all(|pixel| pixel[3] == 255),
        "and the programme is still opaque"
    );
}

#[test]
fn the_offline_slate_never_reaches_the_cache_under_the_pictures_key() {
    // The reason availability is asked *before* the graph is built. A source
    // node's identity covers the media, the tick and the description and not
    // whether the file happened to be reachable -- so a node that fell back
    // during evaluation would cache the slate under the real picture's key and
    // hand it back once the drive came home.
    //
    // One pool across both renders, which is exactly the condition that would
    // expose it.
    let (project, sequence, only) = one_clip();
    let digest = digest_of(&project, only);
    let mut shared = pool();

    let mut absent = Sometimes {
        inner: Flat {
            colours: std::vec![(digest, [200, 30, 40, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec![digest],
    };
    let offline = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut shared,
        &mut absent,
    )
    .expect("a render");

    let mut present = Sometimes {
        inner: Flat {
            colours: std::vec![(digest, [200, 30, 40, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec::Vec::new(),
    };
    let restored = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut shared,
        &mut present,
    )
    .expect("a render");

    assert_ne!(
        offline.digest(),
        restored.digest(),
        "the drive came home and the picture came back"
    );
    assert_eq!(pixel(&restored), (200, 30, 40, 255));
}

#[test]
fn a_planner_does_not_ask_for_media_it_was_told_is_missing() {
    // The `Sometimes` library panics if asked, so this asserting nothing extra
    // is the assertion: an unavailable source is never named in the graph, so
    // nothing ever tries to fetch it.
    let (project, sequence, only) = one_clip();
    let digest = digest_of(&project, only);
    let mut library = Sometimes {
        inner: Flat {
            colours: std::vec::Vec::new(),
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec![digest],
    };
    let (graph, _) =
        timeline::plan(&project, sequence, at(5), described(), &mut library).expect("a plan");
    assert_eq!(
        graph.len(),
        4,
        "a blank, a slate, the legend naming what is missing, and one `over`"
    );
    timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut library,
    )
    .expect("a render");
}

#[test]
fn a_transform_scales_about_the_centre_rather_than_the_corner() {
    // The decision that separates "make it bigger" from "make it bigger and
    // slide it off the bottom right". Halving a full-frame clip about the
    // centre leaves the picture in the middle with transparency around it;
    // halving about the corner would leave it in the top-left quarter.
    use media_editor_core::Rational;
    use media_editor_model::{Resampling, Transform};

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let white = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(white, 0, frames(10)).expect("a clip"))],
    );
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(
                    Transform::scaled(
                        Rational::new(1, 2).expect("a half"),
                        Rational::new(1, 2).expect("a half"),
                        (Rational::ZERO, Rational::ZERO),
                        Resampling::Area,
                    )
                    .expect("a transform"),
                ),
            },
        )
        .expect("a transform");
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, white), [255, 255, 255, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let bytes = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render")
    .to_packed()
    .expect("bytes");
    let at_pixel = |x: usize, y: usize| bytes[(y * 4 + x) * 4];
    // Four pixels across, halved about the centre: the picture occupies the
    // middle two columns and rows, and the outside is the black underneath.
    assert_eq!(at_pixel(1, 1), 255, "the middle is the picture");
    assert_eq!(at_pixel(2, 2), 255);
    assert_eq!(at_pixel(0, 0), 0, "and the corner is not");
    assert_eq!(at_pixel(3, 3), 0);
    for column in 0..4 {
        assert_eq!(
            bytes[(column) * 4 + 3],
            255,
            "the programme is opaque where the clip is not, at {column}"
        );
    }
}

#[test]
fn a_transform_that_moves_nothing_is_not_resampled_at_all() {
    // Exact is a stronger promise than "the arithmetic works out". A clip
    // nobody has moved must not go through a resampler, so the plan has no
    // node for it -- which is what this counts.
    use media_editor_core::Rational;
    use media_editor_model::{Resampling, Transform};

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let one = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(one, 0, frames(10)).expect("a clip"))],
    );
    let mut library = Flat {
        colours: std::vec![(digest_of(&project, one), [200, 30, 40, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let (bare, _) =
        timeline::plan(&project, sequence, at(5), described(), &mut library).expect("a plan");

    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(
                    Transform::new(
                        [Rational::ONE, Rational::ZERO, Rational::ZERO, Rational::ONE],
                        (Rational::ZERO, Rational::ZERO),
                        Resampling::Area,
                    )
                    .expect("a transform"),
                ),
            },
        )
        .expect("a transform");
    let (still, _) =
        timeline::plan(&project, sequence, at(5), described(), &mut library).expect("a plan");
    assert_eq!(still.len(), bare.len(), "the identity adds no node");

    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(
                    Transform::scaled(
                        Rational::new(1, 2).expect("a half"),
                        Rational::new(1, 2).expect("a half"),
                        (Rational::ZERO, Rational::ZERO),
                        Resampling::Area,
                    )
                    .expect("a transform"),
                ),
            },
        )
        .expect("a transform");
    let (moved, _) =
        timeline::plan(&project, sequence, at(5), described(), &mut library).expect("a plan");
    assert_eq!(
        moved.len(),
        bare.len() + 1,
        "and a real one adds exactly one"
    );
}

#[test]
fn a_mask_stays_in_the_frame_while_the_clip_moves_through_it() {
    // A mask is in *frame* coordinates, applied after the transform, so moving
    // a clip moves the picture through a stationary mask. That is what a
    // garbage matte and a split screen both want; a mask that travelled with
    // its clip would be a different feature and would have to say so.
    use media_editor_core::Rational;
    use media_editor_model::{Mask, Resampling, Transform};

    let build = |offset: Rational| {
        let mut project = Project::new();
        let sequence = project.add_sequence(RATE).expect("a sequence");
        let white = media(&mut project, 1);
        lay(
            &mut project,
            sequence,
            0,
            &[Item::Clip(Clip::new(white, 0, frames(10)).expect("a clip"))],
        );
        project
            .apply(
                sequence,
                Edit::SetClipMask {
                    track: 0,
                    index: 0,
                    mask: Some(
                        Mask::rectangle(
                            Rational::ZERO,
                            Rational::ZERO,
                            Rational::new(1, 2).expect("a half"),
                            Rational::ONE,
                        )
                        .expect("a rectangle"),
                    ),
                },
            )
            .expect("a mask");
        project
            .apply(
                sequence,
                Edit::SetClipTransform {
                    track: 0,
                    index: 0,
                    transform: Some(
                        Transform::scaled(
                            Rational::new(1, 2).expect("a half"),
                            Rational::ONE,
                            (offset, Rational::ZERO),
                            Resampling::Area,
                        )
                        .expect("a transform"),
                    ),
                },
            )
            .expect("a transform");
        let mut source = Flat {
            colours: std::vec![(digest_of(&project, white), [255, 255, 255, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        };
        let bytes = timeline::render(
            &project,
            sequence,
            at(5),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render")
        .to_packed()
        .expect("bytes");
        (0..4)
            .map(|column| bytes[column * 4])
            .collect::<std::vec::Vec<u8>>()
    };

    let centred = build(Rational::ZERO);
    let shifted = build(Rational::new(1, 4).expect("a quarter"));
    assert_ne!(centred, shifted, "moving the clip changes the picture");
    // The mask keeps the right half of the frame dark in both, because the
    // mask did not move.
    assert_eq!(centred[3], 0, "the masked side stays masked: {centred:?}");
    assert_eq!(shifted[3], 0, "and still does after the move: {shifted:?}");
}

#[test]
fn an_animated_clip_plans_the_graph_a_still_one_at_that_framing_plans() {
    // M8.10 added animated framings and changed nothing in this crate or in
    // the renderer, which is a claim worth a test rather than a sentence in a
    // commit message. It holds because the layer stack hands out a *resolved*
    // transform: by the time a frame is described, a motion has already become
    // the framing it reads at that moment, so the renderer never learns that
    // anything moves.
    //
    // Three renders, then. Two arrive at the same framing at frame five, one
    // flatly and one by animating through it, and must agree node for node and
    // byte for byte. The third is at a *different* framing, and is here
    // because the first two would agree just as well if the framing were being
    // dropped on the floor -- a flat colour scaled is the same flat colour, so
    // the first fixture written for this passed without the transform doing
    // anything at all.
    use media_editor_model::{Curve, Interpolation, Keyframe, Motion, Resampling, Transform};

    let ratio = |numerator, denominator| {
        media_editor_core::Rational::new(numerator, denominator).expect("a rational")
    };
    let moved = |across| {
        Transform::scaled(
            media_editor_core::Rational::ONE,
            media_editor_core::Rational::ONE,
            (across, media_editor_core::Rational::ZERO),
            Resampling::Bilinear,
        )
        .expect("a transform")
    };

    // A ramp from nought to one over ten frames reads a half at frame five.
    let ramp = Curve::new(std::vec![
        Keyframe::new(
            at(0),
            media_editor_core::Rational::ZERO,
            Interpolation::Linear
        )
        .expect("a keyframe"),
        Keyframe::new(
            at(10),
            media_editor_core::Rational::ONE,
            Interpolation::Linear
        )
        .expect("a keyframe"),
    ])
    .expect("a curve");

    let mut planned = std::vec::Vec::new();
    let mut pixels = std::vec::Vec::new();
    for (base, animate) in [
        (ratio(1, 2), false),
        (media_editor_core::Rational::ZERO, true),
        (ratio(1, 4), false),
    ] {
        let mut project = Project::new();
        let sequence = project.add_sequence(RATE).expect("a sequence");
        let red = media(&mut project, 1);
        lay(
            &mut project,
            sequence,
            0,
            &[Item::Clip(Clip::new(red, 0, frames(10)).expect("a clip"))],
        );
        project
            .apply(
                sequence,
                Edit::SetClipTransform {
                    track: 0,
                    index: 0,
                    transform: Some(moved(base)),
                },
            )
            .expect("a framing");
        if animate {
            project
                .apply(
                    sequence,
                    Edit::SetClipMotion {
                        track: 0,
                        index: 0,
                        motion: Some(
                            Motion::new(None, Some(ramp.clone()), None, None).expect("a motion"),
                        ),
                    },
                )
                .expect("a motion");
        }
        let mut source = Flat {
            colours: std::vec![(digest_of(&project, red), [200, 30, 40, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        };
        let (graph, _) =
            timeline::plan(&project, sequence, at(5), described(), &mut source).expect("a plan");
        planned.push(graph.len());
        let rendered = timeline::render(
            &project,
            sequence,
            at(5),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render");
        pixels.push(rendered.to_packed().expect("bytes"));
    }
    assert_ne!(
        pixels[0], pixels[2],
        "the fixture must vary along the axis under test, or the rest is vacuous"
    );
    assert_eq!(planned[0], planned[1], "the same graph, node for node");
    assert_eq!(pixels[0], pixels[1], "and the same picture, byte for byte");
}

/// The description the slate tests render at, big enough to read a caption on.
///
/// The rest of this file works at four pixels across, which is the size the
/// freestanding image composites at and is right for everything that is about
/// arithmetic. A caption is about *legibility*, and four pixels cannot carry
/// one -- so this is the one fixture here that has to be a picture.
fn readable() -> FrameDescription {
    FrameDescription::square(
        Geometry::new(320, 180).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description")
}

/// A project with one clip of a given media tag, and that media's digest.
fn one_tagged_clip(tag: u8) -> (Project, SequenceId, Digest) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let only = media(&mut project, tag);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(only, 0, frames(10)).expect("a clip"))],
    );
    let digest = digest_of(&project, only);
    (project, sequence, digest)
}

/// What an offline clip of `tag` renders as, at a size a caption fits.
fn offline_slate(tag: u8) -> Frame {
    let (project, sequence, digest) = one_tagged_clip(tag);
    let mut library = Sometimes {
        inner: Flat {
            colours: std::vec::Vec::new(),
            description: readable(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec![digest],
    };
    timeline::render(
        &project,
        sequence,
        at(5),
        readable(),
        &mut pool(),
        &mut library,
    )
    .expect("a render")
}

#[test]
fn an_offline_slate_names_the_media_it_is_missing() {
    // The sentence that stood in the risk section for three milestones:
    // "offline media renders a slate but cannot say which media is missing --
    // that is text on a frame, and text needs a font". There is a font now.
    //
    // Two clips of two different pieces of media, both offline. The slates
    // must differ, and they can only differ in what they say, because the
    // stripes underneath are a pure function of the frame's size.
    let one = offline_slate(1);
    let other = offline_slate(2);
    assert_ne!(
        one.digest(),
        other.digest(),
        "two missing clips get two slates"
    );

    let bare = TestPattern::Offline
        .render(readable())
        .expect("a bare slate");
    assert_ne!(
        one.digest(),
        bare.digest(),
        "and a named slate is not the bare pattern"
    );

    // How much of it is the caption: enough to be a sentence rather than a
    // stray pixel, and far less than half the frame.
    let named = one.to_packed().expect("bytes");
    let plain = bare.to_packed().expect("bytes");
    let changed = named
        .iter()
        .zip(&plain)
        .filter(|(here, there)| here != there)
        .count();
    assert!(
        (2_000..named.len() / 4).contains(&changed),
        "{changed} bytes of {} differ, which is not a caption",
        named.len()
    );
}

#[test]
fn a_slate_too_small_for_a_caption_is_the_bare_pattern() {
    // At four pixels across there is nothing legible to say, and a slate that
    // said it anyway would be a grey smear claiming to be information. The
    // rest of this file renders at that size, so this is also the reason none
    // of those tests changed when captions arrived.
    let (project, sequence, digest) = one_tagged_clip(3);
    let mut library = Sometimes {
        inner: Flat {
            colours: std::vec::Vec::new(),
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec![digest],
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(5),
        described(),
        &mut pool(),
        &mut library,
    )
    .expect("a render");
    let bare = TestPattern::Offline
        .render(described())
        .expect("a bare slate");
    assert_eq!(
        rendered.to_packed().expect("bytes"),
        bare.to_packed().expect("bytes"),
        "byte for byte the pattern, with nothing written on it"
    );
}

/// A project with one title clip on one track, and that title's digest.
fn titled(words: &str) -> (Project, SequenceId, Digest) {
    use media_editor_model::Title;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let title = Title::line(
        words.into(),
        media_editor_core::Rational::new(1, 6).expect("a size"),
        media_editor_core::Rational::new(1, 2).expect("a place"),
        media_editor_core::Rational::new(1, 2).expect("a place"),
    )
    .expect("a title");
    let asset = MediaAsset::titled(title, RATE, frames(1000)).expect("an asset");
    let digest = asset.digest();
    let media = project.add_media(asset).expect("an identifier");
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(media, 0, frames(10)).expect("a clip"))],
    );
    (project, sequence, digest)
}

/// A library that refuses everything and records what it was asked.
struct Refuses {
    asked: std::vec::Vec<Digest>,
}

impl Library for Refuses {
    fn frame(
        &mut self,
        media: Digest,
        _tick: i64,
        _description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        self.asked.push(media);
        Err(RenderStatus::OutsideDomain)
    }

    fn look(&mut self, _look: Digest) -> Result<Look, RenderStatus> {
        Err(RenderStatus::OutsideDomain)
    }

    fn available(&mut self, media: Digest) -> bool {
        self.asked.push(media);
        false
    }
}

#[test]
fn a_title_clip_is_drawn_rather_than_fetched() {
    // And the library is never asked about it -- not for the frame, and not
    // even whether it *has* it. There is nothing to find, so asking would be
    // asking about a file that does not exist, and a library that answered
    // "no" would put an offline slate where somebody's card should be.
    let (project, sequence, _) = titled("MEDIAEDTO");
    let mut library = Refuses {
        asked: std::vec::Vec::new(),
    };
    let (graph, _) =
        timeline::plan(&project, sequence, at(5), readable(), &mut library).expect("a plan");
    assert!(
        library.asked.is_empty(),
        "the library was asked about a title"
    );
    assert_eq!(graph.len(), 3, "a blank, the type, and one `over`");

    let rendered = timeline::render(
        &project,
        sequence,
        at(5),
        readable(),
        &mut pool(),
        &mut library,
    )
    .expect("a render");
    let blank = Frame::blank(readable()).expect("a blank");
    assert_ne!(
        rendered.digest(),
        blank.digest(),
        "and there are words on it"
    );
}

#[test]
fn two_different_cards_are_two_different_pictures() {
    // The identity carries the words, so a programme with two cards on it does
    // not show the first one twice.
    let mut shared = pool();
    let mut pictures = std::vec::Vec::new();
    for words in ["MEDIAEDTO", "THE END"] {
        let (project, sequence, _) = titled(words);
        let mut library = Refuses {
            asked: std::vec::Vec::new(),
        };
        pictures.push(
            timeline::render(
                &project,
                sequence,
                at(5),
                readable(),
                &mut shared,
                &mut library,
            )
            .expect("a render")
            .digest(),
        );
    }
    assert_ne!(pictures[0], pictures[1]);
}

#[test]
fn a_title_is_the_same_picture_at_every_frame_of_its_clip() {
    // A card does not move, so every instant of it is one cached frame. That
    // is not an optimisation this arranged -- it falls out of the node not
    // carrying a tick, which it does not carry because there is nothing for a
    // tick to select.
    let (project, sequence, _) = titled("MEDIAEDTO");
    let mut library = Refuses {
        asked: std::vec::Vec::new(),
    };
    let mut shared = pool();
    let first = timeline::render(
        &project,
        sequence,
        at(1),
        readable(),
        &mut shared,
        &mut library,
    )
    .expect("a render");
    let later = timeline::render(
        &project,
        sequence,
        at(8),
        readable(),
        &mut shared,
        &mut library,
    )
    .expect("a render");
    assert_eq!(first.digest(), later.digest());
}

#[test]
fn a_title_clip_grades_and_masks_like_a_recording() {
    // The claim the whole design rests on, asked of the *renderer* rather than
    // of the model: a title goes where a source goes, so everything above it
    // is the machinery a recording already goes through and none of it had to
    // be told.
    let (mut project, sequence, _) = titled("MEDIAEDTO");
    project
        .apply(
            sequence,
            Edit::SetClipMask {
                track: 0,
                index: 0,
                mask: Some(
                    media_editor_model::Mask::new(std::vec![
                        (
                            media_editor_core::Rational::ZERO,
                            media_editor_core::Rational::ZERO
                        ),
                        (
                            media_editor_core::Rational::new(1, 2).expect("a corner"),
                            media_editor_core::Rational::ZERO
                        ),
                        (
                            media_editor_core::Rational::new(1, 2).expect("a corner"),
                            media_editor_core::Rational::new(1, 1).expect("a corner")
                        ),
                        (
                            media_editor_core::Rational::ZERO,
                            media_editor_core::Rational::new(1, 1).expect("a corner")
                        ),
                    ])
                    .expect("a mask"),
                ),
            },
        )
        .expect("a mask");
    let mut library = Refuses {
        asked: std::vec::Vec::new(),
    };
    let (graph, _) =
        timeline::plan(&project, sequence, at(5), readable(), &mut library).expect("a plan");
    assert_eq!(
        graph.len(),
        4,
        "a blank, the type, the mask, and one `over`"
    );
    let masked = timeline::render(
        &project,
        sequence,
        at(5),
        readable(),
        &mut pool(),
        &mut library,
    )
    .expect("a render");

    let (plain, plain_sequence, _) = titled("MEDIAEDTO");
    let whole = timeline::render(
        &plain,
        plain_sequence,
        at(5),
        readable(),
        &mut pool(),
        &mut library,
    )
    .expect("a render");
    assert_ne!(
        masked.digest(),
        whole.digest(),
        "half the card is gone, which is what the mask asked for"
    );
}

#[test]
fn a_card_of_two_lines_is_not_a_card_of_one() {
    // Through the whole program rather than through the layout: the model
    // holds the lines, the planner hands them to the node, the node sets them,
    // and two cards that say different things are two pictures.
    use media_editor_model::{Alignment, Title};

    let mut pictures = std::vec::Vec::new();
    for lines in [
        std::vec!["Media Editor".to_string()],
        std::vec!["Media Editor".to_string(), "MMXXVI".to_string()],
    ] {
        let mut project = Project::new();
        let sequence = project.add_sequence(RATE).expect("a sequence");
        let title = Title::new(
            lines,
            media_editor_core::Rational::new(1, 10).expect("a size"),
            media_editor_core::Rational::new(1, 2).expect("a place"),
            media_editor_core::Rational::new(1, 2).expect("a place"),
            Alignment::Centre,
        )
        .expect("a title");
        let media = project
            .add_media(MediaAsset::titled(title, RATE, frames(1000)).expect("an asset"))
            .expect("an identifier");
        lay(
            &mut project,
            sequence,
            0,
            &[Item::Clip(Clip::new(media, 0, frames(10)).expect("a clip"))],
        );
        let mut library = Refuses {
            asked: std::vec::Vec::new(),
        };
        pictures.push(
            timeline::render(
                &project,
                sequence,
                at(5),
                readable(),
                &mut pool(),
                &mut library,
            )
            .expect("a render")
            .digest(),
        );
    }
    assert_ne!(pictures[0], pictures[1]);
}

#[test]
fn the_alignment_a_card_was_given_is_the_alignment_it_is_set_in() {
    // The one thing that could quietly go wrong between the model and the
    // renderer, because they name the alignment separately -- they are
    // siblings and neither may depend on the other, so a `match` translates
    // and a `match` is where a wire gets crossed.
    use media_editor_model::{Alignment, Title};

    let mut pictures = std::vec::Vec::new();
    for alignment in [Alignment::Left, Alignment::Centre, Alignment::Right] {
        let mut project = Project::new();
        let sequence = project.add_sequence(RATE).expect("a sequence");
        let title = Title::new(
            std::vec!["MMMMMMMM".to_string(), "I".to_string()],
            media_editor_core::Rational::new(1, 10).expect("a size"),
            media_editor_core::Rational::new(1, 2).expect("a place"),
            media_editor_core::Rational::new(1, 2).expect("a place"),
            alignment,
        )
        .expect("a title");
        let media = project
            .add_media(MediaAsset::titled(title, RATE, frames(1000)).expect("an asset"))
            .expect("an identifier");
        lay(
            &mut project,
            sequence,
            0,
            &[Item::Clip(Clip::new(media, 0, frames(10)).expect("a clip"))],
        );
        let mut library = Refuses {
            asked: std::vec::Vec::new(),
        };
        pictures.push(
            timeline::render(
                &project,
                sequence,
                at(5),
                readable(),
                &mut pool(),
                &mut library,
            )
            .expect("a render")
            .digest(),
        );
    }
    for (index, one) in pictures.iter().enumerate() {
        for other in &pictures[index + 1..] {
            assert_ne!(one, other, "three alignments, three pictures");
        }
    }
}

/// A project with one clip on V1, faded up and down over `rising`/`falling`.
fn faded(rising: i64, falling: i64) -> (Project, SequenceId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let only = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(only, 0, frames(24)).expect("a clip"))],
    );
    project
        .apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: frames(rising),
                fade_out: frames(falling),
            },
        )
        .expect("fades");
    (project, sequence)
}

#[test]
fn a_clip_fades_up_from_black_and_back_down_to_it() {
    // The whole gesture, through the whole program: the model holds the fade,
    // the stack reads it, the planner folds it into the layer's opacity, and
    // the compositor puts the result over black.
    let (project, sequence) = faded(8, 8);
    let mut source = Flat {
        colours: std::vec![(
            digest_of(&project, media(&mut project.clone(), 1)),
            [200, 200, 200, 255]
        )],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let read = |instant, source: &mut Flat| {
        timeline::render(
            &project,
            sequence,
            at(instant),
            described(),
            &mut pool(),
            source,
        )
        .expect("a render")
    };
    assert_eq!(
        pixel(&read(0, &mut source)).0,
        0,
        "the first frame is black"
    );
    assert_eq!(pixel(&read(23, &mut source)).0, 0, "and so is the last");
    let up = pixel(&read(12, &mut source)).0;
    assert_eq!(up, 200, "and in between it is the picture, whole");
    let rising = pixel(&read(4, &mut source)).0;
    assert!(
        rising > 0 && rising < up,
        "halfway up is between the two: {rising}"
    );
}

#[test]
fn a_clip_nobody_faded_is_whole_at_its_first_frame() {
    // The fixture that would have caught the fade being applied where there is
    // none, which is the failure mode a default of "nothing" has.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let only = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(only, 0, frames(24)).expect("a clip"))],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, only), [200, 200, 200, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    for instant in [0, 12, 23] {
        let frame = timeline::render(
            &project,
            sequence,
            at(instant),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render");
        assert_eq!(pixel(&frame).0, 200, "at {instant}");
    }
}

#[test]
fn a_clips_fade_and_its_tracks_opacity_multiply_into_one_node() {
    // One node rather than two: two would be two rounding points and two cache
    // entries for one picture, and the product of two rationals is a rational.
    let (mut project, sequence) = faded(8, 0);
    let only = project.media().iter().next().expect("an asset").0;
    project
        .apply(
            sequence,
            Edit::SetTrackOpacity {
                track: 0,
                opacity: Some(
                    media_editor_model::Curve::constant(
                        at(0),
                        media_editor_core::Rational::new(1, 2).expect("a half"),
                    )
                    .expect("a curve"),
                ),
            },
        )
        .expect("an opacity");
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, only), [200, 200, 200, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let (graph, _) =
        timeline::plan(&project, sequence, at(4), described(), &mut source).expect("a plan");
    assert_eq!(
        graph.len(),
        4,
        "a blank, a source, one fade, and one `over` -- not two fades"
    );
}

#[test]
fn a_faded_clip_still_dissolves() {
    // A fade belongs to the clip and a dissolve belongs to the cut, and where
    // they meet they multiply. Neither is allowed to swallow the other.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let one = media(&mut project, 1);
    let two = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[
            Item::Clip(Clip::new(one, 100, frames(24)).expect("a clip")),
            Item::Clip(Clip::new(two, 100, frames(24)).expect("a clip")),
        ],
    );
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 0,
                transition: media_editor_model::Transition::new(1, frames(8)).expect("a dissolve"),
            },
        )
        .expect("a dissolve");
    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, one), [200, 200, 200, 255]),
            (digest_of(&project, two), [200, 200, 200, 255]),
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let dissolving = timeline::render(
        &project,
        sequence,
        at(22),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(
        pixel(&dissolving).0,
        200,
        "a dissolve between two identical pictures is that picture"
    );

    project
        .apply(
            sequence,
            Edit::SetClipFades {
                track: 0,
                index: 0,
                fade_in: frames(0),
                fade_out: frames(8),
            },
        )
        .expect("a fade out");
    let both = timeline::render(
        &project,
        sequence,
        at(22),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert!(
        pixel(&both).0 < 200,
        "and the outgoing clip's own fade still takes it down: {}",
        pixel(&both).0
    );
}

/// A project with one coloured title clip on one track.
fn inked(words: &str, ink: media_editor_model::Ink) -> (Project, SequenceId) {
    use media_editor_model::Title;

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let title = Title::line(
        words.into(),
        media_editor_core::Rational::new(1, 3).expect("a size"),
        media_editor_core::Rational::new(1, 2).expect("a place"),
        media_editor_core::Rational::new(1, 2).expect("a place"),
    )
    .expect("a title")
    .with_ink(ink);
    let media = project
        .add_media(MediaAsset::titled(title, RATE, frames(1000)).expect("an asset"))
        .expect("an identifier");
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(media, 0, frames(10)).expect("a clip"))],
    );
    (project, sequence)
}

/// The brightest sample in each channel of a rendered frame.
fn brightest(frame: &Frame) -> [u8; 3] {
    let packed = frame.to_packed().expect("the samples");
    let mut most = [0_u8; 3];
    for pixel in packed.chunks_exact(4) {
        for channel in 0..3 {
            most[channel] = most[channel].max(pixel[channel]);
        }
    }
    most
}

#[test]
fn a_titles_ink_reaches_the_picture_through_the_planner() {
    // The model owns the colour, the renderer owns what a colour means, and
    // the planner is the one line between them. A planner that dropped the
    // ink would leave every card white and every test in the two crates
    // either side of it still passing.
    let (project, sequence) = inked(
        "PHIP",
        media_editor_model::Ink::new(
            media_editor_core::Rational::ONE,
            media_editor_core::Rational::ZERO,
            media_editor_core::Rational::ZERO,
        )
        .expect("red"),
    );
    let mut library = Refuses {
        asked: std::vec::Vec::new(),
    };
    let rendered = timeline::render(
        &project,
        sequence,
        at(5),
        readable(),
        &mut pool(),
        &mut library,
    )
    .expect("a render");
    let most = brightest(&rendered);
    assert_eq!(most[0], u8::MAX, "the letters are at full red");
    assert_eq!(
        (most[1], most[2]),
        (0, 0),
        "and there is no green or blue anywhere on the card"
    );
}

#[test]
fn two_cards_that_differ_only_in_ink_are_two_different_pictures() {
    // End to end rather than at the digest: the asset identity test in the
    // model proves they are two *assets*, and this proves the difference
    // survives all the way to the samples rather than being a distinction the
    // renderer then discards.
    let mut shared = pool();
    let mut pictures = std::vec::Vec::new();
    for ink in [
        media_editor_model::Ink::WHITE,
        media_editor_model::Ink::new(
            media_editor_core::Rational::ZERO,
            media_editor_core::Rational::ONE,
            media_editor_core::Rational::ZERO,
        )
        .expect("green"),
    ] {
        let (project, sequence) = inked("PHIP", ink);
        let mut library = Refuses {
            asked: std::vec::Vec::new(),
        };
        pictures.push(
            timeline::render(
                &project,
                sequence,
                at(5),
                readable(),
                &mut shared,
                &mut library,
            )
            .expect("a render")
            .digest(),
        );
    }
    assert_ne!(pictures[0], pictures[1]);
}

#[test]
fn the_planner_carries_the_strength_across() {
    // The seam a negative control found missing once already, in the same
    // place: the model carries the arrival, the node carries the strength, and
    // nothing between them is tested by either side's own tests. Sending one
    // regardless would leave the model holding a colour the picture never
    // shows, and both halves would still look covered.
    //
    // A ramp, and three instants along it, so the picture is a function of
    // *when* it is rendered — a fixture that rendered one instant could not
    // tell a carried strength from a hard-coded one.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(9)).expect("a clip"))],
    );

    let look = swap_look();
    project
        .apply(
            sequence,
            Edit::SetClipGrade {
                track: 0,
                index: 0,
                grade: Some(look.digest().expect("a digest")),
            },
        )
        .expect("a grade");
    project
        .apply(
            sequence,
            Edit::SetClipGradeStrength {
                track: 0,
                index: 0,
                strength: Some(
                    media_editor_model::Curve::new(std::vec![
                        media_editor_model::Keyframe::new(
                            at(0),
                            media_editor_core::Rational::ZERO,
                            media_editor_model::Interpolation::Linear,
                        )
                        .expect("a keyframe"),
                        media_editor_model::Keyframe::new(
                            at(8),
                            media_editor_core::Rational::ONE,
                            media_editor_model::Interpolation::Linear,
                        )
                        .expect("a keyframe"),
                    ])
                    .expect("a curve"),
                ),
            },
        )
        .expect("a strength");

    // Red 200 and blue 10, swapped by the table. At a strength of `s` the red
    // channel is `200 + s(10 - 200)`, in code values, which is 200 at nought,
    // 105 at a half and 10 at one -- derived from the definition rather than
    // read back out of the renderer.
    let mut seen = std::vec::Vec::new();
    for (tick, expected) in [
        (0_i64, (200, 40, 10)),
        (4, (105, 40, 105)),
        (8, (10, 40, 200)),
    ] {
        let mut source = Flat {
            colours: std::vec![(digest_of(&project, id), [200, 40, 10, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec![(look.digest().expect("a digest"), look.clone())],
            answers_wrongly: false,
        };
        let frame = timeline::render(
            &project,
            sequence,
            at(tick),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a frame");
        let (red, green, blue, alpha) = pixel(&frame);
        assert_eq!(
            (red, green, blue, alpha),
            (expected.0, expected.1, expected.2, 255),
            "the arrival is not what the picture shows at tick {tick}"
        );
        seen.push(red);
    }
    assert!(
        seen.windows(2).all(|pair| pair[0] > pair[1]),
        "the picture does not move with the instant, so nothing above is \
         evidence that the strength was carried rather than fixed"
    );
}

/// A clip framed by the identity and turning through a quarter over its length.
///
/// Split out because the test that uses it ran past the hundred lines clippy
/// allows, which is the same reason `sample` in the format suite grew two
/// helpers this week.
fn turning() -> (Project, SequenceId, MediaId) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(24)).expect("a clip"))],
    );
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(
                    media_editor_model::Transform::scaled(
                        media_editor_core::Rational::ONE,
                        media_editor_core::Rational::ONE,
                        (
                            media_editor_core::Rational::ZERO,
                            media_editor_core::Rational::ZERO,
                        ),
                        media_editor_model::Resampling::Bilinear,
                    )
                    .expect("a transform"),
                ),
            },
        )
        .expect("a framing");
    project
        .apply(
            sequence,
            Edit::SetClipMotion {
                track: 0,
                index: 0,
                motion: Some(
                    media_editor_model::Motion::new(
                        None,
                        None,
                        None,
                        Some(
                            media_editor_model::Curve::new(std::vec![
                                media_editor_model::Keyframe::new(
                                    at(0),
                                    media_editor_core::Rational::ZERO,
                                    media_editor_model::Interpolation::Linear,
                                )
                                .expect("a keyframe"),
                                media_editor_model::Keyframe::new(
                                    at(24),
                                    media_editor_core::Rational::ONE,
                                    media_editor_model::Interpolation::Linear,
                                )
                                .expect("a keyframe"),
                            ])
                            .expect("a curve"),
                        ),
                    )
                    .expect("a motion"),
                ),
            },
        )
        .expect("a turn");

    (project, sequence, id)
}

#[test]
fn a_turned_framing_reaches_the_graph_and_a_turn_of_nothing_does_not() {
    // The seam: the model resolves the turn into the framing, and the planner
    // has to hand that framing's linear part to the node. Both halves look
    // covered from their own side, which is the shape this project's notes say
    // always lacks a test.
    //
    // Asserted on the plan rather than on a picture, deliberately. The only
    // source these tests have draws a flat colour, and a flat colour turned is
    // the same flat colour — the fixture could not see the axis under test.
    // What a turn actually *does* to pixels is pinned in the resampler's own
    // suite, where a quarter turn of a 4x4 picture is an exact permutation.
    let (project, sequence, id) = turning();
    let linear_at = |tick: i64| -> Option<[media_editor_core::Rational; 4]> {
        let mut library = Flat {
            colours: std::vec![(digest_of(&project, id), [200, 40, 10, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        };
        let (graph, root) = timeline::plan(&project, sequence, at(tick), described(), &mut library)
            .expect("a plan");
        let mut found = None;
        let mut pending = std::vec![root];
        while let Some(id) = pending.pop() {
            let node = graph.node(id).expect("a node");
            pending.extend_from_slice(node.inputs());
            if let media_editor_render::Node::Transform { linear, .. } = node {
                found = Some(*linear);
            }
        }
        found
    };

    // At the first frame the parameter is nought, so the framing is the
    // identity and the planner adds no node at all -- exact beats "the
    // arithmetic works out", which is what `Transform::is_still` is for.
    assert_eq!(
        linear_at(0),
        None,
        "a turn of nothing was resampled rather than skipped"
    );
    // At the halfway mark the parameter is a half, which is the three-four-five
    // turn: cos 3/5, sin 4/5, derived by hand from the parametrisation.
    let one = media_editor_core::Rational::ONE;
    let three_fifths = media_editor_core::Rational::new(3, 5).expect("a rational");
    let four_fifths = media_editor_core::Rational::new(4, 5).expect("a rational");
    assert_eq!(
        linear_at(12),
        Some([
            three_fifths,
            media_editor_core::Rational::ZERO
                .checked_sub(four_fifths)
                .expect("a negation"),
            four_fifths,
            three_fifths
        ]),
        "the node did not get the turn the curve read"
    );
    // And the last frame is a different turn again, so the node is a function
    // of the instant rather than of the clip.
    assert_ne!(linear_at(12), linear_at(20));
    assert_ne!(
        linear_at(20),
        Some([
            one,
            media_editor_core::Rational::ZERO,
            media_editor_core::Rational::ZERO,
            one
        ])
    );
}

#[test]
fn the_planner_carries_the_pivot_across() {
    // The seam again. The model knows where a framing pivots and the node
    // needs to be told, and neither side's own tests cross the join.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let id = media(&mut project, 1);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(id, 0, frames(10)).expect("a clip"))],
    );
    let pivot = (
        media_editor_core::Rational::ZERO,
        media_editor_core::Rational::new(1, 4).expect("a rational"),
    );
    project
        .apply(
            sequence,
            Edit::SetClipTransform {
                track: 0,
                index: 0,
                transform: Some(
                    media_editor_model::Transform::scaled(
                        media_editor_core::Rational::new(2, 1).expect("a rational"),
                        media_editor_core::Rational::new(2, 1).expect("a rational"),
                        (
                            media_editor_core::Rational::ZERO,
                            media_editor_core::Rational::ZERO,
                        ),
                        media_editor_model::Resampling::Bilinear,
                    )
                    .expect("a transform")
                    .with_anchor(pivot),
                ),
            },
        )
        .expect("a framing");

    let mut library = Flat {
        colours: std::vec![(digest_of(&project, id), [200, 40, 10, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let (graph, root) =
        timeline::plan(&project, sequence, at(0), described(), &mut library).expect("a plan");
    let mut found = None;
    let mut pending = std::vec![root];
    while let Some(id) = pending.pop() {
        let node = graph.node(id).expect("a node");
        pending.extend_from_slice(node.inputs());
        if let media_editor_render::Node::Transform { anchor, .. } = node {
            found = Some(*anchor);
        }
    }
    assert_eq!(
        found,
        Some(pivot),
        "the model holds a pivot the picture never turns about"
    );
    // And the default reaches the node as the centre rather than as nothing,
    // so a clip nobody moved the pivot of renders the way it always did.
    assert_ne!(
        found,
        Some((
            media_editor_core::Rational::ZERO,
            media_editor_core::Rational::ZERO
        )),
        "the fixture's pivot is not the corner, so this comparison means something"
    );
}

/// A project whose outer sequence has a nested sequence on its own track.
///
/// Two picture tracks on the outer sequence: a clip of one colour on V1, and a
/// **nest** on V2. The nested sequence holds a clip of another colour over
/// only *half* its length, so the second half of the nest is empty — which is
/// the case the whole transparency decision is about.
fn with_a_nest(
    project: &mut Project,
) -> (
    media_editor_model::SequenceId,
    media_editor_model::MediaId,
    media_editor_model::MediaId,
) {
    let under = media(project, 1);
    let over = media(project, 2);
    let outer = project.add_sequence(RATE).expect("a sequence");
    let inner = project.add_sequence(RATE).expect("a sequence");

    lay(
        project,
        inner,
        0,
        &[
            Item::Clip(Clip::new(over, 0, frames(5)).expect("a clip")),
            Item::gap(frames(5)).expect("a gap"),
        ],
    );
    let nest = project
        .add_media(
            media_editor_model::MediaAsset::nesting(inner, RATE, frames(10)).expect("an asset"),
        )
        .expect("room");
    lay(
        project,
        outer,
        0,
        &[Item::Clip(Clip::new(under, 0, frames(10)).expect("a clip"))],
    );
    lay(
        project,
        outer,
        1,
        &[Item::Clip(Clip::new(nest, 0, frames(10)).expect("a clip"))],
    );
    (outer, under, over)
}

#[test]
fn a_nested_sequence_renders_as_the_sequence_it_is() {
    // The end-to-end case. A nest is media the program makes out of a
    // sequence, so it goes where a source would and everything above it is
    // the machinery a recording already goes through -- and what it *is* is
    // the nested sequence composited at the instant the clip reads.
    let mut project = Project::new();
    let (outer, under, over) = with_a_nest(&mut project);

    let mut library = Flat {
        colours: std::vec![
            (digest_of(&project, under), [10, 20, 200, 255]),
            (digest_of(&project, over), [200, 40, 10, 255]),
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    // Frame nought: the nest holds its clip there, so the nest's picture wins
    // over V1.
    let frame = timeline::render(
        &project,
        outer,
        at(0),
        described(),
        &mut pool(),
        &mut library,
    )
    .expect("a frame");
    assert_eq!(
        pixel(&frame),
        (200, 40, 10, 255),
        "the nested sequence's own clip is not what the nest shows"
    );
}

#[test]
fn a_nest_is_transparent_where_it_is_empty() {
    // The decision this milestone turns on, and the reason `Node::Empty`
    // exists at all. A nested sequence is **material**, not a programme:
    // material that is absent is absent, and composited onto black leader it
    // would blank out every track beneath it wherever it happened to be
    // empty -- which would make a nest on V2 useless for anything but a
    // full-length one.
    let mut project = Project::new();
    let (outer, under, over) = with_a_nest(&mut project);

    let mut library = Flat {
        colours: std::vec![
            (digest_of(&project, under), [10, 20, 200, 255]),
            (digest_of(&project, over), [200, 40, 10, 255]),
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    // Frame seven: the nested sequence has a gap there, so V1 shows through.
    let frame = timeline::render(
        &project,
        outer,
        at(7),
        described(),
        &mut pool(),
        &mut library,
    )
    .expect("a frame");
    assert_eq!(
        pixel(&frame),
        (10, 20, 200, 255),
        "the empty half of the nest blanked out the track beneath it"
    );
    // And the two instants really are different pictures, so neither
    // assertion is passing for a reason that has nothing to do with the nest.
    let first = timeline::render(
        &project,
        outer,
        at(0),
        described(),
        &mut pool(),
        &mut library,
    )
    .expect("a frame");
    assert_ne!(pixel(&first), pixel(&frame));
}

#[test]
fn the_two_nesting_bounds_agree() {
    // The model refuses to build a project nested deeper than it renders, and
    // the planner walks with an explicit stack bounded by the same number.
    // Neither crate may depend on the other -- they are siblings -- so the
    // constant is written twice, exactly as the fader's travel is, and this is
    // what stops the two drifting.
    assert_eq!(
        media_editor_model::MAX_NESTING_DEPTH,
        media_editor_app::timeline::MAX_NESTING_DEPTH,
    );
}

#[test]
fn a_nest_shows_itself_at_the_offset_its_clip_reads() {
    // The nest's own boundary lands where the *clip* puts it rather than where
    // the programme's clock is. Five frames of picture and five of nothing,
    // dropped into the outer sequence five frames in: the change from one to
    // the other belongs at outer frame ten, and a planner that walked the nest
    // at the programme's instant would put it at five.
    let mut project = Project::new();
    let under = media(&mut project, 1);
    let over = media(&mut project, 2);
    let outer = project.add_sequence(RATE).expect("a sequence");
    let inner = project.add_sequence(RATE).expect("a sequence");
    lay(
        &mut project,
        inner,
        0,
        &[
            Item::Clip(Clip::new(over, 0, frames(5)).expect("a clip")),
            Item::gap(frames(5)).expect("a gap"),
        ],
    );
    let nest = project
        .add_media(
            media_editor_model::MediaAsset::nesting(inner, RATE, frames(10)).expect("an asset"),
        )
        .expect("room");
    lay(
        &mut project,
        outer,
        0,
        &[Item::Clip(Clip::new(under, 0, frames(20)).expect("a clip"))],
    );
    lay(
        &mut project,
        outer,
        1,
        &[
            Item::gap(frames(5)).expect("a gap"),
            Item::Clip(Clip::new(nest, 0, frames(10)).expect("a clip")),
        ],
    );

    let mut library = Flat {
        colours: std::vec![
            (digest_of(&project, under), [10, 20, 200, 255]),
            (digest_of(&project, over), [200, 40, 10, 255]),
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let shown = |at_frame: i64, library: &mut Flat| {
        pixel(
            &timeline::render(
                &project,
                outer,
                at(at_frame),
                described(),
                &mut pool(),
                library,
            )
            .expect("a frame"),
        )
    };
    // Outer frame nine is the nest's own frame four, which its clip still
    // covers; outer frame ten is the nest's frame five, where it stops.
    assert_eq!(
        shown(9, &mut library),
        (200, 40, 10, 255),
        "the nest ended before its clip did"
    );
    assert_eq!(
        shown(10, &mut library),
        (10, 20, 200, 255),
        "the nest went on past the frame its clip reads it to"
    );
}

#[test]
fn a_ramped_clip_asks_for_the_frame_the_area_names() {
    // The whole of what a ramp does, seen from the far end of the program: the
    // planner asks the clip where it has got to and the clip answers with the
    // area under its speed curve. Nothing between here and there was told what
    // a ramp is.
    use media_editor_core::Rational;
    use media_editor_model::curve::{Curve, Interpolation, Keyframe};

    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let shot = media(&mut project, 1);
    // Full speed for ten ticks, then down to a quarter over the next twenty.
    let ramp = Curve::new(std::vec![
        Keyframe::new(at(10), Rational::ONE, Interpolation::Linear).expect("a keyframe"),
        Keyframe::new(
            at(30),
            Rational::new(1, 4).expect("a quarter"),
            Interpolation::Linear
        )
        .expect("a keyframe"),
    ])
    .expect("a curve");
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(shot, 0, frames(60))
                .expect("a clip")
                .with_ramp(ramp)
                .expect("a ramp"),
        )],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, shot), [200, 30, 40, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    // Tick twenty is halfway down the ramp. The speed there is
    // 1 + (1/4 - 1) x 10/20 = 5/8, so the area is
    // 10 + (1 + 5/8)/2 x 10 = 10 + 65/8 = 145/8, whose floor is 18.
    timeline::render(
        &project,
        sequence,
        at(20),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(
        source.asked,
        std::vec![(digest_of(&project, shot), 18)],
        "the ramp did not reach the frame the render asks for"
    );
}

/// The rows of a scan, gathered into the frame they make.
///
/// A test's business rather than the library's: a caller with room for a whole
/// frame should call `render`, and one without should never assemble one.
fn gathered(scan: &timeline::Scan, source: &mut dyn Library) -> Frame {
    let one = scan.row_description().expect("a row description");
    let stride = one
        .format()
        .plane_row_bytes(one.geometry(), 0)
        .expect("a stride");
    let mut samples = std::vec::Vec::new();
    for row in 0..scan.height() {
        let line = scan.row(row, source).expect("a row");
        assert_eq!(line.description(), &one, "a row is not one row");
        samples.extend_from_slice(line.plane(0).expect("a plane").samples());
    }
    let full = media_editor_media::FrameDescription::new(
        media_editor_media::Geometry::new(
            one.geometry().width(),
            u32::try_from(scan.height()).expect("a height"),
        )
        .expect("a geometry"),
        one.format(),
        one.colour(),
        one.siting(),
        one.alpha(),
        one.pixel_aspect(),
    )
    .expect("a description");
    Frame::new(
        full,
        std::vec![media_editor_media::Plane::new(samples, stride).expect("a plane")],
    )
    .expect("a frame")
}

#[test]
fn a_scans_rows_are_the_frame_the_render_makes() {
    // The property, at the top of the program: a scan and a render are one
    // picture computed two ways. Everything below this — the graph's row form,
    // the spool's plane rows, the catalogue's ranged reads — exists to make
    // this line true without holding a frame.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let under = media(&mut project, 1);
    let over = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(under, 0, frames(10)).expect("a clip"))],
    );
    lay(
        &mut project,
        sequence,
        1,
        &[Item::Clip(
            Clip::new(over, 0, frames(10))
                .expect("a clip")
                .with_fades(frames(4), frames(4))
                .expect("fades"),
        )],
    );
    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, under), [10, 20, 200, 255]),
            (digest_of(&project, over), [200, 40, 10, 255]),
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let whole = timeline::render(
        &project,
        sequence,
        at(2),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    let scan =
        timeline::Scan::open(&project, sequence, at(2), described(), &mut source).expect("a scan");
    assert_eq!(scan.height(), described().geometry().height() as usize);
    assert_eq!(gathered(&scan, &mut source), whole);
}

#[test]
fn a_scan_plans_once_however_many_rows_it_renders() {
    // The reason a scan is a type rather than a function. Planning walks the
    // stack and resolves every clip's media; doing that once a row would trade
    // one kind of waste for a worse one. The library's record of what it was
    // asked is what says so: `available` is called while planning.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let shot = media(&mut project, 3);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(shot, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, shot), [30, 60, 90, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let scan =
        timeline::Scan::open(&project, sequence, at(0), described(), &mut source).expect("a scan");
    source.asked.clear();
    for row in 0..scan.height() {
        scan.row(row, &mut source).expect("a row");
    }
    // One fetch a row, and not one fetch a row plus a plan a row: every entry
    // names the same media at the same tick, which is what a re-plan would
    // also produce -- so the number is what distinguishes them.
    assert_eq!(
        source.asked.len(),
        scan.height(),
        "the scan fetched something other than one row of one shot per row"
    );
}

#[test]
fn a_scan_of_a_turned_clip_agrees_with_the_render_of_it() {
    // This test has now said three different things, and each change was a
    // refusal turning out to be narrower than it claimed. It began as "a
    // framed clip cannot be scanned"; M8.44 found that a scale takes
    // horizontals to horizontals and made it a turn; and strips find that even
    // a turn can be scanned, because whether a row's preimage fits a band is a
    // question about the **width** of the strip and not about the map.
    //
    // Nothing in the planner refuses a programme for the operation it performs
    // any more. What is left is one row of a steep enough downscale, and that
    // is a question about a row.
    let cosine = media_editor_core::Rational::new(4, 5).expect("a cosine");
    let sine = media_editor_core::Rational::new(3, 5).expect("a sine");
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let shot = media(&mut project, 4);
    let framed = Clip::new(shot, 0, frames(10))
        .expect("a clip")
        .with_transform(Some(
            media_editor_model::Transform::new(
                [cosine, sine.checked_neg().expect("a sine"), sine, cosine],
                (
                    media_editor_core::Rational::ZERO,
                    media_editor_core::Rational::ZERO,
                ),
                media_editor_model::Resampling::Area,
            )
            .expect("a transform"),
        ));
    lay(&mut project, sequence, 0, &[Item::Clip(framed)]);
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, shot), [1, 2, 3, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let whole = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    let scan =
        timeline::Scan::open(&project, sequence, at(0), described(), &mut source).expect("a scan");
    let packed = whole.to_packed().expect("bytes");
    let stride = packed.len() / scan.height();
    for row in 0..scan.height() {
        let one = scan.row(row, &mut source).expect("a row");
        assert_eq!(
            one.to_packed().expect("bytes"),
            packed[row * stride..(row + 1) * stride],
            "row {row} of a turned clip disagrees with the render"
        );
    }
}

#[test]
fn a_scan_of_a_scaled_clip_agrees_with_the_render_of_it() {
    // The direction that used to be a refusal, and the same shape M8.42's
    // generators took: the test that said a framing could not be scanned is
    // now the test that says it can, and it compares the gathered rows against
    // the whole render rather than merely observing that nothing refused.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let shot = media(&mut project, 4);
    let framed = Clip::new(shot, 0, frames(10))
        .expect("a clip")
        .with_transform(Some(
            media_editor_model::Transform::scaled(
                media_editor_core::Rational::new(1, 2).expect("a scale"),
                media_editor_core::Rational::new(1, 3).expect("a scale"),
                (
                    media_editor_core::Rational::new(1, 8).expect("a move"),
                    media_editor_core::Rational::ZERO,
                ),
                media_editor_model::Resampling::Area,
            )
            .expect("a transform"),
        ));
    lay(&mut project, sequence, 0, &[Item::Clip(framed)]);
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, shot), [1, 2, 3, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let whole = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    let scan =
        timeline::Scan::open(&project, sequence, at(0), described(), &mut source).expect("a scan");
    let packed = whole.to_packed().expect("bytes");
    let stride = packed.len() / scan.height();
    for row in 0..scan.height() {
        let one = scan.row(row, &mut source).expect("a row");
        assert_eq!(
            one.to_packed().expect("bytes"),
            packed[row * stride..(row + 1) * stride],
            "row {row} of a scaled clip disagrees with the render"
        );
    }
}

#[test]
fn an_offline_programme_scans_like_any_other() {
    // This test used to be `a_scan_of_offline_media_says_which_kind_of_refusal
    // _it_is`, and it was right until the generators grew row forms. An
    // offline clip becomes a slate and a legend, and `NoRowForm` was always a
    // statement about the build rather than about the operation -- which is
    // exactly why it was a different status from `NotRowLocal`, and why this
    // test could change and that one could not.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let missing = media(&mut project, 5);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(
            Clip::new(missing, 0, frames(10)).expect("a clip"),
        )],
    );
    let mut source = Sometimes {
        inner: Flat {
            colours: std::vec::Vec::new(),
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
        missing: std::vec![digest_of(&project, missing)],
    };
    let scan = timeline::Scan::open(&project, sequence, at(0), described(), &mut source)
        .expect("an offline programme scans");
    let whole = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(gathered(&scan, &mut source), whole);
    // And the library was never asked for a row, because there is no media to
    // read: the slate is drawn.
    assert!(source.inner.asked.is_empty());
}

#[test]
fn a_library_with_no_row_form_opens_a_scan_and_refuses_its_first_row() {
    // Where the halves of the contract meet, and the reason `open` does not
    // ask the library. `open` refuses a *plan* that cannot be scanned -- a
    // framing, a generator with no row form -- because that answer is free and
    // a caller choosing between scanning and rendering whole wants it before
    // it commits. Whether the *library* can serve rows is not asked, because
    // asking would mean a second statement of the same fact, on a trait with
    // a default, free to disagree with the row itself; and because the answer
    // costs nothing to discover: every source in the graph is touched by row
    // nought, so a library that cannot serve rows refuses on the first one and
    // never on the four hundredth.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let shot = media(&mut project, 6);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(shot, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Whole {
        inner: Flat {
            colours: std::vec![(digest_of(&project, shot), [7, 8, 9, 255])],
            description: described(),
            asked: std::vec::Vec::new(),
            looks: std::vec::Vec::new(),
            answers_wrongly: false,
        },
    };
    let scan =
        timeline::Scan::open(&project, sequence, at(0), described(), &mut source).expect("a scan");
    assert_eq!(
        scan.row(0, &mut source).err(),
        Some(SlateStatus::Render(RenderStatus::NoRowForm)),
        "the first row, not a later one"
    );
    // And the same programme renders whole through the same library, which is
    // what makes this a missing row form rather than a broken library.
    timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
}

#[test]
fn a_scan_of_an_empty_programme_is_black_leader_row_by_row() {
    // A programme with nothing on it is opaque black, and every row of it is
    // one row of opaque black -- which is the case a row renderer would get
    // wrong by handing back nothing at all.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let mut source = Flat {
        colours: std::vec::Vec::new(),
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let scan =
        timeline::Scan::open(&project, sequence, at(0), described(), &mut source).expect("a scan");
    let whole = timeline::render(
        &project,
        sequence,
        at(0),
        described(),
        &mut pool(),
        &mut source,
    )
    .expect("a render");
    assert_eq!(gathered(&scan, &mut source), whole);
    assert!(source.asked.is_empty(), "nothing was decoded for nothing");
}

#[test]
fn a_row_past_the_bottom_of_a_scan_is_refused() {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let shot = media(&mut project, 6);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(shot, 0, frames(10)).expect("a clip"))],
    );
    let mut source = Flat {
        colours: std::vec![(digest_of(&project, shot), [9, 9, 9, 255])],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    let scan =
        timeline::Scan::open(&project, sequence, at(0), described(), &mut source).expect("a scan");
    assert_eq!(
        scan.row(scan.height(), &mut source).err(),
        Some(SlateStatus::Render(RenderStatus::OutsideDomain))
    );
}

#[test]
fn a_nested_sequence_scans_row_by_row() {
    // Nesting is the deepest graph the planner builds, so it is the one worth
    // checking the two evaluators agree on: a nest is composited onto nothing
    // rather than onto black, and every row of that has to say so.
    let mut project = Project::new();
    let (outer, under, over) = with_a_nest(&mut project);
    let mut source = Flat {
        colours: std::vec![
            (digest_of(&project, under), [10, 20, 200, 255]),
            (digest_of(&project, over), [200, 40, 10, 255]),
        ],
        description: described(),
        asked: std::vec::Vec::new(),
        looks: std::vec::Vec::new(),
        answers_wrongly: false,
    };
    for instant in [0_i64, 7] {
        let whole = timeline::render(
            &project,
            outer,
            at(instant),
            described(),
            &mut pool(),
            &mut source,
        )
        .expect("a render");
        let scan = timeline::Scan::open(&project, outer, at(instant), described(), &mut source)
            .expect("a scan");
        assert_eq!(
            gathered(&scan, &mut source),
            whole,
            "the two evaluators disagree at instant {instant}"
        );
    }
}
