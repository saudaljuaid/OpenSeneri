<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Architecture

This is the shape of Media Editor. Six of its ten crates exist and are tested;
the rest are named here so that the shape is decided before the pressure to
compromise it arrives. Each crate below says which it is.

## The shape in one paragraph

A pure core, a thin platform edge, and exactly five seams between them. The
project model, the timeline, the edit algebra, the render graph, the colour
pipeline, and the mixer are pure functions over typed data with no knowledge of
Phipia at all — they compile and are tested on a host with no operating system
in the picture. Everything that touches the machine is behind five named seams
in one crate. The application is the part in the middle that owns nothing but
decisions.

```text
                        media-editor-app
                  (event loop, commands, policy)
                              |
   +---------------+----------+----------+---------------+
   |               |                     |               |
media-editor-ui   media-editor-io      media-editor-render   media-editor-rt
               (every format)                     (entry, allocator)
   |               |                     |               |
   +-------+-------+----------+----------+-------+-------+
           |                  |                  |
   media-editor-model    media-editor-media    media-editor-audio
   (projects, edits)  (frames, colour)   (buffers, mixing)
           |                  |                  |
           +--------+---------+---------+--------+
                              |
                      media-editor-abi
        (the only unsafe: syscalls, C boundary, shims)
                              |
                     media-editor-core
        (time, rationals, identifiers, status, bounded types)
                              |
        ......................|...................... the five seams
                              |
                           Phipia
```

`media-editor-io` sits above every domain crate rather than below them, because
it is the format layer *for* them: it writes projects, reels, summaries and
lookup tables, so it must know what each of those is.

It moved up a layer of its own when the `.cube` reader arrived, because a
lookup table lives in `media-editor-render` and `io` was beside it rather than
above it. The check refused the dependency, which is what it is for: the
architecture has to move before the manifest can, and the alternative — quietly
adding a sideways edge because a file needed one — is how a layering becomes a
drawing again. An earlier version
of this diagram drew it underneath `media-editor-media` while the manifest
already said otherwise, and nothing noticed, because nothing looked. So the
layers below are machine-readable and `tools/layering.py` reads them, reads
every manifest, and refuses any dependency that does not run strictly
downward. The document is the source of truth; a crate that needs to move
moves here first.

```text layers
0  media-editor-core
1  media-editor-abi
2  media-editor-media  media-editor-model  media-editor-audio
3  media-editor-rt  media-editor-render
4  media-editor-io
5  media-editor-app
6  media-editor-image
```

Sideways counts as a violation. Two crates in one layer that depend on each
other are one crate that has not admitted it, and the layer stops meaning
anything.

## Crates

| Crate | Owns | `unsafe` | State |
| --- | --- | --- | --- |
| `media-editor-core` | Rational time, timebases, timecode, identifiers, fixed-point arithmetic and integer transcendentals, status enums | forbidden | **exists**, 79 tests |
| `media-editor-model` | Project, sequences, tracks, clips, media library, edit operations, undo journal, track faders, dissolves, the layer stack at an instant, keyframed parameter curves, opacity and fader automation, per-keyframe editing, a grade named by digest on a clip, dissolves and wipes, hard or soft, as one kind of transition, convex masks on clips, one asset per digest with a location hint, transforms on clips and animations of them, rolling a cut and sliding an item, titles as media the program makes out of words, of several lines and aligned, a fade on a clip, a clip retimed by an exact rational speed or held on one frame, a title's colour named in light, an opacity a clip animates over its own length, a mask a clip animates about its own centroid, a grade that comes on over a clip, an exact rotation and the lane that animates it, the point a framing acts about, and a razor and a merge across every track at once, a lift and the drop that undoes it, markers on a sequence, a sequence nested inside another as media, a speed that changes over a clip, a note that travels with the shot, and **captions anchored in the source** — words a recording carries, projected onto the programme by inverting whatever retime the clip applies, so that no edit has to know they exist, read from wherever the recording keeps them | forbidden | **exists**, and the same projection read once over a whole span and cut where the words change, 416 tests |
| `media-editor-abi` | The five seams, every `extern "C"`, every raw pointer from outside | **permitted** | **exists**, two seams of five |
| `media-editor-rt` | Program entry, the allocator, the panic path, page and mapping management | **permitted** | **exists**, M1 heap |
| `media-editor-media` | Frame and sample types, full colour and format descriptions, content addressing, the frame pool, test patterns including the offline slate | forbidden | **exists**, and a frame that takes a window rather than copying one, 47 tests |
| `media-editor-io` | Every format: the project file, the reel, the waveform summary, the save protocol, CMX 3600 interchange and the conform that turns a sequence into one and back, the `.cube` lookup table, PNG reference captures, bounded byte readers and writers, Phipia's own filesystem contract, 24-bit bitmaps, and the media vault that holds pasted photographs and footage, read through the seam an entry at a time, a reel read a frame — or a row — at a time, and a reel *written* a row at a time — and a frame of sound at a time — onto a sink that can only be extended, and a vault that serves a run of samples and the words that were said as well as a row of pixels, every one of them a window at a time, and a WebVTT caption sidecar written a cue at a time beside it | forbidden | **exists**, 351 tests |
| `media-editor-app` | The event loop, command dispatch, playback policy, session lifetime, rendering a sequence at an instant, mixing its sound over a span | forbidden | **exists** as the slate — which now renders — the timeline renderer with offline media and nested sequences, the scan that renders one instant a row at a time, the export that winds a span of it onto storage — picture and sound both — without ever holding a frame or a span of samples, the mixdown and the reference capture, and the words on screen burned into the picture as one block of lines, and the sidecar that says the same words the reel does, 117 tests |
| `media-editor-image` | The entry point and nothing else; outside the workspace because it cannot build for the host | **permitted** | **exists**, audited |
| `media-editor-render` | The render graph, compositor, colour pipeline, lookup tables, rasterisation | forbidden | **exists** as the graph the timeline renders through, colour pipeline, conversion, compositor, scopes, 3D lookup tables applied to frames and to graph nodes, and the exact-area shape rasteriser a wipe and a mask are both made of, hard edges and soft, masks, resampling in linear light, and a face written from scratch, capitals and lowercase, whose glyphs are disjoint convex pieces, the legend that sets one across a frame, and titles in a colour named as light, 266 tests, and a row form of the graph for a machine that cannot hold a frame — every node of it, generators included, and every invertible transform — a scale in one strip and a turn in several — and a band form above it, which draws a rectangle at a time so a turn reads its source once for many rows rather than once for each |
| `media-editor-audio` | The mixer, DSP chain, loudness, waveform summaries, the real-time contract | forbidden | **exists** as buffers, gain, panning, the mix bus with moving faders, BS.1770 loudness and the waveform overview, 77 tests |
| `media-editor-ui` | Widgets, layout, damage tracking, interface state | forbidden | planned, M4 |

Everything that exists today has no dependencies at all — not one line of
third-party code — which is why [`DEPENDENCY_POLICY.md`](DEPENDENCY_POLICY.md)
has not yet had to refuse anything.

Two crates may contain `unsafe`. Every other crate carries
`#![forbid(unsafe_code)]`, which makes R-3.1.4 a compiler error rather than a
review comment.

`native/` holds the C shims (R-3.3). `perf/` holds C++ leaves when any exist
(R-3.4). Neither directory may be referenced by any crate except
`media-editor-abi`.

## The five seams

Everything the platform provides arrives through exactly five interfaces. Each
is a trait in `media-editor-abi` with two implementations: the Phipia one, and a
deterministic test one used by the host suite.

| Seam | Provides | Phipia capability | State |
| --- | --- | --- | --- |
| `Console` | Write bytes to the kernel transcript | `PHIP-01` | **exists** |
| `Time` | Monotonic nanoseconds | `PHIP-05` | **exists** |
| `Presentation` | Acquire a pixel surface; present a damage rectangle | `PHIP-06` | planned |
| `Input` | Drain a bounded queue of key and pointer events | `PHIP-07` | planned |
| `Storage` | Two fixed extents and an atomic swap between them | `PHIP-08` | **exists**, in memory |
| `Audio` | Submit a period of samples; read the presentation clock | `PHIP-13` | planned |

`Console` is the diagnostic seam every program needs before it has a picture,
and it is what the slate writes its report through today. `Storage` has its
trait and its deterministic in-memory implementation, which is what lets the
save protocol below be tested — including all four of its failure modes —
before Phipia can write a byte.

Six, and the sixth is a transcript. Not seven, and no general "system"
interface that would become a seventh by accretion. Adding a seam is an
amendment to this document and to
[`PLATFORM_CONTRACT.md`](PLATFORM_CONTRACT.md).

## The data model

### Time

```text
Rational  { numerator: i64, denominator: NonZeroI64 }   exact, always reduced
Timebase  { rate: Rational }                            e.g. 24000/1001
Instant   { ticks: i64, timebase: Timebase }            a position
Duration  { ticks: i64, timebase: Timebase }            a length
Timecode  { hours, minutes, seconds, frames, drop }     presentation only
SampleAt  { samples: i64, rate: u32 }                   audio position
```

Instants and durations in different timebases do not add. Conversion is an
explicit, exact operation that refuses when it cannot be exact (R-4.8). Timecode
is a rendering of an `Instant`, never a storage type.

### Structure

```text
Project
  media_library : SlotMap<MediaId, MediaAsset>
  sequences     : SlotMap<SequenceId, Sequence>
  history       : EditJournal

MediaAsset  identity (content digest), timebase, duration, location hint,
            source: recording | title | nested sequence
Sequence    timebase, tracks, markers
Track       ordered, non-overlapping items, fader, transitions,
            opacity and level curves
Item        clip | gap
Clip        media reference, source range, length, grade, mask, transform
```

**Every line above is what exists.** An earlier version of this block was a
sketch of the finished model written in the present tense, and it went stale
without anyone noticing: it gave `MediaAsset` a location hint the type did not
have, a "probed description" the *layering* forbids — `media-editor-model` and
`media-editor-media` are siblings, so an asset can never hold a
`FrameDescription`
— and it listed a transition as a kind of `Item` in a document that explains at
length why a transition is not one. What is planned is marked as planned.

Reading that block is what found the location hint missing, and chasing that
found a real bug: two identifiers could name one digest, which quietly
falsified the conform round trip. A diagram nobody checks is a diagram that
describes the program somebody meant to write.

It went stale a second time, in the other direction. `Item` carried *[nested
sequence planned]* — and when nesting arrived it was not a kind of item at all
but a kind of **media**, which is the whole reason it needed no new machinery
in `Track` or in `Item`. A note about a plan is a prediction, and this one was
wrong about where the thing would go. Both the note and the prediction are
gone; `MediaAsset` says what a source can be.

A transition is **not** an item, and that is the model's second most important
decision after non-overlap: an item would have to overlap its neighbours, so a
transition is a length and the cut it is centred on. Both a dissolve and a wipe
are that same shape and differ only in what a renderer does with the fraction.

Nested sequences are named here so the shape is decided before the pressure to
compromise it arrives, and do not exist. **Markers do**, as of M8.28: an
instant and some text, beside the tracks rather than on one, because a note is
about the programme at a moment and not about any one layer of it. Effects do
not exist either: a grade and a mask are fields on a clip rather than entries
in a general effect list, and the general list is M8's problem.

A track's items are non-overlapping by construction: the type cannot represent
an overlap, so an editing operation that would create one fails to compile a
value rather than being caught by a check. This is the model's single most
important design decision.

### Edits and history

An edit is a value:

```text
Edit = Insert | Remove | Lift | Trim | Slip | Slide | Roll | Cut | Heal | SetParameter | ...
```

Every variant carries enough to invert itself. The journal is a sequence of
applied edits with the model root before and after each. Undo is the inverse;
redo is reapplication. The property test that governs this is R-9.2: for any
generated sequence of edits, undoing all of them reproduces the initial model
hash exactly.

Structural sharing (`rpds`, if adopted) makes keeping the old root cheap, which
is what makes this design affordable rather than merely correct.

## Saving

A save must never be able to lose the last good file (R-9.4), so it is four
steps and only the last one touches the project:

```text
  1. encode          the project becomes bytes, in memory
  2. write scratch   the scratch slot takes them; the project is untouched
  3. read back       the scratch slot is read and compared, byte for byte
  4. commit          the seam's one atomic step: scratch becomes project
```

Step three exists because a storage that accepted a write and stored something
else is exactly the failure that would otherwise be committed. Removing its
comparison makes one test fail — the one about a storage that corrupts — which
is how it is known to be load-bearing rather than decorative.

The file format is versioned from its first byte, length-prefixed, and carries
a SHA-256 of its payload, which together mean **every single-byte change to a
valid file is refused**. That is checked by mutating every byte of a real file
to five different values, by truncating it to every possible length, by
extending it, and by pushing a few hundred thousand bytes of seeded garbage
through the decoder.

History is not saved. Undo is a property of a session; a file that carried its
own history would make "open the file" and "open the file and undo twice" two
different projects with one name.

## The media pipeline

```text
  storage bytes
        |  bounded read, digest verified
   container demux            <- safe Rust, fuzzed, bounded
        |  typed packets
     decoder                  <- safe Rust today; sealed C leaf later
        |  frames, fully described
   frame cache                <- content-keyed, bounded, evictable
        |
   render graph               <- pure, deterministic, order-independent
        |
   compositor + colour        <- explicit conversions only
        |
   presentation surface       <- damage rectangle, one seam
```

Every arrow is a typed value with a hash. Every box is a pure function except
the first and the last. Four of them exist: the frame types with their complete
descriptions, the content addressing, the bounded pool, and the `SPRW`
mezzanine that the first three are read from and written to. A frame whose
colour is unstated is not merely refused — the types have no `Unknown`, no
`Unspecified` and no `Default`, so it is not a value this application can
construct. A cache entry's key is a digest over the input digest,
the complete parameter set, and a code version constant (R-8.5) — so a cache
can never return something computed by different code.

The audio path is the same shape with a harder deadline: decode, resample,
mix, meter, submit. Nothing on it allocates (R-5.3), nothing on it locks
(R-6.4), and its latency is declared rather than measured (R-8.6).

## Concurrency

Single-core today, so everything runs in one thread and the render graph
executes serially. The graph is nonetheless written as if it were parallel:
pure tasks, typed edges, no ambient state, fixed reduction shapes. R-6.2's test
— run the same graph in every order a scheduler could choose and compare the
results — runs now, when it is close to trivially true, so that it is still
true on the day `PHIP-11` makes it interesting.

Two of the graph's invariants are structural rather than checked. A node may
only refer to nodes added before it, so **a cycle is unrepresentable**: there
is no `add_edge` to get wrong and no validation pass to forget. And a node's
identity is a digest over its kind, its parameters and its inputs' identities,
so two nodes that would compute the same picture are the same node as far as
the cache is concerned. Leaving the input out of that digest was shown to fail
both the identity test and the order-independence test — the second because a
shared cache then hands back the wrong frame, which is the exact failure a
content-addressed cache exists to prevent.

## Directory layout

```text
crates/            the Rust workspace
native/            C shims (R-3.3)
perf/              C++ leaves, if any (R-3.4)
vendor/            vendored dependencies, exact and complete
deps/manifest.toml the dependency record
targets/           the Media Editor target specification and linker script
tools/             build, fixture, and verification tooling
fuzz/              fuzz targets and corpora
tests/golden/      golden hashes and reference frames
docs/              this documentation set
assets/            the canonical mark and shipped assets
```
