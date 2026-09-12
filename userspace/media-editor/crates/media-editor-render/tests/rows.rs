// SPDX-License-Identifier: GPL-3.0-only
//! The graph, evaluated a row at a time.
//!
//! One property does the work here and everything else supports it: **for
//! every node and every row, `row(id, y)` is byte for byte the `y`-th row of
//! `evaluate(id)`**. Two evaluators that disagreed anywhere would be two
//! answers to one question, and the whole value of the row form is that it is
//! the same picture computed differently.
//!
//! Why it exists: a 1920×1080 eight-bit RGBA frame is 8,294,400 bytes and a
//! Phipia program is mapped 76 KiB, so `evaluate` allocates a hundred and six
//! times the program's whole address space *per node*. One row of it is 7,680
//! bytes.

use media_editor_core::{Digest, Rational};
use media_editor_media::colour::AlphaState;
use media_editor_media::{
    ColourDescription, Frame, FrameDescription, FramePool, Geometry, PixelFormat, Plane,
};
use media_editor_render::{Graph, Library, Look, Node, NodeId, RenderStatus};

const WIDE: u32 = 7;
const TALL: u32 = 5;

fn described(format: PixelFormat) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(WIDE, TALL).expect("a geometry"),
        format,
        ColourDescription::srgb_full(),
        None,
        if format == PixelFormat::Rgba8 {
            Some(AlphaState::Premultiplied)
        } else {
            None
        },
    )
    .expect("a description")
}

/// A picture whose every byte is a function of where it is, so a row taken
/// from the wrong place is visible rather than plausible.
fn picture(description: FrameDescription, tint: u8) -> Frame {
    let format = description.format();
    let geometry = description.geometry();
    let stride = format.plane_row_bytes(geometry, 0).expect("a stride");
    let rows = geometry.height() as usize;
    let mut samples = std::vec::Vec::new();
    for row in 0..rows {
        for column in 0..stride {
            // Premultiplied RGBA has to hold colour no brighter than its own
            // coverage, so the alpha byte is full and the rest is bounded.
            let value = if format == PixelFormat::Rgba8 && column % 4 == 3 {
                255
            } else {
                u8::try_from((row * 29 + column * 11 + tint as usize) % 200).expect("a byte")
            };
            samples.push(value);
        }
    }
    Frame::new(
        description,
        std::vec![Plane::new(samples, stride).expect("a plane")],
    )
    .expect("a frame")
}

/// A library that serves whole frames and rows out of the same pictures.
///
/// The row it hands back is sliced from the frame it would have handed back,
/// which is what makes the agreement below a statement about the *graph*
/// rather than about two different sets of pixels.
struct Pictures {
    held: std::vec::Vec<(Digest, Frame)>,
    looks: std::vec::Vec<(Digest, Look)>,
    /// How many whole frames the library was asked for.
    frames_asked: usize,
    /// How many rows.
    rows_asked: usize,
    /// Answer rows with a frame of the wrong width.
    lies_in_rows: bool,
}

impl Pictures {
    fn new(held: std::vec::Vec<(Digest, Frame)>) -> Self {
        Self {
            held,
            looks: std::vec::Vec::new(),
            frames_asked: 0,
            rows_asked: 0,
            lies_in_rows: false,
        }
    }

    fn find(&self, media: Digest) -> Result<&Frame, RenderStatus> {
        self.held
            .iter()
            .find(|(digest, _)| *digest == media)
            .map(|(_, frame)| frame)
            .ok_or(RenderStatus::MediaAbsent)
    }
}

impl Library for Pictures {
    fn frame(
        &mut self,
        media: Digest,
        _tick: i64,
        description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        self.frames_asked += 1;
        let held = self.find(media)?.clone();
        if held.description() != &description {
            return Err(RenderStatus::SourceDescriptionMismatch);
        }
        Ok(held)
    }

    fn look(&mut self, look: Digest) -> Result<Look, RenderStatus> {
        self.looks
            .iter()
            .find(|(digest, _)| *digest == look)
            .map(|(_, held)| held.clone())
            .ok_or(RenderStatus::LookAbsent)
    }

    fn row(
        &mut self,
        media: Digest,
        _tick: i64,
        description: FrameDescription,
        row: usize,
    ) -> Result<Frame, RenderStatus> {
        self.rows_asked += 1;
        let held = self.find(media)?.clone();
        if held.description() != &description {
            return Err(RenderStatus::SourceDescriptionMismatch);
        }
        if self.lies_in_rows {
            // A row of the right picture at the wrong width: legal on its own
            // and not what was asked for, which is what the graph's own check
            // is there to catch. A library that got this wrong by accident
            // would composite a short row into a full one.
            return slice(&picture(described(PixelFormat::Gray8), 1), row);
        }
        slice(&held, row)
    }
}

/// The `row`-th row of a frame, as a frame one row high.
fn slice(frame: &Frame, row: usize) -> Result<Frame, RenderStatus> {
    let description = *frame.description();
    let geometry = description.geometry();
    if row >= geometry.height() as usize {
        return Err(RenderStatus::OutsideDomain);
    }
    let plane = frame.plane(0)?;
    let stride = plane.stride();
    let one = FrameDescription::new(
        Geometry::new(geometry.width(), 1).map_err(RenderStatus::Media)?,
        description.format(),
        description.colour(),
        description.siting(),
        description.alpha(),
        description.pixel_aspect(),
    )
    .map_err(RenderStatus::Media)?;
    Frame::new(
        one,
        std::vec![
            Plane::new(
                plane.samples()[row * stride..(row + 1) * stride].to_vec(),
                stride,
            )
            .map_err(RenderStatus::Media)?
        ],
    )
    .map_err(RenderStatus::Media)
}

/// The linear part of an exact rotation: the three-four-five triangle, whose
/// cosine and sine are both rational and whose determinant is one.
///
/// A scale, a translation, a mirror and a horizontal shear all take
/// horizontals to horizontals, so a row of each has one band of source rows
/// for a preimage. A rotation takes a horizontal to a *slope*, so its row is
/// drawn in strips -- as many as the width and the slope between them need.
fn turned() -> [Rational; 4] {
    let cosine = Rational::new(4, 5).expect("a cosine");
    let sine = Rational::new(3, 5).expect("a sine");
    [cosine, sine.checked_neg().expect("a sine"), sine, cosine]
}

/// A description with no rows: one chroma row of a 4:2:0 frame serves two
/// luma rows, so a luma row is not independent of its neighbour.
///
/// The only thing left that `row_local` refuses. Every invertible transform
/// scans now — a scale in one strip and a turn in several — so this is what
/// the tests about the *walk* have to be built on.
fn planar() -> FrameDescription {
    FrameDescription::new(
        Geometry::new(WIDE + 1, TALL + 1).expect("a geometry"),
        PixelFormat::Yuv420p8,
        ColourDescription::new(
            media_editor_media::colour::Primaries::Bt709,
            media_editor_media::colour::TransferFunction::Srgb,
            media_editor_media::colour::MatrixCoefficients::Bt709,
            media_editor_media::colour::Range::Full,
        ),
        Some(media_editor_media::colour::ChromaSiting::Centre),
        None,
        Rational::ONE,
    )
    .expect("a description")
}

fn pool() -> FramePool {
    FramePool::new(64, 1 << 20)
}

/// Evaluate a node whole and a row at a time, and require them to agree.
fn agrees(graph: &Graph, id: NodeId, library: &mut Pictures) {
    let whole = graph
        .evaluate(id, &mut pool(), library)
        .expect("a whole frame");
    for row in 0..whole.description().geometry().height() as usize {
        let one = graph.row(id, row, library).expect("a row");
        assert_eq!(
            one,
            slice(&whole, row).expect("a slice"),
            "row {row} of the two evaluators disagrees"
        );
    }
    // And one past the end is refused rather than wrapping.
    assert_eq!(
        graph
            .row(
                id,
                whole.description().geometry().height() as usize,
                library
            )
            .err(),
        Some(RenderStatus::OutsideDomain)
    );
}

#[test]
fn a_blank_and_an_empty_agree_row_for_row() {
    let mut graph = Graph::new();
    let blank = graph
        .add(Node::Blank {
            description: described(PixelFormat::Rgb8),
        })
        .expect("a node");
    let empty = graph
        .add(Node::Empty {
            description: described(PixelFormat::Rgba8),
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec::Vec::new());
    agrees(&graph, blank, &mut library);
    agrees(&graph, empty, &mut library);
}

#[test]
fn a_source_agrees_row_for_row() {
    let description = described(PixelFormat::Rgb8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 3))]);
    agrees(&graph, source, &mut library);
    // And the row form asked for rows rather than for frames: one whole frame
    // for the whole-frame evaluation, and one row per row.
    assert_eq!(library.frames_asked, 1);
    assert_eq!(
        library.rows_asked,
        usize::try_from(TALL).expect("a height"),
        "the row past the end was asked of the library rather than refused first"
    );
}

#[test]
fn every_row_local_node_agrees_row_for_row() {
    // The property, over a chain that uses every node kind the row form
    // covers: a source, faded, associated, masked, wiped, and composited over
    // another source.
    let description = described(PixelFormat::Rgba8);
    let under = Digest::of(b"under");
    let over = Digest::of(b"over");
    let mut graph = Graph::new();
    let bottom = graph
        .add(Node::Source {
            media: under,
            tick: 0,
            description,
        })
        .expect("a node");
    let top = graph
        .add(Node::Source {
            media: over,
            tick: 0,
            description,
        })
        .expect("a node");
    let faded = graph
        .add(Node::Fade {
            input: top,
            opacity: Rational::new(3, 5).expect("an opacity"),
        })
        .expect("a node");
    let masked = graph
        .add(Node::Mask {
            input: faded,
            corners: std::vec![
                (
                    Rational::new(1, 8).expect("a corner"),
                    Rational::new(1, 8).expect("a corner")
                ),
                (
                    Rational::new(7, 8).expect("a corner"),
                    Rational::new(1, 6).expect("a corner")
                ),
                (
                    Rational::new(3, 4).expect("a corner"),
                    Rational::new(7, 8).expect("a corner")
                ),
                (
                    Rational::new(1, 6).expect("a corner"),
                    Rational::new(3, 4).expect("a corner")
                ),
            ],
            inverted: false,
        })
        .expect("a node");
    let wiped = graph
        .add(Node::Wipe {
            input: masked,
            across: Rational::new(2, 3).expect("a direction"),
            down: Rational::new(1, 3).expect("a direction"),
            fraction: Rational::new(2, 5).expect("a fraction"),
            softness: Rational::new(1, 4).expect("a softness"),
        })
        .expect("a node");
    let composited = graph
        .add(Node::Over {
            layers: [bottom, wiped],
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![
        (under, picture(description, 7)),
        (over, picture(description, 61)),
    ]);
    agrees(&graph, composited, &mut library);
    // And every node along the way, not only the last.
    for id in [bottom, top, faded, masked, wiped] {
        agrees(&graph, id, &mut library);
    }
}

#[test]
fn an_inverted_mask_and_a_hard_wipe_agree_too() {
    // The two branches the coverage code takes that the chain above does not:
    // an inverted mask, and a wipe with no softness at all.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let inverted = graph
        .add(Node::Mask {
            input: source,
            corners: std::vec![
                (
                    Rational::new(1, 4).expect("a corner"),
                    Rational::new(1, 4).expect("a corner")
                ),
                (
                    Rational::new(3, 4).expect("a corner"),
                    Rational::new(1, 4).expect("a corner")
                ),
                (
                    Rational::new(3, 4).expect("a corner"),
                    Rational::new(3, 4).expect("a corner")
                ),
            ],
            inverted: true,
        })
        .expect("a node");
    let hard = graph
        .add(Node::Wipe {
            input: inverted,
            across: Rational::ONE,
            down: Rational::ZERO,
            fraction: Rational::new(1, 2).expect("a fraction"),
            softness: Rational::ZERO,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 11))]);
    agrees(&graph, hard, &mut library);
}

#[test]
fn a_conversion_and_an_association_agree_row_for_row() {
    let straight = FrameDescription::square(
        Geometry::new(WIDE, TALL).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description");
    let premultiplied = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description: premultiplied,
        })
        .expect("a node");
    let associated = graph
        .add(Node::Associate {
            input: source,
            target: straight,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(premultiplied, 23))]);
    agrees(&graph, associated, &mut library);
}

#[test]
fn a_turn_is_scanned_in_strips_and_is_the_render_of_it() {
    // The refusal this used to assert was wrong, and finding that out is the
    // milestone. A turn's row has a *slope* for a preimage, and a slope of m
    // across w columns crosses about m·w rows -- so whether it fits a band is
    // a question about the **width**, not about the map. Narrow the strip and
    // it always fits.
    //
    // What that buys is not speed: a turn re-reads its source, because
    // neighbouring strips have overlapping bands. What it buys is a bound on
    // memory, which on Phipia is the difference between running and not.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    for bilinear in [false, true] {
        let mut graph = Graph::new();
        let source = graph
            .add(Node::Source {
                media,
                tick: 0,
                description,
            })
            .expect("a node");
        let moved = graph
            .add(Node::Transform {
                input: source,
                linear: turned(),
                offset: (
                    Rational::new(1, 10).expect("an offset"),
                    Rational::new(1, 10).expect("an offset"),
                ),
                anchor: (
                    Rational::new(1, 2).expect("an anchor"),
                    Rational::new(1, 2).expect("an anchor"),
                ),
                bilinear,
            })
            .expect("a node");
        let mut library = Pictures::new(std::vec![(media, picture(description, 5))]);
        assert_eq!(graph.row_local(moved), Ok(()), "a turn says it scans");
        agrees(&graph, moved, &mut library);
        // And through something above it, because reaching a fade means
        // walking the turn underneath.
        let faded = graph
            .add(Node::Fade {
                input: moved,
                opacity: Rational::new(1, 2).expect("an opacity"),
            })
            .expect("a node");
        agrees(&graph, faded, &mut library);
    }
}

#[test]
fn a_turn_wide_enough_to_need_them_is_drawn_in_more_than_one_strip() {
    // The fixture above is seven pixels wide, so its turn fits one strip and
    // proves the arithmetic without exercising the slicing. This one is wide
    // enough that it cannot, and the evidence is the *count*: a turn that
    // fetched one band a row would be a turn that had not been sliced, and it
    // would still draw every pixel correctly.
    let wide = FrameDescription::square(
        Geometry::new(160, 160).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    let media = Digest::of(b"a wide shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description: wide,
        })
        .expect("a node");
    let moved = graph
        .add(Node::Transform {
            input: source,
            linear: turned(),
            offset: (Rational::ZERO, Rational::ZERO),
            anchor: (
                Rational::new(1, 2).expect("an anchor"),
                Rational::new(1, 2).expect("an anchor"),
            ),
            bilinear: false,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(wide, 9))]);
    // The three-four-five turn's inverse moves v by 3/5 a column, so a hundred
    // and sixty columns span 96 rows and one strip of them would be a band of
    // ninety-seven -- past sixty-four. It has to be cut, and it is.
    graph.row(moved, 80, &mut library).expect("a row");
    assert!(
        library.rows_asked > 64,
        "a row of a turn read {} source rows, which is one band",
        library.rows_asked
    );
    // And the pixels are still the pixels: the strips are a range of columns,
    // not a different arithmetic.
    let whole = graph
        .evaluate(moved, &mut pool(), &mut library)
        .expect("a whole frame");
    assert_eq!(
        graph.row(moved, 80, &mut library).expect("a row"),
        slice(&whole, 80).expect("a slice")
    );
}

#[test]
fn a_band_of_a_turn_reads_its_source_once_instead_of_once_a_row() {
    // The whole of the milestone, counted rather than claimed.
    //
    // Drawn a row at a time, a turn re-reads: neighbouring strips have
    // overlapping bands, so the same source rows are fetched again for every
    // destination row. Drawn a band at a time, each tile's band is fetched
    // **once** and every row of the tile is drawn from it.
    //
    // So the same sixteen rows, produced two ways, and the number of source
    // rows fetched is the difference. Both produce identical pixels, which is
    // why counting is the only way to see it.
    //
    // The numbers below are worked out from the map, not read back out of the
    // code. The three-four-five turn about the centre of a hundred-and-sixty
    // square gives v(x, y) = -3(x - 80)/5 + 4(y - 80)/5 + 80, so a tile of w
    // columns and h rows spans 3w/5 + 4h/5 in v. The width halves from 160
    // until the band fits in sixty-four:
    //
    //   together, h = 16 : w = 160 -> 96 + 12.8 = 108.8, too tall
    //                      w =  80 -> 48 + 12.8 =  60.8, which fits
    //     columns   0..80 : v spans 73.6 .. 134.4 -> rows 73..=134 = 62
    //     columns 80..160 : v spans 25.6 ..  86.4 -> rows 25..= 86 = 62
    //                                                       total = 124
    //
    //   apart, h = 1     : w = 160 -> 96 +  0.8 =  96.8, too tall
    //                      w =  80 -> 48 +  0.8 =  48.8, which fits
    //     two strips of about fifty rows is about a hundred a row, and
    //     sixteen rows is about 1,600 -- 1,594 once the rows near the top and
    //     bottom of the picture clamp against its edges.
    //
    // A hundred and twenty-four against one thousand five hundred and
    // ninety-four.
    let wide = FrameDescription::square(
        Geometry::new(160, 160).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    let media = Digest::of(b"a wide shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description: wide,
        })
        .expect("a node");
    let moved = graph
        .add(Node::Transform {
            input: source,
            linear: turned(),
            offset: (Rational::ZERO, Rational::ZERO),
            anchor: (
                Rational::new(1, 2).expect("an anchor"),
                Rational::new(1, 2).expect("an anchor"),
            ),
            bilinear: false,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(wide, 9))]);

    let mut apart = std::vec::Vec::new();
    for row in 72..88 {
        apart.push(graph.row(moved, row, &mut library).expect("a row"));
    }
    let one_at_a_time = library.rows_asked;

    library.rows_asked = 0;
    let together = graph.rows(moved, 72, 88, &mut library).expect("a band");
    let all_at_once = library.rows_asked;

    // The rows are the same rows: a tile is a rectangle of columns and rows,
    // not a different arithmetic.
    let packed = together.to_packed().expect("bytes");
    let stride = packed.len() / 16;
    for (index, row) in apart.iter().enumerate() {
        assert_eq!(
            row.to_packed().expect("bytes"),
            packed[index * stride..(index + 1) * stride],
            "row {index} of the band is not the row drawn alone"
        );
    }
    // And the band read far fewer of them. Sixteen rows drawn one at a time
    // fetch about sixteen times what one row fetches; drawn together they
    // fetch about what one row fetches, because the band is the same band.
    assert_eq!(one_at_a_time, 1_594, "sixteen rows apart");
    assert_eq!(all_at_once, 124, "and together");
    assert!(
        all_at_once * 7 < one_at_a_time,
        "sixteen rows read {all_at_once} source rows together against \
         {one_at_a_time} apart, which is not a saving worth having"
    );
}

#[test]
fn a_band_of_a_flat_map_costs_no_more_than_its_rows_and_usually_less() {
    // Tiles were built for turns, and it turns out they help everything, for
    // a reason worth writing down: consecutive destination rows have
    // *overlapping* bands under almost any map, and a tile fetches the union
    // once instead of each row's share separately.
    //
    // This is a bilinear move down by a fifth of a five-row frame, which is
    // one pixel, so the inverse is v = y - 1. Bilinear takes the sample above
    // each centre and the one below, and the centre of row y sits at
    // v = y - 1/2, so the sample above it is floor(y - 1):
    //
    //   row 0 : floor(-1) = -1, rows -1 and 0, clamped to the picture = 1
    //   row 1 : floor( 0) =  0, rows  0 and 1                         = 2
    //   row 2 : floor( 1) =  1, rows  1 and 2                         = 2
    //                                                        apart    = 5
    //
    // Together, the tile's corners are rows 0 and 2, whose samples are -1 and
    // 1, so the band is [-1, 2] and clamps to rows 0, 1 and 2 -- **three**.
    // Two rows saved out of five, on a map with no slope at all.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let moved = graph
        .add(Node::Transform {
            input: source,
            linear: [Rational::ONE, Rational::ZERO, Rational::ZERO, Rational::ONE],
            offset: (Rational::ZERO, Rational::new(1, 5).expect("a move")),
            anchor: (
                Rational::new(1, 2).expect("an anchor"),
                Rational::new(1, 2).expect("an anchor"),
            ),
            bilinear: true,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 5))]);
    library.rows_asked = 0;
    graph.rows(moved, 0, 3, &mut library).expect("a band");
    let together = library.rows_asked;
    library.rows_asked = 0;
    for row in 0..3 {
        graph.row(moved, row, &mut library).expect("a row");
    }
    assert_eq!(
        together, 3,
        "the band is the union of the three rows' bands"
    );
    assert_eq!(library.rows_asked, 5, "and apart they are one, two and two");
}

#[test]
fn a_band_and_its_rows_are_the_same_picture_for_every_node() {
    // `Graph::rows` is the band form of `Graph::row`, and for everything but a
    // transform it *is* the rows stacked. This is that, held for one of each
    // kind, so a future arm that made a band differently would be caught here
    // rather than by somebody comparing two exports.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let faded = graph
        .add(Node::Fade {
            input: source,
            opacity: Rational::new(1, 3).expect("an opacity"),
        })
        .expect("a node");
    let blank = graph.add(Node::Blank { description }).expect("a node");
    let over = graph
        .add(Node::Over {
            layers: [blank, faded],
        })
        .expect("a node");
    let pattern = graph
        .add(Node::Pattern {
            pattern: media_editor_media::TestPattern::Checkerboard { square: 2 },
            description,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 11))]);
    for id in [source, faded, blank, over, pattern] {
        let band = graph.rows(id, 1, 4, &mut library).expect("a band");
        let packed = band.to_packed().expect("bytes");
        let stride = packed.len() / 3;
        for (index, row) in (1..4).enumerate() {
            assert_eq!(
                graph
                    .row(id, row, &mut library)
                    .expect("a row")
                    .to_packed()
                    .expect("bytes"),
                packed[index * stride..(index + 1) * stride],
                "row {row} of a band"
            );
        }
    }
    // And an empty or backwards range is refused rather than answered.
    assert_eq!(
        graph.rows(source, 2, 2, &mut library).err(),
        Some(RenderStatus::OutsideDomain)
    );
    assert_eq!(
        graph.rows(source, 3, 1, &mut library).err(),
        Some(RenderStatus::OutsideDomain)
    );
}

#[test]
fn every_generator_agrees_row_for_row() {
    // These three refused with `NoRowForm` until this milestone, and the
    // refusal was always a statement about the build rather than about the
    // operation: a test pattern's colour is a function of position, and so is
    // a glyph's coverage. So the property holds for them exactly as it does
    // for a source.
    //
    // What each is placed against is the whole picture, which is the part that
    // could have been got wrong: bars are eighths of the *width*, a
    // checkerboard is counted from the *top*, and an em is a fraction of the
    // *height*. Drawn into a frame one row high, each would be a different
    // picture rather than a row of this one.
    let description = described(PixelFormat::Rgb8);
    let mut graph = Graph::new();
    let mut library = Pictures::new(std::vec::Vec::new());

    for pattern in [
        media_editor_media::TestPattern::Bars,
        media_editor_media::TestPattern::Ramp,
        media_editor_media::TestPattern::Offline,
        media_editor_media::TestPattern::Checkerboard { square: 2 },
        media_editor_media::TestPattern::Flat { value: 77 },
    ] {
        let id = graph
            .add(Node::Pattern {
                pattern,
                description,
            })
            .expect("a node");
        agrees(&graph, id, &mut library);
    }

    // A card, and a legend over a pattern -- which is what an offline slate
    // is. Both draw letters over what is behind them, so both need a
    // description that can carry coverage -- and one **large enough for the
    // letters to fit**. Seven by five is what the rest of this file uses and
    // it is far too small for a caption, so `lettered` hands the picture back
    // untouched and the type is never drawn at all. A control found that: the
    // legend's layout could be got wrong and nothing noticed. A hundred and
    // twenty by sixty is what the legend's own tests use, and it is the
    // smallest this file has that the words fit in.
    let lettered = FrameDescription::square(
        Geometry::new(120, 60).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    let card = graph
        .add(Node::Type {
            description: lettered,
            lines: std::vec!["MEDIAEDTO".into(), "take two".into()],
            size: Rational::new(1, 4).expect("a size"),
            across: Rational::new(1, 2).expect("a place"),
            down: Rational::new(1, 2).expect("a place"),
            alignment: media_editor_render::font::Alignment::Centre,
            ink: [Rational::ONE, Rational::ONE, Rational::ONE],
        })
        .expect("a node");
    agrees(&graph, card, &mut library);

    let slate = graph
        .add(Node::Pattern {
            pattern: media_editor_media::TestPattern::Offline,
            description: lettered,
        })
        .expect("a node");
    let legend = graph
        .add(Node::Legend {
            input: slate,
            text: "media offline".into(),
            brief: "0000".into(),
        })
        .expect("a node");
    // The letters really are drawn, which is what makes the rows worth
    // comparing: a legend too small for its caption is the bare pattern.
    let whole = graph
        .evaluate(legend, &mut pool(), &mut library)
        .expect("a frame");
    let bare = graph
        .evaluate(slate, &mut pool(), &mut library)
        .expect("a frame");
    assert_ne!(whole, bare, "the legend drew no letters");
    agrees(&graph, legend, &mut library);
    // And every one of them says it can be scanned before it is.
    assert_eq!(graph.row_local(legend), Ok(()));
    assert_eq!(graph.row_local(card), Ok(()));
}

#[test]
fn a_subsampled_format_has_no_row() {
    // A sharper refusal than the transform's, and a truer one: one chroma row
    // of a 4:2:0 frame serves two luma rows, so a luma row is not independent
    // of its neighbour. That is a fact about the format rather than about this
    // build, so it is `NotRowLocal`.
    let planar = FrameDescription::new(
        Geometry::new(8, 4).expect("a geometry"),
        PixelFormat::Yuv420p8,
        ColourDescription::new(
            media_editor_media::colour::Primaries::Bt709,
            media_editor_media::colour::TransferFunction::Srgb,
            media_editor_media::colour::MatrixCoefficients::Bt709,
            media_editor_media::colour::Range::Full,
        ),
        Some(media_editor_media::colour::ChromaSiting::Centre),
        None,
        Rational::ONE,
    )
    .expect("a description");
    let mut graph = Graph::new();
    let blank = graph
        .add(Node::Blank {
            description: planar,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec::Vec::new());
    assert_eq!(
        graph.row(blank, 0, &mut library),
        Err(RenderStatus::NotRowLocal)
    );
}

#[test]
fn a_library_without_a_row_form_says_so_rather_than_loading_a_frame() {
    // The default refuses instead of fetching the whole frame and slicing it,
    // and that is the decision worth having a test for: a default that quietly
    // loaded eight megabytes to hand back six thousand bytes would make a
    // row-at-a-time renderer look like it was working while doing exactly the
    // thing it exists to avoid.
    struct WholeFramesOnly(Frame);

    impl Library for WholeFramesOnly {
        fn frame(
            &mut self,
            _media: Digest,
            _tick: i64,
            _description: FrameDescription,
        ) -> Result<Frame, RenderStatus> {
            Ok(self.0.clone())
        }

        fn look(&mut self, _look: Digest) -> Result<Look, RenderStatus> {
            Err(RenderStatus::LookAbsent)
        }
    }

    let description = described(PixelFormat::Rgb8);
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media: Digest::of(b"a shot"),
            tick: 0,
            description,
        })
        .expect("a node");
    let mut library = WholeFramesOnly(picture(description, 1));
    assert_eq!(
        graph.row(source, 0, &mut library),
        Err(RenderStatus::NoRowForm)
    );
    // And the whole-frame path still works through the same library.
    graph
        .evaluate(source, &mut pool(), &mut library)
        .expect("a whole frame");
}

#[test]
fn a_source_that_answers_the_wrong_question_is_refused_in_both_forms() {
    let asked = described(PixelFormat::Rgb8);
    let other = FrameDescription::square(
        Geometry::new(WIDE, TALL).expect("a geometry"),
        PixelFormat::Gray8,
        ColourDescription::srgb_full(),
        None,
        None,
    )
    .expect("a description");
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description: asked,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(other, 9))]);
    assert_eq!(
        graph.row(source, 0, &mut library),
        Err(RenderStatus::SourceDescriptionMismatch)
    );
    assert_eq!(
        graph.evaluate(source, &mut pool(), &mut library),
        Err(RenderStatus::SourceDescriptionMismatch)
    );
}

#[test]
fn a_row_that_is_not_the_row_that_was_asked_for_is_refused() {
    // The library's own check passes -- it holds the right picture and was
    // asked for the right description -- and it hands back a row described
    // some other way. Only the graph's check catches that, which is why it is
    // there as well as in the library.
    let description = described(PixelFormat::Rgb8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 3))]);
    library.lies_in_rows = true;
    assert_eq!(
        graph.row(source, 0, &mut library),
        Err(RenderStatus::SourceDescriptionMismatch)
    );
    // And with the library honest it is the row it should be.
    library.lies_in_rows = false;
    agrees(&graph, source, &mut library);
}

#[test]
fn a_mask_over_a_turn_is_placed_against_the_whole_frame() {
    // A mask is placed against the whole frame, so it has to know how tall
    // that frame is -- and the walk that finds out goes straight through a
    // transform, because a transform resamples into its source's own
    // description. That was true when a turn refused and it is what makes a
    // masked turn scan now that one does not.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let moved = graph
        .add(Node::Transform {
            input: source,
            linear: turned(),
            offset: (Rational::ZERO, Rational::ZERO),
            anchor: (
                Rational::new(1, 2).expect("an anchor"),
                Rational::new(1, 2).expect("an anchor"),
            ),
            bilinear: false,
        })
        .expect("a node");
    let masked = graph
        .add(Node::Mask {
            input: moved,
            corners: std::vec![
                (Rational::ZERO, Rational::ZERO),
                (Rational::ONE, Rational::ZERO),
                (Rational::ONE, Rational::ONE),
            ],
            inverted: false,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 2))]);
    assert_eq!(graph.row_local(masked), Ok(()));
    agrees(&graph, masked, &mut library);
    // The geometry the mask is placed against is the *source's*, not the
    // turned picture's bounding box: a transform changes what the picture
    // looks like rather than how large it is.
    let (width, height) = (
        masked_extent(&graph, masked).0,
        masked_extent(&graph, masked).1,
    );
    assert_eq!((width, height), (WIDE, TALL));
}

/// The geometry a node produces, read back out of the graph the way
/// `extent_of` does.
///
/// Written here rather than exposed, because a test that wanted to check the
/// private walk should say so rather than widen the crate's surface for it.
fn masked_extent(graph: &Graph, id: NodeId) -> (u32, u32) {
    let node = graph.node(id).expect("a node");
    match node {
        Node::Mask { input, .. } | Node::Wipe { input, .. } | Node::Fade { input, .. } => {
            masked_extent(graph, *input)
        }
        Node::Transform { input, .. } => masked_extent(graph, *input),
        Node::Source { description, .. } | Node::Blank { description } => (
            description.geometry().width(),
            description.geometry().height(),
        ),
        other => panic!("no geometry for {other:?}"),
    }
}

#[test]
fn asking_first_gives_the_same_answer_as_asking_at_the_row() {
    // `row_local` exists so a caller can find out *before* it commits, and the
    // only thing that makes it worth having is that it agrees with `row`. Two
    // answers to one question would be worse than no question: a caller that
    // asked, was told yes, and was refused at row nought would have paid for
    // the question and got nothing.
    //
    // So every case is checked twice here, against the same graph, and the
    // statuses have to match — not merely both be errors.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let mut library = Pictures::new(std::vec![(media, picture(description, 7))]);
    // A source scans, and so does a chain of decorations over it.
    assert_eq!(graph.row_local(source), Ok(()));
    let faded = graph
        .add(Node::Fade {
            input: source,
            opacity: Rational::new(1, 3).expect("an opacity"),
        })
        .expect("a node");
    let masked = graph
        .add(Node::Mask {
            input: faded,
            corners: std::vec![
                (Rational::ZERO, Rational::ZERO),
                (Rational::ONE, Rational::ZERO),
                (Rational::ONE, Rational::ONE),
            ],
            inverted: false,
        })
        .expect("a node");
    assert_eq!(graph.row_local(masked), Ok(()));
    assert!(graph.row(masked, 0, &mut library).is_ok());
    // A turn scans too, in strips.
    let moved = graph
        .add(Node::Transform {
            input: masked,
            linear: turned(),
            offset: (Rational::new(1, 10).expect("an offset"), Rational::ZERO),
            anchor: (
                Rational::new(1, 2).expect("an anchor"),
                Rational::new(1, 2).expect("an anchor"),
            ),
            bilinear: false,
        })
        .expect("a node");
    assert_eq!(graph.row_local(moved), Ok(()));
    assert!(graph.row(moved, 0, &mut library).is_ok());
    // A subsampled format does not, and neither does anything reached through
    // one. It is the last thing `row_local` refuses, and unlike a turn it is
    // a fact about the *format*: one chroma row serves two luma rows.
    let subsampled = graph
        .add(Node::Blank {
            description: planar(),
        })
        .expect("a node");
    let over_it = graph
        .add(Node::Fade {
            input: subsampled,
            opacity: Rational::ONE,
        })
        .expect("a node");
    assert_eq!(graph.row_local(subsampled), Err(RenderStatus::NotRowLocal));
    assert_eq!(
        graph.row_local(over_it),
        Err(RenderStatus::NotRowLocal),
        "the walk stopped at the fade instead of going through it"
    );
    assert_eq!(
        graph.row(over_it, 0, &mut library),
        Err(RenderStatus::NotRowLocal)
    );

    // A generator scans now, and says so before it is asked -- which is the
    // same agreement, in the direction that used to be a refusal.
    let pattern = graph
        .add(Node::Pattern {
            pattern: media_editor_media::TestPattern::Checkerboard { square: 2 },
            description,
        })
        .expect("a node");
    assert_eq!(graph.row_local(pattern), Ok(()));
    assert!(graph.row(pattern, 0, &mut library).is_ok());
}

#[test]
fn asking_first_looks_at_both_layers_of_a_composite() {
    // Both, and in a graph where only one of them is the problem — twice, once
    // each way round. A walk that checked the top layer only would pass the
    // first of these and fail the second, and a stack is built bottom first,
    // so the layer a planner puts the awkward one on is whichever one the
    // cutter happened to put there.
    let description = described(PixelFormat::Rgba8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description,
        })
        .expect("a node");
    let _ = source;
    let moved = graph
        .add(Node::Blank {
            description: planar(),
        })
        .expect("a node");
    let plain = graph.add(Node::Blank { description }).expect("a node");
    let framing_on_top = graph
        .add(Node::Over {
            layers: [plain, moved],
        })
        .expect("a node");
    let framing_beneath = graph
        .add(Node::Over {
            layers: [moved, plain],
        })
        .expect("a node");
    assert_eq!(
        graph.row_local(framing_on_top),
        Err(RenderStatus::NotRowLocal)
    );
    assert_eq!(
        graph.row_local(framing_beneath),
        Err(RenderStatus::NotRowLocal),
        "the lower layer was not walked"
    );
    assert_eq!(graph.row_local(plain), Ok(()));
}

#[test]
fn asking_first_refuses_a_description_that_has_no_rows() {
    // The subsampling refusal, arriving at the question rather than at the
    // first row. Worth its own case because the description is carried by the
    // node rather than reached through one: a walk that only looked at node
    // *kinds* would say yes to every one of these.
    let planar = FrameDescription::new(
        Geometry::new(8, 4).expect("a geometry"),
        PixelFormat::Yuv420p8,
        ColourDescription::new(
            media_editor_media::colour::Primaries::Bt709,
            media_editor_media::colour::TransferFunction::Srgb,
            media_editor_media::colour::MatrixCoefficients::Bt709,
            media_editor_media::colour::Range::Full,
        ),
        Some(media_editor_media::colour::ChromaSiting::Centre),
        None,
        Rational::ONE,
    )
    .expect("a description");
    let straight = described(PixelFormat::Rgb8);
    let media = Digest::of(b"a shot");
    let mut graph = Graph::new();
    for node in [
        Node::Blank {
            description: planar,
        },
        Node::Empty {
            description: planar,
        },
        Node::Source {
            media,
            tick: 0,
            description: planar,
        },
    ] {
        let id = graph.add(node).expect("a node");
        assert_eq!(graph.row_local(id), Err(RenderStatus::NotRowLocal));
    }
    // And a conversion is refused for the target it converts *into*, not only
    // for what it reads: a chain that ends in 4:2:0 cannot be scanned however
    // row-local its source is.
    let source = graph
        .add(Node::Source {
            media,
            tick: 0,
            description: straight,
        })
        .expect("a node");
    assert_eq!(graph.row_local(source), Ok(()));
    let converted = graph
        .add(Node::Convert {
            input: source,
            target: planar,
        })
        .expect("a node");
    assert_eq!(graph.row_local(converted), Err(RenderStatus::NotRowLocal));
}
