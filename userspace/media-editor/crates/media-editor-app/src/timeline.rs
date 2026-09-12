// SPDX-License-Identifier: GPL-3.0-only
//! Turning a stack of layers into a picture.
//!
//! [`media_editor_model::Sequence::stack_at`] answers what is on each track at an
//! instant. This answers what that *looks* like, and the two are deliberately
//! separate: the first is a fact about the project, the second is a policy
//! about rendering, and the policy lives here because the application is the
//! part that owns decisions.
//!
//! Two of those decisions are worth naming.
//!
//! **The programme is opaque.** A sequence with nothing at an instant shows
//! black, not a hole — a viewer displays black leader, an export writes black
//! frames, and neither shows whatever was behind the window. So the stack is
//! composited onto an opaque black base, and the result is opaque whatever the
//! layers were.
//!
//! **Layers composite bottom first**, in the order the stack comes back, each
//! one `over` what is beneath it. That is [`media_editor_render::over`], which
//! means it happens in linear light and only on premultiplied values — so a
//! title with a soft edge on V2 lands on V1 without a fringe, and it lands
//! there identically on every machine.
//!
//! A dissolve needs no second operator. The model reports both sides of the
//! cut, the outgoing one at full opacity and the incoming one at the fraction
//! of the way through — so `over` computes `in x t + out x (1 - t)`, which is
//! what a cross-fade is. Opacity scales a layer's coverage, and scaling a
//! premultiplied frame's coverage means scaling its colour by the same amount,
//! because that is what premultiplied means.
//!
//! Where the frames come from is a [`media_editor_render::Media`], because on
//! Phipia today there is nowhere to read media from (`PHIP-08`). A test
//! provides one that draws flat colours; a real session will provide one that
//! decodes, and neither this module nor the model changes when it does.
//!
//! The render is expressed as a **graph** rather than performed directly, and
//! that is not ceremony. A node's cache key is a digest over its kind, its
//! parameters and its inputs' identities, so the pool answers for anything the
//! render already has — most usefully a `Source`, where the cost is a decode.
//! Scrub back over a cut and nothing is decoded twice; a dissolve that reaches
//! the same frame from both sides fetches it once.
//!
//! The graph also names media by **what it is** rather than by this project's
//! index for it, so the same footage in two sequences shares one cached frame,
//! and a file swapped underneath is a different key rather than a stale hit.

use alloc::vec::Vec;

use media_editor_core::Instant;
use media_editor_media::{AlphaState, Frame, FrameDescription, FramePool, TestPattern};
use media_editor_model::{Lane, Layer, Project, Sequence, SequenceId};

/// How deep this planner will walk a chain of nested sequences.
///
/// The same number `media_editor_model::MAX_NESTING_DEPTH` refuses to build past,
/// restated here because the two crates are siblings and neither may depend on
/// the other -- exactly as the fader's bounds are restated in the model.
/// `the_two_nesting_bounds_agree` asserts they are one number, so the
/// duplication cannot drift without something failing.
pub const MAX_NESTING_DEPTH: usize = 8;
use media_editor_render::{Graph, Library, Node, NodeId};

use crate::SlateStatus;

/// How many characters of a digest a slate shows.
///
/// Eight. Enough to pick one clip out of a bin by eye, short enough to read
/// off a screen at a glance, and the same prefix the conform module already
/// writes into a reel name — so the slate and the edit decision list name the
/// same thing the same way.
const SLATE_DIGEST_CHARACTERS: usize = 8;

/// What a slate says about a clip whose media is not there.
///
/// The hex is written out here rather than taken from the digest's `Display`,
/// because that would want a `String` built by an allocation nobody can refuse
/// — and everything in this program allocates by asking first (R-5.2).
fn missing(
    media: media_editor_core::Digest,
) -> Result<(alloc::string::String, alloc::string::String), SlateStatus> {
    const LABEL: &str = "MEDIA OFFLINE ";
    const HEX: [u8; 16] = *b"0123456789ABCDEF";
    let out_of_memory = || SlateStatus::Render(media_editor_render::RenderStatus::OutOfMemory);
    let mut brief = alloc::string::String::new();
    brief
        .try_reserve(SLATE_DIGEST_CHARACTERS)
        .map_err(|_| out_of_memory())?;
    for byte in media.bytes().iter().take(SLATE_DIGEST_CHARACTERS / 2) {
        brief.push(char::from(HEX[usize::from(byte >> 4)]));
        brief.push(char::from(HEX[usize::from(byte & 0x0F)]));
    }
    let mut text = alloc::string::String::new();
    text.try_reserve(LABEL.len() + brief.len())
        .map_err(|_| out_of_memory())?;
    text.push_str(LABEL);
    text.push_str(&brief);
    Ok((text, brief))
}

/// The node that draws a title card.
///
/// The alignment is translated rather than shared, because the model and the
/// renderer are *siblings* and neither may depend on the other: the model owns
/// which alignment somebody chose, and the renderer owns what the choice
/// means. This one `match` is the price of that, and it is the price the
/// resampler's filter already pays.
fn typeset(title: &media_editor_model::Title, description: FrameDescription) -> Node {
    Node::Type {
        description,
        lines: title.lines().to_vec(),
        size: title.size(),
        across: title.across(),
        down: title.down(),
        alignment: match title.alignment() {
            media_editor_model::Alignment::Left => media_editor_render::font::Alignment::Left,
            media_editor_model::Alignment::Centre => media_editor_render::font::Alignment::Centre,
            media_editor_model::Alignment::Right => media_editor_render::font::Alignment::Right,
        },
        // The ink crosses as three rationals rather than as a type, because
        // both sides already agree on what a fraction of full light is and
        // neither has to learn the other's name for a colour.
        ink: title.ink().channels(),
    }
}

/// What description a layer's source is fetched in.
///
/// A graded layer is fetched **straight**. A look is a non-linear function and
/// on premultiplied samples computes `f(ac)` where `a·f(c)` was wanted; `over`
/// is only correct on premultiplied ones. The two want opposite things, so the
/// frame arrives the way the look needs it and is associated afterwards —
/// which loses nothing for a recording, since it was never premultiplied to
/// begin with.
///
/// A **nest** is the exception, and it costs something. A nested sequence is
/// composited before it is used, so it arrives premultiplied whatever the
/// layer wants; grading one therefore needs a real unpremultiply, which is
/// exact at full coverage and lossy where the nest is partly transparent.
/// [`media_editor_render::Node::Associate`] is the name of that step, and the
/// planner asks for it explicitly rather than letting the look do it quietly —
/// which is the escape `Look::apply` names when it refuses to do it itself.
fn fetched_for(
    layer: &Layer,
    description: FrameDescription,
) -> Result<FrameDescription, SlateStatus> {
    match layer.grade() {
        None => Ok(description),
        Some(_) => Ok(description
            .with_alpha(AlphaState::Straight)
            .map_err(SlateStatus::Media)?),
    }
}

/// Everything above a layer's source: the grade, the framing, the shape, the
/// fade and the transition, composited onto what is beneath.
///
/// Out of the loop because a nested layer reaches it from a different place —
/// its source is not built in one step but assembled over several passes of
/// the stack below — and one function that both paths call is what stops the
/// two drifting.
fn decorate(
    graph: &mut Graph,
    layer: &Layer,
    source: NodeId,
    top: NodeId,
    description: FrameDescription,
) -> Result<NodeId, SlateStatus> {
    // The grade goes on before the fade, and that order is a decision. A
    // look is a function of colour; fading is a statement about how much
    // of a layer is showing. Grading a half-faded layer would ask the
    // table about a colour that is neither on the screen nor in the clip.
    // So: graded, then shown at whatever opacity it is at.
    let graded = match layer.grade() {
        None => source,
        Some(grade) => {
            // The node is added whatever the strength reads, including
            // nought. Skipping it there would be an optimisation whose
            // absence changes no answer — a look at no strength hands back
            // the frame's own code values exactly — and this project has
            // four entries in its notes about guards no test can hold.
            // The look is fetched straight for the same reason at every
            // strength, so there would be nothing else to skip either.
            let looked = graph.add(Node::Look {
                input: source,
                look: grade.look(),
                strength: grade.strength(),
            })?;
            graph.add(Node::Associate {
                input: looked,
                target: description,
            })?
        }
    };
    // The transform goes between the grade and the mask, and both halves
    // of that are decisions. After the grade, because a look is a function
    // of colour and resampling averages colours -- grading the average is
    // not the average of the grade, and the table was written for the
    // clip's own values rather than for whatever a scale produced. Before
    // the mask, because a mask is in *frame* coordinates: moving a clip
    // moves the picture through a stationary mask, which is what a
    // garbage matte and a split screen both want. A mask that travelled
    // with its clip would be a different feature, and it would have to say
    // so.
    //
    // A transform that moves nothing is skipped entirely rather than
    // resampled by the identity. Exact is a stronger promise than "the
    // arithmetic works out", and it is the promise a project deserves
    // after being opened and saved a hundred times.
    let placed = match layer.transform() {
        Some(transform) if !transform.is_still() => graph.add(Node::Transform {
            input: graded,
            linear: transform.linear(),
            offset: transform.offset(),
            anchor: transform.anchor(),
            bilinear: matches!(
                transform.resampling(),
                media_editor_model::Resampling::Bilinear
            ),
        })?,
        _ => graded,
    };
    // The mask goes on after the grade and before the fade, and both
    // halves of that are decisions. After the grade, because a mask is
    // about *where* and a look is about colour, and grading only the part
    // that survives would ask the table about a picture nobody assembled.
    // Before the fade, because the mask says what this clip *is* and the
    // fade says how much of the track is showing — and a fade applied
    // first would then be masked away in the places the mask cuts, which
    // is the same number by accident and the wrong order in principle.
    let shaped = match layer.mask() {
        None => placed,
        Some(mask) => graph.add(Node::Mask {
            input: placed,
            corners: mask.corners().to_vec(),
            inverted: mask.is_inverted(),
        })?,
    };
    // The track's opacity and the clip's own fade multiply, and they go
    // into one node rather than two. Two would be two rounding points and
    // two cache entries for one picture; one is exact, because the product
    // of two rationals is a rational.
    let showing = layer
        .opacity()
        .checked_mul(layer.fade())
        .map_err(media_editor_model::ModelStatus::from)?;
    let faded = if showing == media_editor_core::Rational::ONE {
        shaped
    } else {
        graph.add(Node::Fade {
            input: shaped,
            opacity: showing,
        })?
    };
    // A wipe after the fade, and that order is the same decision the grade
    // makes in the other direction. The fade is the track's automation,
    // which says how much of this track is showing at all; the wipe is the
    // transition, which says how much of *this clip* has been revealed.
    // Wiping first and then fading would give the same picture here, since
    // both are multiplications by a coverage — but only because neither is
    // a function of colour, and putting the one that is (the grade) in the
    // wrong place is exactly the mistake this ordering exists to avoid.
    let revealed = match layer.wipe() {
        None => faded,
        Some(sweep) => graph.add(Node::Wipe {
            input: faded,
            across: sweep.wipe().across(),
            down: sweep.wipe().down(),
            fraction: sweep.fraction(),
            softness: sweep.wipe().softness(),
        })?,
    };
    Ok(graph.add(Node::Over {
        layers: [top, revealed],
    })?)
}

/// One sequence being planned, and how far through its layers it has got.
///
/// A nested sequence is planned by pushing one of these rather than by calling
/// `plan` again. R-5.5 forbids recursion over user-controlled structure and
/// names nested sequences among the places it means, so the depth here is a
/// `Vec` with a bound rather than a call stack with none.
struct Level {
    layers: alloc::vec::Vec<Layer>,
    next: usize,
    top: NodeId,
    description: FrameDescription,
}

/// Build the graph that renders one instant of a sequence.
///
/// Separate from evaluating it, because they are separate questions and a
/// caller may want only the first: an export that wants to know whether an
/// instant needs decoding at all, or an interface drawing what is on the
/// timeline, has no frames to fetch.
///
/// # Errors
///
/// [`SlateStatus::Model`] if the instant is not in the sequence's timebase or
/// the sequence is not in the project, [`SlateStatus::BaseNotPremultiplied`]
/// if the description is not one this can composite onto, and
/// [`SlateStatus::Render`] if the graph is at capacity.
pub fn plan(
    project: &Project,
    sequence: SequenceId,
    instant: Instant,
    description: FrameDescription,
    library: &mut dyn Library,
) -> Result<(Graph, NodeId), SlateStatus> {
    if description.alpha() != Some(AlphaState::Premultiplied) {
        // Compositing is only correct on premultiplied values, so the base of
        // the stack has to be one. Refusing rather than converting keeps the
        // decision where the caller made it (R-1.3).
        return Err(SlateStatus::BaseNotPremultiplied);
    }
    let mut graph = Graph::new();
    let mut levels: alloc::vec::Vec<Level> = alloc::vec::Vec::new();
    levels
        .try_reserve(1)
        .map_err(|_| SlateStatus::Render(media_editor_render::RenderStatus::OutOfMemory))?;
    levels.push(Level {
        layers: project
            .sequence(sequence)?
            .stack_at(Lane::Picture, instant)?,
        next: 0,
        // The programme is opaque: an empty instant is black leader, not a
        // hole. A viewer shows black and an export writes it, and neither
        // shows whatever was behind the window.
        top: graph.add(Node::Blank { description })?,
        description,
    });

    // What the level that just finished produced, waiting to be used as the
    // source of the layer that asked for it.
    let mut finished: Option<NodeId> = None;
    loop {
        let depth = levels.len();
        let Some(level) = levels.last_mut() else {
            // Unreachable: the loop returns when the last level is popped.
            return Err(SlateStatus::Render(
                media_editor_render::RenderStatus::UnknownNode,
            ));
        };
        if let Some(nested) = finished.take() {
            let layer = level
                .layers
                .get(level.next)
                .ok_or(SlateStatus::NestWithoutLayer)?
                .clone();
            // A nest is composited before it is used, so it arrives
            // premultiplied. If the layer is graded it has to go the other
            // way first, which is a real step and is named as one.
            let source = if layer.grade().is_some() {
                graph.add(Node::Associate {
                    input: nested,
                    target: fetched_for(&layer, level.description)?,
                })?
            } else {
                nested
            };
            level.top = decorate(&mut graph, &layer, source, level.top, level.description)?;
            level.next += 1;
            continue;
        }
        if level.next == level.layers.len() {
            let done = level.top;
            levels.pop();
            if levels.is_empty() {
                return Ok((graph, done));
            }
            finished = Some(done);
            continue;
        }
        let layer = level
            .layers
            .get(level.next)
            .ok_or(SlateStatus::NestWithoutLayer)?
            .clone();
        let asset = project.media().get(layer.media())?;
        if let Some(nested) = asset.nested() {
            if depth >= MAX_NESTING_DEPTH {
                // The model refuses to build a project this deep, so nothing
                // this crate's own doors can hand over reaches here. Written
                // out rather than left to a call stack, because R-5.5 asks for
                // a named maximum and because the day the model's guard is
                // wrong the answer should be a refusal rather than a runaway.
                return Err(SlateStatus::NestingTooDeep);
            }
            let inner = project.sequence(nested)?;
            let at = Instant::new(layer.source(), inner.timebase());
            let layers = inner.stack_at(Lane::Picture, at)?;
            levels
                .try_reserve(1)
                .map_err(|_| SlateStatus::Render(media_editor_render::RenderStatus::OutOfMemory))?;
            levels.push(Level {
                layers,
                next: 0,
                // **Empty**, not blank. A nested sequence is material rather
                // than a programme, and material that is absent is absent --
                // composited onto black it would blank out every track
                // beneath it wherever the nest happened to be empty.
                top: graph.add(Node::Empty { description })?,
                description,
            });
            continue;
        }
        let fetched = fetched_for(&layer, level.description)?;
        let source = direct_source(&mut graph, project, library, asset, &layer, fetched)?;
        level.top = decorate(&mut graph, &layer, source, level.top, level.description)?;
        level.next += 1;
    }
}

/// The node a layer's frames come from, for media that is not a nest.
///
/// Ask whether the bytes are there *before* naming them, rather than finding
/// out while evaluating. A source node's identity covers the media, the tick
/// and the description and not whether the file happened to be reachable — so
/// a node that fell back to a slate during evaluation would put that slate in
/// the cache under the real picture's key and hand it back after the drive
/// came home. The fallback belongs here, where nothing is cached by it.
fn direct_source(
    graph: &mut Graph,
    project: &Project,
    library: &mut dyn Library,
    asset: &media_editor_model::MediaAsset,
    layer: &Layer,
    fetched: FrameDescription,
) -> Result<NodeId, SlateStatus> {
    let _ = project;
    Ok(if let Some(title) = asset.title() {
        // A title is media the program *makes*, so it goes where a source
        // would and everything above it -- the grade, the mask, the
        // transform, the fade -- is the machinery a recording already goes
        // through. Nothing else in this function had to be told.
        //
        // And it is never offline. There is nothing to find, so the
        // question the branch below asks does not arise: asking the
        // library whether it has a title would be asking about a file that
        // does not exist, and a library that answered "no" would put a
        // slate where somebody's card should be.
        graph.add(typeset(title, fetched))?
    } else if library.available(asset.digest()) {
        graph.add(Node::Source {
            media: asset.digest(),
            tick: layer.source(),
            description: fetched,
        })?
    } else {
        let slate = graph.add(Node::Pattern {
            pattern: TestPattern::Offline,
            description: fetched,
        })?;
        // And it says *which* media. "Offline media renders a slate but
        // cannot say which media is missing" stood in the risk section for
        // three milestones, and it is the whole reason the face exists.
        //
        // The digest rather than a file name, because the digest is what
        // the clip actually refers to: a name is a hint that may have
        // moved, and two clips pointing at one digest are pointing at one
        // thing whatever they were called when they were imported.
        let (text, brief) = missing(asset.digest())?;
        graph.add(Node::Legend {
            input: slate,
            text,
            brief,
        })?
    })
}

/// How many caption lines may be on screen at once.
///
/// Four: two speakers of two lines each, which is what broadcast practice
/// allows and rather more than anybody reads. Past it is a refusal rather than
/// a truncation (R-1.4) — a caption silently dropped is worse than a caption
/// refused, because the person it belongs to is the one who cannot tell.
pub const MAX_LINES_ON_SCREEN: usize = 4;

/// How tall a caption line is, as a fraction of the picture.
///
/// A twelfth, which at 1080 lines is ninety pixels — a little over the
/// twenty-four-point type broadcast captions are set in against a 720-line
/// frame, scaled. A fraction rather than a number of pixels, because a card
/// set in pixels is a different size on every deliverable.
const CAPTION_SIZE: (i64, i64) = (1, 12);

/// Where the caption block sits, as fractions of the picture.
///
/// Centred across, and seven eighths down — above the bottom edge rather than
/// on it, because a caption against the frame edge is the first thing a
/// projector's overscan eats.
const CAPTION_PLACE: ((i64, i64), (i64, i64)) = ((1, 2), (7, 8));

/// Add the words on screen at an instant over a plan, and hand back the new
/// root.
///
/// **A separate step from [`plan`], and that is the decision.** A viewer draws
/// captions as an overlay it can switch off; an export may burn them into the
/// picture and then they are there for good. Those are different things done
/// at different times by different callers, so burning is something a caller
/// asks for rather than something a programme has.
///
/// The captions are set as **one card of several lines** rather than as one
/// card each, which is what a captioner does: two speakers at once are two
/// lines of one block, so they cannot overlap each other on screen however
/// they overlap in time.
///
/// ## It takes a reading rather than a transcript
///
/// A projection over a *span* rather than one over an instant, and the
/// difference is four orders of magnitude on a long programme: see
/// [`media_editor_model::caption::Reading`]. It also makes this function pure in
/// the way the rest of the planner is — it asks nothing of the project, so
/// nothing about it can be stale.
///
/// # Errors
///
/// [`SlateStatus::Model`] past [`MAX_LINES_ON_SCREEN`], for an instant the
/// reading was not projected over, and [`SlateStatus::Render`] if the graph is
/// at capacity.
pub fn burn(
    graph: &mut Graph,
    root: NodeId,
    reading: &media_editor_model::caption::Reading,
    instant: Instant,
    description: FrameDescription,
) -> Result<NodeId, SlateStatus> {
    let mut shown = reading.lines(reading.stretch(instant)?)?;
    if shown.is_empty() {
        // Nothing said is nothing drawn, and *not* an empty card composited
        // over the picture: a transparent frame over a programme is a pass
        // over every pixel of it for no change.
        return Ok(root);
    }
    if shown.len() > MAX_LINES_ON_SCREEN {
        return Err(media_editor_model::ModelStatus::TooManyCaptions.into());
    }
    // By voice and then by when they began, so two speakers come out in a
    // fixed order rather than in whatever order the tracks were walked. A
    // caption block that reshuffled itself between frames would flicker.
    shown.sort_by_key(|held| (held.caption.voice(), held.from.ticks(), held.track));
    let mut lines = Vec::new();
    lines
        .try_reserve(shown.len())
        .map_err(|_| media_editor_model::ModelStatus::OutOfMemory)?;
    for held in &shown {
        let mut line = alloc::string::String::new();
        line.try_reserve(held.caption.text().len())
            .map_err(|_| media_editor_model::ModelStatus::OutOfMemory)?;
        line.push_str(held.caption.text());
        lines.push(line);
    }
    let card = graph.add(Node::Type {
        description,
        lines,
        size: media_editor_core::Rational::new(CAPTION_SIZE.0, CAPTION_SIZE.1)
            .map_err(media_editor_model::ModelStatus::Time)?,
        across: media_editor_core::Rational::new(CAPTION_PLACE.0.0, CAPTION_PLACE.0.1)
            .map_err(media_editor_model::ModelStatus::Time)?,
        down: media_editor_core::Rational::new(CAPTION_PLACE.1.0, CAPTION_PLACE.1.1)
            .map_err(media_editor_model::ModelStatus::Time)?,
        alignment: media_editor_render::font::Alignment::Centre,
        // Full light, which the card resolves through the frame's own transfer
        // table rather than writing 255 -- an illegal code value in limited
        // range, which some equipment reads as a sync pattern.
        ink: [
            media_editor_core::Rational::ONE,
            media_editor_core::Rational::ONE,
            media_editor_core::Rational::ONE,
        ],
    })?;
    Ok(graph.add(Node::Over {
        layers: [root, card],
    })?)
}

/// Render one instant of a sequence.
///
/// The picture tracks are composited bottom first onto opaque black in
/// `description`, so the result is always exactly that description and always
/// opaque — a viewer never has to ask what it is looking at.
///
/// # Errors
///
/// As [`plan`], plus [`media_editor_render::RenderStatus::SourceDescriptionMismatch`]
/// if a source hands back a frame that is not what was asked for, and whatever
/// the source itself refuses.
pub fn render(
    project: &Project,
    sequence: SequenceId,
    instant: Instant,
    description: FrameDescription,
    pool: &mut FramePool,
    library: &mut dyn Library,
) -> Result<Frame, SlateStatus> {
    let (graph, root) = plan(project, sequence, instant, description, library)?;
    Ok(graph.evaluate(root, pool, library)?)
}

/// One instant of a sequence, planned once and rendered a row at a time.
///
/// [`render`] builds a graph and evaluates it into a whole frame, which for a
/// 1920×1080 eight-bit RGBA programme is 8,294,400 bytes *per node* against
/// the 76 KiB a Phipia program is mapped. A scan builds the same graph and
/// evaluates it a row at a time: 7,680 bytes a node, and the graph built
/// **once** rather than once a row.
///
/// That last part is the whole reason this is a type rather than a function.
/// Planning walks the sequence's stack, resolves every clip's media, and
/// builds a node for each decoration; doing that a thousand and eighty times
/// to render one frame would trade one kind of waste for a worse one.
///
/// ## It asks before it starts, and what it does not ask
///
/// [`Scan::open`] refuses a *programme* that cannot be scanned — a clip with a
/// framing on it, a title, an offline slate — rather than letting a caller
/// discover it at row four hundred with four hundred rows of work already
/// spent. The refusal says which kind it is: a framing will never scan, and a
/// title is a generator whose row form this build has not written.
///
/// It does **not** ask whether the library can serve rows, and that asymmetry
/// is deliberate. [`Library::row`] refuses by default, so a library says it
/// has no row form by not writing one; a capability flag beside it would be a
/// second statement of the same fact, on a trait with a default, free to
/// disagree with the method. And there is nothing to buy: every source in the
/// graph is touched by row nought, so a library that cannot serve rows refuses
/// on the *first* row and never on the four hundredth. A guard whose absence
/// changes no answer is one no test can hold, and this project has deleted
/// three of those already.
///
/// ## What it is for
///
/// An export that writes a picture a row at a time, and a viewer on a machine
/// that cannot hold a frame. Neither exists yet in this program, and the shape
/// is here first on purpose: the memory bound is a fact about the target
/// rather than about how far the interface has got.
pub struct Scan {
    graph: Graph,
    root: NodeId,
    description: FrameDescription,
}

impl Scan {
    /// Plan an instant, and check it can be scanned at all.
    ///
    /// # Errors
    ///
    /// As [`plan`], plus [`media_editor_render::RenderStatus::NotRowLocal`] for a
    /// programme holding something whose rows are not independent — a clip
    /// with a framing on it, or a subsampled description — and
    /// [`media_editor_render::RenderStatus::NoRowForm`] for one holding a
    /// generator this build cannot produce a row of, which is a title or an
    /// offline slate. Not for a library that has no row form — that one
    /// refuses at [`Scan::row`], for the reason on the type.
    pub fn open(
        project: &Project,
        sequence: SequenceId,
        instant: Instant,
        description: FrameDescription,
        library: &mut dyn Library,
    ) -> Result<Self, SlateStatus> {
        let (graph, root) = plan(project, sequence, instant, description, library)?;
        graph.row_local(root)?;
        Ok(Self {
            graph,
            root,
            description,
        })
    }

    /// How many rows the programme has.
    #[must_use]
    pub fn height(&self) -> usize {
        self.description.geometry().height() as usize
    }

    /// What every row of this scan is described as: the programme's own
    /// description, one row high.
    ///
    /// # Errors
    ///
    /// [`media_editor_render::RenderStatus::NotRowLocal`] for a description that
    /// cannot be cut into rows, which [`Scan::open`] has already refused.
    pub fn row_description(&self) -> Result<FrameDescription, SlateStatus> {
        Ok(media_editor_render::row_description(self.description, 0)?)
    }

    /// One row of the programme, as a frame one row high.
    ///
    /// Byte for byte the same row [`render`] would have put there, which is
    /// the property the render graph's own tests hold and this one inherits.
    ///
    /// # Errors
    ///
    /// [`media_editor_render::RenderStatus::OutsideDomain`] for a row past the
    /// bottom of the picture, [`media_editor_render::RenderStatus::NoRowForm`] if
    /// the library serves whole frames only, and whatever a source refuses.
    pub fn row(&self, row: usize, library: &mut dyn Library) -> Result<Frame, SlateStatus> {
        Ok(self.graph.row(self.root, row, library)?)
    }

    /// A band of rows of the programme, as one frame that many rows tall.
    ///
    /// Byte for byte the rows [`Scan::row`] would have produced one at a time,
    /// and the reason to ask for several is that they can be **cheaper
    /// together**. A framing resamples, and consecutive destination rows read
    /// overlapping bands of the source; a band fetches the union once instead
    /// of each row's share separately. On a turn that is more than a tenfold
    /// difference in rows read.
    ///
    /// How many to ask for is the caller's decision, because the caller is the
    /// one that knows what it can hold: a band of `h` rows is `h` rows of
    /// memory. [`media_editor_render::resample::MAX_TILE_ROWS`] says how tall a
    /// tile *may* be; nothing here says how tall it should be.
    ///
    /// # Errors
    ///
    /// As [`Scan::row`], plus
    /// [`media_editor_render::RenderStatus::OutsideDomain`] for an empty or
    /// backwards range or one past the bottom of the picture.
    pub fn rows(
        &self,
        from: usize,
        to: usize,
        library: &mut dyn Library,
    ) -> Result<Frame, SlateStatus> {
        Ok(self.graph.rows(self.root, from, to, library)?)
    }

    /// The graph this scan renders through.
    ///
    /// Here because a caller that wanted to know what an instant *is* before
    /// rendering it — how many nodes, whether anything is offline — should not
    /// have to plan it a second time to find out.
    #[must_use]
    pub const fn graph(&self) -> &Graph {
        &self.graph
    }
}

/// What a sequence shows at an instant, without rendering it.
///
/// Useful on its own: this is what a timeline draws in its own interface, and
/// what an export uses to decide whether an instant needs decoding at all.
///
/// # Errors
///
/// As [`media_editor_model::Sequence::stack_at`].
pub fn stack(sequence: &Sequence, instant: Instant) -> Result<Vec<Layer>, SlateStatus> {
    Ok(sequence.stack_at(Lane::Picture, instant)?)
}
