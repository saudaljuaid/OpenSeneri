<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Glossary

Editing vocabulary, defined tightly enough to implement. Where a term maps to
a type in [`ARCHITECTURE.md`](ARCHITECTURE.md), the type is named. Where the
industry uses a word loosely, Media Editor picks one meaning and keeps it.

## Time

**Timebase** — the exact rate a position is counted in, as a rational.
`Timebase`.

**Frame rate** — a timebase used for pictures. Always exact: 24000/1001, never
23.976.

**Instant** — a position on a timeline, in ticks of a stated timebase.
`Instant`. Positions in different timebases do not compare or add without an
explicit, exact conversion.

**Duration** — a length in ticks of a stated timebase. `Duration`.

**Timecode** — a human-readable rendering of an instant as hours, minutes,
seconds, and frames. A presentation form, never a storage form.

**Drop-frame** — a timecode counting convention for 30000/1001 and 60000/1001
that skips labels, not frames, so that the label tracks wall-clock time. It
changes what an instant is called and never what it is.

**Sample position** — an audio position as an integer count of samples at a
stated rate. `SampleAt`.

## Media

**Media asset** — a file Media Editor has probed, identified by a content digest
and described by what the probe found. `MediaAsset`.

**Essence** — the picture or sound itself, as distinct from the container that
holds it or the metadata that describes it.

**Container** — the file format wrapping one or more streams: ISO-BMFF, MKV,
WAV.

**Codec** — the encoding of a stream inside a container. A container is not a
codec, and confusing the two is the most common bug in media software.

**Packet** — one compressed unit read from a container, with its timestamps.

**Frame** — one decoded picture with its complete description: geometry, pixel
format, primaries, transfer function, matrix, range, chroma siting, pixel
aspect. An undescribed frame is refused.

**Intra-only** — a codec where every frame decodes independently. What an
editor wants, because seeking is exact and cheap.

**Long-GOP** — a codec where frames depend on other frames. What cameras
deliver, and why ingest transcoding exists.

**Mezzanine, intermediate** — the intra-only format material is transcoded to
for editing. Media Editor's own are `SPRW`, then FFV1.

**Proxy** — a smaller, cheaper stand-in for editing, swapped for the original
at render. A proxy is never the thing that gets exported.

**Conform** — reconstructing an edit against the original media after it was
cut against proxies or against another system's export.

**Relink** — reconnecting a project's references to media that moved.
Media Editor relinks by content digest, never by path alone.

## The timeline

**Sequence** — an editable programme: a timebase and its tracks. `Sequence`.

**Track** — an ordered, non-overlapping series of items. Overlap is
unrepresentable in the type, not merely rejected.

**Item** — a clip, a gap, a transition, or a nested sequence.

**Clip** — a reference to a media asset, a source range inside it, a timeline
range it occupies, and the effects applied to it. `Clip`.

**Source range** — the part of the media the clip uses, in the media's
timebase.

**Timeline range** — where the clip sits in the sequence, in the sequence's
timebase.

**In point, out point** — the first instant included and the first instant
excluded. Ranges are half-open, everywhere, without exception.

**Handle** — media outside a clip's source range, available for trimming and
required by transitions.

**Gap** — an explicit absence of material. A gap is an item, not a hole,
because arithmetic on holes is where timelines go wrong.

**Marker** — a named instant or range attached to a sequence or a clip.

**Playhead** — the instant currently being viewed. Interface state, not model
state.

## Edits

**Edit** — a typed, invertible operation on the model. `Edit`.

**Cut** — splitting one item into two adjacent items at an instant. Nothing
else changes.

**Trim** — moving one edge of a clip. Ripple trim moves everything after it;
roll trim moves the adjacent edge instead.

**Slip** — changing a clip's source range without moving it on the timeline.

**Slide** — moving a clip on the timeline without changing its source range,
absorbing the difference into its neighbours.

**Insert** — placing material and pushing everything after it later.

**Overwrite** — placing material and replacing whatever was there.

**Three-point edit** — an edit specified by any three of source in, source out,
timeline in, and timeline out; the fourth is derived exactly.

**J-cut, L-cut** — an edit where the sound and the picture change at different
instants. A consequence of tracks being independent, not a special feature.

**Transition** — an item that consumes handles from both neighbours and
produces a blend across a duration.

**Ripple** — any operation that shifts later items to preserve adjacency.

**Handles** — media a clip has beyond its in and out points. What a dissolve
reaches into, and the reason a clip trimmed to the very ends of its source
cannot have one.

**Dissolve** — a transition where the outgoing clip fades out as the incoming
one fades in. In Media Editor it sits at a cut rather than being an item, and it
is computed as an opacity so that `over` performs it.

**Edit decision list, EDL** — a cut written as a list of events, each naming a
source, an in point, an out point, and where it lands on the record. CMX 3600
is the one every system still reads.

**Out point** — in an EDL, the first frame **not** used. Reading it as the last
frame used makes every clip one frame too long.

**Reel** — in an EDL, the eight-character name of the source. Not a path, and
not enough to identify a file, which is why the format has a comment for the
rest of the name.

**FCM** — an EDL's frame code mode line: drop frame or non-drop frame. It
applies to every event after it until the next one.

## Rendering

**Render graph** — the pure, deterministic computation from a sequence and an
instant to a frame.

**Composite** — combining layers into one frame, in a stated colour space, by a
stated operator. In Media Editor, always in linear light.

**Alpha, coverage** — the fraction of a pixel a layer occupies. Not light: it
passes through no transfer function and obeys no limited range.

**Straight alpha** — colour samples that stand on their own, with alpha saying
how much of them to use.

**Premultiplied alpha** — colour samples already multiplied by coverage. In
Media Editor the multiply happens **in linear light**: a premultiplied sample is
the encoding of `light x coverage`, not the encoded value scaled by coverage.
Both conventions exist in the wild and they disagree.

**Over** — the Porter-Duff operator that lays one layer on another. Correct and
associative only on premultiplied values.

**Dark fringe** — the artefact of compositing straight samples as though they
were premultiplied. Half-covered edge pixels keep their full-strength colour,
so adding the background beneath them makes the edge wrong — too dark over a
light background, too bright over a dark one.

**Bake, flatten** — rendering an effect's result into new media so it is not
recomputed. Always additive; source media is never modified.

**Export, deliver** — producing the final file. The bytes are reproducible from
the project and the inputs (R-4.1).

**Golden hash** — the SHA-256 of a render, committed as the definition of
correct output for a test.

**Turn** — a full revolution, as a unit of angle. Media Editor's sine and cosine
take turns rather than radians, because reducing an angle modulo one turn is
exact in binary and reducing modulo two pi is not.

**Vectorscope** — a count of a frame's chroma plotted as a plane:
blue-difference across, red-difference up, so hue is an angle and saturation is
a distance from the middle. Neutral is the origin exactly.

**Graticule** — the reference marks on a scope. On a vectorscope, the six boxes
the colour bars must land in; four of them sit on the axes in every matrix, and
two move with the coefficients.

## Colour

**Primaries** — the chromaticities of the red, green, and blue the numbers
refer to. Rec. 709 and Rec. 2020 are different primaries.

**Transfer function** — the relationship between stored code values and light.
Sometimes called gamma; the two are not synonyms.

**Matrix** — the coefficients converting between RGB and a luma–chroma
representation.

**Range** — whether code values use the full interval or the limited broadcast
one. Getting this wrong is the classic washed-out or crushed picture.

**Chroma subsampling** — storing colour at lower resolution than brightness:
4:2:0, 4:2:2, 4:4:4. Siting matters and is part of a frame's description.

**Pixel aspect ratio** — the shape of a pixel, for material that is not square.

**LUT** — a lookup table applied to colour, one-dimensional or three-
dimensional, read from `.cube` or `.3dl`.

**Scope** — a measurement display: waveform, vectorscope, histogram, parade.
Diagnostics, computed from the same frames the viewer shows.

## Sound

**Sample rate** — samples per second. 48 kHz for picture work.

**Bit depth** — bits per sample. The mix runs deeper than the delivery.

**Decibel** — a logarithmic unit of level. For amplitude, `20 log10(factor)`, so
twenty decibels is a factor of ten and a doubling is 6.020599913 — not 6.

**Unity** — a gain of exactly one, at zero decibels. A fader at unity passes its
input through untouched.

**Fader** — where a track's level is set. A property of the project: set by an
edit, undoable, and saved.

**Pan law** — what a pan control does at the centre. Constant power keeps
`left² + right²` at one across the image, which puts centre 3.01 dB down.

**Full scale** — the loudest sample a format can hold. Two's complement gives
one more value below zero than above it, so the rails are not symmetric.

**Headroom** — how far a signal sits below full scale. What a mix runs out of.

**Null test** — summing a signal with its own inverse. An exact mixer gives
exact silence; anything else leaves a residue.

**LUFS** — loudness units relative to full scale, as EBU R128 defines them.
Delivery specifications are written in these.

**True peak** — the peak of the reconstructed analogue waveform, which can
exceed the peak sample value. Limiters that ignore this clip on playback.

**Underrun** — the audio device wanting samples that were not ready. Counted,
surfaced, and never concealed (R-8.7).

**Waveform overview** — a cached, downsampled amplitude picture for display.
Derived data; disposable.

## Interchange

**EDL** — a plain-text edit list, CMX 3600 being the enduring dialect. Old,
exact, and universally readable.

**OTIO** — OpenTimelineIO, the modern interchange model for editorial.

**AAF, XML** — richer interchange formats used between systems, each of which
is hostile input under R-11.

**Bin** — a folder in the media library. An organisational concept only; it
never affects a render.
