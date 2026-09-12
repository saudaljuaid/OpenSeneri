// SPDX-License-Identifier: GPL-3.0-only
//! The render graph, and the order-independence that has to hold before
//! there is a second core to make it interesting.

use media_editor_media::colour::{MatrixCoefficients, Primaries, Range, TransferFunction};
use media_editor_media::{
    ColourDescription, Frame, FrameDescription, FramePool, Geometry, PixelFormat, TestPattern,
};
use media_editor_render::{Graph, Look, Node, NodeId, RenderStatus};

/// A media source that refuses everything.
///
/// Every node in this file is a pattern, a blank or a conversion, so nothing
/// here should ever ask for media. Handing over a source that refuses is how
/// that is checked rather than assumed.
struct NoMedia;

impl media_editor_render::Library for NoMedia {
    fn frame(
        &mut self,
        _media: media_editor_core::Digest,
        _tick: i64,
        _description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        Err(RenderStatus::UnknownNode)
    }

    fn look(&mut self, _look: media_editor_core::Digest) -> Result<Look, RenderStatus> {
        Err(RenderStatus::UnknownNode)
    }
}

fn described(colour: ColourDescription) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(16, 9).expect("a geometry"),
        PixelFormat::Rgb8,
        colour,
        None,
        None,
    )
    .expect("a description")
}

fn srgb() -> FrameDescription {
    described(ColourDescription::srgb_full())
}

fn wide() -> FrameDescription {
    described(ColourDescription::new(
        Primaries::Bt2020,
        TransferFunction::Srgb,
        MatrixCoefficients::Identity,
        Range::Full,
    ))
}

fn pool() -> FramePool {
    FramePool::new(64, 1 << 20)
}

/// A graph with three sources and a conversion of each, plus one chain.
fn sample() -> (Graph, std::vec::Vec<NodeId>) {
    let mut graph = Graph::new();
    let mut leaves = std::vec::Vec::new();
    for pattern in [
        TestPattern::Bars,
        TestPattern::Ramp,
        TestPattern::Checkerboard { square: 3 },
    ] {
        let source = graph
            .add(Node::Pattern {
                pattern,
                description: srgb(),
            })
            .expect("a node");
        let converted = graph
            .add(Node::Convert {
                input: source,
                target: wide(),
            })
            .expect("a node");
        let back = graph
            .add(Node::Convert {
                input: converted,
                target: srgb(),
            })
            .expect("a node");
        leaves.push(back);
    }
    (graph, leaves)
}

#[test]
fn a_node_cannot_refer_to_one_that_does_not_exist_yet() {
    // Which is the only way a cycle could be written. There is no `add_edge`
    // to get wrong and no validation pass to forget.
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Blank {
            description: srgb(),
        })
        .expect("a node");
    assert_eq!(source.index(), 0);

    let mut other = Graph::new();
    let stolen = graph
        .add(Node::Convert {
            input: source,
            target: wide(),
        })
        .expect("a node");
    assert_eq!(
        other.add(Node::Convert {
            input: stolen,
            target: wide()
        }),
        Err(RenderStatus::UnknownNode),
        "an identifier from another graph names nothing here either"
    );
}

#[test]
fn a_nodes_identity_covers_everything_that_changes_its_picture() {
    let mut graph = Graph::new();
    let bars = graph
        .add(Node::Pattern {
            pattern: TestPattern::Bars,
            description: srgb(),
        })
        .expect("a node");
    let ramp = graph
        .add(Node::Pattern {
            pattern: TestPattern::Ramp,
            description: srgb(),
        })
        .expect("a node");
    let wide_bars = graph
        .add(Node::Pattern {
            pattern: TestPattern::Bars,
            description: wide(),
        })
        .expect("a node");
    let converted = graph
        .add(Node::Convert {
            input: bars,
            target: wide(),
        })
        .expect("a node");
    let converted_from_ramp = graph
        .add(Node::Convert {
            input: ramp,
            target: wide(),
        })
        .expect("a node");

    // The same node added twice has the same identity, which is what makes the
    // cache able to notice.
    let again = graph
        .add(Node::Pattern {
            pattern: TestPattern::Bars,
            description: srgb(),
        })
        .expect("a node");

    let identity = |id: NodeId| graph.identity(id).expect("an identity");
    assert_ne!(identity(bars), identity(ramp), "a different pattern");
    assert_ne!(
        identity(bars),
        identity(wide_bars),
        "a different description"
    );
    assert_ne!(identity(bars), identity(converted), "a different operation");
    assert_ne!(
        identity(converted),
        identity(converted_from_ramp),
        "the same operation on a different input"
    );
    assert_eq!(identity(bars), identity(again));
    assert_eq!(
        graph.cache_key(bars).expect("a key"),
        graph.cache_key(again).expect("a key")
    );
}

#[test]
fn evaluating_a_node_twice_is_a_cache_hit() {
    let (graph, leaves) = sample();
    let mut cache = pool();
    let first = graph
        .evaluate(leaves[0], &mut cache, &mut NoMedia)
        .expect("a frame");
    let hits_before = cache.census().hits;
    let second = graph
        .evaluate(leaves[0], &mut cache, &mut NoMedia)
        .expect("a frame");
    assert_eq!(first.digest(), second.digest());
    assert!(
        cache.census().hits > hits_before,
        "the second evaluation should have come from the pool"
    );
}

#[test]
fn evaluating_in_any_order_gives_the_same_answers() {
    // R-6.2: graph output is independent of evaluation order.
    let (graph, leaves) = sample();

    let reference: std::vec::Vec<_> = {
        let mut cache = pool();
        leaves
            .iter()
            .map(|leaf| {
                graph
                    .evaluate(*leaf, &mut cache, &mut NoMedia)
                    .expect("a frame")
                    .digest()
            })
            .collect()
    };

    // Every order of the three leaves, each with a fresh cache and with a
    // shared one, because a cache that leaked between evaluations would show
    // up as a difference here.
    let orders = [
        [0_usize, 1, 2],
        [0, 2, 1],
        [1, 0, 2],
        [1, 2, 0],
        [2, 0, 1],
        [2, 1, 0],
    ];
    for order in orders {
        let mut shared = pool();
        for index in order {
            let fresh = {
                let mut cache = pool();
                graph
                    .evaluate(leaves[index], &mut cache, &mut NoMedia)
                    .expect("a frame")
                    .digest()
            };
            let pooled = graph
                .evaluate(leaves[index], &mut shared, &mut NoMedia)
                .expect("a frame")
                .digest();
            assert_eq!(fresh, reference[index], "order {order:?}, fresh cache");
            assert_eq!(pooled, reference[index], "order {order:?}, shared cache");
        }
    }
}

#[test]
fn a_pool_too_small_to_hold_a_frame_does_not_change_the_answer() {
    // A cache is an optimisation. If it ever changed a result it would not be
    // one, and a render that refused because the cache was small would be the
    // cache making a decision that is not its to make.
    let (graph, leaves) = sample();
    let mut generous = pool();
    let expected = graph
        .evaluate(leaves[0], &mut generous, &mut NoMedia)
        .expect("a frame");

    let mut tiny = FramePool::new(64, 16);
    let got = graph
        .evaluate(leaves[0], &mut tiny, &mut NoMedia)
        .expect("a frame");
    assert_eq!(got.digest(), expected.digest());
    assert_eq!(tiny.census().frames, 0, "nothing fitted, and nothing broke");
}

#[test]
fn a_pool_that_evicts_does_not_change_the_answer() {
    let (graph, leaves) = sample();
    let mut generous = pool();
    let expected: std::vec::Vec<_> = leaves
        .iter()
        .map(|leaf| {
            graph
                .evaluate(*leaf, &mut generous, &mut NoMedia)
                .expect("a frame")
                .digest()
        })
        .collect();

    // Room for one frame at a time, so every evaluation evicts the last.
    let mut cramped = FramePool::new(1, 1 << 20);
    for (index, leaf) in leaves.iter().enumerate() {
        let got = graph
            .evaluate(*leaf, &mut cramped, &mut NoMedia)
            .expect("a frame");
        assert_eq!(got.digest(), expected[index]);
    }
    assert!(cramped.census().evictions > 0, "the pool did evict");
}

#[test]
fn a_chain_of_conversions_lands_where_a_single_one_would() {
    // 709 to 2020 to 709 must return the picture, so the leaves of the sample
    // graph must equal their own sources. Greys survive exactly; the bars and
    // the checkerboard contain saturated primaries, which is a different test.
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Pattern {
            pattern: TestPattern::Ramp,
            description: srgb(),
        })
        .expect("a node");
    let widened = graph
        .add(Node::Convert {
            input: source,
            target: wide(),
        })
        .expect("a node");
    let back = graph
        .add(Node::Convert {
            input: widened,
            target: srgb(),
        })
        .expect("a node");

    let mut cache = pool();
    let original = graph
        .evaluate(source, &mut cache, &mut NoMedia)
        .expect("a frame");
    let returned = graph
        .evaluate(back, &mut cache, &mut NoMedia)
        .expect("a frame");
    assert_eq!(
        returned.digest(),
        original.digest(),
        "a round trip through a wider gamut must return the picture"
    );
}

#[test]
fn the_graph_refuses_to_grow_past_its_bound() {
    let mut graph = Graph::new();
    for _ in 0..4096 {
        graph
            .add(Node::Blank {
                description: srgb(),
            })
            .expect("room");
    }
    assert_eq!(
        graph.add(Node::Blank {
            description: srgb()
        }),
        Err(RenderStatus::GraphTooLarge)
    );
    assert_eq!(graph.len(), 4096);
}

#[test]
fn an_unknown_node_is_refused() {
    let (graph, _) = sample();
    let stranger = Graph::new();
    assert!(stranger.is_empty());
    let mut cache = pool();
    let beyond = NodeId::from_index_for_test(graph.len());
    assert_eq!(graph.node(beyond), Err(RenderStatus::UnknownNode));
    assert_eq!(graph.identity(beyond), Err(RenderStatus::UnknownNode));
    assert_eq!(
        graph.evaluate(beyond, &mut cache, &mut NoMedia),
        Err(RenderStatus::UnknownNode)
    );
}

#[test]
fn a_node_may_only_name_nodes_that_already_exist_on_every_input() {
    // Every input must refer to an earlier node. Check both inputs of `Over`.
    let mut graph = Graph::new();
    let real = graph
        .add(Node::Pattern {
            pattern: TestPattern::Bars,
            description: srgb(),
        })
        .expect("a node");
    let ghost = NodeId::from_index_for_test(99);

    assert_eq!(
        graph.clone().add(Node::Over {
            layers: [ghost, real],
        }),
        Err(RenderStatus::UnknownNode),
        "a lower layer that does not exist"
    );
    assert_eq!(
        graph.clone().add(Node::Over {
            layers: [real, ghost],
        }),
        Err(RenderStatus::UnknownNode),
        "an upper layer that does not exist"
    );
    assert_eq!(
        graph.clone().add(Node::Fade {
            input: ghost,
            opacity: media_editor_core::Rational::ONE,
        }),
        Err(RenderStatus::UnknownNode)
    );

    // And `inputs` reports both, which is what the acyclicity argument rests
    // on even where something else happens to catch it first.
    let over = graph
        .add(Node::Over {
            layers: [real, real],
        })
        .expect("a node");
    assert_eq!(graph.node(over).expect("a node").inputs().len(), 2);
}

#[test]
fn a_faded_node_is_a_different_node_at_every_opacity() {
    // Opacity is a parameter, so it is part of a node's identity. If it were
    // not, a dissolve would cache its first frame and show it for the whole
    // transition.
    let mut graph = Graph::new();
    let bars = graph
        .add(Node::Pattern {
            pattern: TestPattern::Bars,
            description: srgb(),
        })
        .expect("a node");
    let mut seen = std::vec::Vec::new();
    for step in 1..6 {
        let node = graph
            .add(Node::Fade {
                input: bars,
                opacity: media_editor_core::Rational::new(step, 6).expect("a ratio"),
            })
            .expect("a node");
        let identity = graph.identity(node).expect("an identity");
        assert!(!seen.contains(&identity), "opacity {step}/6 collided");
        seen.push(identity);
    }
}

#[test]
fn two_patterns_are_two_nodes() {
    // A pattern's identity has to cover *which* pattern. Two that hashed the
    // same would let a cache hand back a ramp where an offline slate belongs,
    // or the other way round -- and the offline slate is the one case where
    // that matters most, because it is what a viewer sees when the media is
    // gone and it must never be confused for footage.
    let mut graph = Graph::new();
    let mut identities = std::collections::BTreeSet::new();
    for pattern in [
        TestPattern::Bars,
        TestPattern::Ramp,
        TestPattern::Checkerboard { square: 8 },
        TestPattern::Flat { value: 128 },
        TestPattern::Offline,
    ] {
        let node = graph
            .add(Node::Pattern {
                pattern,
                description: srgb(),
            })
            .expect("a node");
        identities.insert(graph.identity(node).expect("an identity"));
    }
    assert_eq!(identities.len(), 5, "five patterns, five identities");
}

#[test]
fn a_transform_node_is_named_by_its_pivot_as_well() {
    // The anchor was in the identity and nothing asked it to be. A control
    // that hashed the across component twice — so two pivots differing only
    // down the frame collide — changed no test at all, which is a coverage
    // report rather than a finding about the code.
    //
    // Both axes are varied here, separately, because that is the axis the
    // control moved and a fixture that only varied one could not see it.
    // Premultiplied and with coverage, because the resampler requires both --
    // averaging straight samples across an edge is what puts a dark fringe on
    // a keyed title, and it refuses rather than doing it.
    let described = FrameDescription::square(
        Geometry::new(16, 9).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(media_editor_media::AlphaState::Premultiplied),
    )
    .expect("a description");
    let mut graph = Graph::new();
    let base = graph
        .add(Node::Pattern {
            // A checkerboard varies on both axes, so horizontal and vertical
            // pivot changes are both observable.
            pattern: TestPattern::Checkerboard { square: 3 },
            description: described,
        })
        .expect("a node");
    let linear = [
        media_editor_core::Rational::new(2, 1).expect("a rational"),
        media_editor_core::Rational::ZERO,
        media_editor_core::Rational::ZERO,
        media_editor_core::Rational::new(2, 1).expect("a rational"),
    ];
    let half = media_editor_core::Rational::new(1, 2).expect("a rational");
    let quarter = media_editor_core::Rational::new(1, 4).expect("a rational");
    let mut identities = std::vec::Vec::new();
    for anchor in [
        (half, half),
        // Differs across only.
        (quarter, half),
        // Differs down only, which is the one the control crossed out.
        (half, quarter),
    ] {
        let id = graph
            .add(Node::Transform {
                input: base,
                linear,
                offset: (
                    media_editor_core::Rational::ZERO,
                    media_editor_core::Rational::ZERO,
                ),
                anchor,
                bilinear: false,
            })
            .expect("a node");
        identities.push(graph.identity(id).expect("an identity"));
    }
    assert_ne!(
        identities[0], identities[1],
        "two pivots differing across the frame share a cache key"
    );
    assert_ne!(
        identities[0], identities[2],
        "two pivots differing down the frame share a cache key"
    );
    assert_ne!(identities[1], identities[2]);

    // And the three really are three pictures, so the identities above are
    // distinguishing things that differ rather than things that happen to.
    let mut pool = pool();
    let mut pictures = std::vec::Vec::new();
    for anchor in [(half, half), (quarter, half), (half, quarter)] {
        let id = graph
            .add(Node::Transform {
                input: base,
                linear,
                offset: (
                    media_editor_core::Rational::ZERO,
                    media_editor_core::Rational::ZERO,
                ),
                anchor,
                bilinear: false,
            })
            .expect("a node");
        pictures.push(
            graph
                .evaluate(id, &mut pool, &mut NoMedia)
                .expect("a frame")
                .to_packed()
                .expect("bytes"),
        );
    }
    assert_ne!(pictures[0], pictures[1], "the pivot changed nothing across");
    assert_ne!(pictures[0], pictures[2], "the pivot changed nothing down");
}
