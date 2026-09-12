// SPDX-License-Identifier: GPL-3.0-only
//! Compare a 320×180 reference render against its committed PNG.
//!
//! The frame includes two test patterns, a soft wipe, and an upper-layer mask.
//! On mismatch the test writes the actual render beside the reference for
//! visual inspection.

use std::path::PathBuf;

use media_editor_core::{Digest, Duration, Instant, Rational, Timebase};
use media_editor_media::colour::{MatrixCoefficients, Primaries, Range, TransferFunction};
use media_editor_media::{
    AlphaState, ColourDescription, Frame, FrameDescription, Geometry, PixelFormat, TestPattern,
    pool::FramePool,
};
use media_editor_model::{
    Clip, Edit, Item, Mask, MediaAsset, Project, TrackKind, Transition, Wipe,
};
use media_editor_render::{Library, Look, RenderStatus};

const RATE: Timebase = Timebase::FILM_24;

/// The size the reference is captured at.
const WIDTH: u32 = 320;
const HEIGHT: u32 = 180;

fn described() -> FrameDescription {
    FrameDescription::square(
        Geometry::new(WIDTH, HEIGHT).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription {
            primaries: Primaries::Bt709,
            transfer: TransferFunction::Srgb,
            matrix: MatrixCoefficients::Identity,
            range: Range::Full,
        },
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description")
}

/// Three premultiplied sources keyed by media digest.
///
/// The upper track uses distinct images on each side of the wipe so its
/// feathered edge remains visible in the reference.
struct Patterns {
    bars: Digest,
    ramp: Digest,
    flat: Digest,
}

impl Library for Patterns {
    fn frame(
        &mut self,
        media: Digest,
        _tick: i64,
        description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        if media == self.flat {
            let pixels = usize::try_from(WIDTH).expect("a width")
                * usize::try_from(HEIGHT).expect("a height");
            let mut packed = Vec::new();
            for _ in 0..pixels {
                packed.extend_from_slice(&[240, 96, 16, 255]);
            }
            return Ok(Frame::from_packed(description, &packed)?);
        }
        let pattern = if media == self.bars {
            TestPattern::Bars
        } else if media == self.ramp {
            TestPattern::Ramp
        } else {
            return Err(RenderStatus::SourceDescriptionMismatch);
        };
        // Drawn opaque, so the samples are already valid premultiplied ones:
        // at full coverage the two forms are the same bytes.
        Ok(pattern.render(description)?)
    }

    fn look(&mut self, _look: Digest) -> Result<Look, RenderStatus> {
        Err(RenderStatus::LookSpaceMismatch)
    }
}

fn golden() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../tests/golden/reference.png")
}

/// The project the capture renders: bars underneath, a ramp and a flat colour
/// above them meeting at a soft wipe, both inside a six-sided mask.
fn composed() -> (Project, media_editor_model::SequenceId) {
    let mut project = Project::new();
    let bars = project
        .add_media(
            MediaAsset::new(
                Digest::of(b"bars"),
                RATE,
                Duration::new(240, RATE).expect("a length"),
            )
            .expect("an asset"),
        )
        .expect("room");
    let ramp = project
        .add_media(
            MediaAsset::new(
                Digest::of(b"ramp"),
                RATE,
                Duration::new(240, RATE).expect("a length"),
            )
            .expect("an asset"),
        )
        .expect("room");
    let flat = project
        .add_media(
            MediaAsset::new(
                Digest::of(b"flat"),
                RATE,
                Duration::new(240, RATE).expect("a length"),
            )
            .expect("an asset"),
        )
        .expect("room");
    let sequence = project.add_sequence(RATE).expect("room");
    for index in 0..2 {
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

    // The lower track: bars all the way across.
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(
                    Clip::new(bars, 0, Duration::new(48, RATE).expect("a length")).expect("a clip"),
                ),
            },
        )
        .expect("a clip");

    // The upper track: a ramp cut against itself, with a soft wipe on the cut
    // and a six-sided mask on both halves, so the capture shows a wipe's
    // feathered edge and a mask's anti-aliased one in one picture.
    for (index, source) in [(0_usize, ramp), (1, flat)] {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track: 1,
                    index,
                    // Sixty frames in, so the incoming clip has the handles a
                    // sixteen-frame wipe needs to start eight frames early.
                    // The model checks that and is right to.
                    item: Item::Clip(
                        Clip::new(source, 60, Duration::new(24, RATE).expect("a length"))
                            .expect("a clip"),
                    ),
                },
            )
            .expect("a clip");
    }
    dress(&mut project, sequence);
    (project, sequence)
}

/// The mask on both upper clips, and the soft wipe between them.
fn dress(project: &mut Project, sequence: media_editor_model::SequenceId) {
    let hexagon = Mask::new(vec![
        (
            Rational::new(1, 8).expect("a rational"),
            Rational::new(1, 2).expect("a rational"),
        ),
        (
            Rational::new(1, 3).expect("a rational"),
            Rational::new(1, 12).expect("a rational"),
        ),
        (
            Rational::new(2, 3).expect("a rational"),
            Rational::new(1, 12).expect("a rational"),
        ),
        (
            Rational::new(7, 8).expect("a rational"),
            Rational::new(1, 2).expect("a rational"),
        ),
        (
            Rational::new(2, 3).expect("a rational"),
            Rational::new(11, 12).expect("a rational"),
        ),
        (
            Rational::new(1, 3).expect("a rational"),
            Rational::new(11, 12).expect("a rational"),
        ),
    ])
    .expect("a hexagon");
    for index in 0..2 {
        project
            .apply(
                sequence,
                Edit::SetClipMask {
                    track: 1,
                    index,
                    mask: Some(hexagon.clone()),
                },
            )
            .expect("a mask");
    }
    project
        .apply(
            sequence,
            Edit::AddTransition {
                track: 1,
                transition: Transition::wiping(
                    1,
                    Duration::new(16, RATE).expect("a length"),
                    Wipe::soft(
                        Rational::ONE,
                        Rational::new(1, 3).expect("a rational"),
                        Rational::new(1, 5).expect("a rational"),
                    )
                    .expect("a wipe"),
                )
                .expect("a wipe"),
            },
        )
        .expect("a wipe");
}

#[test]
fn the_reference_capture_is_the_picture_beside_it() {
    let (project, sequence) = composed();
    let mut library = Patterns {
        bars: Digest::of(b"bars"),
        ramp: Digest::of(b"ramp"),
        flat: Digest::of(b"flat"),
    };
    let mut pool = FramePool::new(64, 1 << 24);
    let frame = media_editor_app::timeline::render(
        &project,
        sequence,
        Instant::new(21, RATE),
        described(),
        &mut pool,
        &mut library,
    )
    .expect("a render");

    // The wipe has to be visible in the capture, or the capture is claiming to
    // show something it does not. Two pixels well inside the hexagon, either
    // side of the edge, and a band of partial values between them: a hard edge
    // would step straight from one to the other.
    let packed = frame.to_packed().expect("bytes");
    let at = |x: usize, y: usize| {
        let start = (y * usize::try_from(WIDTH).expect("a width") + x) * 4;
        [
            packed[start],
            packed[start + 1],
            packed[start + 2],
            packed[start + 3],
        ]
    };
    assert_ne!(
        at(120, 90),
        at(200, 90),
        "the two sides of the wipe must differ, or the edge is invisible"
    );
    let row: Vec<u8> = (100..190).map(|x| at(x, 90)[1]).collect();
    let distinct: std::collections::BTreeSet<u8> = row.iter().copied().collect();
    assert!(
        distinct.len() > 8,
        "a soft edge crosses many values, got {} across the band",
        distinct.len()
    );

    // PNG's alpha is straight, and the capture writer refuses premultiplied
    // samples rather than writing them as though they were not -- which is the
    // dark fringe around every badly keyed title. The programme is opaque, so
    // this conversion is a divide by one and is exact.
    let straight = media_editor_render::composite::unpremultiply(&frame).expect("a straight frame");
    let captured = media_editor_io::png::encode(&straight).expect("a capture");
    let reference = golden();
    let held = std::fs::read(&reference).ok();
    if held.as_deref() == Some(captured.as_slice()) {
        return;
    }

    // Write what actually came out, beside the reference, and say where. A
    // digest would say only that something changed.
    let actual = reference.with_file_name("reference-actual.png");
    std::fs::write(&actual, &captured).expect("somewhere to put the capture");
    assert!(
        held.is_some(),
        "no reference at {}; this run's capture is at {}",
        reference.display(),
        actual.display()
    );
    panic!(
        "the capture differs from {}; this run's is at {} — open both",
        reference.display(),
        actual.display()
    );
}
