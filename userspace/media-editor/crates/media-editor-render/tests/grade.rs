// SPDX-License-Identifier: GPL-3.0-only
//! A look as a step in a render graph.
//!
//! The node names its look by digest and fetches it from the library, exactly
//! as a source names its media. Most of what follows is about the consequences
//! of that: a grade edited between two renders must be a different cache key,
//! and the same grade in two places must be fetched once.

use media_editor_core::{Digest, Fixed, Rational};
use media_editor_media::colour::{MatrixCoefficients, Primaries, Range, TransferFunction};
use media_editor_media::{
    ColourDescription, Frame, FrameDescription, FramePool, Geometry, PixelFormat,
};
use media_editor_render::lut::{Colour, Interpolation, Lut3D};
use media_editor_render::{Graph, Library, Look, Node, RenderStatus};

fn at(numerator: i64, denominator: i64) -> Fixed {
    Fixed::from_rational(Rational::new(numerator, denominator).expect("a ratio")).expect("a value")
}

fn colour() -> ColourDescription {
    ColourDescription {
        primaries: Primaries::Bt709,
        transfer: TransferFunction::Srgb,
        matrix: MatrixCoefficients::Identity,
        range: Range::Full,
    }
}

/// The same, with coverage, for the one test that composites.
fn described_keyed() -> FrameDescription {
    FrameDescription::square(
        Geometry::new(4, 2).expect("a geometry"),
        PixelFormat::Rgba8,
        colour(),
        None,
        Some(media_editor_media::AlphaState::Premultiplied),
    )
    .expect("a description")
}

fn described() -> FrameDescription {
    FrameDescription::square(
        Geometry::new(4, 2).expect("a geometry"),
        PixelFormat::Rgb8,
        colour(),
        None,
        None,
    )
    .expect("a description")
}

/// A table that swaps red and blue, neutral on its own diagonal.
fn swap(size: usize) -> Lut3D {
    let last = i64::try_from(size - 1).expect("a size");
    let mut samples = std::vec::Vec::new();
    for blue in 0..size {
        for green in 0..size {
            for red in 0..size {
                let (r, g, b) = (
                    i64::try_from(red).expect("an index"),
                    i64::try_from(green).expect("an index"),
                    i64::try_from(blue).expect("an index"),
                );
                samples.push([at(b, last), at(g, last), at(r, last)] as Colour);
            }
        }
    }
    Lut3D::new(size, samples).expect("a table")
}

/// A library holding one flat colour and a set of looks, counting fetches.
struct Held {
    pixel: [u8; 3],
    looks: std::vec::Vec<(Digest, Look)>,
    frames_asked: usize,
    looks_asked: usize,
}

impl Library for Held {
    fn frame(
        &mut self,
        _media: Digest,
        _tick: i64,
        description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        self.frames_asked += 1;
        // Opaque where the format carries coverage, so a premultiplied frame
        // is trivially a valid one — the tests that use coverage here are
        // counting fetches rather than looking at pixels, and a frame whose
        // colour outran its own coverage would be refused before it got
        // anywhere near what they are measuring.
        let pixel: std::vec::Vec<u8> = if description.format().has_alpha() {
            std::vec![self.pixel[0], self.pixel[1], self.pixel[2], 255]
        } else {
            self.pixel.to_vec()
        };
        let wanted = description.packed_bytes().expect("a size");
        let bytes: std::vec::Vec<u8> = pixel.into_iter().cycle().take(wanted).collect();
        Frame::from_packed(description, &bytes).map_err(RenderStatus::Media)
    }

    fn look(&mut self, look: Digest) -> Result<Look, RenderStatus> {
        self.looks_asked += 1;
        self.looks
            .iter()
            .find(|(digest, _)| *digest == look)
            .map(|(_, held)| held.clone())
            .ok_or(RenderStatus::UnknownNode)
    }
}

fn library(pixel: [u8; 3], looks: &[Look]) -> Held {
    Held {
        pixel,
        looks: looks
            .iter()
            .map(|held| (held.digest().expect("a digest"), held.clone()))
            .collect(),
        frames_asked: 0,
        looks_asked: 0,
    }
}

#[test]
fn a_look_node_grades_what_it_reads() {
    let look = Look::new(swap(5), colour(), Interpolation::Tetrahedral);
    let mut held = library([200, 40, 10], std::slice::from_ref(&look));

    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media: Digest::of(b"footage"),
            tick: 0,
            description: described(),
        })
        .expect("a node");
    let graded = graph
        .add(Node::Look {
            input: source,
            look: look.digest().expect("a digest"),
            strength: Rational::ONE,
        })
        .expect("a node");

    let mut pool = FramePool::new(64, 1 << 20);
    let frame = graph
        .evaluate(graded, &mut pool, &mut held)
        .expect("a frame");
    assert_eq!(
        &frame.to_packed().expect("bytes")[..3],
        &[10, 40, 200],
        "the look did not reach the pixels"
    );
}

#[test]
fn a_grade_edited_between_renders_is_a_different_key() {
    // The reason the node names its look by digest rather than by a handle. A
    // colourist changes a table, the file behind the handle changes, and a
    // graph that named it by handle would answer the next render out of the
    // pool with the old look — which is the fault that never gets reported as
    // a bug, because it looks like the grade not having been saved.
    let first = Look::new(swap(5), colour(), Interpolation::Tetrahedral);
    let mut other = std::vec::Vec::new();
    let last = 4_i64;
    for blue in 0..5_i64 {
        for green in 0..5_i64 {
            for red in 0..5_i64 {
                // Green and blue swapped instead, so it is a different look
                // and not merely a different table with the same effect.
                other.push([at(red, last), at(blue, last), at(green, last)] as Colour);
            }
        }
    }
    let second = Look::new(
        Lut3D::new(5, other).expect("a table"),
        colour(),
        Interpolation::Tetrahedral,
    );
    assert_ne!(
        first.digest().expect("a digest"),
        second.digest().expect("a digest"),
        "two different looks hash the same, so nothing below proves anything"
    );

    let mut held = library([200, 40, 10], &[first.clone(), second.clone()]);
    let mut pool = FramePool::new(64, 1 << 20);
    let mut render = |look: &Look| {
        let mut graph = Graph::new();
        let source = graph
            .add(Node::Source {
                media: Digest::of(b"footage"),
                tick: 0,
                description: described(),
            })
            .expect("a node");
        let graded = graph
            .add(Node::Look {
                input: source,
                look: look.digest().expect("a digest"),
                strength: Rational::ONE,
            })
            .expect("a node");
        graph
            .evaluate(graded, &mut pool, &mut held)
            .expect("a frame")
            .to_packed()
            .expect("bytes")
    };
    let before = render(&first);
    let after = render(&second);
    assert_ne!(
        before, after,
        "the second grade was answered out of the pool with the first"
    );
}

#[test]
fn the_same_grade_twice_is_computed_once() {
    // What the graph is for. Two branches naming the same source and the same
    // look are one cache key, so the frame is fetched once and graded once.
    // Premultiplied, because `over` needs coverage — and a look refuses
    // premultiplied samples, which is right and is tested elsewhere. So the
    // two branches here are `Fade` rather than `Look`, and the *source* is
    // what must be fetched once. The look's own caching is the assertion
    // below it.
    let look = Look::new(swap(5), colour(), Interpolation::Tetrahedral);
    let mut held = library([200, 40, 10], std::slice::from_ref(&look));

    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media: Digest::of(b"footage"),
            tick: 0,
            description: described_keyed(),
        })
        .expect("a node");
    let graded = graph
        .add(Node::Fade {
            input: source,
            opacity: Rational::new(1, 2).expect("a ratio"),
        })
        .expect("a node");
    let again = graph
        .add(Node::Fade {
            input: source,
            opacity: Rational::new(1, 2).expect("a ratio"),
        })
        .expect("a node");
    let over = graph
        .add(Node::Over {
            layers: [graded, again],
        })
        .expect("a node");

    let mut pool = FramePool::new(64, 1 << 20);
    graph.evaluate(over, &mut pool, &mut held).expect("a frame");
    assert_eq!(
        held.frames_asked, 1,
        "the source was decoded more than once"
    );

    // And the look's own caching, on a graph that can carry one: two nodes
    // naming the same source and the same look are one key, so the table is
    // fetched once and the grade computed once.
    let mut plain = Graph::new();
    let base = plain
        .add(Node::Source {
            media: Digest::of(b"footage"),
            tick: 0,
            description: described(),
        })
        .expect("a node");
    let mut ids = std::vec::Vec::new();
    for _ in 0..2 {
        ids.push(
            plain
                .add(Node::Look {
                    input: base,
                    look: look.digest().expect("a digest"),
                    strength: Rational::ONE,
                })
                .expect("a node"),
        );
    }
    let mut counted = library([200, 40, 10], &[look]);
    let mut second = FramePool::new(64, 1 << 20);
    for id in ids {
        plain
            .evaluate(id, &mut second, &mut counted)
            .expect("a frame");
    }
    assert_eq!(
        counted.looks_asked, 1,
        "the look was fetched more than once"
    );
    assert_eq!(
        counted.frames_asked, 1,
        "the source was decoded more than once"
    );
}

#[test]
fn a_looks_identity_covers_more_than_its_samples() {
    // Two of the three things a look holds are as capable of changing every
    // pixel as the cube is. A digest over the samples alone would make a table
    // read tetrahedrally and the same table read trilinearly one look, and a
    // cache holding either would answer for the other.
    let table = swap(5);
    let tetrahedral = Look::new(table.clone(), colour(), Interpolation::Tetrahedral);
    let trilinear = Look::new(table.clone(), colour(), Interpolation::Trilinear);
    assert_ne!(
        tetrahedral.digest().expect("a digest"),
        trilinear.digest().expect("a digest"),
        "the interpolation is not in the identity"
    );

    let mut elsewhere = colour();
    elsewhere.transfer = TransferFunction::Gamma22;
    let other_space = Look::new(table, elsewhere, Interpolation::Tetrahedral);
    assert_ne!(
        tetrahedral.digest().expect("a digest"),
        other_space.digest().expect("a digest"),
        "the encoding it was authored for is not in the identity"
    );

    // And the same look built twice is the same look.
    assert_eq!(
        tetrahedral.digest().expect("a digest"),
        Look::new(swap(5), colour(), Interpolation::Tetrahedral)
            .digest()
            .expect("a digest")
    );
}

#[test]
fn a_look_the_library_does_not_hold_is_refused() {
    // A graph names a look and does not carry it. A project referring to a
    // grade nobody can supply must refuse rather than render ungraded, because
    // ungraded footage that looks plausible is the one outcome nobody catches.
    let look = Look::new(swap(5), colour(), Interpolation::Tetrahedral);
    let mut held = library([200, 40, 10], &[]);

    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media: Digest::of(b"footage"),
            tick: 0,
            description: described(),
        })
        .expect("a node");
    let graded = graph
        .add(Node::Look {
            input: source,
            look: look.digest().expect("a digest"),
            strength: Rational::ONE,
        })
        .expect("a node");
    let mut pool = FramePool::new(64, 1 << 20);
    assert!(
        graph.evaluate(graded, &mut pool, &mut held).is_err(),
        "a missing look rendered as though there were none"
    );
}

#[test]
fn the_strength_is_in_the_nodes_identity() {
    // A look coming on over a shot is a different picture at every frame. A
    // key that named the look and not how much of it was on would serve the
    // whole arrival out of one cache entry — which is the same fault as
    // leaving the tick out of a source's identity, and that one is already in
    // this project's table.
    let look = Look::new(swap(5), colour(), Interpolation::Tetrahedral);
    let mut held = library([200, 40, 10], std::slice::from_ref(&look));

    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media: Digest::of(b"footage"),
            tick: 0,
            description: described(),
        })
        .expect("a node");
    let mut ids = std::vec::Vec::new();
    let strengths = [
        Rational::ZERO,
        Rational::new(1, 3).expect("a ratio"),
        Rational::new(2, 3).expect("a ratio"),
        Rational::ONE,
    ];
    for strength in strengths {
        ids.push(
            graph
                .add(Node::Look {
                    input: source,
                    look: look.digest().expect("a digest"),
                    strength,
                })
                .expect("a node"),
        );
    }
    let identities: std::vec::Vec<Digest> = ids
        .iter()
        .map(|id| graph.identity(*id).expect("an identity"))
        .collect();
    for (first, later) in
        (0..identities.len()).flat_map(|i| (i + 1..identities.len()).map(move |j| (i, j)))
    {
        assert_ne!(
            identities[first], identities[later],
            "two strengths of one look share an identity, so a cache would \
             answer one with the other"
        );
    }

    // And the pictures really are different, so the identities above are not
    // distinguishing things that happen to look the same anyway.
    let mut pool = FramePool::new(64, 1 << 20);
    let mut pictures = std::vec::Vec::new();
    for id in ids {
        pictures.push(
            graph
                .evaluate(id, &mut pool, &mut held)
                .expect("a frame")
                .to_packed()
                .expect("bytes"),
        );
    }
    assert_eq!(
        &pictures[0][..3],
        &[200, 40, 10],
        "no strength is the source"
    );
    assert_eq!(
        &pictures[3][..3],
        &[10, 40, 200],
        "full strength is the look"
    );
    for pair in pictures.windows(2) {
        assert_ne!(pair[0], pair[1], "two strengths drew the same picture");
    }
}
