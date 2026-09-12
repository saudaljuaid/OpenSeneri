<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Roadmap

Smallest first, and each milestone is finished when its evidence exists — not
when its code compiles. A milestone that cannot state a QEMU scenario and a
negative control is not a milestone, it is a branch.

Dates are absent on purpose. The sequence is the commitment.

## M0 — Foundation

*Requires nothing. This is where the repository is now.*

The engineering rules, the open-source map, the platform contract, the planned
architecture, and the hygiene that enforces them.

**Done when:** the documents are merged, `make lint` is green, and the
capability ladder in [`PLATFORM_CONTRACT.md`](PLATFORM_CONTRACT.md) has been
read by whoever will implement it in Phipia.

**State: done.**

## M1 — Slate

*Requires `PHIP-01`, `PHIP-02`, `PHIP-06`.*

A native Phipia program, written in Rust, that acquires a surface, draws a
clapperboard and a frame counter, presents once, and exits cleanly. No model,
no media, no interaction. The point is the platform, not the picture.

**Done when:** a QEMU scenario boots Phipia, launches Media Editor from the
read-only volume, matches the presented pixels against a pinned image, observes
a status-zero exit and an equal resource census, and a negative control proves
that a one-pixel mutation fails the scenario. The linked ELF passes the R-13.4
audit: `ET_EXEC`, static, non-PIE, W^X, no dynamic section, no relocations, no
undefined symbols, empty GOT, and no SIMD instruction.

**Also delivers:** the linker script, the build, and `media-editor-rt` and
`media-editor-abi` in their first form.

**State: everything that does not need Phipia is done.**
`targets/media-editor.ld`,
the build in the `Makefile`, `media-editor-rt`, `media-editor-abi`, and
`media-editor-image` exist; `make audit` proves the linked image has the shape
Phipia's ELF validation accepts, and `tools/audit-control.py` proves the audit
can refuse. What is left is `PHIP-01`, `PHIP-02`, and `PHIP-06` — the kernel
side
— and the picture that `PHIP-06` makes possible. Until then the program's
report goes to the console seam, and its whole output is pinned by a golden
transcript test.

## M2 — Model

*Requires nothing from Phipia. Runs entirely on the host.*

`media-editor-core` and `media-editor-model`: rational time, timebases,
drop-frame
timecode, identifiers, the project structure, the full edit algebra, and the
undo journal. The allocator and the project file format arrive here too.

**Done when:** the R-9.2 property holds under `proptest` for generated edit
sequences of bounded depth; timecode round-trips exactly for every supported
rate including 24000/1001 and 30000/1001 across a full day of frames; the
project file round-trips byte-identically; the format's parser has a fuzz
target with a committed corpus; and a negative control proves a corrupted
project file is refused rather than partially loaded.

This is the largest milestone and the one with no platform risk at all. It can
proceed in parallel with Phipia's work on M1's capabilities.

**State: done.** `media-editor-core` holds
exact rational time, timebases, half-open ranges, and drop-frame timecode, and
its tests sweep every frame of a whole day at 24, 25, 30, 48, 50, and 60
non-drop and at 30 and 60 drop-frame — some twenty-nine million round trips,
each of which must name its own frame back. `media-editor-model` holds the
project, sequences, tracks, items, the eight edit operations, and the journal;
the undo property is checked over two thousand generated sessions plus forty
long ones, and a deliberately broken inverse was shown to fail it.
`media-editor-io` holds the project file: versioned, length-prefixed, and
digested, so that **every single-byte change to a valid file is refused** —
which is a sweep, not a claim. Saving is the four-step protocol in
[`ARCHITECTURE.md`](ARCHITECTURE.md), and each of its four failure modes is a
test that requires the previous file to survive whole; removing the read-back
comparison was shown to fail exactly one of them. The allocator over `PHIP-03`'s
pages is what remains, and it needs `PHIP-03`.

## M3 — Reel

*Requires `PHIP-03`, `PHIP-05`, and read-only storage as it already exists.*

Play one deterministic uncompressed clip from the read-only volume: read,
describe, cache frames in a bounded pool, and present them on time.

**Done when:** a QEMU scenario plays a pinned clip and proves every frame was
presented, in order, with its interval measured against `PHIP-05`'s clock and
recorded; the frame pool's census is equal at teardown; a negative control
proves a truncated clip is refused with a named error and leaves the
application usable.

**Also delivers:** the `SPRW` uncompressed mezzanine format, the frame pool,
the cache key discipline of R-8.5, and the first golden-hash tests.

**State: the half that needs no operating system is done.** `media-editor-media`
holds frames that cannot be built without a complete colour description —
there is no `Unknown` primary, no `Unspecified` transfer, no `Default`
anywhere, so the untagged frame that causes every washed-out export in this
industry is not a value this application can construct. Plane arithmetic is
checked at every dimension in a range rather than at three examples. The pool
is bounded in frames and in bytes, evicts by use with deterministic
tie-breaking, and refuses to let one key ever name two different frames. The
`SPRW` mezzanine round-trips every format, and its single-byte sweep found a
real gap — a digest over only the samples let a flipped transfer-function byte
silently change every frame in the file — which is now closed by digesting the
header too. What remains is reading a reel from Phipia's storage and presenting
it on time, and that is `PHIP-03`, `PHIP-05` and `PHIP-06`.

## M3.5 — Interchange

*Requires nothing new.*

CMX 3600, the edit decision list every system still speaks: a text format
designed in the 1970s to drive tape decks, and still how a cut moves between
two applications that agree on nothing else.

Four things about it are traps, and each has a test named after it. **The out
point is exclusive** — `00:00:10:00` as a source out means the last frame used
is the one before it, and an importer that reads it as inclusive makes every
clip one frame too long, which is invisible until something goes to air.
**`FCM` is stateful** — it applies to every event after it until the next one,
so a parser that reads the first and forgets to watch for more mistimes the
second half of the reel. **Drop-frame is stated twice**, once by the `FCM` line
and once by the semicolon before the frames field; when they disagree the file
is refused rather than read one way or the other, because they are two
statements about the same fact. **A reel name is eight characters**, so writing
an EDL loses the identity of the source — deliberately, with the full name in
the comment the format provides for it.

A CMX 3600 timecode has two digits for frames and no field anywhere saying what
rate it counts in, so a 24-frame film cut and a 25-frame PAL cut are written
identically. That is the format's deepest limitation and it is not one a parser
can fix: this one will not invent the rate, and anything that needs it must be
told.

Every prefix of a valid event line is swept and must be refused; lines, event
counts and reel names are all bounded before anything is allocated (R-11.2).

**State: done.** Contiguity is asked one channel at a time — picture and sound
both start at the top of the programme, so a whole-list version of the question
would report every ordinary two-channel list as full of holes, which is true of
the file and false of the cut.

## M4 — Cut

*Requires `PHIP-07` and `PHIP-08` step one.*

A timeline the user can actually edit: load media, place clips, cut, trim,
ripple, undo, save, reopen, and render the sequence to a file.

**Done when:** a QEMU scenario drives real keyboard and pointer events through
a scripted edit session, saves, reloads, and produces a render whose SHA-256
matches a pinned value; the same project rendered twice produces identical
bytes; a negative control interrupts the save and proves the previous file
survives intact (R-9.4).

This is the first milestone at which Media Editor is, in the narrowest possible
sense, an editor.

## M4.5 — Dissolve

*Requires nothing new.*

The first transition, and the first thing on a timeline that is not a cut.

A dissolve sits **at a boundary** rather than being an item of its own. An item
of its own would have to overlap its neighbours, and this model's guarantee is
that a track's items tile it with no overlap and no missing region — a
guarantee that holds by construction rather than by validation, and is not
worth giving up for one feature.

So a dissolve is a length and the cut it is centred on. During it both
neighbouring clips are on screen: the outgoing one reaching past its out point
and the incoming one starting before its in point, into material each has but
does not otherwise use. That material is called handles, and whether it exists
is a question about the media — which the model does not know, and does not
pretend to. What it *can* check it does: a dissolve may not outlast either clip
it is between, and the incoming clip's in point must be far enough into its
source for the dissolve to reach back at all.

It needs no new operator. The stack reports both sides — the outgoing at full
opacity and the incoming at the fraction of the way through — so `over`
computes `in x t + out x (1 - t)`, which is what a cross-fade is. The fraction
is an exact rational and runs from `1/(N+1)` to `N/(N+1)`, touching neither
end: a dissolve whose first frame is entirely the outgoing clip has spent a
frame showing what the frame before it already showed.

An edit that would renumber a cut a dissolve sits on is refused rather than
silently renumbering the dissolve — take it off first. That is one more step
for the user and one fewer way for a project to end up describing something
nobody edited (R-1.3).

**State: done**, for dissolves and — since **M8.3** — for wipes, which needed
a shape (M8.2) and then a place in the model. A dip to colour needs a colour
source, which is a clip of colour rather than a special case here.

## M4.6 — Keyframes

*Requires nothing new.*

Opacity that fades, a scale that pushes in, a volume that ducks under
dialogue. Each is one number with a different value at different instants, and
a **curve** is the shape it takes between the moments somebody actually set it.

Held at both ends, never extrapolated. Continuing the slope past the last key
is how a parameter set to reach 100% at the end of a shot arrives at 340% two
shots later; the editor who set two keyframes described what happens between
them and nothing else.

Three interpolations. **Hold** keeps the outgoing value and then jumps, which
is what a parameter with discrete settings needs and what an editor reaches for
when a smooth ramp would be a mistake. **Linear** runs straight between, and is
exact: a rational fraction of a rational change is a rational, so a twenty-four
frame ramp is `n/24` at frame `n` on the nose, including the thirds and
sevenths a binary fraction cannot hold.

**Ease** is the cubic Bézier every editor draws as two handles, and it is the
one with an arithmetic problem worth stating plainly. A Bézier is parameterised
by `t`, but a curve is asked for a value at a *time*, and getting from one to
the other means solving `x(t) = time` — a cubic, whose solution needs a cube
root and is therefore not rational. There is no exact answer to find.

So the approximation is stated instead of hidden. `t` is found by bisection to
twenty halvings — one part in a million, far finer than any pixel, sample, or
parameter an editor sets — and the value is then computed at that `t` in 128-bit
integers and rounded once, half away from zero, to the same precision. The
number handed back is the curve's value at a dyadic parameter within `2^-20` of
the right one, with one stated rounding on top. Not "close enough": a size,
written down, and asserted.

Twenty rather than thirty-two because exact arithmetic on a cubic *cubes* the
denominator. A parameter over `2^32` would make `t³` a fraction over `2^96`,
which no 64-bit rational holds — so the count is set by what the arithmetic can
carry rather than by what sounds precise.

The bisection needs `x(t)` monotone, so the handles' *horizontal* positions are
held inside the span and a handle outside it is refused rather than clamped: a
folded curve has more than one value at an instant and no arithmetic will pick
between them. The verticals are not clamped, because a handle past one is an
overshoot — a push that goes a little past and settles — and that is something
an editor asks for on purpose.

One pleasant fact the tests pin: with the horizontal handles at a third and two
thirds, `x(t) = t` identically, so the default ease costs no inversion error at
all.

The first parameter to read one is a **picture track's opacity**. It is set by
an edit like everything else, so it undoes, it redoes, and it is saved; and
`None` is deliberately not the same as a curve holding one, because a track
with no automation has none to read and the difference is what lets automation
be switched off and back on without losing the shape somebody drew.

At the layer stack a track's opacity *multiplies* what the items on it are
doing rather than replacing it. Two things decide what is on screen during a
dissolve inside a fade — how much of the incoming clip is showing, and how much
of the track is — and either one alone throws the other away.

The value is clamped into nought-to-one at the point it is read, because a
curve may overshoot on purpose and nothing can be more than fully opaque. That
is a different decision from refusing an ease handle outside its span: a handle
outside the span changes which curve was drawn, silently, while a value past
full opacity is the curve the editor drew meeting a physical limit.

`SPRJ` moves to version four to carry it. A curve is written after a track's
dissolves as a keyframe count and then, per keyframe, an instant, a value as
its two halves, and an interpolation tag — with an ease's four handles after
the tag. Rationals are written as numerator and denominator rather than as a
scaled integer, because a third is a third and this would otherwise be the one
place in the project file where an exact number stopped being one.

### Sound, where a frame is a stretch rather than a moment

A sound track's fader reads a curve too, in the decibels the fader is already
labelled in — so an automated fader and a static one are the same control. But
picture and sound part company here, and the difference is the whole of the
extra work: a frame of picture is a *moment*, so one value answers it, while a
frame of sound is two thousand samples and a fader may be somewhere different
at each of them.

Applying one gain per frame and holding it flat puts a step at every frame
boundary. On a slow move nobody hears it; on a fast one it is a buzz at
twenty-four hertz, and it is the noise every mixer that ever shipped has had to
be taught not to make. So the mixer takes a **ramp**: a source whose gain runs
from one value at its first sample to another at the sample *after* its last.

That half-open interval is the same shape as everything else here — a frame's
samples run up to the next frame's first, an EDL's out point is exclusive, a
summary block ends where the next begins. It is what makes consecutive blocks
*tile* a fader move: block `n` stops one step short, block `n + 1` starts
exactly there, and no sample is given a gain twice. Closing the interval
instead would repeat one gain at every boundary, and a repetition at a regular
interval is a tone — it would put a hum at the frame rate into every automated
fade.

The interpolation runs in the **factor** rather than in decibels, which is a
choice and is bounded. A fader's travel is logarithmic, so the true path
between two positions is the geometric mean and this takes the arithmetic one:
for a six-decibel move the two differ by half a decibel at the middle of a
block and by nothing at either end. The ends are what matter, and a block is
one frame, so the approximation lives inside a fortieth of a second and shrinks
as blocks do. Interpolating in decibels would mean a logarithm and an
exponential at every sample, at every rate, for a difference no listener meets.

**Mute is not a fader position.** It is a switch, and a curve holds decibels —
its floor is very quiet and is still a level. So a muted track stays muted
whatever is drawn on it, and a track turned off at the surface does not come
back on because somebody drew a fade. That also means mute cannot change within
a block, which is why the mixdown may decide not to decode a muted track from
one end alone; if mute ever gets a lane of its own, that becomes a two-ended
question again.

### Editing one keyframe

Replacing a whole curve is correct and coarse. It undoes, but a journal entry
carries two entire curves, and fifty drags of one keyframe are fifty copies of
everything else on the lane. So there is a second edit that is what a keyframe
*drag* actually is: add one, remove one, move one in time, or change what one
holds.

Adding to a lane with nothing on it **starts** the automation, and removing the
last keyframe **turns it off** — which is what makes those two each other's
exact inverse. An editor who adds a keyframe to a parameter nobody has animated
expects the parameter to become animated, and undoing that expects it to stop,
not to leave a flat curve nobody asked for sitting on the track.

Moving and setting are separate operations because they are separate gestures
with separate risks: a move can reorder the curve and cannot change a value, a
set can change a value and cannot reorder anything. Neither can do the other's
damage by accident. A keyframe may move *past* its neighbours, which is a thing
an editor does on purpose and inverts cleanly, but it may not land *on* one:
two values at an instant is the same nothing as none.

The four operations are nested inside one edit rather than spread across four
more, and that is a structural decision rather than a tidying one. `Edit::apply`
is a dispatch, so its length is the number of operations it dispatches; a subset
of a dispatch needs an arm for every variant the subset does not handle, which
is a branch nothing reaches and no test can cover. Nesting gives one arm at the
top and a match underneath that is exhaustive over exactly four.

**State: done for both lanes**, and — since **M8.10** — for a clip's framing
too. The reason given here for not doing that was wrong, and it is worth
saying why rather than quietly deleting it: curves on items were said to need
"a name for a keyframe that survives its item being renumbered", and they do
not. The worry dissolves by putting the curve *on the clip*, where there is no
index to survive. What it actually needed was an answer to a question this
line never asked — what a keyframe's instant is *measured from* — and the
answer, the clip's own start, is what makes a ripple renumber nothing.

Not done: automation for anything that is not a level or a framing — a pan, a
parameter on an effect that does not exist yet.

## M5 — Sound

*Requires `PHIP-13`, and `PHIP-10` for comfort.*

Audio decode, sample-rate conversion, a mixer with a declared latency, EBU R128
metering, and synchronised playback.

**Done when:** a QEMU scenario plays a pinned tone sequence and proves
sample-exact alignment against the video presentation clock; underruns are
counted and are zero for the scenario's duration; a negative control forces an
underrun and proves it is reported rather than concealed (R-8.7).

## M5.5 — Mix arithmetic

*Requires nothing new. `PHIP-13` is the sound device these buffers reach.*

Everything a mixer needs before it needs somewhere to play. Buffers that carry
their own sample rate, gain in the decibels a fader is labelled in, a pan law
that keeps energy constant across the image, and a sum that says what full
scale cost.

No floating point, for two independent reasons. Phipia preserves no
floating-point state anywhere, so a Ring 3 program has no guarantee it may
execute a single such instruction (`PHIP-04`) — and even on a machine that
could, `pow` and `log` are not specified bit-for-bit by IEEE 754, so two
machines mixing one session would bounce different files. A decibel is a
logarithm and a constant-power pan is a square root, and both come from the
same integer arithmetic the transfer curves use.

Three anchors, and none of them approximate. **Zero decibels is exactly one** —
a mixer whose unity is a hair off colours every channel it touches, compounding
down a bus. **Twenty decibels is exactly ten**, which is what the definition
says and the cleanest check that the logarithm and the exponential are
inverses. And **six decibels is not a doubling**: it is 6.020599913, and the
tests measure what rounding it costs rather than repeating the folklore — two
full-scale sources trimmed six decibels each still clip, by a quarter of a
percent.

The pan law is constant power: `left² + right²` is one at every position, so a
source does not change loudness as it crosses the image, and centre is
−3.01 dB rather than 0 or −6. The usual route is a sine and a cosine; this one
substitutes for the angle and needs only a square root, which is exact and
needs no table anyone has to trust. Measured across two thousand positions the
power never moves more than three of the last bits, which is what the rounding
of two roots and two squares allows.

Summing is `i64` throughout, so it is exact: a signal against its own inverse
nulls to precisely zero, adding a muted channel changes a mix bit for bit not
at all, and a bus is the same bus however its channels are scheduled — the same
order-independence argument the render graph makes for pictures (R-6.2).

Full scale is **reported rather than reached quietly**. A mixer that clips in
silence is one whose user finds out on a listening copy; one that refuses
cannot open a session a fader move would fix. So the mix comes back with a
count of the samples it had to write differently from how it computed them, and
by how much.

**State: done.** Resampling, dither and every filter are deliberately absent:
each is a decision with a name, and a mixer that quietly performed one would be
answering a question nobody asked (R-1.3).

## M5.6 — Mixdown

*Requires nothing new.*

The sound half of rendering a sequence, and the place where picture time and
sound time have to agree.

A frame at 24 into 48 kHz is 2000 samples exactly. At 30000/1001 — the
commonest rate in television — it is 1601.6, which is not a number of samples.
No frame at 29.97 holds a whole number of samples and none ever will, so an
implementation that multiplies frames by a constant is wrong before it starts.

A frame's samples are therefore *bounded* rather than counted:
`Instant::floor_into` says which sample a timeline position falls in, and the
samples of frame `n` are those from its floor up to the floor of `n + 1`. Each
block is 1601 samples or 1602, each block's end is the next one's beginning,
and over three hundred frames they sum to exactly 480,480 — arrived at by
arithmetic, not by running the code.

`floor_into` is a separate name rather than a flag on `convert`, because the
two answer different questions: one asks which tick this position *is*, and
refuses when the answer is not a tick; the other asks which tick it is
*inside*, which always has an answer. A caller has to say which it wants
(R-1.3).

Each track mixes at its own fader, and the fader is a property of the
*project*: it is set by an edit, it has an inverse, it undoes, and it is saved.
A mix level that lived only in a function call would be a mix nobody could
deliver.

The model stores the exact decibel value the user set rather than the factor it
becomes, so a fader moved and moved back sits precisely where it started, and a
quarter of a decibel comes back as a quarter of a decibel rather than as
something near it. Muted is a separate detent rather than a very small number,
because the logarithm of zero is not a point on the scale — and a muted track's
media is never read at all, which is the difference between a mix that keeps up
and one that does not.

The travel is written down in both the model and the mixer, because the two are
siblings and neither may depend on the other. The application is the only place
that sees both, so that is where a test asserts they are the same numbers.

The project file goes to version 2 for it, on the same reasoning as `SPRW`: a
version-one file read as version two would take its faders out of what follows
the track kind, which is an item count.

**State: done.** Panning waits for a stage that changes channel counts;
storing a pan nothing applies would be data pretending to be a feature.

## M5.7 — Loudness

*Requires nothing new.*

A peak meter says how close a signal came to the rails and almost nothing about
how loud a programme sounds. Delivery specifications are written in LUFS, so
this is what a delivery is measured against.

Three things in order, as ITU-R BS.1770-4 defines them. **K-weighting**: a high
shelf for the way a head shadows and reinforces sound from in front, then a
high-pass that discards the rumble the ear largely ignores — the coefficients
the standard prints, held as exact decimals. **Mean square, weighted and
summed** across channels, turned into a level with the standard's `-0.691`
offset, which very nearly cancels the weighting's own gain at 1 kHz so that a
tone reads at the level it was recorded at. **Gating** into 400-millisecond
blocks overlapping by three quarters, with an absolute threshold and then a
relative one — without which a minute of silence before the titles would drag a
whole programme down, and the fix would be to trim the silence rather than to
mix it better.

The compliance cases are EBU Tech 3341's own and are *generated* rather than
shipped: a 1 kHz tone at a stated level is exactly reproducible, and generating
it with the integer sine makes the fixture bit-identical on every machine too.
A −23 dBFS tone reads −23.0 and a −33 one reads −33.0, both inside the tenth of
a unit the standard allows.

Everything is integers at forty-eight fractional bits inside the filter. That
is not taste: a biquad is a feedback loop, the high-pass has a pole at 0.995,
and at thirty-two fractional bits the coefficient rounding alone would move it
enough to matter.

**State: done at 48 kHz.** BS.1770 prints its coefficients for that rate and
says to re-derive the filter at others; reusing them would measure the wrong
thing quietly, so another rate is refused (R-1.3). More than two channels is
refused too — the surrounds weigh more than the front, which needs to know
which channel is which, and a buffer carries a count rather than a layout.

## M5.8 — Overview

*Requires nothing new.*

A waveform on a timeline is not the samples. A minute at 48 kHz is nearly three
million numbers and the strip it is drawn into is perhaps two thousand pixels
wide, so every pixel stands for more than a thousand samples and something has
to decide what it stands for. Reading the samples afresh on every scroll makes
scrolling cost the file; deciding badly makes the drawing lie.

So a **summary** is built once and kept: for each block of samples, the lowest,
the highest, and the mean of their squares. Editors call the result a peak
file, and it is what makes a waveform appear the moment a clip lands rather
than a second later.

Two numbers a block rather than one magnitude, because almost nothing is
symmetric. Brass, speech, a kick drum, anything with even harmonics or a trace
of offset goes further one way than the other, and a mirrored drawing is a
picture of a signal nobody recorded — worse, it hides a one-sided approach to
the rails, which is exactly what an editor is looking at the waveform to see.

A **pyramid**, because zoom is continuous and reduction is not free. Level zero
summarises sixty-four samples a block by default; each level above summarises
two blocks of the level below, so the whole zoom range costs under twice the
finest level and any zoom is a read rather than a reduction over a million
samples.

The property everything rests on: **the highest of two highests is the
highest**, exactly. A sample that reached the rails is the maximum of its
block, of the pair containing it, and of every pair above — so zooming out
cannot hide a click, and cannot invent one either. Without that, the way to
find a click you cannot see when zoomed out is to zoom in and scrub the whole
clip, which is the job the waveform exists to save.

The mean square is the one number here that is not exact, because a mean is a
division. The sums fold upward in 128-bit integers and divide once at the end,
so every stored value is one floor from the truth however far out the zoom goes
— not one floor per level. A window of blocks combines stored means and is
within two. Both bounds are asserted, and a floor-of-floors implementation
fails the first.

A summary carries the digest of the sound it summarises, so a stale peak file
is something you can *see* rather than infer from a modification time — which a
copy, a restore, or a clock change will happily lie about.

It is stored as `SPPK`, alongside the project file and the reel. The header
holds the block size, the sample count, the rate, the channel count and the
digest of the sound — and *not* the number of levels or the number of blocks in
each, because both follow from what is already there. Two statements of one
fact is a file that can disagree with itself and a reader that must then decide
which half to believe. The reader computes the shape it expects and reads
exactly that.

The file's own digest covers the header as well as the blocks, and a probe
named what that buys: without it, thirty-three header bytes are undetectably
editable, including all thirty-two of the digest of the sound the summary is
*of*. A staleness check that can itself go stale is not a check.

**State: done, in memory and on the wire.** Not done: writing it anywhere,
which waits on `PHIP-08` for a writable filesystem, and the waveform of a whole
sequence rather than of one buffer.

## M6 — Colour

*Requires nothing new.*

The full colour pipeline: tagged frames end to end, explicit conversions,
primaries and transfer functions, scopes, 3D LUT support, and a defensible
Rec. 709 and Rec. 2020 path.

**Done when:** conversions round-trip within a stated tolerance recorded as a
test; every frame in the pipeline is fully described and an untagged frame is
refused (R-8.2); scopes agree with independently computed reference values.

### The gamut half, exactly

A standard defines a gamut with
twelve exact decimal numbers, and everything else — the RGB to XYZ matrix, the
luma coefficients, the matrix from one gamut to another — is derived from them
by linear algebra. `media-editor-render` does that derivation in exact rationals
rather than in floating point, so there is no tolerance to state: a gamut
converted to itself is the identity matrix, a round trip through XYZ is the
identity matrix, and every row of a conversion sums to exactly one. The
derived BT.709 luma coefficients round to the 0.2126, 0.7152 and 0.0722 the
standard prints, the derived BT.709-to-BT.2020 matrix rounds to the one
BT.2087 publishes, and BT.601's derived coefficients are asserted **not** to
match the 0.299, 0.587 and 0.114 its matrix uses — because those came from the
1953 primaries and were kept when the primaries changed, and an implementation
that derived one from the other would quietly produce a different picture.

Sine and cosine came later and are measured in **turns**, not radians. That is
the whole reason they are exact where it matters: reducing an angle to one
revolution is taking a fractional part, which in binary fixed point is masking
bits and loses nothing — where in radians the same reduction is a division by
an irrational number that loses a little more accuracy for every revolution. So
a quarter turn's sine is exactly one, by construction rather than by luck, and
an angle ten thousand turns out gives bit-for-bit the same answer as the same
angle at the origin. Checked against identities rather than a table: the
squares sum to one at two thousand angles, the sine is odd and the cosine even,
each quadrant has the signs it should, and the addition formula holds.

The transfer functions are done too, and they needed a deterministic power
function first. Phipia has no libm, and even where one exists R-4.1 would not
accept it: `pow`, `exp` and `log` are not specified bit-for-bit by IEEE 754, so
two machines with different libraries produce different pixels for the same
project. So the arithmetic is integers, all of it — `log2` by the classic
bit-by-bit method, `exp2` by multiplying the successive square roots of two
that the fractional bits select, and those roots computed with an integer
square root rather than embedded as a table anyone has to trust. sRGB, BT.709,
BT.2020, both pure gammas, ST 2084 and hybrid log-gamma are all checked against
their standards' own formulas, in both directions, and every curve is checked
to rise everywhere and to undo itself.

The scopes are done as well, and they are measurements rather than pictures:
a histogram and a waveform are counts, so they are exact integers and two runs
over one frame produce identical scopes. Every expectation in their tests is
arrived at by counting rather than by running the code and writing down what it
said — a ramp two hundred and fifty-six pixels wide puts exactly one sample in
every bin, eight bars four rows tall put sixteen samples at each end of every
channel, and a waveform of bars steps down monotonically the way one does on
any monitor in any suite.

The one judgement call is how an RGB frame becomes luminance. A luma-chroma
frame needs no decision — plane zero already is luma — and an RGB frame has no
matrix coefficients to use, so its primaries decide, through the same exact
derivation. The weights are integers that sum to exactly 65,536, because if
they did not a white field would not measure as white.

Frames convert, which is what makes all of the above do something. The order
is the one colour science requires: normalise by range, matrix out of
luma-chroma if there is one, decode to linear light, change gamut **in linear
light and nowhere else**, encode, matrix back, quantise. Quantisation is a
binary search over the transfer table for the code value whose light is
nearest, which is what quantisation means, rather than an encode-and-round.
The legal code range is part of that table: limited range does not merely
scale differently, it forbids the values outside it, and a table that searched
the whole byte answered black with zero — an illegal sample. That was a real
bug, found by a test that asserted the range rather than the round trip.

A scaler and a chroma filter are refused rather than guessed, because each is
a filter and a filter is a decision with a name.

The graph is what the timeline renders *through*, rather than an abstraction
waiting for a caller. A node's key is a digest over its kind, its parameters
and its inputs' identities, so a pool kept between renders answers for anything
already seen — most usefully a source, where the cost avoided is a decode.
Scrub back over a cut and nothing is decoded twice. And because the graph names
media by content digest rather than by any project's index for it, two
sequences cutting the same footage share one cached frame, and a file swapped
underneath is a different key rather than a stale hit.

Compositing is done, and it is the part of a colour pipeline that most often
looks nearly right. Two questions have to be answered before a single sample
is touched, and both are answered here rather than assumed. *In what space?*
Alpha is coverage, so what reaches the eye is a fraction of the top layer's
light plus the rest of the bottom layer's — a sentence that is only true of
light. Every sample is decoded, composited, and encoded back, whatever the
frames are encoded in, and a test computes one such pixel by hand: full white
at coverage 128 over a mid-grey background is code value 205, where adding the
code values instead gives 252, forty-seven too bright. *Straight or
premultiplied?* `over` is correct and associative only on premultiplied
values, so a frame says which it holds, and one that claims premultiplied
while carrying colour brighter than its own coverage is refused — that is the
dark fringe arriving with a note saying it is not one.

`over` is checked as the algebra it is: a transparent layer over anything
leaves it bit-for-bit unchanged, an opaque layer hides everything beneath it,
anything over an opaque layer is opaque, and the two ways of grouping three
layers agree to within one code value, so grouping clips into a compound
cannot change the picture. Premultiplying and dividing back out is stated
honestly as the lossy operation it is: exact at full coverage, within two code
values at half, and total at zero, where the colour is genuinely gone.

Compositing found a real bug in the conversion path that had been passing its
tests. `convert` was writing 255 for every alpha byte, so any keyed frame that
crossed a colour space came out a solid rectangle — and the test that should
have caught it converted opaque bars, which have nothing to lose. Coverage is
now carried through untouched, and a conversion that would gain, lose, or
re-associate alpha is refused, because each of those is an operation with its
own name.

The vectorscope is done, and two things about it are exact rather than pinned.
Neutral is the origin: every grey, at every brightness, under every matrix, has
zero chroma on both axes — checked at a thousand levels for three matrices,
because that property is what makes the middle of the graticule mean "no
colour". And the primaries land on the axes: full red has a red-difference of
exactly one half, cyan exactly minus one half, blue and yellow the same on the
other axis, in BT.601, BT.709 and BT.2020 alike, because `Cr = (R' - Y')/2(1 -
Kr)` and full red makes `Y' = Kr`, so the coefficient cancels itself. Those are
the graticule's fixed marks on every vectorscope ever built, and here they are
derived rather than looked up.

Green and magenta are *not* fixed — they move with the coefficients, which is
why a colourist can tell BT.601 material from BT.709 by where the green box
lands, and why the scope asserts both: a control that replaced the derived
matrix with a hardcoded one left the axis test passing and failed the green
one, which is what that test is for. Chroma is computed from gamma-encoded
samples rather than from linear light, because `Y'CbCr` is defined on the
encoded signal and the primes in its name are the point.

It found a bug on its first run. `Frame::blank` filled every byte with zero,
which is not black: in a limited-range luma plane zero sits below the legal
floor of sixteen, and in a chroma plane zero is not neutral but the most
negative value the byte can hold. A blank frame was a saturated blue-green with
an illegal luma, and it showed up in the corner of the graticule instead of at
the origin. A blank frame is now an opaque black slug, legal in its own range.

### Lookup tables

A look — a film emulation, a show LUT, a camera's log-to-display transform — is
a function from colour to colour that nobody can write down as an equation, so
it is *sampled*: a cube of `size³` output colours and an interpolation between
them. Thirty-three to a side is the common case, which is 35,937 samples for a
function of three variables.

Between eight surrounding samples there is more than one defensible answer, and
the two in use are not equally good. **Trilinear** takes all eight corners of
the cell weighted by position, which is the obvious generalisation and has a
fault on the one part of the picture everybody looks at. **Tetrahedral** cuts
the cell into six tetrahedra along the plane diagonals, picks the one holding
the sample by the *ordering* of the three fractions, and interpolates between
its four vertices — and on the neutral axis those four terms telescope:

```text
c000 + (c100-c000)f + (c110-c100)f + (c111-c110)f  =  c000 + (c111-c000)f
```

which is a straight run between the cell's two diagonal corners. So if a table
is neutral along its own diagonal, **every grey stays grey** — exactly, and in
fixed point, because all three channels then evaluate the identical expression
and round identically. Trilinear mixes all eight corners and drifts off neutral
wherever a table has cross-channel content, which every real look does. A grade
that tints the greys is the first thing a colourist notices.

Both are implemented, and trilinear exists to be *failed*: one test runs a
table that is neutral on its diagonal and lively off it through both, and
asserts tetrahedral holds all thirty-nine greys while trilinear tints
twenty-nine of them. A design decision with no test showing what the rejected
option does is a preference rather than a decision. Trilinear stays available
because reproducing another system's output bit for bit is a real requirement,
and a different one from wanting the better answer.

Fixed point rather than exact rationals, and the reason is worth stating: a
lookup table is a *sampled* function, so the error from sampling a smooth
transform at thirty-three points is orders of magnitude larger than anything
`Fixed` loses. Exact rationals here would be precision spent where it cannot be
observed, paid for with a greatest common divisor on every operation, per
pixel. Where exactness *is* observable it survives anyway and is checked rather
than assumed: a sample on a lattice point comes back untouched because every
weight is nought or one, and a neutral input stays neutral because the three
channels compute the same thing.

`.cube` is the file a colourist hands over — Adobe and Iridas's text format,
and the one every grading system reads and writes. It has traps, and each one
gets a test that fails when the trap is sprung.

**Red varies fastest**, and reading it the other way transposes the cube. A
transposed grade is not a crash or a garish mess; it is a *plausible* picture
with the wrong look, which is the worst kind of wrong for a format to be.

**The numbers are decimal text**, which means they can be read *exactly*:
`0.123456` is `123456/1000000` and nothing is lost. Going through a binary
floating-point type would throw that away on the way in, for no reason, in a
project with no floating point anywhere else.

**`DOMAIN_MIN` and `DOMAIN_MAX` are not always nought and one.** A table
authored for another input range, applied as though the range were the unit
interval, is the wrong look on every pixel — silently. So the domain is read
and refused by name rather than skipped like an unknown key, which is exactly
what a lenient reader would do.

**Output values may leave nought to one and must not be clamped**, because a
look can send a highlight above white on purpose. **`LUT_1D_SIZE` is a
per-channel curve in the same file extension** and is refused rather than read
as a cube. And **scientific notation is refused rather than guessed at**: files
in the wild carry it, saying so plainly is better than reading `1e-3` as one,
and adding it is a small change on the day a file that needs it turns up.

One limitation is the format's rather than the reader's, and it is recorded
because it is an argument for something this project already does. `.cube`
carries no length and no digest, so a file truncated *inside its final number*
still spells three numbers and is accepted — `200.0 0.0 200.0` cut to
`200.0 0.0 200.` even has the same values. `SPRJ`, `SPRW` and `SPPK` all refuse
*every* prefix, because they all carry both. An interchange format without them
has a class of corruption that is undetectable by construction, and a `.cube`
is therefore untrusted input rather than a record.

### A clip carries a grade

A clip holds the **digest** of a look rather than the look, for exactly the
reason the render graph names media by digest: the same grade in two projects
is the same grade, a project-local handle would cache it twice, and a file
swapped underneath a handle is a different look wearing the same name. It is
also the only shape the layering permits — a cube lives in
`media-editor-render`,
which sits *above* the model, so holding one in a clip would invert the
layering as well as the size.

It travels. Through a trim, through a slip, through a save, and through a
split — which gives *both* halves the look, because otherwise joining them back
would not give the clip that was there and join is defined as the exact inverse
of split. Two clips of one piece of media, adjacent in its source and graded
differently, are refused rather than joined: they are two shots with two looks,
and joining them would keep the first's and discard the second's without saying
so.

`SPRJ` moves to version six, a flag byte per clip and thirty-two more when
there is a look. A flag rather than a fixed field, because most clips have no
grade and an all-zero digest is a real digest of something rather than a spare
value to spend on "none".

The render node that reads it is `Node::Look`, which names its look by digest
and fetches it from the library exactly as a source names its media. A grade
edited between two renders is therefore a different cache key rather than a
stale hit — the fault that never gets reported as a bug, because it looks like
the grade not having been saved.

A graded layer is fetched **straight** and associated afterwards. A look is a
non-linear function and on premultiplied samples computes `f(αc)` where
`α·f(c)` was wanted; `over` is only correct on premultiplied ones. The two want
opposite things, so the frame arrives the way the look needs it and
`Node::Associate` puts it back — which loses nothing, because it was never
premultiplied. Unpremultiplying one that had been would, which is why
`Look::apply` refuses rather than doing it quietly.

### Which space a look is applied in

The decision that was left open, now made. A look is authored **for** an
encoding: a show LUT built for a camera's log curve expects log-encoded values,
a display LUT expects display-referred ones. The cube carries no record of
which — it is 35,937 triples — so applying one to the wrong encoding is the
wrong look on every pixel, with nothing crashing and nothing to compare
against.

So a `Look` pairs a table with the description it was authored for, and a frame
that does not match is refused by name. Converting the frame first is the
caller's decision to make and to name, and `convert` is how it is made.

**Not in linear light, and that is not an inconsistency.** The compositor works
in linear light and nowhere else, because `over` is a statement about how much
light reaches the eye and that sentence is only true of light. A lookup table
is the opposite case: it is a sampled function of *code values*, authored by
somebody looking at code values, and decoding to light first would feed it
inputs it was never sampled at. Both rules come from the same place — apply an
operation in the space its definition is written in — and they point in
different directions because the two definitions do.

**Straight coverage only.** A table is a non-linear function applied per pixel,
and on premultiplied samples that computes `f(αc)` where the answer wanted is
`α·f(c)`. Those agree only when `f` is linear or `α` is one — and full coverage
is exactly what a test made of opaque bars would cover, which is how the same
mistake reached the conversion path once already. Unpremultiplying here would
be worse than refusing: it is lossy, so doing it silently would spend a real
quantity of the caller's picture on a step nobody asked for.

**State: done.** M6 is now what its opening paragraph claims.

This milestone said "done" while 3D LUT support, which its own opening
paragraph lists, did not exist in a single line of code. It also carried two
"State:" lines that disagreed with each other. Both are corrected above. A
roadmap that overstates is worse than one that is behind, because the second is
a schedule and the first is a wrong map.

## M8.1 — Conform

*Requires nothing new.*

M3.5 taught this application to read and write CMX 3600. This teaches it to
read and write a **cut** — the layer that knows a track from a channel, a clip
from an event, and a dissolve centred on a cut from one written as a frame
count beside an incoming source.

It is one claim, stated as a theorem rather than as a hope: **if an export
leaves nothing behind, writing its list, parsing it, and importing the result
produces a sequence equal to the one that went in.** Equal by `PartialEq` on
the whole value — not a hand-written comparison of the fields somebody
remembered to check, which is the version of this test that passes forever
after the thing it should have compared stops being compared.

### What is lost and what is refused

That theorem needs an honest account of when it does not apply, and the line
it draws is this milestone's one real decision.

A grade, a fader, an automation curve — each of those *decorates a cut that is
still correct without it*. They are counted and reported, and the list is
written. A second picture track is not a decoration: the format has one video
channel, so a second track's pictures would be written as though they replaced
the first's, and what comes back is a different programme rather than a plainer
one. That is refused.

The test of which side a thing falls on: after a lossy export the frames are in
the right order at the right times and only look wrong; after a refused one
they would not have been.

Five things are reported rather than refused — grades, automation curves,
faders including mute, tracks with no clip on them at all, and a track's
trailing gap. Each has two tests: that it is reported, and that a sequence
carrying it really does come back different. A reported loss that turned out to
be no loss would be a warning nobody should heed, and a warning nobody heeds is
worse than none.

### The reel name is the source digest

Eight characters is what the format holds, so the reel is the first eight
characters of the media's content digest and the whole sixty-four goes in the
`FROM CLIP NAME` comment. Three things follow, and the third was a surprise.

The comment is how an import knows what a source *is*. Without it there is
nothing to look up, so it is refused rather than invented — which means this
conforms lists **this application wrote**. Anyone's list still parses; matching
somebody else's reel names to a media library is a question for a person, and
M8 is where a person gets asked.

The reel and the comment are then two statements about one fact, so when they
disagree the list is refused — the same rule the parser already applies to
drop-frame being stated by both the `FCM` line and the punctuation.

And two sources whose digests agree in those eight characters are one reel name
for two pieces of media. Thirty-two bits is not an identity; the export refuses
rather than writing a list that nothing, including this, could read back
correctly.

### A dissolve, twice

The model centres a dissolve on its cut and opens half its length, rounded
down. CMX writes one as a frame count on the *incoming* event, whose record in
is where the dissolve begins — so the outgoing event stops early, by exactly
that opening, and the frames between are stated by the dissolve rather than by
either event.

Both directions are exact, including for an odd count: twenty-five frames opens
twelve before the cut and thirteen after, and the list leans the same way or
the picture moves by a frame. The model's own bounds are what make the export
total — a dissolve is never longer than its neighbours and its incoming clip
always has room for handles, both checked when it is added — so there is no
arithmetic here that can run off an end, and no branch for one that no test
could reach.

### The rate is still not in the file

M3.5 said the parser will not invent a rate, and it does not: it labels every
timecode at thirty and says so. Which means a label arriving from it carries a
rate that is a *placeholder*, and an importer that asks it for a frame number
as it stands counts a twenty-four frame cut at thirty and stretches the
programme by a quarter.

This module did exactly that on its first run. What caught it was that the
round-trip test goes through the **text** — export, write, parse, import. The
tests that handed the exported list straight back to the importer all passed,
because that is not a round trip; it is a comparison of a value with itself.

So every label is relabelled at the rate the caller stated before it becomes a
number, and a label that cannot count at that rate — a frames field of 27 in a
cut said to be at 24 — is refused rather than taken. There is one backstop
beyond that, and it is not in the file: the cut is built out of media whose
rate the library knows, so a film cut conformed as PAL is refused by the model.
The file has no objection to make. The project does.

**State: done.** Not done: matching another system's reel names to a library,
which needs a person; wipes, which need a shape (M4.5); and lists at rates
whose frames field passes thirty — 48, 50 and 60 write correct files that this
parser cannot be told how to read, which is checked here rather than merely
admitted.

## M8.2 — Shapes

*Requires nothing new.*

M4.5 recorded why wipes were not built: *a wipe needs a shape and a shape needs
a rasteriser*. This is that rasteriser, and it is the same primitive a mask
needs, so it is built once and used twice.

### Coverage is an area, not a sample

The usual rasteriser asks whether a pixel's centre is inside the shape and gets
a jagged edge, or asks at sixteen sub-positions and gets seventeen possible
answers where a byte holds two hundred and fifty-six. This one computes the
**exact area** of the pixel square lying inside the shape, as a rational, and
quantises once at the end.

That is worth the arithmetic for a reason that shows up immediately in motion.
A wipe is one straight line crossing a whole frame over a second, so the same
edge is drawn at a thousand slightly different offsets. Sampled coverage makes
the edge *crawl* — each pixel flips between two values at a different moment —
while an exact area makes it slide, because the area under a line is a
continuous function of where the line is and a sample is not. The difference is
not subtle and it is not a matter of taste; it is the difference between an
edge that looks like an edge and one that looks like a staircase shivering.

A convex shape is an intersection of half-planes, and coverage is found by
clipping the pixel square against each in turn and taking the shoelace area of
what survives. One edge is a wipe, four are a rectangle, many are a mask.

### Two implementations, one of them to be checked against

There is also a closed form for the single-edge case — the area of a unit
square under a line, in three lines of arithmetic — and the rasteriser does not
call it. It exists to be compared against, pixel for pixel, over whole frames
at six orientations: axis-aligned both ways, both diagonals, and two shallow
angles. Two implementations sharing no code and agreeing on all six hundred and
forty-eight pixels is a far stronger statement than one implementation and a
test written after reading it. It is the same argument [`crate::lut`]'s
trilinear path makes, for the same reason.

A third check comes free from a shape that allows one: for an axis-aligned
rectangle the covered area of a pixel is exactly how much of its width overlaps
times how much of its height does, and the general clipper has to agree with a
product it never forms.

And the strongest of them is a relationship rather than a bound: **a shape's
coverage summed over the picture is the exact area the shape encloses in it.**
A rectangle of 3½ by 3¾ sums to exactly 105/8. Any per-pixel error at all — a
wrong partial, a double-counted edge, a column off by one — moves that total.

### A wipe needs no operator of its own

Nor does it, and for the reason a dissolve does not. `faded` scales a
premultiplied frame by one opacity; `masked` scales it by a different one at
every pixel. Mask the incoming layer with the coverage plane, put it `over` the
outgoing one, and the result is `in x coverage + out x (1 - coverage)` — a
cross-fade whose fraction varies across the picture instead of over time.

### What it refuses, and what it costs

Every coordinate is an exact rational and every clip is exact, so denominators
grow: an intersection point is a ratio of products of the inputs and the
shoelace area multiplies those together again. A shape whose arithmetic will
not fit is **refused by name**. There is no floating point to fall back to,
which is the point rather than a limitation.

Two things were written down here as load-bearing and turned out not to be, and
both are now recorded as what they are. That the boundary line belongs to the
region is a decision **with no consequence in this module** — making it strict
changes no coverage anywhere, because a line has no area and the clipper puts
back as a crossing point exactly the corner a strict test would drop. And the
early exit when a polygon has been clipped to fewer than three vertices is an
optimisation, not a correctness guard: the shoelace sum over two vertices is
already nought. Both are kept, and both say so.

**State: done**, for the geometry. Not done here: wipes in the *model*, which
need a transition to say what kind it is — **M8.3**. A soft edge was recorded
here as "a much larger case analysis"; that was **wrong**, and **M8.4** says
why and builds it. And CMX wipe pattern numbers stay
refused on import, now for a sharper reason than "no shape": the shape exists,
and the mapping from a machine's pattern number to it is a convention of that
machine rather than anything the file states.

## M8.3 — Wipes

*Requires nothing new.*

M8.2 built the geometry; this gives it somewhere to live. A `Transition` used
to be a boundary and a length, and reading one back could only produce a
dissolve because a dissolve was the only kind there was. It now says which.

### A wipe is a dissolve that spends its fraction differently

That is the design in one sentence, and everything follows from it. A wipe and
a dissolve at the same cut with the same length are **timed identically**: the
same frames have both clips on screen, and the exact fraction that decides how
far a dissolve has faded decides how far a wipe's edge has travelled. A test
compares the two stacks frame by frame across a whole programme and requires
them to agree about everything except what the fraction is *for*.

So a dissolve spends the fraction on the incoming layer's opacity, and a wipe
carries it. Both of a wipe's clips stay **whole** — the incoming one is not
half-faded, it is entirely there behind an edge — and anything that spent the
fraction twice would show it through the outgoing clip on the covered side.

A track's automation still multiplies, because that is a different question: a
wipe inside a fade has the transition deciding which side of the edge a pixel
is on and the track deciding how much of the whole track is showing, and either
alone throws the other away. The same argument M4.6 makes about a dissolve
inside a fade, met again and answered the same way.

### The direction is a vector, not an angle

An angle needs a sine and a cosine, and neither is exact. A rational vector
needs nothing. Every interface that offers an angle can turn it into a vector
at the moment somebody sets it, so the approximation — if there is one —
happens once at the edge of the system rather than on every frame of every
render.

It also makes the wipes anybody actually uses exact: straight across is
`(1, 0)`, straight down is `(0, 1)`, and a true diagonal is `(1, 1)` rather
than a rounding of forty-five degrees. The vector's *length* carries no
meaning, because the transition's fraction sets the edge's position — so
`(2, 0)` and `(1, 0)` are the same wipe, which is a test, and a normalisation
creeping in would need a square root and would not be exact.

Both ends are reached exactly, by geometry rather than by arithmetic: at nought
the edge sits on the corner the direction points away from and covers nothing,
at one it has passed the opposite corner and covers everything. A transition
showing a sliver of the incoming clip before it started would waste the same
frame the dissolve's fraction is shaped to avoid.

### The graph carries the edge, not the plane

`Node::Wipe` holds four rationals — a direction and a fraction — rather than
the coverage plane they produce. A node's identity is a digest over its
parameters, and a plane of a million bytes would hash into it at a million
times the cost, for a value that is a pure function of the four numbers and the
frame's own size. So the plane is computed at evaluation and the node stays
small enough to be a cache key.

### What moved in the file

`SPRJ` goes to **version seven**. A transition writes its boundary, its length,
then a **tag**, then whatever that kind needs — a wipe's two rationals, a
dissolve's nothing. The tag is before the parameters rather than after so that
a third kind adds a tag rather than changing what the bytes after the duration
mean; version six wrote no tag at all, which is exactly the shape that makes
adding a second kind a break rather than an extension.

An unknown tag is refused by name. Reaching that check needs a **resealed**
file — the byte sweep in this format's tests proves the digest refuses a
mutated byte long before any field is parsed — so the test recomputes the
payload digest around the changed byte, which is what a reader has to survive
being handed by something that meant it.

The slate's golden digest moved, and only because of the version. That was
checked rather than assumed: encoding one transition-free project under six and
under seven produces files of the same length differing in **exactly one byte,
at offset four**. The digest the slate prints is over the whole file, header
included, so a version bump moves it and a payload that did not change is still
a payload that did not change.

### Conforming a wipe

CMX has a wipe event, and it names the shape by a **pattern number** — a
convention of whichever machine wrote the list rather than anything the format
defines. Inventing one would write a file naming a shape this application
refuses to read back, so a wipe is exported as the dissolve it is timed like:
every frame lands where it belongs and the edge is gone.

That is a decoration by M8.1's own line — the cut is still correct and only
looks wrong — so it is reported in `LeftBehind` rather than refused, with the
same two tests every other reported loss has: that it is reported, and that a
sequence carrying it really does come back different.

**State: done**, and **M8.4** adds the soft edge this section listed as
missing. Still not done: wipes with a shape other than a straight line, which
the rasteriser can already describe and the model has no vocabulary for; and
importing somebody else's wipe patterns, which needs a table of one machine's
conventions rather than anything derivable.

## M8.4 — Soft edges

*Requires nothing new.*

M8.2 left a soft edge out and gave a reason: it is "the area weighted by a ramp
rather than a plain area", which is "a much larger case analysis". **The reason
was wrong.** It is two clips and a moment, it is exact, and there is no case
analysis in it at all.

### Why it is exact

**The integral of an affine function over a polygon is its area times its value
at the polygon's centroid.** That is the definition of a centroid rather than a
result about it.

A soft edge is a linear ramp between two parallel lines. The ramp is affine.
The region it runs over is the pixel square clipped by those two half-planes —
convex, because the complement of a half-plane is a half-plane, so the machinery
M8.2 already has produces it. And a convex polygon's area and centroid are
rational. So the coverage of a pixel is the area the near line already covers,
at full weight, plus the area of the slab between the lines weighted by where
in it that slab lies. Exactly.

One arithmetic nicety: the *first moments* — area times centroid — are what the
answer needs, and they are computed without ever dividing by the area. The
division would only be undone by the multiplication that follows, and not doing
it keeps the arithmetic one step shorter and leaves no degenerate case when a
slab has no area.

### What it is checked against

The properties are equalities between rationals, not tolerances.

**A soft edge and its complement still partition every pixel exactly.** The
ramp is symmetric, so what it takes from one side it gives to the other — at
every pixel, not on average over the picture. That is what lets a soft wipe be
built from one plane and its complement without the two disagreeing about a
code value somewhere along the edge.

**A pixel entirely inside the band is the ramp at its centre.** The integrand
is affine over the whole square there, so the integral is the value at the
square's own centroid — a closed form to check the moment arithmetic against.

**Softening an axis-aligned wipe conserves the total coverage exactly.** The
ramp is antisymmetric about its centre line, and over a rectangle the
cross-section along an axis-aligned sweep is the same everywhere, so the two
sides cancel: softness moves coverage around the picture without creating or
destroying any. The condition is real and is stated — a diagonal sweep meets
triangular cross-sections near the corners, the sides do not cancel, and the
total genuinely moves.

**And a vanishing band already agrees with the hard edge.** Nought delegates to
the hard path rather than dividing by a band of nothing, and it would be easy
to assume that delegation papers over a jump. It does not: a band a thousandth
of the travel wide produces the same plane byte for byte, so the soft path
*converges* on the hard one. That started as a control, which passed — and the
reason it passed was worth more as an assertion than the control was as a
control.

### Softness is a fraction of the travel

Not a distance in pixels. It means the same thing at every size and every
angle, and a project conformed to a different frame size keeps the look
somebody set rather than a pixel count that no longer corresponds to it.

`SPRJ` goes to **version eight** — a wipe now writes three rationals rather
than two. Version seven lived for exactly one commit, which is the right
outcome rather than an embarrassing one: the alternative was to redefine what
seven means, and a version whose meaning changed is worse than a version
number nobody spent. The format test uses a softness of **one third**, which is
the value that catches any storage that is secretly binary.

**State: done.** **M8.5** puts the same coverage machinery on a clip as a
mask. Not done here: a soft edge whose ramp is anything but linear —
a smoothstep is not affine, so the centroid identity does not apply and it
would need genuine numerical integration, which is the point at which this
would stop being exact and would have to say so.

## M8.5 — Masks

*Requires nothing new.*

The rasteriser has been able to describe a convex region since M8.2 and the
model had no vocabulary for one. A mask is to *where* what a grade is to
colour: a property of the clip, carried by the layer stack, applied by the
renderer — the same exact-area coverage a wipe is made of, pointed at a clip
instead of at a transition.

### In fractions of the frame

Every corner is a fraction of the width and height, so a mask drawn on a proxy
is the same mask on the finish and a project conformed to another size keeps
the shape somebody drew rather than a pixel count that no longer means
anything. A corner outside nought to one is *not* refused: a mask whose points
sit off the frame is how an editor says "all the way to the edge and past it".

The winding is not the caller's problem either. A polygon's edges point inward
on one winding and outward on the other, so this measures which from the sign
of the polygon's own area rather than demanding one. Getting that wrong inverts
the mask, which is the single most confusing failure a mask can have, and it is
a test: the same shape given both ways round is the same plane.

Inverting flips the **byte**, not the shape. Rasterising the complement instead
would quantise twice and the two sides could sum to two hundred and fifty-six —
the same trap the wipe records, avoided the same way, and asserted at every
pixel.

"Hard" means no feather rather than aliased: the coverage is still an exact
area, so a diagonal mask edge lands on partial values rather than on a
staircase.

### Convex, and what that costs

A concave outline is **refused**, not repaired. Taking its convex hull would be
a different shape, drawn by nobody, and impossible to notice until something
went to air. A concave mask is a union of convex ones and the union is not
built; the refusal says so.

A corner that does not turn at all — three points in a line — is allowed,
because it describes the same region and refusing it would refuse a rectangle
somebody built by dragging a fifth point onto a side.

**Softness is absent, and the reason is about cost rather than possibility** —
which is a distinction this project learned to make one milestone ago. A soft
polygon's ramp is a *minimum* of affine functions, and a minimum of affine
functions is not affine, so the centroid identity that makes a soft wipe exact
does not apply directly. It is still exactly computable: the region where each
edge is the nearest is itself convex, so the band partitions into pieces each
with an affine ramp. That is O(n²) clips per pixel, and it is deferred on that
number rather than on any claim that it cannot be done.

### What it cost the model

`Clip` and `Item` were `Copy`, and a mask owns a list of corners. They are
`Clone` now. Fourteen sites needed a `.clone()` or a borrow, all mechanical,
and the alternative — a fixed array of thirty-two corners on every clip,
masked or not — would have been a kilobyte per clip inside a sixty-four
kilobyte arena to preserve a trait nothing needed.

`SPRJ` goes to **version nine**: a clip writes a mask flag before its grade
flag, then the shape if there is one. The slate's file grew by exactly three
bytes, one per clip, and that was **measured against the previous commit in a
git worktree** rather than reasoned about: one clip grew by one byte, three by
three, seven by seven.

A file describing a shape the model would refuse is refused too, because the
reader goes through `Mask::new` like everything else. Reaching that check needs
a resealed file *and* the right bytes — a corner is two rationals, so an
eight-by-eight square's third corner is the words `8, 1, 8, 1`, and pulling it
in to `(3, 3)` is what turns a square into an arrowhead.

### And a reference capture, at last

`docs/VERIFICATION.md` has said since its first version that a reference frame
is kept beside its hash *so a failure can be looked at, not just counted*. The
writer landed several milestones ago and no reference was committed,
deliberately: the only frame the freestanding image composites is sixteen
pixels wide, and 16×9 is not a picture.

The host has no such bound. `tests/golden/reference.png` is three hundred and
twenty by a hundred and eighty: colour bars underneath, a ramp and a flat
colour above them meeting at a **soft wipe**, both inside a six-sided **mask**.
On a mismatch the test writes what it actually rendered next to the reference
and says where — which is the whole point of keeping a picture rather than a
hash. A digest says something changed; two files side by side say what.

The first version of it showed nothing, because both sides of the wipe rendered
the same test pattern and a wipe between two identical pictures is invisible.
That is the fixture lesson for the fifth time, so the capture now asserts that
the two sides differ and that a band of partial values lies between them.

**State: done.** Not done: soft masks, as above; concave masks, which need the
union; and a mask that moves, which needs the corners to be curves rather than
values.

## M8.6 — One asset per digest, and where it came from

*Requires nothing new.*

This milestone exists because a paragraph in the
[architecture](ARCHITECTURE.md) was read and checked. It said a media asset
carries a **location hint** and a **probed description**; the type had neither,
and the second one the *layering* forbids — `media-editor-model` and
`media-editor-media` are siblings, so an asset can never hold a
`FrameDescription`. The same block listed a transition as a kind of item, in a
document that explains at length why a transition is not one.

Chasing the missing hint found a real bug, and it is the more serious half of
this milestone.

### Two identifiers could name one digest

`Project::add_media` inserted whatever it was given, so a project could hold the
same content twice under two identifiers. That quietly **falsified the conform
round trip**: an export writes a source's digest, an import looks it up, and
with two candidates it finds the first — so a sequence cutting the same footage
under two identifiers came back pointing at one of them, with `LeftBehind`
reporting that nothing was lost.

The theorem was stated three milestones before the case that breaks it was
tried. It was not wrong about anything it tested; it was wrong about the world
it assumed.

The fix is what content addressing already meant: **one asset per digest.**
Adding the same content again gives back the identifier it already has. The
same bytes described two different ways — one length, then another — is a
contradiction and is refused, because a project whose record of an asset
depends on which order two files were opened in is a project that cannot be
reasoned about.

A file is different from the API here, and deliberately. `add_media` handing
back an existing identifier is right for a caller opening the same file twice
and *wrong* for a reader: a file listing one piece of content as two assets
would leave every clip indexing the second record pointing at the first, which
is a different programme arrived at silently. So the format refuses it by
name.

### A hint, and the word is load-bearing

The digest is what the media *is*; the location is only where somebody found
it. The model never resolves it, never opens it, and never compares two assets
by it — the same content found in a second place is the same content, and
adding it again does not rewrite the record, because moving a hint is its own
operation and doing it as a side effect of opening a file would edit a project
nobody asked to edit.

It is **bytes rather than text**. A path is whatever the platform says it is,
and Phipia's is not decided yet; refusing to interpret it is what keeps the
model free of the operating system, and it means a format that round-trips only
the paths it can read is not a format that loses somebody's media. A hint with
no bytes in it is refused, because a hint that says nothing looks like an
answer.

**Relinking is a hint moving, and nothing else.** Pointing a clip at different
bytes is different media and the digest says so, so there is no operation here
that swaps one piece of content for another while keeping its name — which is
the operation that silently changes what a programme is.

It is **not in the undo journal**, and that is a limitation rather than a
decision. The journal applies edits to a *sequence* and the media library
belongs to the project; making a media change undoable means the journal
becoming project-level, which is a larger change than a hint is worth. The
previous hint is returned instead, which is the whole of what undo would do.

`SPRJ` goes to **version ten**: every asset writes a length and then its hint,
four bytes when there is none. Measured against the previous commit rather than
reasoned about, again — one asset grew by four bytes, three by twelve, seven by
twenty-eight.

**State: done**, and **M8.7** makes offline media renderable. Not done: media
that is scanned or probed, which needs somewhere to scan.

## M8.7 — Offline media

*Requires nothing new.*

A project opens when the drive is not mounted. Until now a clip whose bytes
were missing failed the whole render, which is what makes an editor unusable on
the day it matters most.

### The fallback is in the planner, not in the graph

That is the only interesting decision here, and it is forced.

A source node's identity is a digest over the media, the tick and the
description — and **not** over whether the bytes happened to be reachable. So a
node that quietly fell back to a slate while evaluating would put that slate
into the cache under the real picture's key, and hand it back once the drive
came home. That is exactly the staleness a content-addressed cache exists to
prevent, met from the inside.

So `Library` gains one question — *can this be read at all* — asked **before**
a graph is built. An unavailable source is never named in the graph; the
planner emits a slate instead, and nothing is ever cached under a key that
lies. The test that pins it renders twice through **one pool**, absent and then
present, and requires the second to be the picture.

The trait's default answer is `true`. A library that cannot tell should try:
failing at `frame` is a worse answer than refusing to plan, and a default of
`false` would make every implementor that had not thought about it render
nothing at all.

### What offline looks like

Diagonal stripes, at an angle no camera produces. Not black, because that is
what an empty timeline shows; not a solid colour, because a programme might
legitimately contain one. Either would let "the drive is not mounted" look like
footage.

The stripe period is a **fraction of the frame** rather than a pixel count, and
that came from a test failing. A fixed sixteen-pixel period is a solid colour
on anything narrower than sixteen pixels — which is precisely the case where a
slate must not be mistaken for a shot of a red wall, and precisely the size the
freestanding image renders at. It is checked at five sizes from four pixels
across to full HD, and checked for running down *and* across, because a pattern
that varied along one axis only would be bars, which is a thing programmes
contain.

**State: done.** Not done: saying *which* media is missing, which is text on a
frame and text needs a font; and a per-clip record of the last time a source
was looked for, which is a cache rather than a project fact.

## M8.8 — Resampling

*Requires nothing new.*

Scaling a clip is the most-used operation in an editor after cutting, and the
one where "looks about right" hides the most. This is the arithmetic; putting a
transform on a clip is the next milestone, and saying so is the point — M6 once
claimed done while a named part of it did not exist.

### Two decisions, both already made elsewhere

**In what space?** Linear light. A resampled pixel is a *weighted average of
what was there*, and an average only means something over quantities that add —
which light does and code values do not. Averaging encoded values makes a
reduced picture darker than the one it came from, most visibly on fine bright
detail against dark.

**Straight or premultiplied?** Premultiplied, and nothing else. Averaging
straight samples across an edge mixes the colour of pixels that are barely
there with the colour of pixels that are fully there at equal weight, which is
the dark fringe around every badly keyed title. In premultiplied form a
transparent pixel contributes nothing to the colour sum because its colour *is*
nothing.

Both are the compositor's arguments, arrived at independently and landing in
the same place — which is the point of having written them down once.

### The map runs backwards, and inverting it is exact

A caller says what they want — twice the size, shifted right — which is a
*forward* map. A resampler needs the opposite: for each destination pixel,
which part of the source landed on it. So the mapping takes the forward map and
inverts it, exactly, because a rational two-by-two inverse is a determinant and
four divisions. A map that squashes the picture onto a line has no inverse and
is refused rather than producing pixels drawn from regions of no area.

### Two filters, and choosing is the caller's

**Area** gives every destination pixel the exact area-weighted mean of the
source it covers — the correct answer for *reduction*, because a destination
pixel really is standing in for a region and this is that region's mean. Point
sampling instead is what makes a reduced picture shimmer as it moves.

It is also, honestly, a poor filter for *enlargement*: a destination pixel
falling entirely inside one source pixel gets that pixel and nothing else,
which is nearest-neighbour with extra arithmetic. **Bilinear** is for that.

Choosing between them is the caller's rather than a heuristic's, because a
heuristic keyed on the scale factor would change a picture's look at the moment
somebody dragged past a hundred per cent.

The area path reuses M8.2's clipper: a destination pixel's preimage under an
affine map is a parallelogram, and the area a source pixel contributes is that
parallelogram clipped against the pixel square. The **check** is a product of
one-dimensional overlaps in the axis-aligned case — which the resampler never
forms, so agreeing with it is a check by something sharing no code.

### The extent and the reconstruction are different questions

This one came out of a failing test and is the finding worth keeping.

Outside the picture's *extent* there is nothing: no colour and no coverage, so
a picture scaled smaller than its frame arrives surrounded by transparency
rather than by its edge column smeared outwards forever.

But the samples sit at pixel **centres**, so the outer half-pixel of a picture
lies beyond every sample while still being inside the picture. Treating that as
"outside" made the last column of an enlarged picture fade out — a real edge
artefact, and the kind nobody notices until two versions of a shot are
compared. There the reconstruction clamps to the edge sample, which is the best
estimate of a signal past its last measurement.

**State: done**, for the arithmetic; **M8.9** puts it on a clip. Not done:
bicubic or Lanczos, which need a kernel wider than two samples and would be
the first thing here whose weights are not exact.

## M8.9 — Transform

*Requires nothing new.*

M8.8 built the resampler. This gives it a clip to move. A transform is to
*geometry* what a grade is to colour and a mask is to extent: a property of the
clip, carried by the layer stack, applied by the renderer.

### Dimensionless, and about the centre

The linear part is dimensionless — twice the size is twice the size at every
resolution — and the translation is in **fractions of the frame**, so a project
cut on a proxy and finished at four times the size keeps the framing somebody
chose rather than a pixel count that no longer means the same thing.

And it acts about the frame's **centre**. Scaling about the corner is what the
arithmetic does if nobody decides otherwise, and it sends the picture sliding
off to the lower right the moment somebody drags a scale slider — which is not
what anybody means by "make it bigger".

### An angle is not a matrix

There is no rotation-in-degrees, for the reason the wipe's direction is a
vector: a sine and a cosine are not exact. The linear part is four rationals,
and an interface offering a dial converts once when somebody turns it. The
pleasant consequence is that the transforms people actually use are exact — a
half, a third, a mirror, a quarter turn.

A **mirror** is a transform and not a refusal, which is worth a test of its
own: a mirror has a *negative* determinant, and it is the zero one that has no
inverse. Confusing the two would refuse flipping a shot.

### Where it sits in the chain

Between the grade and the mask, and both halves of that are decisions.

**After the grade**, because a look is a function of colour and resampling
averages colours: grading the average is not the average of the grade, and the
table was written for the clip's own values rather than for whatever a scale
produced.

**Before the mask**, because a mask is in *frame* coordinates. Moving a clip
moves the picture through a stationary mask, which is what a garbage matte and
a split screen both want. A mask that travelled with its clip would be a
different feature, and it would have to say so.

### The identity is skipped, not computed

A clip nobody has transformed goes through **no resampler at all**, and the
test counts nodes in the plan rather than comparing pixels. Exact is a stronger
promise than "the arithmetic works out", and it is the promise a project
deserves after being opened and saved a hundred times.

`SPRJ` goes to **version eleven**: a clip writes a transform flag, then the
filter, four rationals and a move. Measured against the previous commit
again — one byte per clip, at one, three and seven.

**State: done**, and **M8.10** makes a transform change over time; **M8.22**
animates the mask and **M8.24** turns both, exactly, which the "an angle is not
a matrix" paragraph here had ruled out and should not have. **M8.25** takes the
anchor, which turned out to be a second pair of rationals *and* a measurement
about which space they may be added in. Not done: an anchor that animates.

## M8.10 — Motion

*Requires nothing new.*

M8.9 gave a clip a framing. This lets the framing change over the length of the
clip it is on — the push-in, the reframe, the slow drift across a still.

The whole milestone is one decision made twice, and it is not the arithmetic.

### The curve goes on the clip

M4.6 opened by naming three things a curve is for: "opacity that fades, **a
scale that pushes in**, a volume that ducks under dialogue." Two of the three
got lanes. The third was deferred with a reason, and the reason was that a
curve on an item would need "a name for a keyframe that survives its item being
renumbered".

That is a real problem for a curve stored *beside* the items, in some lane that
refers to them by index — insert an item and every reference after it is wrong.
It is not a problem at all for a curve stored *on* the clip, because there is
no index: the animation is a field of the thing it animates. A ripple that
renumbers every item after this one renumbers nothing here.

Worth recording as a mistake rather than a change of plan. The deferral was not
wrong about the difficulty of the design it had in mind; it was wrong to treat
that design as the only one, and a reason attached to a deferral is a claim
like any other.

### Measured from the clip's own start

Which is the other half of the same decision, and the half that has
consequences. A keyframe at tick twelve is twelve ticks into *this clip*, not
twelve ticks into the programme. So sliding a shot down the timeline slides its
push-in with it, and "move this shot later" and "re-animate this shot" stay two
different gestures — which they would not be if the curve were measured from
the programme.

The consequence is that a **cut re-bases the tail**. Split an animated clip at
tick twenty-four and the tail's start is twenty-four ticks later than the
original's, so its keyframes move back by that much and the ones before the cut
go *negative*. Dropping them would be the obvious tidy and it would be wrong: a
curve holds its first value before its first keyframe, so the pair straddling
the cut is exactly the pair that says what the tail's opening frames do, and
discarding half of it would flatten a move already underway into a hold. A
keyframe at a negative instant is not a keyframe before the programme began. It
is one before *this clip* began, which is an ordinary thing for a cut to
produce.

And **join is the exact inverse**, as it is for everything else here: two
clips join only when the second's animation is the first's re-based onto the
second's start. Anything else is refused rather than reconciled, because
joining would keep the first's move and discard the second's without saying so.

### A scale and two moves, not four curves

The base transform holds the *shape* — a mirror, a quarter turn, a shear — and
the motion scales and moves it. Not the whole linear part: a matrix that
changes over time is four curves that can disagree about whether the picture is
still a rectangle, and the shear that produces is a different feature with its
own name.

A motion requires a base transform, and the base cannot be taken off a clip
that has one. Both directions, because a guard on one gesture that is missing
from its opposite is not a guard — it is a door somebody has not tried yet.

The scale is refused at nought or below when a keyframe is set. It is refused
*again* when it is read, and that second check is not redundant: an ease's
verticals are deliberately unclamped, so an overshoot between two positive
scales can pass through nothing on its way. The test for that guard uses an
ease that actually reaches it, because a guard is only checked by an input that
gets there.

### The renderer did not change

Not one line, and that is the design rather than a happy accident. The layer
stack hands out a **resolved** transform: by the time a frame is described, a
motion has already become the framing it reads at that moment. A renderer told
about curves would need a clock, and a node that depends on a clock is a node
whose cache key is a lie.

There is a test for that claim — an animated clip and a still one at the same
framing must plan the same graph node for node and render the same picture byte
for byte — and a third render at a *different* framing beside it, because the
first two would agree just as well if the framing were being dropped on the
floor.

`SPRJ` goes to **version twelve**: a clip writes a motion flag and then, if it
is set, three curves in the format's existing shape — scale, across, down. An
absent lane stays absent rather than becoming a curve holding its neutral,
because a save that grew two curves nobody drew would be a save that lied.
Measured against the previous commit: one byte per clip, at one, three and
seven.

### The field that made an item too big to ignore

Adding seventy-two bytes of lanes to a clip pushed `Item` past clippy's
`large_enum_variant` threshold: 288 bytes for a clip against 24 for a gap — 304
against 24 since M8.17 added fades. The
lint is right about the numbers and its remedy is not available — boxing the
clip is an infallible allocation, and this crate allows none that is not both
bounded and fallible (R-5.1, R-5.2), while `Box::try_new` does not exist.

So the exemption is written down with its argument, as an `expect` rather than
an `allow` so the day it stops being needed the build says so. And the number
it argues from is pinned by a test, because the cost of an item is exactly the
kind of number that grows one field at a time with nothing watching. The
argument itself is worth stating: the lint warns because a vector of the small
variant pays for the large one, and a track is mostly clips — the waste is on
the *minority* variant, against a clip's own bytes that have to live somewhere
whatever this enum looks like.

**State: done.** Not done: an animated mask, an animated grade, and an anchor
point — the three remaining places where a parameter is a value where it could
be a curve.

## M8.11 — Trims

*Requires nothing new.*

Two operations every editor has and this model could not perform, and one that
turned out to be there already.

### A roll is not two trims

`RollCut` moves a cut: the outgoing item runs on and the incoming one starts
further into its source, so the material either side is unbroken and **the
programme is exactly as long as it was**. That last clause is the whole
operation. Doing it as two trims changes the length of the programme in
between, which moves every position after the cut and then moves it back —
and a state nothing downstream should ever be able to observe is a state
something downstream will eventually observe.

`SlideItem` moves an item along its track and does not touch the item at all:
same source, same length, same grade, same everything. Its neighbours give and
take to make room. So it needs a neighbour on both sides — an item at either
end of a track has nothing to absorb the difference, and inventing a gap to
absorb it would be a different edit performed silently.

**Both are their own inverses** with the sign turned round, which is why
neither carries a saved copy of what it replaced. That is not a convenience;
it is the reason these belong in this model rather than beside it.

### And the one that was already there

There is no ripple delete, because there is nothing to ripple. A track stores
no positions — an item's place is the sum of the lengths before it — so
removing an item closes the hole by construction, and a second operation that
closed it would be closing a hole that never opened. It is now pinned by a
test rather than left as a property somebody would have to notice.

### A dissolve is about exactly what a trim changes

A transition carries two conditions: it may not outlast either neighbour, and
the incoming clip must start far enough into its media for half a dissolve of
handle. Both are statements about the lengths and in points a trim exists to
change.

So the check came out of `add_transition`, where it had been living as if it
were a question asked once, and is now asked again by every trim — of the
items the trim is *about to write*, before it writes any of them. Checking
against what is on the track would pass and then let the write break it.

Only the dissolves touching something that changed are re-checked. Checking
all of them would be correct and would make a trim's cost the number of
dissolves on the track, on an operation that runs on every frame of a drag.

**State: done.** Not done: a trim that is allowed to move a dissolve rather
than being refused by it; a three-point edit, which needs a notion of a
selected range rather than a new operation on a track; and trimming across
tracks at once, which needs a notion of which tracks are enabled.

## M8.12 — Type

*Requires nothing new.*

"Offline media renders a slate but cannot say which media is missing — that is
text on a frame, and text needs a font." That line has stood in the risk
section for three milestones. This is the font.

### Why it had to be written

A font could not be taken from anywhere. Every outline format worth reading is
a parser and a hinting engine — a large one, with a long history of memory
faults, and no way to load a file on a platform that does not yet mount a file
system. Every free face worth shipping is somebody else's licence to reconcile
with GPL-3.0-only. And a bitmap face would have to be drawn at every size a
title might be set at, which is the opposite of what this project does about
resolution everywhere else.

So Media Editor writes its own, and what it writes is decided by what it already
has.

### A glyph is disjoint convex pieces

The shape rasteriser has computed the **exact area** of a pixel inside a convex
region since M8.2. A letter is not convex. But a letter *is* a small number of
convex pieces, and the area of a union of pieces whose interiors do not overlap
is the sum of their areas — exactly, with no reasoning about antialiasing at
all.

So every glyph is authored as pieces that **touch but never overlap**, and
coverage is their sum. That is not a convention the drawing hopes for. It is
measured: for every pair of pieces in every glyph, the exact area of their
intersection, which must be nought. And it is enforced a second, cheaper way —
`quantise` refuses a coverage above full, so a face whose pieces overlapped
enough to fill one pixel past one is *refused* rather than drawn wrong. The
pairwise measurement is the stronger of the two and catches an overlap far too
small to fill a pixel.

The consequence worth having is that this face is exact at every size. There is
no bitmap, no hinting, no grid fitting and no size at which it stops being the
same letter — a title at 4K and the same title on a proxy are the same shape,
measured the same way. A test asserts the form of that: a glyph at twice the
size covers exactly four times the area.

### What it does not do, and why each

**No curves.** Every piece is a straight-edged quadrilateral or a triangle. A
curve would be a polygonal approximation of a curve, which is a decision about
how many segments — a number that would have to be defended, would change with
size, and would make "the same shape at every size" false.

**No lowercase.** An x-height, ascenders and descenders are a second set of
metrics, not more of the same work. The first thing this face draws is a slate,
which broadcast sets in capitals.

**Monospaced.** The first things drawn are a timecode and a digest, and a
proportional face makes a counting number dance in place while it counts. It
also makes disjointness *between* glyphs free rather than authored: the advance
is wider than the drawing box, so no two letters can share a pixel.

**No substitution.** A character the face cannot set is refused by name. A face
that drew a box would put a rectangle on a slate and call it a message; one
that drew nothing would put a gap there. Both are a picture that says something
other than what it was given, which is the one thing a slate must not do.

### The design grid, and the two computed coordinates

Half-units, sixteen to the em. A glyph is drawn in a box ten half-units wide
and sixteen tall, with a stroke two thick, and the pen advances twelve. Every
design coordinate is an integer on that grid, so the face is exact rational
data rather than a table of decimals somebody rounded.

Two coordinates are not integers, and they are the interesting ones: the
crossbar of an `A` meets two slanted legs, so its own sides *are* those legs'
facing edges, evaluated at the bar's top and bottom. Authored by hand it would
either leave a hairline or overlap, and a hairline is invisible until somebody
sets the letter large.

### And it is kept as a picture

Every other test here measures a number, and all of them would pass on a face
whose letters were the wrong letters — a `G` drawn as a `C` is a perfectly
disjoint, exactly rasterised, entirely wrong glyph. So the whole repertoire is
committed as `tests/golden/specimen.png` and compared byte for byte, with what
it actually drew written beside it on a mismatch. A face is the one thing in
this project whose correctness is a judgement by eye and cannot be anything
else.

**State: done**, and **M8.13** puts it on a frame. Lowercase is **M8.15**, and
the reason given here for deferring it — that it is a second set of metrics
rather than more of the same drawing — turned out to be right. Not done:
curves, kerning, and a face at more than one weight.

## M8.13 — Legend

*Requires nothing new.*

M8.12 wrote the face. This puts it on a frame, and closes the sentence that
has stood in the risk section since M8.7: **offline media renders a slate but
cannot say which media is missing**.

### It says the digest, not the file name

Because the digest is what the clip refers to. A file name is a hint that may
have moved, and two clips pointing at one digest are pointing at one thing
whatever they were called when they were imported. Eight characters of it,
which is what a person reads off a screen and types into a search — and the
same prefix the conform module already writes into a reel name, so a slate and
an edit decision list name the same thing the same way.

### Two captions, because a proxy has a real choice to make

A legend carries the whole sentence *and* the part that matters, and neither
is right at both sizes. `MEDIA OFFLINE 4F3C9A21` needs a frame three hundred
pixels across to be read; the eight characters that identify the clip need a
hundred and sixty. Setting the long one small enough to fit a proxy is a grey
smear, and dropping straight to nothing throws away what somebody actually
needs.

So: the whole sentence, then the part that matters, then **nothing at all**. A
slate whose caption cannot be read has told the viewer something false about
how much it knows, and the stripes underneath already say the one thing that
matters. Below the floor the frame comes back byte for byte unchanged, which
is a test.

The floor is measured rather than chosen. A stroke is an eighth of the em, so
nine pixels puts it just over one; at four it is half a pixel and the whole
face is grey, which another test measures.

### The one arithmetic claim

The type is premultiplied **in light**, through the same conversion every other
layer goes through. Writing the coverage byte into the colour channels is the
obvious way to build white type and it is wrong everywhere the coverage is
partial, by exactly the amount the transfer curve bends. It looks like a
slightly thin font rather than like a bug.

The test does not need the curve's numbers to catch it: the wrong arithmetic
makes the colour code equal the coverage code at every pixel, and the right one
makes it strictly greater wherever there is partial coverage. It also had to be
told where to look — over an *opaque* field the composited alpha is 255
everywhere, so the first version of that test found nothing partly covered at
all and was reading the wrong plane.

### What the node carries, and what it does not

The words, and nothing else. No size, no place, no colour. Everything a
caption's look could be is a decision that would have to be the same on every
slate to be worth anything, and a parameter nobody varies is a parameter that
goes wrong in one place and nowhere else.

The words *are* in the identity, length-prefixed, so two slates that say
different things are two cache entries and the same slate twice is one. Without
that a timeline with two clips offline would show the first one's digest on
both.

**State: done**, and **M8.14** makes text something a project contains. Not
done: a caption whose look somebody chose, which is what a title is and is why
that is a different node.

## M8.14 — Titles

*Requires nothing new.*

M8.12 wrote the face and M8.13 put a caption on a slate. This makes text
something a **project contains**: a card somebody wrote, at a size and a place
somebody chose, saved in the file and cut like anything else.

### A title is media

Not a new kind of item. Not a property of a clip. An **asset**, which a clip
cuts from exactly as it cuts from a recording.

That is the whole milestone, and everything below is a consequence of it.
Trimming, rolling, sliding, splitting, joining, dissolving, wiping, grading,
masking, transforming and animating a title all work already, and not one of
them had to be told what a title is. A title that were its own kind of item
would need every one of those written a second time, and would get one of them
subtly wrong — probably the one nobody thought to test.

What separates a title from a recording is only where its frames come from,
and that is one enum on the asset. The *planner* acts on it, not the graph: the
same decision M8.7 made about offline media, for the same reason. A node that
chose for itself whether to fetch or to draw would be a node whose cache key
did not record which it had done.

### Named by what it says

A title's digest is the digest of its own description. That is not a
convenience; it is what content addressing already meant. The same card in two
projects is the same card, two clips of it share one cached frame, and changing
a word makes a **different asset** rather than quietly changing what every clip
of it shows.

Two consequences fall out. A title **cannot be relinked** — there is nothing to
find, and a location hint on one would be inviting somebody to point a card at
a file, which would be a different asset the moment it was opened. And a title
is **never offline**: the library is not asked whether it has one, because
asking about a file that does not exist would get "no" and put a slate where
somebody's card should be.

The file writes the digest anyway, as it does for every asset — and then checks
it. A title is named by its description, so the two are one fact written twice,
and a file where they disagree has been edited. Recomputing and accepting
whichever came out would silently repoint every clip of the card.

### White, and the reason is not laziness

*Left as it was written, because M8.19 answers it rather than contradicting it
— and because a record of what was believed at the time is worth more than a
record edited to have been right.*

A title here is white. Not because colour is hard to store, but because three
bytes in a model that has never held a colour would be three bytes in *which
encoding* — and answering that is the colour pipeline's job, with a name on it,
rather than a silent assumption in a struct (R-8.3).

The answer that costs nothing is already built. A clip carries a grade, and a
grade is a lookup table applied in the encoding it was authored for. **To
colour a title, grade it.** One mechanism doing two jobs, rather than two that
have to agree.

### No floor, unlike a caption

A caption's size is chosen *for* the reader, so `caption` refuses to set one
too small to read. A title's size is the editor's own decision, and a program
that quietly declined to draw somebody's card because it judged it too small
would be worse than one that drew it. So `title` has no floor and no ceiling,
and a card placed off the frame is placed off the frame.

The size is a fraction of the **height** and the place is fractions of both, so
a card laid out on a proxy is the same card on the finish — the same argument a
transform's move makes, and the same reason a mask's corners are fractions.

`SPRJ` goes to **version thirteen**: every asset writes a source tag, and a
title writes its words and its three fractions after it. Measured against the
previous commit — one byte per asset, at one.

**State: done**, and **M8.16** makes a card say more than one thing.
**Superseded in part by M8.19**, which gives a title a colour after all — the
argument above was right that a colour must not be three bytes, and wrong that
the only way out was a grade. Three fractions of light are not bytes.

Not done: a title that *animates* — which wants the treatment a clip's framing
got in M8.10, with the curve on the asset rather than on the clip.

## M8.15 — Lowercase

*Requires nothing new.*

M8.12 wrote the face and deferred lowercase with a reason: "an x-height,
ascenders and descenders are a second set of metrics, not more of the same
work." That reason was **right**, which is worth saying because the last two
deferrals examined here were not.

A capital is one measurement. It runs from the cap line to the baseline and
there is nothing else to say about where it sits. Lowercase needed three more
numbers before a single glyph could be drawn:

- an **x-height** the bodies sit on, at ten of the sixteen half-units — much
  smaller and the lowercase reads as small capitals, much larger and the
  ascenders stop being visible as ascenders;
- **ascenders**, which reach the cap line: `b d f h k l t`, and the dots of
  `i` and `j`;
- **descenders**, five half-units below the baseline: `g j p q y`.

And a fourth that only exists once there are descenders: **line spacing**. Set
at the em — which is what "line height equals font size" means everywhere it
is offered — every `g` in one line goes through every `A` in the next. This
face needs the em plus the descender plus a half-unit of air.

### The metrics are checked, not written down

Four numbers in a header file are four numbers nothing enforces. So each is a
claim a test measures against the glyph data: every capital starts at the cap
line and sits on the baseline, every lowercase body without an ascender starts
exactly at the x-line, every descender reaches exactly the descender line, and
nothing at all reaches outside the two. A letter that drifted off its own line
would still draw, still be disjoint, still be exact — and would set a line of
type that sat on nothing.

The twenty-six new glyphs were disjoint on the first run, which is what the
pairwise-intersection test is for and the first time it has had nothing to say.

### What it unlocks

A title card that can set a **name**. That is what titles are usually for, and
a face that could set a slate and not a name would have been a face for slates.

**State: done.** Not done: curves, kerning, a second weight, and accented
letters — which are a fifth metric (where the accent sits above the cap line)
and a decision about whether they are their own glyphs or a mark composed onto
one.

## M8.16 — A card that says more than one thing

*Requires nothing new.*

A lower third is two lines. An end card is a handful. M8.14 shipped a title of
one line, which is the shape a slate has and not the shape a card has.

### Two questions kept apart

Where the *block* goes, and how the lines sit inside it. They are separate on
purpose: a left-aligned card dragged across the frame stays left-aligned, and
that is only true if moving it changes the block's place and nothing else.

So a title carries an alignment — left, centred or right — and its `across` and
`down` place the **middle of the block**. A two-line card straddles the point
it was placed at rather than hanging below it, which is what anybody dragging a
card to the centre of the frame means by the centre.

Alignment lines up the **boxes** the glyphs are drawn in, not their ink. An `I`
has its own bearings well inside its box, and chasing the ink would make a
left-aligned column start in a different place depending on which letter each
line happened to begin with — the same argument centring already makes.

### The leading is the face's, and that is not a shortcut

Lines stack at [`font::LINE_SPACING`] rather than at the em. Set at the em —
which is what "line height equals font size" means everywhere it is offered —
every `g` in one line goes through every `A` in the next.

Here that is not a matter of taste. Two overlapping lines cover a pixel twice,
the coverage sums past full, and the rasteriser **refuses** — so a card set too
tight would not be drawn badly, it would not be drawn. That makes the test for
it unusually clean: a card of descenders over capitals either draws or is
refused, and there is no third outcome for the test to be wrong about.

A leading *control* is deliberately not offered. It is a typographic preference
where a correct default exists, and every parameter is a thing to keep true in
the model, the format, the node's identity and the planner.

### Named by all of it

A card's digest already covered its words; it now covers **every line, each
with its own length, and the alignment**. Two cards whose lines concatenate the
same — `"AB"`, `"C"` against `"A"`, `"BC"` — are two cards, and without the
per-line length they would have been one asset with two meanings.

A blank line among others is fine, and is how a card puts air between two
stanzas. A card where *every* line is blank is refused, because that is a gap.

`SPRJ` goes to **version fourteen**: a title writes a count of lines, each line
with its length, and an alignment tag. Measured against the previous commit —
**no change at all** for a project with no titles, which is right: the cost is
per card, not per asset.

**State: done.** Not done: a leading control; a card whose lines are set in
more than one size; and a title that *animates*, which wants the treatment a
clip's framing got in M8.10, with the curve on the asset rather than the clip.

## M8.17 — A fade on a clip, and the bug it found

*Requires nothing new.*

A dissolve sits at a cut and needs two clips. The first item of a programme has
nothing before it — so until this, the most ordinary thing an edit does,
bringing a programme up from black, was the one thing this model could not
describe.

### It is a different thing from a transition

A transition is about *two* clips and belongs to the boundary between them. A
fade is about one clip and belongs to it. Where they meet they multiply, which
is a test.

And it rises from **nought** on the clip's own first frame and falls back to
nought on its last. That is not how a dissolve behaves and the difference is
the point: a dissolve's fraction never reaches nought or one, because a frame
at either end would repeat a neighbour. A fade from black *is* the black, and a
first frame that showed the picture would not be a fade from anything.

Where a clip's two fades meet, the smaller wins. Adding them would pass more
than the material has; multiplying them would dip in the middle of a clip
nobody asked to dip.

A trim shorter than the fades on it is **refused**. Clamping them would
silently re-time somebody's fade and dropping them would silently remove it,
and neither is what a trim was asked to do.

### Picture and sound, and why they are applied differently

For picture the fade folds into the layer's opacity and reaches the compositor
as one node — one rounding and one cache entry rather than two, because the
product of two rationals is a rational.

For sound it scales the **samples**. A fader is a position in decibels and a
fade is a fraction of the material, and converting one into the other's units
would mean a logarithm at every sample for a number that only takes two values
a block. So the fade scales the buffer and the fader scales the source, exactly
as a mask scales a picture and an opacity fades one.

The pair of fractions a block of sound needs is *this clip's* fade one tick on
— not whatever the timeline shows there. Reading the next frame off the track
instead made every unfaded clip duck to silence over its last block, which the
mixdown tests said within a minute of it being written.

### The bug it found

`composite::faded` and `composite::masked` scaled a premultiplied layer's
colour in **code values**. The compositor's own module header has said since
its first version that "a premultiplied sample is the encoding of `light ×
coverage`, not the encoded value scaled by coverage" — so the convention was
written down and two functions broke it.

Nothing caught it because every fixture faded a layer that was **black**, where
nought times anything is nought and the two arithmetics agree exactly. That is
the third time this project has been bitten by a test over black, and it is
recorded in [Verification](VERIFICATION.md) as such.

What found it was a fixture that had never existed: a dissolve between two
*identical* pictures, which has to be that picture and instead sagged by
twenty-eight code values in the middle — a visible dip in every dissolve
between two shots of anything.

The correction moved three hand-derived numbers, and each was re-derived rather
than accepted: the wipe's edge pixel from 154 to **205**, the mask's channels
from `[100, 50, 25]` to **`[147, 72, 34]`**, and the slate's `picture red` from
73 to **98**. The wipe's comment had also drawn the wrong moral — it said the
linear answer was the darker one, which was the bug talking rather than a fact
about light.

One more thing had to be enforced: the colour goes through a curve and the
coverage through an integer multiply, and the two roundings land a fraction
apart — so a sample sitting exactly at its coverage can come out a code value
above it, which `checked_premultiplied` refuses and is right to. The colour is
held under its own coverage, which is not a clamp for safety; it is what
premultiplied means.

`SPRJ` goes to **version fifteen**: a flag per clip, and two lengths when it is
set. Measured — one byte per clip, at three.

**State: done.** Not done: a fade shape that is not linear, which would want
the curve machinery and a decision about what an *ease* on a fade means at the
frame it reaches nought; and a fade on a *track*, which is a different thing
again and is what the automation lane already does.

## M8.18 — Retiming

*Requires nothing new.*

A clip plays its media at a speed somebody chose. The clip keeps its length on
the timeline; what changes is how much media it consumes to fill it. A half is
slow motion, a two is fast, and a negative runs the media backwards out of the
in point.

### The speed is an exact rational, and that is not decoration

A clip at `24/25` is the standard pull-down. A clip at `0.96` is a rounding of
it that drifts a frame every twenty-five seconds — slowly enough that nobody
notices until a delivery, which is the worst speed for a fault to have. An
exact fraction is a speed two builds cannot disagree about, and it is the same
reason the wipe's direction is a vector rather than an angle.

### The size decides how far, the sign decides which way

`source_at(offset)` is the in point plus or minus `floor(offset x |speed|)`,
and splitting it that way rather than flooring `offset x speed` directly is a
decision. Flooring a negative ramp rounds the other way, so a clip reversed at
half speed would give `100, 99, 99, 98` — the in point once and everything
after it twice — against the forward `100, 100, 101, 101`. Same speed,
different frames, for no reason anybody could point at. Written this way a
reversed clip shows exactly the frames its forward twin shows, and there is a
test that holds the two the same distance from the in point at every offset.

The floor is the floor a sample position already takes, and for the same
reason: a tick names a frame rather than a moment, so a position part way
through one is that frame and not the next.

### What a speed reaches

Everything that asks a clip where it is in its media, which turned out to be
more places than the field itself: the layer stack's ordinary arm *and* its
dissolve arm, the tail of a split, the join that asks whether two clips
continue into each other, and the end the library checks against the asset's
length. Each has its own control, and the dissolve arm's found a gap in the
tests rather than in the code — the two arms had one control between them,
because the anchor that was meant to name one of them was a substring of the
other.

A reverse that would read before its media is refused **when the speed is
set**, not at the frame that reads it: the editor who set it is the one who
can do something about it. A speed of nought is refused outright — it would
show one frame forever and consume no media, which is a freeze, a different
edit with a different name. Sound is refused at any speed but real time, by
name, until there is a resampler to pitch it with; a silent pitch shift would
be worse than a refusal.

### A guard that could not be made to fail

`source_at` began with an arm for real time, on the grounds that a clip nobody
retimed should be added to rather than multiplied. Its negative control could
not be made to fail: a speed of one takes `floor(offset x 1) = offset`, so the
general path gives the same answer on every input. A guard whose absence
changes no answer is a guard no test can hold, and it went. That is the fourth
time this project has found a guard duplicating one further in.

### And two numbers that had gone stale

The speed is sixteen bytes on every clip, which put `Item` at 320 — exactly the
ceiling `tests/size.rs` was holding, so the field that arrived was the one that
spent the last of the slack. Raising a ceiling is allowed; raising it without
saying what ate the room is not, and the constant now says.

`Edit` crossed clippy's `large_enum_variant` threshold at the same time, since
`InsertItem` carries a whole item and must — a `RemoveItem`'s inverse is the
`InsertItem` that puts back what came out, and nothing else in the journal
remembers it. The remedy clippy names is boxing, which R-5.2 forbids. So the
exemption is written out with its argument, and the width is pinned by a test.

Pinning it surfaced something else. `MAX_HISTORY` is 4096, and an entry is a
pair of edits at 336 bytes each — 2.6 MiB against the nineteen mapped pages a
Phipia program is given. The constant is not the bound that bites there, and
reading it as a promise of four thousand undos would be reading it wrong: what
arrives is `OutOfMemory` from the fallible reservation, not `CapacityExhausted`
from the policy. The two deserve different words, and the arithmetic is now a
test so the day the platform grows the ordering is re-checked rather than
assumed.

`SPRJ` goes to **version sixteen**: a flag per clip, and a rational when it is
set. Measured — one byte per clip, at three. The image went from 90 pages to
**91**.

**State: done.** Not done: a speed that *changes* over a clip, which is a curve
and a different mapping — the offset would integrate rather than multiply, and
`source_end` would have to sum it; and retimed sound, which needs a resampler
that does not exist yet and is named by `ModelStatus::SoundCannotBeRetimed`
rather than left to happen.

## M8.19 — A title's colour, named in light

*Requires nothing new.*

M8.14 shipped titles white and argued the case: "three bytes in a model that
has never held a colour would be three bytes in *which encoding* — and
answering that is the colour pipeline's job, with a name on it, rather than a
silent assumption in a struct (R-8.3). **To colour a title, grade it.**"

The argument was right and the conclusion was wrong, and the way out is in the
argument itself. The problem was never storing a colour; it was storing a
colour *as bytes*. An [`Ink`] is three fractions of **full light** — the domain
the compositor already works in and the domain every transfer table converts to
and from. A colour named that way means the same thing in sRGB, in Rec. 709 and
in a linear working space, and the encoding happens once, at the frame, through
that frame's own table.

Grading a title still works and is still the right tool for a *look*. It was
the wrong tool for "make this caption yellow", which needed a lookup table
authored in an encoding and a file on disk to say one thing about one card.

### What "in light" is worth, in one number

sRGB bends. Half of full light is **188**, not 128 — and a colour picked in
code values would be a colour whose meaning changed the moment somebody
delivered in a different range. The same ink is 255 in a full-range frame and
235 in a limited-range one, and a mid-grey is 188 in the first and **177** in
the second. Every one of those four numbers is derived from the definition in
the test's own comment, not read back out of the code.

The ink is in the title's digest, so the same words in two colours are two
assets, two cache entries and two pictures — the same reason the size and the
alignment have been in there since the card had them.

### The bug it found, and the bug it did not

Type was packed as `u8::MAX` regardless of what the frame was encoded in, and
255 is not a legal code value in limited range. The first draft of this
milestone said so in the roadmap, the tests and the commit message, and then a
negative control refused to fail.

What measurement said is that the fault is real in one of the two places type
is drawn and absent in the other, for a reason neither prose had considered:

- A **card**'s letters come from a hard-edged stencil — the coverage plane
  holds nought and full and nothing between. The only premultiplied samples
  are full light at full coverage and none at none, and `encode(decode(255))`
  searches only the legal codes and lands on 235. The illegal byte was clamped
  away on the way out and nobody ever saw it.
- A **slate caption** is antialiased. A partly covered pixel premultiplied
  from a code of 255 holds `encode(decode(255) x coverage)`, and `decode(255)`
  in limited range is about 1.09 of full light — so the sample sits above the
  ceiling its own coverage allows and `checked_premultiplied` refuses it on the
  way into `over`. A limited-range slate did not draw a slightly wrong caption.
  It failed, by name, with `NotPremultiplied`.

So the legend asks the table for white too, even though a legend carries no
colour and never will: what a caption's *look* is remains not somebody's
choice, but "white" is still a code value and it is this frame's to spell.

Both facts now have tests that say which is which, and the card's test asserts
its own premise — that the stencil has no soft edge — rather than resting on it.

`SPRJ` goes to **version seventeen**: a tag per title, and three rationals when
it is set. Measured — a titled project 183 bytes to 184, one byte, and the
slate unchanged at 286 because it has no titles. The image went from 91 pages
to **92**.

**State: done.** A follow-up commit fixed something M8.18 had left open, and
it belongs here because it is the same field's fault: a retimed clip could read
**past the end** of its media and nothing refused it. `Project::check_source`
compared the in point plus the clip's *timeline length* against the asset,
which was the same number as what a clip reads right up until a clip could be
retimed. `Edit::SetClipSpeed` was not in `validate`'s match at all.

The fix is a new question rather than a new guard. `Clip::source_span` gives
the lowest and highest ticks a clip reads, in that order, so a caller does not
have to know which way the clip runs; `check_source` takes the clip the edit
*would produce* and asks it. Four edits reach it now — insert, lengthen, slip
and retime — and each builds the candidate rather than assembling a range out
of the fields it happens to be changing. Recorded in
[Verification](VERIFICATION.md) as what it is: a caller recomputing a callee's
arithmetic, which is a copy nobody is told has fallen out of step. Measured:
the image went from 92 pages to **93**, and no behaviour changed at real time
— `start + length <= ticks` and `highest < ticks` are the same comparison
there, which is why the whole suite stayed green under the new bound.

Not done: a colour per *line* rather than per card; a ground
behind the letters, which is what a lower third with a bar under it wants and
which is a second colour and an extent rather than a second field; and an ink
that animates, which wants the curve machinery the same way a title's position
does.

## M8.20 — Freeze

*Requires nothing new.*

A clip held on one frame, for as long as somebody wants. M8.18 named this
milestone while refusing to be it: a speed of nought "would show one frame
forever and consume no media — a freeze, which is a different edit with a
different name".

### The second half of that sentence is the whole design

A freeze does **not** consume no media. It consumes exactly one frame, and
`floor(offset x 0)` cannot say so — it puts `source_end` at the in point,
which claims a clip that shows a frame reads none of it. Three things would
have believed that: a join, the library's bound check, and a reel.

So playback is two cases rather than one number:

    enum Playback { At(Rational), Frozen }

`At` is never nought, `Frozen` maps every offset to the in point, and
`source_end` for a freeze is the in point **plus one**. The span is a single
tick, which is what lets a still be held past the end of its own media: a
frozen clip's length on the timeline is not bounded by what is left of the
asset behind it. That is what a still *is*, and the library check written
against the timeline length — which this project shipped for eleven
milestones, and fixed one commit before this one — would have refused it.

### Where a freeze needs its own answer, and where it does not

Its own: **contiguity**. Two frozen clips continue each other when they hold
the *same* frame, not when the second begins where the first's one frame
ended. That is not a convenience — split a still in two and both halves hold
the same frame, so join has to accept exactly that pair or stop being split's
inverse. Two stills of *different* frames do not join, and neither does a
still beside a moving clip, even where the source arithmetic lines up.

Not its own: everything else. A freeze fades, grades, masks, moves, trims,
rolls and slides like any other clip, because it is one — the freeze is a
property of how it reads its media and nothing else looks at that.

**Sound is refused**, by the clause that already refuses a speed, and for a
sharper reason: a held frame of sound is a held *block* of samples, which is a
tone at the block rate. Silence would be a different answer, and choosing one
for somebody is what R-1.3 forbids.

### One edit, and one construction

`Edit::SetClipSpeed` became `Edit::SetClipPlayback`, because its inverse has
to be able to say either: undoing a freeze on a clip that was at double speed
must put it back at double speed, and undoing a retime of a still must put the
still back. Two variants with cross-inverses would work and would be two
things to keep agreeing.

The same argument one level down. `Edit::apply` and `Project::validate` both
need the clip an edit *would* produce, and both began by matching on the
playback to build it — one construction written twice, which is the exact
fault the source bound had one commit earlier. It is `Playback::applied_to`
now, asked for by name.

`SPRJ` goes to **version eighteen**: a third value for the tag that was
already there. Measured — **no change at all** for a project with no stills,
and a frozen clip costs the same one byte a real-time one does, because the
frame it holds is the in point the file already carries. The image is
unchanged at 93 pages.

**State: done.** Not done: a freeze that holds a frame *other* than the clip's
in point without slipping to it, which is a second field and a question about
what trimming such a clip means; and a still exported to an edit decision list,
where CMX has no way to say "hold" that this could write without inventing a
convention.

## M8.21 — A clip that animates itself

*Requires nothing new.*

Fades were the quick answer: two lengths and a straight ramp, which is what
somebody means nine times out of ten by "bring it up". This is the general one
— a curve on the clip, with whatever shape somebody drew: a hold, a linear
run, an ease. The two **multiply**, like everything else here that decides
what is on screen, so a clip can carry both and neither throws the other away.

### On the clip, measured from its own start

The same decision M8.10 made for a framing that animates, for the same reason:
there is no keyframe name to survive a renumbering, so a ripple that moves
every item after this one moves its animation with it and renames nothing. And
a cut **re-bases** the tail — carried unchanged, the animation would restart at
the cut, which is worse than not animating because the tail would still move,
just from the wrong place. Join is the exact inverse, and refuses two halves
whose curves do not line up.

An overshooting ease is **clamped at the read** rather than refused at the
edit. An ease's verticals are deliberately unclamped — an overshoot is a
useful thing for a curve to do — and a layer past full coverage is a frame the
compositor refuses. A track's automation already clamps, so this clamps: one
rule rather than two that have to agree.

Sound is refused an opacity, and not because sound cannot fade. It can, and
does. A sound clip's loudness is its track's fader and its own fade, **in
decibels**; an opacity is a coverage. One multiplies light and the other is a
logarithm of amplitude, and they are not one quantity wearing two names.

### Which animates a title, because a title is media

That is the claim M8.14 was built on, cashed in without a line of new code: a
card that fades up and pushes in is a clip with an opacity curve and a motion,
and neither had to be told what a title is. There is a test that would notice
if some path had quietly special-cased them.

### Two pages back, and not for the reason it looks like

The image *fell* from 93 pages to **91** — from a milestone that only added a
field, an edit, two functions and a lane in the file. `media-editor-model` lost
7,376 bytes and `Edit::apply` alone lost 3,447 of them.

Nothing in the diff explains that, so it was tested. On the **previous**
commit, with none of this milestone's code, twenty-four bytes of dummy padding
added to `Clip` — exactly what the opacity field costs — takes the image to 91
pages and `Edit::apply` to 16,476 bytes. The saving was bought by the clip
crossing 320 bytes: past that size the optimiser stops emitting an inline copy
of a clip in each of `Edit::apply`'s many arms and calls out instead.

A struct getting bigger made the program smaller. Recorded in
[the platform contract](PLATFORM_CONTRACT.md) and in
[Verification](VERIFICATION.md), because the tempting version — "the milestone
paid for itself" — would have been a number credited to the wrong thing.

`SPRJ` goes to **version nineteen**: a curve per clip, written the way an
absent lane has always been written. Measured — four bytes a clip for a count
of nought, and twenty-five more for a clip with one keyframe on it.

**State: done**, and the two places it named are now both filled: **M8.22**
animates the mask and **M8.23** the grade. Not done: a shape for the simple
fade, which would now be the same curve machinery reached a second way and
wants a decision about which of the two an editor's "fade" gesture writes.

## M8.22 — A mask that animates

*Requires nothing new.*

An iris that opens, a vignette that breathes, a shape that sweeps a card on.
The framing has animated since M8.10 and the shape had not, which made "reveal
this from behind an edge" something this model could describe only at a cut, as
a wipe — and a wipe is between two clips. This is about one.

### A uniform scale and a move, not the corners

The same shape of answer M8.10 gave for the framing, and here the argument is
sharper than "four curves can disagree about whether it is still a rectangle".
A corner that moves on its own can turn a convex outline **concave** part way
through, and this build computes an exact area only for a convex one — so
per-corner animation would mean a refusal arriving at a *frame* rather than at
the edit that caused it. A positive scale and a translation are a similarity:
convex in, convex out, wound the same way, for every value the curves can take.

Which is why `Mask::moved_by` returns a mask rather than something that could
refuse mid-render, and why a scale at or below nought is refused where it is
set: at nought the shape collapses to a point with no area, and below it the
shape turns inside out through its own middle, which is a mirror and belongs
in the corners somebody drew.

### About the mask's own centroid, weighed by area

Not the frame's centre, which is right for a *transform* because a transform
moves the whole picture and the picture's middle is the frame's. A mask is a
shape somebody put somewhere, and scaling it about the frame's middle would
slide it toward the middle while it grew.

And the **area** centroid rather than the mean of the corners, which are the
same point only for a shape whose corners are evenly spread. A trapezoid's
corners average to `(1/2, 1/2)` and its area balances at `(1/2, 4/9)`, derived
by hand from the definition in the test that asserts it. Scaling about the
wrong one drifts a shape sideways while it grows, which reads as a bug in the
animation rather than as a choice about which point is the middle. All of it
rational, so a mask that opens to a half and back is the mask it started as,
exactly — and the sign cancels, so a shape wound either way balances on the
same point.

### Which is a text reveal

A strip from nought to a quarter, scaled about its middle at an eighth, has its
left edge at `1/8 - s/8`; moving it right by `(s - 1)/8` puts that edge back at
nought for every `s`. So the strip grows rightwards only, and at `s = 4` it is
the whole frame — a card swept on from its left edge, out of the two lanes that
were already there. That is the milestone's point, and it is why the scale and
the move are separate lanes rather than one "size" that would have to mean both.

Separate from the framing's animation, too: a mask glued to the picture is what
*tracking* wants and exactly wrong for a vignette, which should stay where it
was put while the shot pushes in. Two animations, because they are two
questions, and a test that would notice if one field had been made to serve
both.

### What it cost

`Clip` went from 344 bytes to **416** — an `Option<Motion>` is 72, and there
are two of them now — and the image went from 91 pages back to **93**. Which
is the mirror of M8.21's surprise: that milestone's 320 → 344 took two pages
*off*, and this one's 344 → 416 put them back. One threshold effect at one
size, not a trend, and the only way to know which side of it a change lands on
is to build the image and look. The size test's argument now says so.

`SPRJ` goes to **version twenty**: a tag per clip, and three curves when it is
set — exactly how the framing's animation is written. Measured: one byte a
clip, at three.

**State: done**, and **M8.23** takes the last of the three — the animated
grade — leaving no parameter in this model that is a value where it could be a
curve. **M8.24** takes the rotation, and the decision it wanted from the
transform turned out to be available exactly rather than not at all. Not done:
per-corner animation, for the reason above.

## M8.23 — An animated grade

*Requires nothing new.*

A look was applied or it was not. That made "bring this look on over the shot"
something this model could not describe at all — and it was the phrase the last
two milestones both ended on: **the last place a parameter is a value where it
could be a curve.**

### Not which look — how much of it

A digest is not a quantity. Two tables have nothing between them to
interpolate, and a grade that changed identity every frame would be a different
grade every frame rather than an animated one. What animates is the
**strength**: how far the picture has travelled from ungraded towards graded,
nought for the clip untouched and one for the look applied exactly as it always
was.

One is the value an absent curve reads, and that is the whole compatibility
story: neutral for something that multiplies is one, and neutral for a *grade*
is fully applied. A default of nought would have turned off every look in every
project ever written.

### In the table's own domain, which is the opposite of the compositor

The decision this milestone rests on. `look.rs` has said since its first
version that a lookup table is a sampled function of **code values**, and that
a look is applied in the space its definition is written in — which is why it
refuses a frame in another encoding and refuses premultiplied coverage. A
*fraction* of a look follows from the same sentence: it is a fraction of the
way along that function's own output, `c + s·(f(c) − c)`, in the function's own
domain.

Mixing in linear light would decode both sides, average them there and encode
again, which is a different picture that nobody authored. That points the
**opposite way** from the compositor, which mixes only in light because `over`
is a statement about how much light reaches the eye. Both follow from one rule
pointing in two directions, because the two definitions do.

It is a testable difference rather than a stated one, and the number is
derived from the definition by hand: a mid-grey of 128 through a table that
takes everything to black, half on, is 64 in code values. In light it is
sRGB's 128 decoded to 0.215861, halved to 0.107930 and encoded again, which is
92.374. The control that moves the mix into light lands on **92**, exactly.

Black and white could not have shown this — nought and one are the fixed points
of every transfer curve, and this project has already recorded a resampling
test that could not tell the two spaces apart for precisely that reason.

### On the clip, and the fourth lane counted that way

The same decision M8.10, M8.21 and M8.22 each made, for the third time and the
same reason: there is no keyframe name to survive a renumbering, so the
animation is a field of the thing it animates. A cut **re-bases** the tail, a
join is the exact inverse and refuses two halves whose curves do not line up,
and an overshooting ease is **clamped at the read** rather than refused at the
edit — one rule shared with the track's automation and the clip's own opacity,
rather than three that have to agree.

A strength with no grade is refused where it is set, and taking the grade off
an animated clip is refused too — the guard the mask already carries, by the
same door. One control breaks both the model's refusal and the file's, which is
what says they are one guard rather than two.

### A rounding claim that did not survive its own control

The mix is written `from + s(to − from)` and the comment said the other
arrangement is not exact at the ends. It is. Both forms are, because
multiplying by nought and by one are each exact, and a sweep over sixty-five
strengths and every code value finds no disagreement at either end — only
59,520 in the middle, each one unit in the last place, moving a quantised byte
in about one case in sixty. The form is still the right one, for a smaller
reason than the one written down, and the exactness the milestone rests on
belongs to the multiply. Recorded in [Verification](VERIFICATION.md), because
the claim was falsified by *specifying* the control rather than by running it.

### What it cost

`Clip` went from 416 bytes to **440** — one more `Option<Curve>` at 24 — and
the image went from 93 pages to **94**.

The symbol table says something the page count hides. `Edit::apply` **fell
9,599 bytes**, the threshold effect M8.21 first recorded, and its helpers took
**7,381** of that straight back: `refade` +1,257, `remotion` +1,245, `reshape`
+1,194, `slip` +1,077, `regrade` +1,009, `remove` +962, `retime` +637. When the
optimiser stops inlining a clip into each arm it does not delete the work, it
moves it into the functions it now calls. A footprint that falls when a struct
grows is a relocation rather than a saving, and no previous entry had subtracted
two of the tool's own outputs to find that out.

`SPRJ` goes to **version twenty-one**: a curve per clip, written the way an
absent lane has always been written. Measured against the previous commit in a
worktree — 203 → 207 bytes at one clip, 267 → 279 at three, 395 → 423 at seven
— which is **four bytes a clip**, and twenty-five more for each keyframe on it.

**State: done.** Not done: a strength lane on the *track* rather than the clip,
which is the grade's version of a track's opacity and wants the same argument
about which of the two an editor's gesture writes; a second look on one clip,
which is an effect *list* and is M8's real problem rather than a field; and a
look whose table is generated rather than read, which is what a primary
corrector is and which wants parameters in the model where a digest sits now.

## M8.24 — A turn, and it is exact

*Requires nothing new.*

M8.9 wrote down why this model has no rotation: "a sine and a cosine are not
exact, and a project whose framing depended on them would drift." M8.22 left a
turning mask undone and pointed back at that paragraph. The paragraph is right
about **angles** and wrong about **rotations**, and the difference is a
substitution two hundred years old.

### Every rational point on the circle, and there are many

Put `t = tan(θ/2)`. Then

```text
    cos θ = (1 − t²) / (1 + t²)        sin θ = 2t / (1 + t²)
```

which is a rational pair whenever `t` is, with `cos² + sin² = 1` **exactly**
and a determinant of exactly one. `t = 1` is a quarter turn. `t = 1/3` is the
three-four-five triangle, about 36.87°. The rational points on the unit circle
are dense, so the rotations somebody can actually ask for are dense, and not
one of them is approximated.

What follows is not decoration. A turn **composes** without renormalising —
the angle-addition formulae keep the product on the circle by an identity, not
by rounding — so a turn applied a thousand times is a turn through a thousand
times the angle and is still exactly a turn. Four quarter turns are the
identity, on the nose. A shape's area is unchanged to the last bit. A picture
turned four times through the resampler comes back **byte for byte**.

### The point, not the parameter — and the parameter, not the point

A [`Turn`] stores the pair, because `t` reaches every rotation *except* the
half turn: `(−1, 0)` sits at infinity, and a type that stored the parameter
could not turn a picture upside down. A pair that is not on the circle is
refused by name, because a pair off the circle is a scale wearing a rotation's
name and there is already a lane for scales, where it is checked for being
positive rather than smuggled in through a matrix.

The **curve** holds the parameter, for the mirror reason: `t` runs over the
whole line while the angle runs over an interval, and a curve needs somewhere
unbounded to live. A lane holding a cosine and a sine would be two curves that
can leave the circle, which is the same defect as four curves that can shear.

It is honestly not constant angular speed — `dθ/dt = 2/(1 + t²)`, so a straight
ramp sweeps fastest through nought — and constant angular speed is not
available at all: composing a fixed small turn once per frame would be exact
and is unrepresentable, because the denominators multiply. A fifth of a right
angle composed a few dozen times leaves what an `i64` holds, which is measured
in a test rather than asserted here.

### One lane, two consumers

The turn joins the motion's three existing lanes rather than becoming its own
animation, and M8.10's objection to a general matrix does not apply: a positive
scale, a rotation and a translation compose to a **similarity**, which takes a
convex outline to a convex outline for every value the curves can take. That is
the same argument the scale made alone, which is why the turn was allowed to
join it.

So one lane turns **both** a mask, about its own area centroid, and a
**framing**, on the left of whatever base transform is there — `R·M` turns the
picture as the viewer sees it, `M·R` turns the source first, and they differ
exactly when the framing mirrors, which is the case an editor actually has.

The mask turns in the **frame's own coordinates** rather than in pixels, and
that is stated rather than glossed: a mask's corners have always been fractions
of the frame, so the square from `(1/4, 1/4)` to `(3/4, 3/4)` has never been
square on screen. The space somebody dragged the corners in is the space the
shape turns in. The alternative needs the delivery aspect, which lives in a
crate this one is a sibling of and may not reach.

### The renderer did not change, and the test that says so is new

Not one line. `area_at` takes the destination pixel square back through the
inverse map and clips against the parallelogram that results, which it has done
since it was written — and its comment has claimed "however the picture is
turned" for just as long, with nothing turning a picture.

Testing that claim produced the milestone's sharpest finding. A quarter turn
sends the pixel grid onto itself, so its preimages stay **axis-aligned** and
the tilted path is never entered: replacing the preimage with the exact box
around it fails *one* test in two hundred and thirty-five, and it is neither
quarter-turn test. Recorded in [Verification](VERIFICATION.md) as the eighth
meeting with the fixture rule, in its most specific form yet — the property
that makes an answer exact is often the property that keeps the code under test
from running.

### What it cost

`Clip` went from 440 bytes to **488** — a fourth lane in each of two motions —
and the image did **not move**: 385,024 bytes, 94 pages, before and after. The
symbol table says why rather than leaving it to luck: the net change is **143
bytes**, 6,097 grown against 5,954 shrunk as the compiler re-laid the motion's
functions out, and `.text` is padded to a page boundary with room to spare.
Zero pages is a measurement here, not a rounding.

`SPRJ` goes to **version twenty-two**: a fourth lane, written where the reader
reads it. Measured against the previous commit in a worktree — a project with
no motion is 207 bytes under both versions and the two files differ in
**exactly one byte, at offset four**, and a project that carries a motion grows
by **four**, which is the turn's own keyframe count of nought. The slate, which
animates nothing, paid nothing but its version byte.

The same measurement found something else worth fixing: the byte sweeps run
over a fixture that had **never contained a motion at all**, so every lane
added since M8.10 was covered by a round trip and by nothing else. It contains
one now, with all four lanes and an ease among the keyframes.

**State: done.** Not done: per-corner animation of a mask, for the reason M8.22
gives; a turn on a *wipe*, whose direction is already a vector and which would
want the same treatment; a rotation about a point that is not the centroid,
which is the anchor question M8.9 left open from the other side; and constant
angular speed, which this says plainly is not available.

[`Turn`]: ../crates/media-editor-model/src/transform.rs

## M8.25 — The point a framing acts about

*Requires nothing new.*

M8.9 fixed it at the frame's centre with a good argument: scaling about the
corner "sends the picture sliding off to the lower right the moment somebody
drags a scale slider", which is not what anybody means by "make it bigger". The
centre is the right **default** and it is a poor **rule** — a lower third
swings in on its left edge, a card flips about the line it flips about, and
M8.24 made that sharper than it had been, because a turn about the centre was
suddenly the only turn available.

### It cannot be folded into the move, and that was measured

It looks as though it can. Acting about `a` rather than about the centre `c`
contributes `(a − c) − M(a − c)`, which is a **translation** — and the model
already passes one. So a model could add the two and the renderer would never
need to know the anchor exists.

That is true in **pixels** and false in **fractions**, and fractions are the
only space the model has: the vector from the centre to the anchor is
`(W·Δx, H·Δy)`, a linear part that mixes the components does not commute with
scaling them separately, and dividing back per axis does not undo it. The
folding is therefore exact for a scale, exact for a move, and **wrong for every
rotation** — which is to say, wrong for the case the anchor was added for.

There is a test that does the folding in fractions and compares. On an 8×4
picture a diagonal map agrees to the last byte and the three-four-five turn
does not. Both halves are asserted, because showing only the failure would not
show that the tempting version works exactly where somebody would have tried
it.

So the anchor reaches the renderer, where the pixel dimensions are known and
the arithmetic happens once.

### A default, and the two places a default goes wrong

The pivot defaults to the centre, so every project written before this renders
identically. Two things had to hold for that and each has a control.

`Transform::moved_by` rebuilds through `Transform::new`, which starts from
nothing — right in a constructor, wrong everywhere else, and this is the
**third** field to find that trap after the grade and the motion. An animated
clip now keeps the pivot its framing was given.

And `is_still` deliberately does **not** consult the anchor. The identity fixes
every point, so it fixes the anchor too; asking would send every clip with a
pivot through a resampler to compute the picture it already had.

There is no refusal. Every point is a pivot, including points outside the
frame — a card swinging in from above the picture turns about a point above it
— so a bound here would be a bound on moves rather than on values.

### A control that found a gap, and a fixture that proved the rule again

The anchor went into the node's identity as a matter of course. The control
that absorbs its across component twice — so two pivots differing only *down*
the frame collide in the cache — failed **nothing**: the field was right and no
test had ever asked it to be.

Writing that test produced the eighth meeting with the fixture rule, and the
circumstance is the recordable part: the first version drew vertical bars,
which are constant down the frame, so the assertion that the pivot's second
component matters passed without it mattering — in a test being written *for* a
control that had just found a coverage gap. Both are in
[Verification](VERIFICATION.md).

### What it cost

`Clip` went from 488 bytes to **520** — a transform now carries two more
rationals — and the image from 94 pages to **95**. The code added is **816
bytes** measured against the previous commit, and the pairing with M8.24 is
what makes that number worth having: that milestone added 143 bytes and cost
**no** pages, this one added 816 and cost one. `.text` is padded to a page
boundary, so what a change costs in pages depends on how much slack the last
page had when it arrived. "No growth" and "one page" are the same measurement
at two offsets.

`SPRJ` goes to **version twenty-three**: two rationals per transform, written
after the move they are not interchangeable with. Measured in a worktree — a
project with no transform is 207 bytes under both versions and the two differ
in **exactly one byte, at offset four**; a project that carries one grows by
**thirty-two**.

**State: done.** Not done: an anchor that *animates*, which is a fifth lane and
wants a decision about whether a moving pivot is a move or a re-framing; an
anchor on a **mask**, which turns about its area centroid and for which M8.22's
argument still holds; and a pivot expressed in the *source's* coordinates
rather than the frame's, which is what pinning a corner of a scaled clip wants
and is a different question.

## M8.26 — The razor, and the merge that undoes it

*Requires nothing new.*

`Edit::SplitItem` has cut one item on one track since the model had items, and
`Edit::JoinItems` has put two back together. Neither is the gesture an editor
makes. A blade is dragged **down** the timeline and cuts every track it
crosses; dragged back, it heals every cut it made.

### The difference is undo, not convenience

A razor performed as four splits is four entries in the history. Undo it once
and three cuts are still there — a state nobody edited into existence, and one
the person who pressed the key has to press it three more times to leave.

So a column is **one edit**. `Edit::CutAt` names an instant and a set of
tracks; its inverse is `Edit::HealAt` over the same instant and the same set.
Each is the other's exact inverse, which is what R-9.2 asks of every edit and
what a sequence of independent splits cannot give.

### The set is passed, not computed, and that is what makes the inverse exact

`Sequence::cuttable_at` answers which tracks a blade would land on. A caller
that hands the answer straight back gets the ordinary razor; a caller that
narrows it first gets a blade that cuts some tracks and not others, which is
the same gesture with a modifier held down and needs no second edit.

More importantly, the set the cut *performed* travels in its inverse. A blade
does not cut a track whose material has stopped, or one whose cut is already
there, and neither is a refusal — neither is a mistake the person holding the
blade made. If the heal recomputed the set instead of carrying it, an undo
could heal a cut the razor had not made.

A set is a `u128`: one bit per track, against a bound of a hundred and
twenty-eight tracks, and a compile-time assertion that the two numbers are one
number. It holds no allocation, so an edit can carry it (R-5.2), and it has no
bound of its own to keep agreeing with the sequence's.

### Two passes, because a column is atomic

A refused razor must leave **nothing** behind, so the edit works out the whole
answer before it writes any of it: the first pass finds each named track's
item, asks it to split, checks for a dissolve in the way and for room, and
reserves that room; the second pass only writes, and cannot fail. The
alternative — cut track by track and unwind on a refusal — needs an unwind that
is itself correct under every partial failure, which is more code in the path
that runs when something has already gone wrong.

The heal is the same shape and it is where the milestone found something. Its
control, which collapses the two passes into one, **failed nothing**: the cut
had an atomicity test and the heal had only the argument. The tempting excuse
is that healing removes an item rather than adding one so nothing allocates —
which is an argument about allocation answering a question about atomicity.
`Item::join` refuses a pair that is not one item cut in two, and a column that
healed three tracks and refused the fourth is a merge nobody can undo in one
step. Recorded in [Verification](VERIFICATION.md).

### A merge heals a cut, not a boundary

Two shots that happen to abut are not a shot that was cut, and fusing them
would lose one of them. So the condition is `Item::continues_into` — the same
one `JoinItems` uses, which already checks the media, the timebase, the grade,
the playback and all four animation lanes line up. A merge inherits every one
of those without being told about them, which is the payoff for join having
been written that way in the first place.

### What it cost

No format change: history is not saved, so an edit that adds two variants adds
nothing to a file. `Edit` went from 536 bytes to **544** and the image from 95
pages to **96** — **5,527 bytes** measured against the previous commit, of
which `Edit::apply` is 1,422 for its two new arms and about three thousand more
is its helpers growing again as the enum crossed another inlining threshold.
That is the third time that effect has shown up in four milestones, which is
what a threshold does when a struct is sitting near one.

**State: done.** Not done: a ripple cut, which closes the gap it leaves and is
a different edit; a blade that cuts only the tracks a selection names, which is
this edit with a set somebody else computed and therefore already possible; and
a merge across a **dissolve**, which is refused here for the reason `split` and
`join` both refuse it — a transition describes a boundary by index, and healing
one renumbers it.

## M8.27 — A lift, which is the other delete

*Requires nothing new.*

`Edit::RemoveItem` has been here since the model had items, and it moves
everything after the hole earlier — what an editor calls a ripple delete, or an
extract. The model has never had the other one, and half the deletes anybody
performs are the other one.

### The choice is about the rest of the programme, not about taste

A lift takes the shot and leaves the hole; nothing after it moves. An extract
closes up.

Which one somebody wants is decided by what is *elsewhere on the timeline*.
Sound cut to picture stays in sync through a lift and slides through an
extract. A title two minutes later stays where it was written through a lift
and arrives early through an extract. An editor that offers only one is an
editor making that decision on the user's behalf and not mentioning it, and
that is the kind of thing this project treats as a defect rather than a
simplification.

It composes with the razor from **M8.26** into the commonest gesture there is:
blade a shot in two, take the half you do not want, and leave everything after
it where the edit before this one put it.

### The inverse carries the shot, and checks where it is going

`RemoveItem` already sets the precedent — its inverse holds the item that came
out, because nothing else in the journal remembers it. `DropItem` is the same
and is **not** a general "replace this item": the slot has to still be a gap of
exactly that length, or it refuses. A history that could drop a shot into a
slot something else had happened to since is a history describing a project
nobody edited.

### A narrower transition check, and the fixture that could not see it

A dissolve reaches into the clips either side of its cut, so lifting one of
them would leave it mixing a **gap** — which the layer stack refuses at the
frame, and a refusal at the frame is one nobody can act on. So a lift refuses
when a transition sits on either boundary the item touches.

*Either boundary it touches* rather than `transition_from`'s "at or after", and
the difference is what an edit does to the numbering: a split inserts an item,
so every boundary after it moves and a dissolve beyond the edit lands on a cut
nobody put it on; a lift replaces in place and renumbers nothing. The coarse
check would refuse a lift at the head of a programme because somebody drew a
dissolve at the end of it.

The control that swapped the narrow check for the coarse one **failed
nothing**, because the fixture lifted an item *after* the dissolve — where the
two checks agree. A case that lifts at the head with a dissolve at the end now
exists. That is the ninth meeting with the same rule, and this time the axis
the fixture failed to vary along was *which side of the transition the edit
happened on*.

### An exemption that expired on schedule

`Edit` carried `#[expect(clippy::large_enum_variant)]`, arguing why **one**
variant is so much bigger than the rest. A lift gives it a second variant
holding an item, so there is no outlier, so the lint stopped firing — and
because it was an `expect` rather than an `allow`, **the build failed**. The
paragraph beside it had predicted exactly that, and the exemption is gone. The
size ceiling in `tests/size.rs` stayed, because it was the half doing the work.

### What it cost

No format change: history is not saved, so two more edit variants cost a file
nothing. `Edit` stayed at **544 bytes** and the image at **96 pages** —
**1,512 bytes** measured against the previous commit, which fitted inside
`.text`'s page padding, the third such measurement in five milestones.

**State: done.** Not done: a **column** lift to match the razor, and the reason
is measurable rather than a preference — its inverse would have to carry one
item per track, and a hundred and twenty-eight items at 520 bytes is 66 KiB
against the 76 KiB a Phipia program is given, for one entry in a history that
holds thousands. A ripple delete over a *span* has the same shape and the same
arithmetic. Both want an inverse that refers to material rather than carrying
it, which is a different design and not a bigger version of this one.

## M8.28 — Markers

*Requires nothing new.*

`ARCHITECTURE.md` has listed markers as planned since its first version,
beside nested sequences, "so the shape is decided before the pressure to
compromise it arrives". This is the shape, and it is a small one — which is
worth saying, because a marker is the one thing in this model that exists
**purely for the person editing**. Nothing renders it, nothing composites it,
and no clip is affected by one.

### At an instant, and absolutely

A marker names a position in the *programme*, and it does **not** move when an
item ripples. That is a decision rather than an omission: a note reading "the
sync drifts here" is about a place on the timeline, and moving it because an
unrelated shot got longer would move it away from the thing it is about.

The opposite decision — a marker that belongs to a clip and travels with it —
is a different feature with a different name, and it is one an editor wants
too. It is not this one.

Being absolute is a property of the *absence* of code, which makes it awkward
to control: there is no line to mutate. So the control **adds** the behaviour
instead — a trim that slides every note by its own change — and the test that
pins the decision fails. An invariant held by nothing having been written still
needs a test, and the mutation for it is the code somebody would one day add.

### One per instant

Two markers at one instant is the same nothing as none: neither can be named,
moved, or removed without saying which, and "which" is exactly what an instant
was going to answer. So a collision is refused — the same decision
[`Curve`](../crates/media-editor-model/src/curve.rs) makes about keyframes.

Which makes a **move** one edit rather than a remove and an add. One gesture,
one undo step, and the text does not go through the history twice for a move
that never changed it. Its inverse is itself with the ends swapped, like a roll
and a slide. And because the addition can refuse — onto an occupied instant, or
onto its own — the removal is put back before the refusal is returned, so a
move that did not happen does not quietly delete the note it was moving.

### Text, bounded in characters

The same bound a title's line has, and not by coincidence: both are text a
person types into a box, both are bounded because a hostile file must not talk
its way past a bound (R-11.2), and one number for "a line of text somebody
typed" is better than two that drift.

**Characters rather than bytes**, because a bound in bytes means something
different in every language — a hundred and twenty-eight notes in English and
sixty-four in French. The test that pins it writes the bound's worth of `é`,
which is longer in bytes than in characters and therefore the only fixture
that can tell the two rules apart.

Empty text is allowed, and deliberately: a marker with nothing written on it is
the commonest kind there is — somebody pressed the key to mark a spot and will
come back to it.

### What it cost

`SPRJ` goes to **version twenty-four**: a count per sequence, and twelve bytes
plus its own text for each note. Measured against the previous commit in a
worktree — **four bytes a sequence** at one sequence (207 → 211) and at two
(227 → 235), which is what says the step is per sequence rather than per file,
and fifteen bytes for a three-character note, at one note and at three.

The image went from 96 pages to **98** — 5,389 bytes, of which `Edit::apply`
is 1,660 for its three new arms and `decode_payload` and `encode` are 1,857
between them.

**State: done.** Not done: a marker on a **clip**, which travels with it and is
the other half of this pair; a marker with a *duration*, which is a region
rather than an instant and wants the collision rule rethought; and a colour or
a kind, which is presentation and belongs wherever the interface does.

## M8.29 — Nested sequences

*Requires nothing new.*

The other half of the sentence `ARCHITECTURE.md` has carried since its first
version. A nested sequence is a sequence used as **material**: cut into
another sequence, trimmed, graded, masked, framed, faded, and animated in all
four — a compound clip, a precomp, whichever word the editor grew up with.

### A nest is media, not an item

The one decision everything else follows from. `Item` carried *[nested
sequence planned]* in the structure block for the whole of this project's
life, and that prediction was wrong: nesting is a third
[`MediaSource`](../crates/media-editor-model/src/media.rs), beside a recording
and a title.

Which means it arrived with no new machinery, for the second time. A clip cuts
from media, so a clip cuts from a nest; the razor blades one, the lift takes
one out, a transition dissolves between two, `Motion` animates one, and not a
line of any of them knows what a nest is. That was the claim titles were built
on in **M8.11**, and it has now been cashed twice — which is the difference
between a claim and a property.

### Named by which sequence it is, not by what is in it

A recording is named by its bytes and a title by what it says. A sequence is a
thing somebody is still editing, so a digest over its *contents* would give the
asset a new identity at every keystroke — repointing, or orphaning, every clip
that referred to it. So a nest is named by its **identifier**, hashed under its
own domain tag: without the tag a nest's eight bytes could collide with a
recording whose content happened to hash the same way, and `add_media` would
refuse the second as a duplicate of the first.

Content still gets to matter, and in the place where it should. A nested clip
becomes a **subgraph** rather than a fetch, so its cache key is a function of
everything inside the nest without anybody arranging that.

### A length that is a fact about the project

A nest is the one asset whose duration is not a fact about the world. Shorten
the inner sequence and the outer sequence's clip is reading past the end of
what it reads — so `Project::apply` refreshes every nest's length after every
edit, and then rechecks every clip's source span against it.

That check runs *after* the edit, which makes the refusal path the interesting
part (R-1.4). A refused edit has to put back **both** halves: undo the edit,
and then refresh the lengths again — because the refresh ran before the check
and left the library saying a length no sequence is. The first version did only
the first half, and `shortening_a_nest_past_what_a_parent_reads_is_refused`
caught it. A partial publication is a partial publication whether the state it
leaks is a clip or a number in a table nobody looks at.

### No recursion, by name

R-5.5 forbids recursion over user-controlled structure and names nested
sequences among the places it means. Both walks are explicit:

- The model's cycle check carries its own path and refuses a repeat
  (`SequenceWouldContainItself`) and a depth past `MAX_NESTING_DEPTH`
  (`NestingTooDeep`).
- The planner's `plan` is a `Vec<Level>` stack rather than a call stack, with
  the same bound written a second time — the two crates are siblings and
  neither may depend on the other, exactly as the fader's travel is written
  twice, and `the_two_nesting_bounds_agree` is what stops them drifting.

The planner's own depth guard is unreachable through this crate's doors, since
the model refuses to build a project that deep. It is written out anyway: the
day the model's guard is wrong, the answer should be a refusal rather than a
runaway.

### Empty, not blank

The decision the render half turns on. A programme is opaque — an empty
instant is black leader, not a hole — and `Node::Blank` is what says so. A
*nest* is material, and material that is absent is absent: composited onto
black leader, a nest on V2 would blank out every track beneath it wherever it
happened to be empty, which makes nesting useless for anything but a
full-length one.

So a nest is composited onto `Node::Empty` — blank with alpha nought — and the
two nothings are two nodes with two identities and two cache keys.
`Frame::clear` refuses a format with no alpha to make one in, which is the only
way a caller could ask for the empty one and get the blank one by accident.

The cost is named rather than hidden. A nest is composited before it is used,
so it arrives **premultiplied** whatever the layer wants; grading one needs a
real unpremultiply, exact at full coverage and lossy where the nest is partly
transparent. The planner asks for that `Associate` explicitly rather than
letting the look do it quietly.

### The file had a knot in it

Three tables that each refer to the next. A nest's media record names a
sequence by position, and a clip in a sequence's body names media by position,
so the media table needs the sequences and the sequence bodies need the media.

What unties it is that creating a sequence needs only its **timebase**. So the
payload is three phases rather than two: every sequence's header, then the
media table, then the bodies. `SPRJ` goes to **version twenty-five** for it.

And the bodies are written **innermost first** — a topological order by Kahn's
algorithm, each body prefixed by the header index it belongs to. Store order
would put a parent's nested clip before the nest it reads had any length, and
the model would refuse a file it had just written. A cycle would leave
sequences the loop can never emit, and it refuses rather than looping; the
model refuses one long before a file can be written, which is the difference
between an invariant and a hope.

### What it cost

Measured against the previous commit in a worktree. **Four bytes a sequence**:
one sequence 80 → 84, two 104 → 112, three 128 → 140, and the sample project
1,505 → 1,513 — which is what says the step is per sequence rather than per
file. It is the body's header index, and every one of those bytes buys the
ordering above.

A nest's media record is **65 bytes**: thirty-two of digest, sixteen of
timebase, eight of duration, four of location length, one of source tag and
four of sequence index. Derived from `write_media`'s field list and then
measured — a two-sequence project is 112 bytes bare, 173 with one recording
(61) and 177 with one nest.

The image went from 98 pages to **102** — 16,384 bytes, the first four-page
milestone since the slate learned to name its media. Where it went:
`media-editor-model` +5,996 (`Project::apply` +2,765 for the refresh-check-undo
path and `refresh_nests` +2,032, which is new), `media-editor-io` +4,062,
`media-editor-app` +4,000 (`plan` +2,363 and `decorate` +1,494 for the level
stack), `media-editor-media` +656 for `Frame::clear`, and `media-editor-render`
**+81** — the whole of `Node::Empty`.

The io figure is worth reading twice. `read_body` shows +7,962 as a *new*
symbol while `decode_payload` fell 5,382 and `read_item` fell 3,732: −1,152
between the three, so the extraction the length lint asked for moved bytes
rather than adding them. The crate's real growth is `encode` +2,733 and
`read_asset` +2,784 — the media table and the body order, which is what was
actually built.

**State: done.** Not done: a nest that is **live** rather than snapshotted at
one instant, which is what this already is and is worth saying plainly — the
inner sequence is read at render time, so editing it changes every clip of it;
what is *not* here is a nest that renders once and caches a flattened result,
which is a different feature with a different invalidation rule. Also not done:
sound through a nest, since only `Lane::Picture` is walked; a nest of a
sequence in **another project**, which is a reference across a file boundary
and wants the relink machinery; and an interface gesture for "make this
selection a nest", which is an editing operation over a selection and belongs
wherever selections do.

## M8.30 — A speed that changes: the ramp

*Requires nothing new.*

M8.18 gave a clip an exact rational speed and ended by naming what it had not
done: *"a speed that changes over a clip, which is a curve and a different
mapping — the offset would integrate rather than multiply, and `source_end`
would have to sum it."* This is that, and the sentence turned out to be the
whole specification.

### A ramp integrates

Every parameter this model animates is read by asking a curve for its **value**
at an instant. An opacity at offset forty is the curve's value at forty; so is
a mask corner, a framing, a grade's strength.

A speed is not. A clip's position in its media is where it has *got to*, which
is the sum of everywhere it has been — the **area** under the speed curve from
the clip's start to the offset. That is the first integral in this program and
it is why a ramp is a different kind of thing from the four lanes beside it,
rather than a fifth of the same kind.

So `Curve::area_to` exists, and it is exact:

- a **hold** is a rectangle, `v × E`;
- a **straight run** is a trapezium, and the arithmetic is
  `∫ a + (b−a)e/span de = a·E + (b−a)/2 · (E/span) · E`.

The fraction `E/span` is taken *first* rather than writing `E²/(2·span)`,
because it is never above one, so the product is never larger than the area it
is computing. Squaring the tick count first would overflow a sixty-four bit
numerator on a ramp a few thousand frames long — while computing a number that
fits.

### The ease is refused, and that is the interesting refusal

Over a **whole** segment the area under a cubic Bézier is exactly rational: the
integral is a polynomial in the parameter and at the ends the parameter is
nought and one. Part way through a segment it is not — finding the parameter at
a given tick means solving a cubic, which is the cube root `EASE_BITS` exists to
approximate to one part in a million.

A clip is asked where it has got to at **every tick**. So the case that would be
exact is never the case that is asked, and answering with the approximation
would put drift into the one thing whose whole claim is that it does not drift.
A ramp's keyframes hold or run straight.

It is refused over the whole curve rather than only where the clip walks, and
that half took a control written twice to hold. An ease the clip *does* walk
through is refused by `area_to` regardless, so the guard at the door only ever
caught the case nothing else would have: an ease past the clip's last tick,
which changes no answer today and turns a later trim into a refusal at a
distance from the edit that caused it. Refusing at the door is R-1.3.

### Two more refusals, and one guard deleted

A keyframe at **nought** is refused: a speed of nought is a freeze, and a ramp
that touched it would stop consuming media without saying so.

A **sign change** is refused, and checking the keyframes is enough to know:
between two of one sign a hold stays at the first and a straight run stays
between the two, so neither shape can cross nought in the middle. (The ease
could — a handle past one is an overshoot — which is a second reason it is not
here.) A ramp that turned around would read part of its media twice and would
put `source_span`'s extremes somewhere other than at its ends, which is a
different feature with a different span check.

And a **timebase check went**. `with_ramp` had one; its control could not be
made to fail, because `source_span` asks `area_to` for the area at a tick in
the clip's own timebase four lines later, and a curve counted another way
refuses that with the same status. A guard whose absence changes no answer is a
guard no test can hold — the same finding `source_at` records about the
real-time arm it used to have. The guard went and the test stayed.

### The cut, and the bug the cut found

A ramp is measured from its clip's own start, so a split re-bases it exactly as
it re-bases the four animation lanes, and a join accepts only a pair standing in
that relation. That makes a ramp the fifth re-based lane and the first one that
decides *which frames the clip shows* rather than how they look.

Re-basing puts keyframes **below nought**, and that is what found a real bug.
`area_to` was written as one walk starting at tick nought, charging the held
first value for everything before the first keyframe. For a re-based tail the
stretch before the first keyframe is not held — it is inside a segment — so the
walk charged a rectangle where a trapezium was. It came out as six and a quarter
ticks of media on the first tail that was asked: a frame and a half of drift, at
the first cut through a ramp. It is now the difference of two walks from the
first keyframe, which is independent of where the curve happens to sit.

### What it cost

`SPRJ` goes to **version twenty-six**, and the bump itself costs **nothing** —
the sample project is 1,513 bytes under both versions, because a clip nobody
ramped writes the same tag it always did. The version moves because the tag
gained a meaning, which is R-1.2: widening is a new contract.

A ramp costs four bytes for its count and **twenty-five a keyframe** — eight of
instant, sixteen of value, one saying how it leaves. Derived from the field list
and then measured: one clip is 215 bytes at real time, 231 at a fixed speed
(the sixteen of a rational, exactly), and 269, 294 and 319 with ramps of two,
three and four keyframes.

`Playback` went from **16 bytes to 24**, and the reason is worth knowing: a
freeze used to be free. The enum was `At(Rational)` and `Frozen`, and a rational
cannot have a denominator of nought — so the second variant lived in that niche
and the whole enum was exactly one rational. A ramp is a `Curve`, which is a
`Vec` at 24. `Clip` went 520 → **528**, `Edit` 544 → **560**, and the image 102
pages → **103**: 4,096 bytes, of which `Item::split` is 3,023 — it is on the
largest-symbol list for the first time, at 5,410 — `area_to` and its walk 1,410,
and `Clip::with_duration`, `with_source`, `frozen` and `Project::apply` gave
back 2,201 between them.

And `media-editor-model` has passed `media-editor-render` to become the largest
crate
in the image — 24.4 pages against 23.4. The two were a hundred and twenty-four
bytes apart one milestone ago.

**State: done.** Not done: **retimed sound**, which is what a ramp makes urgent
rather than what it solves — the resampler `ModelStatus::SoundCannotBeRetimed`
names is still a milestone of its own, and a ramp needs a *varying* rate
conversion rather than a fixed one, which is strictly harder than the thing that
was already not done. Also not done: a ramp that **turns around**, refused here
and wanting a span check over the curve's extrema rather than its ends; a ramp
expressed as a **time map** — keyframing where the media should be rather than
how fast it runs, which is the same feature entered from the other side and
which trades exact integration for exact inversion; and **frame blending or
optical flow** across a slow ramp, which is a render feature, not a model one:
this ramp shows the nearest frame, which is what a floor means.

## M8.31 — A note that travels with the shot

*Requires nothing new.*

M8.28 gave a sequence markers and ended by naming the other half: *"a marker on
a **clip**, which travels with it and is the other half of this pair."* This is
that half, and it is the same type with a different meaning for its instant.

### What the instant is measured from is the whole feature

A sequence's marker names a position in the **programme** and does not move
when an item ripples — a note reading "the sync drifts here" is about a place
on the timeline. A clip's marker names an offset from the **shot's own start**,
like every curve a clip carries, so it moves when the shot moves, survives a
trim, goes into the bin with a lift, and is divided by a cut. A note reading
"his eyeline goes off here" is about the shot, and it should still be on the
shot after somebody moves it a minute later.

Which one an editor wants is decided by what the note is *about*, and offering
only one would be making that decision for them without mentioning it — the
same argument M8.27 makes about the lift and the extract.

Same type, same text bound, same one-per-instant rule. The rule is worth having
twice for the reason it was worth having once: two notes at one instant is the
same nothing as none, because neither can be named, moved or removed without
saying which.

### A bound paid per clip, not per project

Eight notes on a shot, against four thousand on a sequence, and the difference
is arithmetic rather than taste. A marker is 48 bytes plus up to 128 characters
— 512 bytes of UTF-8 in the worst case — so eight of them is **4,480 bytes**,
about one of the nineteen pages a Phipia program is given.

The sequence's four thousand is affordable because there is one sequence being
edited. A clip's bound is paid *per clip*, and `MAX_ITEMS_PER_TRACK` is 65,536:
at sixty-four notes each, a single track could ask for hundreds of megabytes of
notes on a machine with seventy-six kilobytes. This is the measured-budget
argument M8.27 used to refuse a column lift, reached from the other side —
except that here the answer is a smaller number rather than a refusal.

Eight notes on one shot is already more than anybody writes. Refusing the ninth
is a bound a person can hit; refusing the ninth *thousandth* would be a bound
only a generator can, which is a bound that is not doing anything.

### A trim is not a delete

There is deliberately **no upper bound** on where a note may sit. A note past
the clip's last tick is carried and not shown, so shortening a shot and pulling
it back out brings the notes back — which is what a curve on a clip already
does, held past its end rather than refused for reaching past it.

The control for this is a good example of a mutation aimed at the wrong door:
the first version dropped hidden notes in `Item::with_duration` and nothing
failed, because the test trims a `Clip` and `Item::with_duration` delegates.
Pointed at `Clip::with_duration`, the same mutation fails immediately. A
control that passes has found either a missing test or a mutation that is not
the code the test runs, and telling those apart is the work.

### The cut divides them, and the join is the one place that merges

A note **at** the cut goes to the tail. A note at tick `k` names the frame at
`k`, the head's frames are the ones below `k`, and that frame is now the tail's
first — the same half-open convention every span in this model uses.

Which makes notes the first thing a **join** has to merge rather than keep. The
five re-based lanes are all still the head's, and the head's already describe
the whole joined length; notes were *divided* by the cut, so joining without the
tail's would delete them. `Item::join` stopped being one call to
`with_duration` for the first time since it was written.

And the merge can refuse: two shots each carrying the bound's worth of notes
would join into one carrying twice it, so a join now answers
`CapacityExhausted` — which is a join refusing on a resource rather than on
contiguity, the first of those too.

### What it cost

`SPRJ` goes to **version twenty-seven**, and this one is not free: **four bytes
a clip** for a count of nought, measured on two independent fixtures — the
sample project 1,513 → 1,525 with three clips, and the slate's own file 321 →
333 with three. A note is **twelve bytes plus its text**: eight of instant,
four of length. Measured at 219 with no notes, 231 with one empty one, 238 with
a seven-character one and 262 with two of seven and twelve.

`Clip` went 528 → **552** — a `Vec` is 24 bytes whatever is in it — and `Edit`
560 → **576**. Both ceilings in `tests/size.rs` moved, 576 → 640 and 592 → 672,
because the headroom had fallen to 24 bytes for the second time in this
project's history and that is a tripwire rather than a bound. What ate it is
named in the test: a ramp at 8 bytes and a note list at 24.

The image went 103 pages → **106** — 12,288 bytes, the largest single milestone
since nesting. `Edit::apply` is 2,248 of it for three new arms, `Item::join`
1,212 (it had never been more than a call to `with_duration`), the format's two
marker helpers 2,018 between them, and `Item::split`, `Clip::noted_from`,
`without_marker`, `note` and `unnote` most of the rest. `media-editor-model` is
now
**26.6 pages** against the renderer's 23.4, which has still not moved by a byte
since nesting.

**State: done.** Not done: a note with a **duration**, which is a region rather
than an instant and wants the collision rule rethought — the same thing M8.28
left, and it is now left twice; a note that survives being **conformed** to an
edit decision list, where CMX has a comment convention this could write but
choosing which one is an interchange decision rather than a model one; and a
colour or a kind on either sort of note, which is presentation and belongs
wherever the interface does.

## M8.32 — Phipia's filesystem, and somewhere to put a photograph

*Requires nothing new.*

Written after reading Phipia 2.1.0 at commit `8fe1817` — the architecture, the
FAT32 design, the syscall ABI, the Rust boundary, the verification discipline,
the First Environment contract, and the Media Editor workspace's own C. Every
number below is somebody else's, and the file it came out of is named beside
it.

### Phipia's filesystem, stated exactly

| What | Value | Where |
| --- | --- | --- |
| A name | ASCII 8.3, case-insensitive, from `A`–`Z`, `0`–`9` and sixteen marks | `src/rust/fat32.rs` |
| A path | 127 bytes, 16 components, relative to one mount | `FAT32_PATH_BYTES`, `PHIPFS_MAX_DEPTH` |
| A directory | 64 live entries | `PHIPFS_MAX_LIST_ENTRIES` |
| A volume | 256 directories, 129,022 clusters of 512 bytes | `docs/FAT32.md` |
| A file | 16 MiB | `PHIPFS_MAX_FILE_BYTES` |
| Open at once | 16 generation-authenticated handles | `PHIPFS_MAX_HANDLES` |

None of that is a criticism. It is what a filesystem costs when every mount
walks the complete live tree, checks both FAT copies sector by sector, and
refuses irreconcilable media rather than exposing a partial mount.

### The two numbers that decide the design

**A directory holds sixty-four entries.** A hundred photographs cannot be a
hundred files.

**A name is eight and three.** Even if they could be a hundred files, they
could not keep their names — and an editor that renamed somebody's material to
`IMG~0007.BMP` on import has destroyed the only thing that said what it was.

A third number is worth having beside them, because it was got wrong first.
One 1920×1080 frame in eight-bit RGBA is 8,294,400 bytes, so one of Phipia's
files holds **two** of them, with 188,416 bytes to spare, and cannot hold
three. The first version of that test asserted that two did not fit — a number
that sounds right and is not — and it is now written out in full so the next
reader does the arithmetic rather than trusting it.

### `phipia`: the contract, in types

Nothing above it knows what FAT32 is. What it needs to know is that a name
somebody types may be impossible, and that is decided here, once, against the
real rule, with a refusal for each way it can fail rather than one for all of
them: `NameEmpty`, `NameTooLong`, `NameNotCanonical`, `NameDotMisplaced`,
`PathEmpty`, `PathAbsolute`, `PathTooLong`, `PathMalformed`, `PathAboveRoot`.

The eight-and-three bound is checked on the two halves **separately**, and that
took a control written twice to hold: a base of nine and an extension of two is
eleven characters, which a bound on their sum accepts and Phipia does not. Both
of the original fixtures were twelve characters, where the two rules agree.

### `vault`: a filesystem inside one of Phipia's files

`SSV1` is one 8.3-named file whose inside Media Editor owns: a 56-byte header,
an
index of 112-byte entries, and the material end to end. Two hundred and
fifty-six items — four times what one of Phipia's directories holds — and
16,748,488 bytes of payload, which is not a chosen number but what is left of
sixteen mebibytes once a full index is in the file. A compile-time assertion
says so, so the day either bound moves the build says which.

Each entry carries the item's digest, its span, and up to **sixty-two bytes of
name**: five times what 8.3 can express, and the one thing a vault has that
Phipia's filesystem cannot give.

Two invariants do most of the work:

- **The spans run end to end, ascending, from nought.** The payload is exactly
  the items in entry order with nothing between them, so there is one file for
  a given vault, and a gap or an overlap is a refusal rather than an oddity.
- **Every digest is recomputed rather than believed.** Phipia says plainly that
  FAT32 is not journaled and that an interruption may leave a size/chain
  mismatch that the next mount refuses. This is the same answer one level up: a
  damaged cluster becomes `VaultItemDigestMismatch` instead of a photograph
  filed under somebody else's name.

And the whole file is sealed by one digest over the header, the index and the
material, which is the shape the project file already uses.

### A vault *is* a library

The payoff, and the reason this is one milestone rather than two. Material in a
vault is keyed by its digest; a clip in the model refers to media by digest; a
source node in the graph asks for a digest and a tick. So `Shelf` implements
[`media_editor_render::Library`] over a vault directly, with nothing in between
translating — "the file is in the vault" and "the picture renders" become one
fact rather than two that have to be kept in step.

Every item is an `SPRW` reel, which is what makes one answer do for both kinds
of material a person pastes in: **a photograph is a reel of one frame** and a
piece of footage is a reel of many. Nothing above has to ask which it got.

Three refusals the library needed and the render crate did not have:
`MediaAbsent`, `MediaUnreadable` and `FrameAbsent`. The first two are separate
deliberately — one means somebody's drive is not mounted and the other means
somebody's file is damaged, and telling a person the first when it is the
second sends them looking in the wrong place.

### `bmp`: the format the two programs already agree on

Phipia's workspace imports "an ordinary uncompressed 24-bit BMP", bounded at
1920×1080, every header field and row offset validated. This is that decoder
written in safe Rust against the same bounds, so a file Phipia accepts this
accepts and a file it refuses is refused here for the same reason.

BMP is not a good format. It is the only raster format with no entropy coding,
no colour management, no chunk structure and no optional features — which makes
it the only one that can be decoded correctly without a library, and R-11.3
says there is no library. Its two warts are pinned by hand rather than by round
trip, because a round trip agrees with itself while being wrong twice: the rows
run **bottom-up** unless a negative height says otherwise, and the channels run
**blue, green, red**.

The row padding is the third: a row of five pixels is fifteen bytes padded to
sixteen, and a decoder that forgot it is exactly right at four and eight. Three
widths are tested for that reason; one test image proves nothing.

### Six controls passed, and five of them were one gap

Twenty-three controls, and six did not fail at first — the worst run this
project has had, and the most useful.

Five were the same gap: **no test built a hostile file.** Every vault test
encoded a vault and decoded it back, so the span check, the digest
recomputation, the item bound and the zero-tail rule had nothing behind them —
the byte sweep's mutations were all caught by some *other* check first. Six
resealed-file tests now exist, built the way the project file's have been since
M4: mutate a field, recompute the seal, and watch the refusal that the seal was
hiding.

The sixth found that the seal over the *header* is redundant. Every field in
those twenty-four bytes is independently checked — magic, version, both
reserved words, the count against its bound, the payload length against the
file's own length — so a mutation there is refused by the field rather than by
the seal. The seal stays, because it is one call and because the day this
layout gains a descriptive field the redundancy stops being redundant; what
changed is the control, which now mutates the half of the seal that is
load-bearing, and the test that holds it changes a name to another *legal*
name, which nothing else can notice.

### What it cost

**Nothing, in the image.** 106 pages before and 106 after; 491 bytes net, of
which 330 is `IoStatus`'s `Debug` growing by nine variants, 51 is
`RenderStatus`'s growing by four, and 110 is one `RawVec<Name>` growth helper.
The vault, the bitmap decoder and the path rules contribute **zero**, because
the freestanding image never reaches them — which is the property this project
recorded at M8.5 and has now watched hold with 1,400 lines of new code.

A vault costs 56 bytes plus 112 an item plus the material. Measured against the
hand-derived layout: one 320×180 photograph is 173,064 bytes, sixty-four of
them are **11,072,568**, and eight 640×360 stills are 5,531,320 — all three
inside one of Phipia's files, which is what the bound was chosen to make true.

**State: done.** Not done: a **`Storage` seam that speaks this contract** —
`media-editor-abi` still offers two slots and a commit, where Phipia offers
open,
read, write, seek, truncate, rename, unlink and sync, and closing that gap is
`PHIP-01`'s shape rather than this milestone's; the **scratch-backup-rename
save** Phipia's own workspace performs, which belongs beside `save.rs` once
there is a seam with a rename in it; **importing into a running project**,
which is an editing gesture over a selection and belongs wherever selections
do; and a **vault that spans more than one file**, which is what a project
larger than sixteen mebibytes needs and which wants a decision about what
happens when one part of it is missing.

## M8.33 — The vault, through the seam

*Requires nothing new.*

M8.32 built a media store and left it hanging in the air: a `Vault` in memory,
encoded and decoded, with no way to persist it and no way to read one that did
not fit. This connects it, and the connecting turned out to be three separate
facts rather than one.

### A slot for material, because a project and its media are two files

`Slot` gains `Vault`. Material is large and changes rarely; a project is small
and changes constantly, and writing sixteen mebibytes of photographs every time
somebody trims a clip would be a save protocol nobody could afford to run.

The two are committed separately and neither can damage the other, which is
what makes "the vault is being written" survivable. One scratch serves both,
because a save is a sequence and there is never more than one in flight —
which is also why Phipia's own workspace uses one `STUTEMP.PHI`.

That meant `commit` had to name its destination, and committing *into* the
scratch is refused: it is where a save is assembled, and an operation that made
it its own destination would consume the scratch and leave the next commit
nothing to find.

### A correction the seam was carrying

`Slot`'s doc opened with *"Phipia has no paths, no directories, and no rename
(`PHIP-08`)"*. True at v1.1.0, false now, and it was the *justification* for the
whole shape — so it was worth asking whether the shape survived the correction.

It does, and for a better reason than the original. A named slot is **less**
than a path on purpose: an application that could name any file could lose any
file, and the property R-9.4 asks for is a property of a *protocol* rather than
of a filesystem. Phipia performs that protocol by hand in five steps with three
names and four syncs; `commit` is the name for the one step in it that has to
be indivisible.

### The ranged read, which is the whole point

`Storage::read_at(slot, offset, into)`.

One of Phipia's files holds **sixteen mebibytes**. A Phipia program is mapped
**seventy-six kilobytes**. So `read`, which fills a buffer the size of the whole
slot, cannot read a full vault on the machine this program is for — by three
orders of magnitude. It is not a tight fit or a thing to optimise later; it is
off by 220×.

A store read an entry at a time can be. And this is not an invention: Phipia's
own bitmap reader issues random row reads through the filesystem rather than
holding a picture, which is `phipfs_seek` followed by `phipfs_read`. The seam
now
has the same shape one layer up.

It is **short at the end rather than refused**, because that is what Phipia's
`phipfs_read` does — "reads at EOF succeed with a short or zero byte count" —
and
a seam that refused there would make every reader carry an arm for a condition
that is not an error.

### `Catalogue`: a vault read without being loaded

It holds its slot, a count and a payload length. Nothing else. An entry is one
112-byte read at an offset arithmetic gives; a search is a bounded scan of at
most two hundred and fifty-six of those; material comes back a window at a
time, so a caller with less memory than a photograph can read a photograph.

**`open` does not check the seal**, and that is said out loud in the doc rather
than left to be discovered, because checking it means reading every byte —
which is the thing a catalogue exists to avoid. What `open` *can* check on its
own it does: the magic, the version, both reserved words, the count against its
bound, and the length the header describes against the length the slot actually
holds.

`verify` is the expensive answer for when a caller can afford it — every span
checked end to end, every digest recomputed, the seal recomputed — done in
windows, so it runs on a file far larger than the memory available. A test
walks it at one byte, seven, sixty-four and four thousand and ninety-six,
because a chunked walk is exactly where an off-by-one lives.

The two are tested against each other directly: a vault with one byte of
material changed **opens perfectly well** and reads all four entries, and
fails `verify`. That is the trade, demonstrated rather than asserted.

### Three controls passed, and two were real

**A doubled guard.** `commit` refused a scratch destination twice — an early
`if` and a match arm — and the early one caught nothing the arm did not. It
went. Same finding as `Clip::with_ramp`'s timebase check, same answer.

**A missing battery.** The project's save has had R-9.4's four faults —
refused write, corrupting write, refused read-back, refused commit — since M4.
The vault's had none. `a_vault_survives_a_save_that_fails_at_every_step` now
drives each in turn and checks the last good vault still *opens and verifies*
after every one, which is more than the project's own test asks.

The third was a bad mutation rather than a missing test: shortening each
verification window still walked the whole file, just in smaller steps. Dropping
one byte per window is the mutation that means what it says, and it fails three
tests.

**A counter that could never count.** `MemoryStorage::reads` was a field nothing
incremented and nothing read — dead by R-15.6 and invisible, because a number
that stays at nought looks like a number. It is now a `Cell`, and there are two
of them: whole-slot reads and ranged reads. That is what makes
`a_catalogue_never_reads_the_whole_slot` a test rather than a hope.

### What it cost

**Nothing again.** 106 pages before and after; 527 bytes net, every one of them
`MemoryStorage`'s three new methods, which the image does link because the app
uses it. The catalogue contributes zero — the freestanding image never reaches
it, for the third milestone running.

**State: done.** Not done: a `Shelf` over a **catalogue** rather than over a
loaded vault, which needs an `SPRW` decoder that streams — the reel decoder
builds every frame at once, and a frame is up to eight mebibytes, so the
catalogue's ceiling has simply moved one layer up rather than gone; a
**`Storage` implementation over Phipia's `phipfs`**, which is where the seam's
five slots become three filenames and `commit` becomes the rename it is named
after, and which needs `PHIP-01` to be reachable from Ring 3 at all; and a
**vault larger than one file**, which is what a project past sixteen mebibytes
needs and which wants a decision about what happens when one part of it is
absent.

## M8.34 — The reel, a frame at a time, and a row at a time

*Requires nothing new.*

M8.33 ended by naming its own ceiling: a catalogue reads a vault without
loading it, and then hands the material to a reel decoder that builds every
frame at once. The ceiling had moved one layer up rather than gone. This is
that layer.

### The number

A reel this build writes is bounded at `MAX_REEL_BYTES` — five hundred and
twelve mebibytes. A Phipia program is mapped seventy-six kilobytes.
`sprw::decode` allocates the whole thing, which is **6,899 times** what there
is: not a tight fit, not something to make smaller, a function that cannot be
called on the machine this program is for.

### A spool holds a description, a rate and a count

Frame `k` lives at `HEADER_BYTES + k × packed_bytes`, which is arithmetic
rather than a search — because every frame in a reel shares one description
and is therefore one size. The format's own module comment argued for that
property on other grounds ("a file whose frames change shape halfway through
is a sequence of files, and pretending otherwise is how a decoder ends up
guessing"). This is where it is cashed.

`Spool::open` reads ninety-six bytes and stops.

### One door, not two

`Spool::open` and `decode` now read the same header through the same function.
A streaming reader that admitted a file the loading one refused would be a
second, weaker way into one format — so the frame bound, the version, the
reserved words and the whole description are checked once, in `read_header`,
and both callers go through it. A test drives four different header mutations
through both doors and requires the **same refusal** from each.

That refactor is where this milestone's bytes went: `read_header` +923,
`decode` −752, net +171 for having one door instead of two.

### Where the ceiling goes, said plainly

Streaming takes the *reel* out of the arithmetic and leaves the *frame*.
`Spool::frame` allocates one, and one 1920×1080 eight-bit RGB frame is
6,220,800 bytes — **eighty times** what a Phipia program is mapped. So this is
not the end of the story, and saying otherwise would be exactly the sort of
claim the platform contract exists to prevent.

`Spool::plane_row` is the end of the story for a reader that needs one. A row
of a 1920-wide RGB picture is **5,760 bytes**: seven per cent of a Phipia
program's whole address space, rather than eighty times it. It reads one row of
one plane of one frame, refusing a destination shorter than the row rather than
partly filling it — a caller that received half a row and no indication would
draw half a row (R-1.4).

And this is not a new idea, which is worth saying: Phipia's own bitmap reader
already does exactly this, issuing "random row reads through the normal
filesystem and NVMe paths" rather than holding a picture. Two programs arrived
at the same shape from opposite ends of the same constraint.

The planar case is what the offset arithmetic is for. In a 4:2:0 frame the
planes are packed one after another, so the chroma begins after the whole of
the luma — an eight-by-four frame is 32 + 8 + 8 = 48 bytes, and the second
plane starts at 32. A control that starts every plane at nought fails
immediately.

### The chain, and the point of it

Four links, each built to avoid holding the one above:

1. a `Catalogue` holds a count and a payload length, not a vault;
2. a `Material` is one entry's bytes, not a vault's;
3. a `Spool` holds a description and a count, not a reel;
4. `Spool::frame` builds one frame, not a reel of them.

`Stacks` implements `media_editor_render::Library` over the whole chain, so the
render graph asks a digest-and-tick question and gets an answer without
anything on the path holding the file. The test that says so asserts the
whole-slot read count is **unchanged** across a catalogue open, a search, five
frame reads, a full verification, and four render queries.

### What `open` does not do, again

It does not check the digest. A reel's digest covers its description *and*
every sample, so checking it means reading everything — which is what a spool
exists to avoid. `Spool::verify` is the expensive answer, in windows, and the
two are tested against each other: a reel with one sample changed **opens
perfectly well, hands back frames**, and fails `verify` — while `decode`, which
does look, refuses the same file outright. Three readers, three honest
positions, each documented where somebody will find it.

### What it cost

**Nothing again.** 106 pages before and after, for the fourth milestone
running. 207 bytes net, and the interesting half of that is the one-door
refactor rather than the streaming: `read_header` +923 against `decode` −752.
The spool, the extent and the stacks contribute zero, because the freestanding
image does not reach them.

**State: done.** Not done: a **row-level renderer**, which is what would make
the seven-per-cent figure mean something end to end — the render graph takes
whole frames, so the row read has no caller inside this program yet and exists
for the one outside it; **`Frame` built from a window rather than a slice**,
which is the next place the frame-sized allocation could go; and a **`Storage`
over Phipia's `phipfs`**, which is still `PHIP-01`'s shape and is still the only
thing between this chain and a running program.

## M8.35 — The graph, a row at a time

*Requires nothing new.*

M8.34 ended by naming the last link: a reel could be read a row at a time and
the render graph could not, so the row read had no caller inside this program.
This is that link, and it closes the chain that began three milestones ago.

### One property

**For every node and every row, `row(id, y)` is byte for byte the `y`-th row of
`evaluate(id)`.** Two evaluators that disagreed anywhere would be two answers
to one question, and the whole value of the row form is that it is the same
picture computed differently. Every test here is that property over a different
node kind, at every row, plus the row past the end.

The chain it is checked over uses everything the row form covers at once: two
sources, a fade, a mask, a wipe, an association and a composite — and then each
node along the way on its own, because a chain that agrees at the top can
disagree in the middle.

### The number, again

A 1920×1080 eight-bit RGBA frame is 8,294,400 bytes and a Phipia program is
mapped 76 KiB, so `evaluate` allocates **a hundred and six times the program's
whole address space per node**. One row of that frame is 7,680 bytes.

### Two refusals, and the difference between them is the point

`RenderStatus::NotRowLocal` is about the **operation**.

`Node::Transform` resamples, and an output row's preimage is a *line* in the
source rather than a row. Only a linear map that takes horizontals to
horizontals makes that line one row, and a rotation never does. No version of
this program will make that untrue.

A **subsampled format** is refused the same way, for a sharper reason: one
chroma row of a 4:2:0 frame serves two luma rows, so a luma row is not
independent of its neighbour. That is a fact about the format.

`RenderStatus::NoRowForm` is about **this build**.

`Pattern`, `Type` and `Legend` are generators whose row form is not written.
Somebody reading a refusal needs to know whether to wait for a later version or
to change what they are asking for, and one status for both would not tell
them. That distinction cost one extra variant and is the most useful thing in
the diff.

### The library default refuses, and that is the load-bearing decision

`Library::row` has a default, and the default is `Err(NoRowForm)` — **not**
"fetch the whole frame and slice it".

A default that quietly loaded eight megabytes to hand back six thousand bytes
would make a row-at-a-time renderer look like it was working while doing
exactly the thing it exists to avoid. A library that can serve rows says so by
writing the method; one that cannot says so by not. There is a test for the
default, because a default nobody exercises is a decision nobody checked.

### No cache, deliberately

`evaluate` caches whole frames by node identity. A row evaluator that did the
same would need one entry per node **per row**, which is the frame it is
avoiding, spelled differently. So a row is computed from its inputs' rows every
time.

That repeats work wherever a node feeds two consumers. In the graphs this
program builds it never does — the planner chains decorations linearly and
combines with `Over`, so the graph is a tree. A future graph with sharing would
pay for it, and would be right to: the alternative is not paying and not
fitting.

### A shape is placed against the whole frame

A mask over one row is not one row of a mask — the shape would be stretched
over that row, which is a different picture. So `masking_row` and
`feathered_row` place the shape against the **full** width and height and
rasterise one line of it, and the frame forms are now the same functions over
the full range. One rasteriser, two ranges, and a control that drops the last
row from the whole form fails the row tests as well as the frame ones.

The geometry is asked for **before** the source's row, which is both the
cheaper order and the one that refuses a mask over something whose height
cannot be known without doing the work first.

### A third guard that changed no answer

The geometry walk had a refusal for `Node::Transform`, on the grounds that a
transform is not row-local. Its control could not be made to fail, and the
reason is worth writing down: a transform **resamples into its source's own
description**, so it changes what the picture looks like and not how large it
is — walking through one gives the right geometry, and `row` refuses the
transform itself a moment later with the same status either way.

Third guard this project has found that changes no answer, and the third it has
deleted. The first was `Clip::with_ramp`'s timebase check, the second was
`commit`'s doubled scratch refusal. Each time the control is what found it.

### The chain, end to end

A graph asks for one row. The stacks ask a spool for one plane row. The spool
asks a material for a run of bytes. The material asks a catalogue, which asks
storage. Nothing on that path holds a vault, a reel, or a frame.

The test that says so measures the thing that actually decides whether a reader
fits — not how many reads, but **how large the largest of them was**. On a
64-wide RGBA picture the row path never asks for more than **256 bytes** at
once and the whole-frame path asks for **1,024**: the two differ by exactly the
height of the picture, which is the arithmetic the whole chain is built out of.

### What it cost

**Nothing, for the fifth milestone running.** 106 pages before and after; 389
bytes net, and all of it the rasteriser being split into a range form and a
whole form — `feathered_rows` +1,755 against `feathered` −1,597,
`masking_rows` +1,554 against `masking` −1,424. `Graph::row` itself contributes
zero, because the freestanding image does not call it.

**State: done.** Not done: **row forms for `Pattern`, `Type` and `Legend`**,
which are the slate's generators and are `NoRowForm` rather than
`NotRowLocal` precisely so that this sentence can be written; a **banded
transform**, which is the honest version of the transform refusal — an output
row's preimage is one row exactly when the linear map takes horizontals to
horizontals, which is a checkable condition and would cover every scale and
every translation, leaving only the rotations refused; a **planner that emits a
row render**, since `timeline::render` still asks for a whole frame and the row
form is reachable only by building the graph oneself; and a **`Frame` built
from a window**, which is the next place the row-sized allocation could go.

## M8.36 — The planner, a row at a time

*Requires nothing new.*

M8.35 ended by naming what was still missing: the row form was reachable only
by building a graph oneself, because `timeline::render` asks for a whole frame
and nothing in this program planned one instant and then scanned it. This is
that, and it is the first caller of the row form that a person would actually
write.

### Why it is a type and not a function

`timeline::Scan::open` plans an instant; `Scan::row` renders one row of it.

The obvious shape is a function taking a row, and it is wrong. Planning walks
the sequence's stack, resolves every clip's media, asks the library whether
each file is reachable and builds a node for each decoration. Doing that a
thousand and eighty times to render one 1080-line frame would trade the
allocation the row form exists to avoid for a worse cost, in a different
currency, and call it progress. A scan plans **once** and there is a test whose
whole job is the number: the library's record of what it was asked has exactly
as many entries as the picture has rows.

### The property, at the top of the program

**A scan's rows are the frame the render makes.** Two layers, the upper one
fading, composited over black: rendered whole through `timeline::render`, and
assembled row by row through `Scan` — and the two frames are equal. Everything
under it exists to make that line true without holding a frame: the graph's row
form, the spool's plane rows, the material's ranged reads.

The assembling is the *test's* business rather than the library's, and
deliberately so. A caller with room for a whole frame should call `render`; one
without should never assemble one.

### What `open` refuses, and what it does not

`Graph::row_local` is new and is the question `open` asks: **can every node
under this one be evaluated a row at a time?** It walks the graph and hands
back the refusal `Graph::row` would have given — `NotRowLocal` for a framing or
a subsampled description, `NoRowForm` for a generator this build has no row
form for, which is a title or an offline slate. A caller choosing between
scanning and rendering whole gets that answer before it commits, rather than at
row four hundred with four hundred rows of work already spent.

That is the same decision `Library::available` makes, for the same reason, and
the walk has a control on every arm of it: through a decoration, through
*both* layers of a composite — a stack is built bottom first, so the framed
layer is whichever one the cutter happened to frame — and on the descriptions
the nodes carry rather than only on their kinds.

`open` does **not** ask whether the library can serve rows, and the asymmetry
is the argued part of this milestone. It would be easy to add a capability
question beside `Library::row` and easy to defend it in a sentence. It buys
nothing:

- `Library::row` already refuses by default, so a library says it has no row
  form by not writing one. A flag beside it would be a *second* statement of
  the same fact, on a trait with a default, free to disagree with the method —
  and the two would be checked by different tests, if at all.
- There is no lateness to prevent. `Graph::row` walks the whole graph for every
  row, so every source in it is touched by row **nought**. A library that
  cannot serve rows refuses on the first one and never on the four hundredth,
  which is the entire cost the question would have saved.

A guard whose absence changes no answer is a guard no test can hold, and this
project has now found and deleted three of them. So the boundary is written
down instead, and pinned by a test: a library that serves whole frames only
opens a scan and refuses its first row, and the same programme renders whole
through the same library — which is what makes it a missing row form rather
than a broken library.

### The fixture that had to be split

The test library grew a `row`, and a second one appeared that is everything the
first is *minus* that method — because not writing it is the only way a library
can say it has no row form, and a fixture that has written it cannot un-write
it. A BMP decoder is the second one exactly: it reads a file into a frame and
has no notion of part of one.

The offline test needed the other fixture too. Answering `available` with the
default `true` while holding no colour for the media is not an offline clip —
it is a library that hands back black — so the planner planned a source and
there was no slate to refuse. The fixture that can be *told* what it cannot
reach already existed for exactly this and is what the test now uses.

### `row_local` had no tests of its own

It is public, it was reached only through `Scan::open`, and every description
that reached it that way was 4×4 RGBA — so its subsampling arm and its
conversion-target arm were carried by nothing. Three tests now hold it
directly, and the load-bearing one checks every case **twice** against the same
graph: `row_local` and `row` must return the same status, not merely both
return an error. A caller that asked, was told yes, and was refused at row
nought would have paid for the question and got nothing.

### What it cost

**Nothing, for the sixth milestone running — and this time exactly nothing.**
106 pages before and after, and the two images compare section by section as
`.text` 335,872 → 335,872, `.rodata` 28,672 → 28,672, `.bss` 69,632 → 69,632.
Every loadable section is byte for byte the size it was. The only sections that
moved are DWARF — `.debug_info` +4,230, `.debug_loc` +4,183, `.debug_str`
+2,258 — and DWARF is not loaded.

The reason is worth stating rather than celebrating: nothing in the freestanding
image reaches `Scan`. `nm` finds no symbol for it. This is the sixth "no growth"
row in the table and the first that could be checked to the byte rather than to
the page, which is a sharper claim and a smaller one.

**State: done.** Not done: an **export that writes a row at a time**, which is
the caller `Scan` was shaped for and needs a writer that can take rows —
`Reel::encode` still takes whole frames; **a `Frame` built from a window**, so
that a row does not allocate at all, which is the last allocation on the path
and the one the 76 KiB number will eventually turn on; **row forms for
`Pattern`, `Type` and `Legend`**, so that an offline programme can be scanned
rather than only rendered; a **banded transform**, unchanged from M8.35 — an
output row's preimage is one row exactly when the linear map takes horizontals
to horizontals, which is checkable and would leave only the rotations refused;
and a **scan over a span rather than an instant**, since a scan is opened per
frame and an export writes thousands of them.

## M8.37 — The export, a row at a time

*Requires nothing new.*

M8.36 ended by naming what was still missing: a scan produced rows and nothing
consumed them, because `Reel::encode` takes whole frames. This is the other
half, and it is the first function in this program that renders a programme and
puts it somewhere — the far end of a chain four milestones long.

### The number this is all for

A 1920×1080 eight-bit RGBA frame is 8,294,400 bytes. Ten seconds of it at 24
frames a second is a reel of **1,990,656,000** bytes, and a Phipia program is
mapped **76 KiB**. Writing that with `encode` needs twenty-five thousand times
the program's whole address space. Writing it a row at a time needs 7,680
bytes, which is a tenth of what there is.

The test that says so measures the thing that actually decides it — not how
many writes, but **how large the largest of them was**. On a 64-wide RGBA
programme the export never hands storage more than **256 bytes** at once, which
is one row and a third of one picture.

### `SPRW` version three: the digest moves to a trailer

This is the part of the milestone that was a *format* decision rather than a
coding one, and it deserves the argument.

The digest lived at offset 64, in the header, which is the obvious place and
has one fatal property: **the first byte of the file cannot be written until
the last byte of the payload is known.** Version two's own `encode` says so —
it built the whole payload, hashed it, and only then assembled a file. That is
a strange thing for a format whose entire purpose is to be what a recorder
writes.

With the digest at the end, a reel is written in one pass, forwards, and the
writer needs nothing from storage but the ability to *extend* a file. It also
makes a torn write **structurally** detectable: with the digest in the header,
a reel cut short is a valid-looking file that only a full rehash — half a
gibibyte of it, at the limit — proves wrong; with the digest at the end, the
file is the wrong length and `Spool::open` refuses it after reading sixty-four
bytes.

Nothing changed about what the digest *means*. Same hash, same bytes, same
order; only where the thirty-two live. The header shrank from 96 to 64 and a
constant went with it — `DESCRIBED_BYTES`, "how much of the header the digest
covers", is meaningless once the digest is outside the header.

**Measured, as a format change must be.** The slate's reel is **528 bytes
under both versions** — 96 + 432 and 64 + 432 + 32 — and of the thirty-four
lines in the golden transcript exactly one moved: the digest of the file, which
would have moved for the version byte alone. A commit where `reel bytes` had
changed as well would need an account this one does not have.

There is also a new test that builds the whole file **by hand**, field by
field, from the layout in the module comment — because a format bump checked by
asking the encoder what the encoder produced would agree with any layout at
all. It caught two of my own guesses on its first run: sRGB's matrix
coefficients are *Identity*, not BT.709, because sRGB is already red, green and
blue and there is nothing to un-mix.

### The seam gains an append, and not a write at an offset

`Storage::append` extends the scratch slot. Phipia's FAT32 has random access as
well as file growth, so this was a **choice** rather than a limitation, and the
weaker capability was the right one: a writer that cannot seek backwards cannot
damage what it has already written. That is a property rather than a
discipline.

It takes **no slot**, where the other four methods all do. There is exactly one
place a save is assembled, and an operation that could name the committed
project or the committed vault would be an operation that could write a live
file in place. Refusing that at run time would be strictly worse than not being
able to ask — the same reasoning that makes overlapping items unrepresentable
rather than merely rejected.

Beside it, `Sink`: the write-side mirror of `Extent`, deliberately the weaker
of the two.

### What the `Winder` refuses, and why each is a refusal

`NotOnePlane` — a packed frame is plane nought entire, then plane one entire.
Rows arrive interleaved across planes and a file wants them segregated, so
winding a three-plane reel forwards would mean holding two thirds of every
picture until its last row: the allocation this exists to avoid, spelled
differently. `Yuv444p8` is not even subsampled, so this is about *planes*
rather than about chroma. A real limitation, and `encode` still writes them.

`RowOutOfOrder` — every row names the frame and the row it is. A winder could
take whatever came next and be shorter for it; then a caller's off-by-one would
produce a file whose pictures are wrong and whose digest is **right**, which is
the worst failure this format can have.

`IncompleteReel` — the count goes in the header before a single sample is
written, because that is what makes frame `k` live at `HEADER_BYTES + k ×
packed_bytes`. A winder that stopped early would have written a header that
lies, so finishing one is refused: no truncation, no rewriting the count.

`SinkNotEmpty` — a reel is the whole file, and its header belongs at offset
nought.

### R-9.4, for a save that takes fourteen steps rather than one

`save` encodes into memory, writes, reads back, compares, commits. An export
cannot hold what it wrote, so it compares differently and not less: it keeps a
running digest while winding, then opens what landed, requires the trailer to
be that digest, and walks the stored bytes in one-page windows requiring them
to hash to the same thing.

**Both checks, and the controls proved it.** The first pass through the plan
had five controls *pass*, and four of them were real gaps rather than bad
mutations. Two were these:

- Removing the trailer comparison changed no answer, because the only fault the
  suite had — a store that damages every write — damages the trailer too.
- Removing the walk changed no answer for the same reason, from the other side.

Each is genuinely load-bearing and the fixture could not express the failure it
catches, so the fixture grew two faults. `CorruptingAppend(n)` damages **one
row** and leaves the trailer alone: the file then states exactly the digest that
was computed and does not hash to it, which only the walk catches.
`ReadsTheWrongSlot` serves the vault's bytes when the scratch slot is asked
for — not a disk fault but a *program* fault, and the one a verification step is
most likely to have. With a reel of the same length already in the vault,
everything about it is sound; it is simply not the reel that was just written,
and only "does this file state the digest I computed" notices.

The third gap was that no test ever left the scratch slot dirty before an
export, and the fourth was that nothing ever asked a `Scratch` how much it
held. The fifth was a bad mutation of mine, which set a reserved byte to the
zero it already was.

Fourteen appends for a four-frame export — a header, twelve rows, a trailer —
and the R-9.4 battery drives a refusal through **every one of them**, requiring
the previous reel to survive each time.

### What it cost

**Nothing, for the seventh milestone running** — and this is the first one
where the total held still while bytes moved *inside* it. 106 pages before and
after; `.text` is 335,872 both sides. Symbol by symbol, 922 bytes moved and
every one is accounted for: `sprw::header` **+826** against `encode` **−773**,
which is the extraction costing 53 bytes of prologue and return;
`MemoryStorage::append` **+406**; a new slice panic path **+200**; `decode`
**+98** for the trailer arithmetic; `IoStatus`'s formatter **+87** for five new
variants; and small change across the fault enum. All of it fits inside
`.text`'s existing page padding, which is why the page count did not move —
and checking that is the point, because a total that holds still can hide as
much as one that moves.

**State: done.** Not done: a **`Frame` built from a window**, so a row does not
allocate at all — the last allocation on the path, and the one the 76 KiB
number will eventually turn on; **row forms for `Pattern`, `Type` and
`Legend`**, so an offline or titled programme can be exported rather than only
rendered; a **planar winder**, which needs a caller that scans a picture three
times or a format that interleaves by row, and both are real designs rather
than omissions; a **banded transform**, unchanged since M8.35; **sound in the
export**, since a reel carries pictures and `mixdown` produces samples with
nowhere to put them; and an export that reports **progress**, which needs a
caller that can be interrupted and a way to resume — and resuming a reel whose
header declares its length is a question with a real answer and no obvious one.

## M8.38 — Sound in the export

*Requires nothing new.*

M8.37 ended by naming what was missing: a reel carries pictures, and
`mixdown` produced samples with nowhere to put them. This is where they go.

### The problem sound posed the format

A container built for *delivery* interleaves — a frame, then the sound that
goes with it — so a player reading forwards has both. This format cannot,
and the reason is the property everything in it rests on: frame `k` lives at
`HEADER_BYTES + k × packed_bytes`, which is arithmetic rather than a search.

A frame at 30000/1001 into 48 kHz covers **1601.6** samples. So the sound
between two pictures is 1601 samples or 1602, the offset of picture `k` would
depend on how many samples happened to precede it, and finding a frame would
mean walking the file. Segregating the two keeps both offsets exact: pictures
where they always were, samples in one run after them.

That makes `SPRW` a working file read by seeking rather than a stream played
forwards, which is what a mezzanine is for — and it costs nothing to write,
since both halves still go down in one pass.

The samples are **interleaved across channels** where the pictures are one
plane, and the two look like opposite decisions and are the same one: **a
streaming writer needs its bytes in the order they are produced.** Rows arrive
one plane at a time, so a reel is one plane; sound arrives a block at a time
across every channel at once, so a reel is interleaved. A planar sound section
would need the whole take held before the first byte — exactly what the
`Winder` refuses planar *pictures* for, arrived at from the other side.

### The number the format cannot know

A reel of `n` frames holds `⌊n·r⌋` or `⌈n·r⌉` samples, and **which one depends
on where in a timeline the take was cut from**, because a span holds
`⌊(s+n)·r⌋ − ⌊s·r⌋` and that varies with `s`. A reel does not record where it
came from.

So the header records the count and the reader checks the **bound** — and the
bound is exact rather than a fudge. Three frames at 30000/1001 into 48 kHz
hold 4804 samples or 4805, and never 4803 or 4806. A take whose sound is a
second short of its pictures is refused, which is the failure that matters; a
take one sample either side of the average is the arithmetic being right.

The exporter is the one thing in the program that *does* know where the span
started, so it computes the true count from the span's own boundaries. A sweep
over six starting frames pins it: 4804, 4805, 4805, 4805, 4805, 4804 — 4804
comes back exactly where the start is a whole number of samples in, since
5 × 1601.6 is 8008. **My first draft of that comment had `s=3` at 4804 and the
test caught it**, which is why the subtraction is written out line by line
rather than summarised.

### Two arithmetics that must never differ

The format checks a length; the mixer produces it. If they disagreed by one
sample the format would refuse every export the mixer handed it. So
`SampleRate::timebase` moved down into the audio crate — it was a private table
in `mixdown` — and a test requires `sound_bounds` and
`Instant::floor_into` to agree at four rates, four timebases and eight frame
counts. Two tables that must agree are one table waiting to disagree.

### Streaming the mix cost nothing, because it was already there

`mix` walks a span **frame by frame** internally, taking each frame's samples
from `floor_into` of one instant to `floor_into` of the next. So mixing one
frame at a time and mixing the whole span give the same samples by
construction, and the export just calls `mix` per frame. Ten seconds of 48 kHz
stereo is 3,840,000 bytes; one frame of it is 12,816, and the measurement
confirms the largest thing handed to storage is one block.

The property test says it directly: the sound in the exported reel equals the
sound `mix` produces over the whole span, at 30000/1001 where a fixed block
size would drift.

### The report carries what clipped

`export` returns a `Report` — the digest and a `MixReport` — rather than a
digest alone. A mix that clipped is the one thing about an export that a
person must be told and that no later reader can recover: the samples in the
file *are* the clipped ones, and they look exactly like samples somebody meant.
Two tracks at 6,000,000 against a full scale of 8,388,607 clip all 12,000
samples of a three-frame export, with an overshoot of 3,611,393, and the reel
is still committed — clipping is a fact about the sound, not a refusal.

### Controls found four gaps

Twenty controls; four passed on the first run and each was real:

- Nothing built a **hostile header**. A rate with no channels, channels with no
  rate, and a silent reel claiming ten thousand samples all read as sound
  files, because every test was a round trip. Three resealed-file tests now
  exist — the same finding the vault produced, and the same answer.
- Nothing exported a span where **the start changes the count**. The sweep
  above is what closed it.
- Nothing exported a mix that **clipped**, so the report could forget every
  block but the last.

Two more mutations printed NOBUILD before they were sharpened, which is not a
pass.

### What it cost

**Two pages, and the run of seven flat milestones ends here.** 106 → 108,
+8,192 bytes, all of it `.text`; the symbols account for +6,158 and the rest is
page padding. `unpacked_sound` +994, `Reel::with_sound` +746, `decode` +661,
`encode` +582, `packed_sound` +544, `read_header` +364, `header` +247, and
`Reel`'s own equality +144 for comparing sound.

The interesting entry is **`AudioBuffer::new` at +817, newly *reached***. The
freestanding image decodes a reel, and a decoder that can carry sound has to be
able to build a buffer — even for a reel, like the slate's, that has none. That
is the same shape as the face costing seven pages the milestone it was first
*set* rather than the one it was written: a footprint moves when code is
reached, not when it is added.

And the file grew sixteen bytes: `reel bytes` in the golden transcript went
528 → 544, which is a rate tag, a channel count, six reserved bytes and a
sample count. **The slate's reel has no sound and pays them anyway** — a header
field present only sometimes would be a header nobody could seek in.

**State: done.** Not done: **sound read back through the vault**, since
`Stacks` serves pictures and a `Spool`'s sound blocks have no caller there;
a **`Frame` built from a window**, still the last allocation on the picture
path; **row forms for `Pattern`, `Type` and `Legend`**; a **planar winder**;
a **banded transform**; **sound that outlives its pictures**, which is what a
split edit or a J-cut delivered as a reel would need and which this format
refuses by construction; and **progress and resume**, unchanged from M8.37 and
now harder, because a resumed export must know which half it stopped in.

## M8.39 — Captions that track the edit, and a vault that serves sound

*Requires nothing new.*

Two things, and the small one first.

### The vault serves sound, and a signature was wrong

`Stacks` has served pictures out of a reel inside a vault since M8.33. It could
not serve sound, and the reason turned out to be a mistake rather than a gap:
`SampleSource` was asked for a **`MediaId`** — an index into *one project's*
table of assets.

A vault is **shared**. It is one of Phipia's files holding material several
projects refer to, keyed by what the material *is*. There is no fourth asset in
a vault to look up. The picture side had already written the argument down —
the render library takes a content digest because "a name is a hint that may
have moved" — and the sound side had quietly not followed it.

So the trait moved down into `media-editor-audio` beside the buffer it produces,
keyed by a digest, and `mix` gained the project so it can resolve one into the
other exactly where the picture planner already does. Both halves of rendering
a sequence now take the project, both name media by what it is, and both can
read from a vault.

`Spool::samples` is the new reading: a run of samples wherever it starts, read
one stored block at a time. A mixer's block boundaries are its own, so a run of
2,000 from sample 900 spans two blocks and part of a third. Finding which block
a sample falls in is **bisection, not division** — block nought runs to
`⌊1601.6⌋`, so sample 1601 is the first of block *one* and a division puts it
in nought. That is the third place in this program where an integer sequence
with no rational inverse is inverted by search, and it will not be the last.

### Captions, and where they live

A caption is a range of the **media** — a source in-point and out-point,
carrying text and a voice. Where it appears in the programme is not stored
anywhere. It is computed, every time it is asked for, from whatever clips
happen to be reading that stretch of that recording.

Every editor that stores captions on the timeline loses caption sync, and loses
it the same way: a caption is a position in the programme, an edit moves the
programme, and now every edit has to remember to move the captions too. Cut,
ripple, roll, slip, retime, undo — each is a place the bookkeeping can be
forgotten, and each of them is forgotten somewhere.

Anchored in the source there is no bookkeeping. **Not one line of `Edit` knows
captions exist**, and there is a test for each of those operations that applies
a real edit through `Project::apply` and finds the words where they belong.
Slip is the one worth naming: slipping changes *which* of the recording is
shown without moving the clip, so the words change and their position does not
— which is exactly backwards from what a timeline-anchored caption does.

### The arithmetic: inverting a retime

This is where the hours went.

A clip maps timeline offset to source tick. At a fixed speed that is
multiplication. On a **ramp** it is the integral of the speed curve, whose
segments are rectangles and trapeziums — so the integral is **quadratic** along
a straight run, and inverting a quadratic wants a **square root**, which is not
rational.

That is the same wall the cubic ease hit from the other side: a Bézier cannot
be evaluated at a *time* without a cube root, so `area_to` refuses to integrate
one at all. The shape a ramp is *allowed* to be is exactly the shape whose
forward map is exact and whose inverse is not.

The way through is to notice what a caption actually needs. Not a moment — a
**frame**. And the frames of a clip are an integer sequence
`source_at(0), source_at(1), …`, monotone because a ramp may not change sign
and a speed may not be nought. So "the first offset at which the clip has
reached this tick" is a lower bound over a sorted sequence, found by halving,
with every comparison exact. The answer is not an approximation of the right
offset; it is the right offset, obtained without asking a question arithmetic
cannot answer. Twenty comparisons cover a clip a million frames long.

The property test does not trust the bisection: it computes the answer by
**walking every frame** and asking the forward map, and requires the two to
agree over every source range in a window either side of what each clip reads,
at seven speeds and four ramp shapes.

### A frame covers a range; it does not sample a tick

The first version asked which offsets *land on* a source tick, and a test
caught it in one line. At triple speed the frames show 100, 103, 106 — so a
caption over ticks 104 and 105 lands on no frame and would have **vanished**.
Speed a clip up and two thirds of a transcript drops out, silently, chosen by
where the sampling happened to fall.

So frame `k` shows from its own tick to the next frame's, and **at least one
tick wide** where the next frame has not moved on. Every tick the clip passes
over is covered, and no caption can fall between two frames.

That one rule dissolved the three cases that looked special. Slow motion
repeats, so two frames cover the same tick and a word held twice as long is on
screen twice as long. A reversed clip covers downwards, so the words come out
backwards — which is what a reversed clip does to everything. And a **freeze**
covers one tick throughout, so a caption over it is on screen for the whole
clip: there was a special case for that and it went, because the general rule
already gave that answer.

### Nesting cost nothing new

A nested sequence's timeline **is** the media its clip reads. So lifting a
caption out of a nest is the same question asked again, composed — one
`offsets_showing` per level, over an explicit stack bounded by
`MAX_NESTING_DEPTH` (R-5.5).

### The bound is a statement about where transcripts have to live

Sixty-four captions an asset, and that number is honest rather than useful. A
caption is 536 bytes in the worst case, so sixty-four is 34,304 — **45% of the
76 KiB a Phipia program is mapped, for one asset**, and the project file is
read in one piece. Sixty-four captions is about five minutes of speech; a real
interview is thousands, and thousands do not fit and never will.

So a transcript belongs where the material does: in the reel, read a window at
a time through the vault, exactly as a hundred photographs turned out to belong
in a store inside one file. That is named below and is not built here. What is
built here is the part that does not change when it moves.

### Two more guards that changed no answer

The fifth was `Spool::samples`' check that a run does not reach past the take:
the walk refuses the same run with the same status a moment later, because
asking for the block past the last is already `FrameOutOfReel`. The sixth was
the format reader's own call to `caption::checked`, which `with_captions`
performs on the very next line. Both went; both controls are repointed at the
check that is load-bearing.

### What it cost

**One page.** 108 → 109, +4,096 bytes, all `.text`; the symbols account for
+3,640. `read_captions` +1,005, `with_captions` +744, `MediaAsset::clone` +551
now that an asset owns a vector, `encode` +490, `Caption::new` +391, its drop
glue +250, `decode_payload` +186 — against `Project::add_media` **−518** and
`refresh_nests` **−459**, which is the inlining threshold M8.21 first recorded,
firing again on a struct that grew.

**The projection itself contributes nothing.** Nothing in the freestanding
image reaches `offsets_showing` or `captions_over`. The page is the *data* and
not the feature, which is the same lesson `AudioBuffer::new` taught last
milestone from the other direction.

The file grew **four bytes an asset** for a transcript of nothing — measured
against the previous commit in a worktree at nought, one, three and seven
assets (84 → 84, 145 → 149, 267 → 279, 511 → 539), and the empty project is
what says the step is per asset rather than per file. A caption that says
something costs **21 bytes plus its text**, which is exactly what the writer's
own fields come to.

**State: done.** Not done: **a transcript in the reel**, read a window at a
time, which the bound above is the argument for and which needs `SPRW`
version five and a source trait beside the two that exist; **captions burned
into the picture**, which the type engine can already draw but which would put
a `Legend` in the plan — and a `Legend` has no row form, so a captioned export
could not be scanned, which is a real interaction rather than an oversight; a
**caption sidecar** in WebVTT or SCC, which is a format module and no new
model; **captions on a title or a nest's own timeline**, since only a recording
carries them today; and **a caption that survives a speed of zero**, which is
a freeze and already works, against a *reverse* through nought, which a ramp
may not do and so does not arise.

## M8.40 — The transcript in the reel

*Requires nothing new.*

M8.39 put a bound on captions in a project file and made an argument out of it:
sixty-four an asset is 45% of the seventy-six kilobytes a Phipia program is
mapped, **for one asset**, in a file read whole. A real transcript is
thousands. So the words belong where the material does, and this is that.

### A third source, and the shape was already there

`media_editor_render::Library` serves pictures.
`media_editor_audio::SampleSource`
serves samples. `media_editor_model::caption::Transcript` now serves words,
and it
is asked for a **range of the recording** rather than for all of it — which is
what makes it affordable, because what comes back is what overlaps a stretch a
clip actually reads.

Two implementations, as with the other two: `Held` reads the project's own
media table, which remains the right place for the few captions a title or a
short insert needs; `Stacks` reads a reel inside a vault inside a slot, without
holding any of the three.

### `SPRW` version five: the words after the samples

The header grew eight bytes — a caption count and how many bytes they occupy —
and the section goes after the sound, for the reason the sound goes after the
pictures: **a section whose length varies may not sit in front of one whose
offsets are arithmetic.**

The transcript is **scanned rather than indexed**, and that is the one place in
this format where an offset is not computed. A caption is a variable-length
record, so there is no `k`-th caption at a computed place — and an index would
cost eight bytes each and buy nothing, because a projection never wants caption
`k`. It wants every caption over a range, and finding those means looking at
all of them whatever the layout. The header carries the byte count so that
`Spool::open` can still check the file's length without reading a word.

A caption is 21 bytes plus its text, so sixteen thousand of them is 8.7
megabytes — **114 times** what a Phipia program is mapped. So the winder takes
them one at a time, declaring the count and the length in the header first,
which is the third section it streams and the third time the same argument has
decided a shape.

### The export carries its own words

An exported reel is the programme, so its source time **is** the span's
timeline time — which means the captions projected onto the programme are
exactly the captions of the reel, rebased to its own nought. A span from frame
ten carries a caption at programme 20..40 as 10..30, which is the same
rebasing a cut through a ramp performs and for the same reason: a thing
extracted from a programme is measured from its own start.

That closes the chain end to end. Words anchored in a recording's source,
projected onto a programme by inverting whatever retime each clip applies,
written into a reel a caption at a time, and read back out of a vault over a
range.

### What it cost

**One page.** 109 → 110, +4,096 bytes, all `.text`; the symbols account for
+4,014. `packed_captions` +1,031 and `unpacked_captions` +969 are the codec;
`Reel`'s equality +904 against −819 and `with_sound` +685 against −746 are the
same functions renumbered as the struct gained a field; `with_captions` +501.
The image decodes a reel, so it links the codec whether or not the reels it
reads say anything.

The file grew **eight bytes** — `reel bytes` in the golden transcript went
544 → 552 — and the slate's reel says nothing and pays them anyway. Two format
versions running where the length moved by exactly the header's growth and the
payload not at all.

**State: done.** Not done: the transcript **read in windows** rather than in
one piece, which is what `Spool::captions` still does and which is more than a
loop because a record can straddle a window boundary; **captions burned into
the picture**, unchanged from M8.39 and still gated on a `Legend` having no row
form; a **caption sidecar** in WebVTT or SCC; and **a transcript on a nested
sequence or a title**, since only a recording carries one.

## M8.41 — The transcript, a window at a time

*Requires nothing new.*

M8.40 put the words in the reel and read the whole section in one piece, which
for the largest transcript the format allows is **8.7 megabytes** against the
seventy-six kilobytes a Phipia program is mapped. This reads it in 4,629.

### A record can straddle a boundary, which is what makes it more than a loop

Every other section of this format has a stride. A caption does not: it is an
in point, an out point, a voice, a length and text, so a window of four
kilobytes ends part way through one about as often as not.

So the reader keeps the remainder, refills **behind** it, and carries on. Four
things have to be true and each has a control: the carry is not cleared, a
record the buffer does not hold entire is not read, the next window goes after
the carry rather than over it, and the bytes a caption consumed are dropped
while the rest are kept.

The carry is bounded by one record — 533 bytes, which is the fixed fields plus
the longest text — because a record cannot straddle more than one boundary if
it cannot be longer than a window. That is why `CAPTION_LARGEST` exists as a
constant rather than as a comment.

And a section that ends inside a record is refused rather than returned short,
which is the failure a header claiming more captions than the bytes hold
produces.

### A bound that was in the wrong place

`caption::checked` counted the captions as well as checking the overlap rule,
and a reel of four hundred captions was refused by it. That is two rules in one
function: **one voice not saying two things at once is a fact about captions**,
and **sixty-four of them is a fact about a project file**, which is read in one
piece. A reel is read a window at a time and carries sixteen thousand.

So the count moved to `MediaAsset::with_captions`, where the container is
known, and `checked` is now the rule that is true everywhere. The test that
found it was the windowed read itself, which could not build a fixture large
enough to have any boundaries in it.

### What it cost

**Nothing, and 101 bytes back.** 110 pages before and after; `.text` is
352,256 both sides. `unpacked_captions` −64 for losing a bound that was not
its, `caption::checked` −16 for losing a rule that was not about captions,
`decode` −14 and `with_captions` −7. The windowed reader itself contributes
nothing: the image decodes reels and does not spool them.

**State: done.** Not done: **captions burned into the picture**, still gated on
`Legend` having no row form; a **caption sidecar** in WebVTT or SCC; and a
transcript on a **nest or a title**.

## M8.42 — A row of everything

*Requires nothing new.*

`Pattern`, `Type` and `Legend` refused with `RenderStatus::NoRowForm` since
M8.35, and that status existed to say **this build has not written one** rather
than *this cannot be done* — which is why it was a different refusal from
`NotRowLocal` and why this milestone could exist at all. A transform's row will
never be a row. A test pattern's always could be.

### Both are functions of position, which is the whole argument

`TestPattern::colour_at(x, y, …)` takes a coordinate and returns a colour.
`Run::coverage(column, row)` takes a coordinate and returns how much of a glyph
covers it. Neither needs anything from the row above, so a row form is not an
approximation of the whole form — it is the same arithmetic over fewer values.

What could be got wrong is what each is placed **against**. Bars are eighths of
the *width*, a checkerboard is counted from the *top*, and an em is a fraction
of the *height*. Drawn into a frame one row high, every one of those would be a
different picture rather than a row of this one. So each is placed against the
whole extent and one row is drawn, which is exactly what `masking_row` and
`feathered_row` already do — and each has its own control.

### One rasteriser, two shapes, three times

`TestPattern::render` and `render_row` both go through `render_rows`.
`Run::plane` and `plane_row` both go through `plane_rows`. And `typeset` became
`carded`, which takes the node and a row rather than seven fields and a row —
so a card and a row of a card cannot disagree about where a letter falls,
because there is nothing to disagree.

### Two controls that passed, and what each found

`Run::plane_row`'s bound on the row could be removed with nothing failing,
because the graph checks the row against the description a moment earlier. It
is a **public** function, though, and one that silently answers for a row past
the end is a hazard however well its callers behave — so it stayed and got a
direct test rather than being deleted. `TestPattern::render_row` was the same
and got the same answer.

The other was better. A legend laid out against the row handed to it, rather
than against the frame it sits on, changed nothing — because the test fixture
was **seven pixels by five**, far too small for any caption to fit, so
`lettered` handed the picture straight back and the type was never drawn at
all. The legend's whole layout could have been wrong and the row tests would
have passed. The fixture is now a hundred and twenty by sixty, and asserts that
the letters actually changed the picture before comparing any rows.

### What it unblocks

An **offline programme can be scanned**, and so can a titled one. The test that
said it could not is now the test that says it can, and it compares the
gathered rows against the whole render — the same property, in the direction
that used to be a refusal.

### What it cost

**No page, and +419 bytes.** 110 both sides, `.text` 352,256 both sides.
`carded` +2,802 against `typeset` −2,377, `render_rows` +1,361 against `render`
−1,372 — `render` is a 21-byte delegator now — and `plane_rows` +785 against
`plane` −736. Drawing a row costs 419 bytes because it is the same code drawing
fewer rows.

**State: done.** Not done: a **banded transform**, which is now the only thing
`Graph::row` refuses and the only `NotRowLocal` left; **captions burned into
the picture**, which this unblocks — a `Legend` has a row form now, so a
captioned export could be scanned; a **caption sidecar**; and a **`Frame` built
from a window**.

## M8.43 — Captions burned into the picture

*Requires nothing new.*

M8.39 gave the model a projection: `captions_at` asks a sequence what is being
said at an instant and answers with the text, in **programme** time, having
walked every clip's speed, ramp, reverse and trim to get there. M8.42 gave
`Legend` a row form. Between them a burned caption became possible, and this
milestone is the whole of it: one function, no new node kind, no new arm.

### Burning is a step, not a property

The temptation is a flag on the export — `burn_captions: true` — and it is the
wrong shape for the same reason M8.7 gave about offline media and
`MediaSource` gave about titles: a node that decided for itself what to draw is
a node whose cache key does not record which it did. `burn` takes a plan and
returns a **new root**. The plan it was handed is still there, still valid,
still cacheable, and still the thing a scan of the uncaptioned programme would
find. Two exports of one sequence, one subtitled and one not, are two roots
over one graph rather than one root with a mode.

That is also why the signature ends in a `NodeId` rather than a `()`: it
composes. Anything that takes a root — `render`, `Graph::row`, `Scan`, the
export — takes this one, and none of them learned a word about captions.

### One card of several lines, in a fixed order

Two people can be speaking at once, and the projection returns both. They
become **one** `Node::Type` of several lines rather than several cards stacked,
because several cards would each be a full-frame composite over the programme
and would each need placing so as not to collide. One card of *n* lines is one
composite and one layout, and the font already lays several lines out.

They are sorted by `(voice, from, track)`. Voice first so a speaker keeps a
position between frames; `from` and `track` after so the order is total, and a
total order is what stops the block reshuffling itself mid-sentence. A caption
block that flickers is worse than one that is wrong in a fixed way, because the
fixed one can be read.

`MAX_LINES_ON_SCREEN` is **four**, and past it the milestone **refuses**
(R-1.3) rather than showing the first four. Dropping the fifth speaker is a
programme that lies about what was said, silently, in a file somebody will
ship — and R-1.4's rule against partial publication is exactly the rule against
that. Four is the bound television has used since teletext.

The ink is full light in *scene* terms, resolved through the frame's own
transfer table, rather than a written 255. In limited range 255 is an illegal
code value and some equipment reads it as a sync pattern.

### The control that had nothing to refuse

"Nothing said is nothing drawn" was the interesting one. `burn` returns `root`
unchanged when the projection comes back empty, and the mutation — build the
card anyway, with no lines — **passed every test**. It had to: an empty card is
transparent, so compositing it over a programme produces the same pixels the
programme already had. Comparing frames could never see it.

What the mutation changes is the **work**, not the answer: two nodes that did
not exist, and a full-frame composite over every frame of every uncaptioned
programme. So the test stopped comparing pictures and started measuring: it
asserts `graph.len()` is unchanged and that the returned root **is** the root
it passed in. That is the fifth time on this project a control has found a test
that was watching the wrong quantity, and the second — after M8.20's freeze —
where the right quantity was cost rather than output.

### What it unblocks

A **captioned programme can be scanned**, and the test says so: the burned root
goes through `timeline::Scan` a row at a time and the gathered rows equal the
whole render. That is M8.42's row form for `Legend` being cashed by something
that is not a test of `Legend`.

### What it cost

**Not one byte, and not one symbol.** 110 pages both sides, `.text` 352,256
both sides, 545 symbols both sides — every one of them the same size, none
added, none removed. It is the flattest result the growth table holds, and the
reason is the first section: `burn` composites a card the graph already knew
how to draw over a picture the graph already knew how to make. The slate never
calls it, so the image never links it.

What did move is presentation only: 10,772 of 28,672 `.rodata` bytes and 14,514
of 352,256 `.text` bytes, all of them pointers into a string pool the linker
re-packed in a different order once the app crate's object changed. The set of
strings is identical; only where each one sits moved.

**State: done.** Not done: a **banded transform**, still the only `NotRowLocal`
left; a **caption sidecar** — WebVTT or SCC beside the reel, for a programme
whose captions should stay switchable; a **`Frame` built from a window**, which
is the last allocation on the row path; a caption on a **nest or a title**,
which the projection refuses today because only a recording carries a
transcript; and **burning over a span** rather than at an instant, so an export
does not rebuild the card for every frame that says the same thing.

## M8.44 — The banded transform

*Requires nothing new.*

`Node::Transform` was the last node `Graph::row` refused, and it had been
refused since M8.35 with a sentence that quietly named its own exception: *an
output row's preimage is a line in the source rather than a row, and only a
linear map that takes horizontals to horizontals makes that line one row.* That
clause was true, and nobody had cashed it. This milestone cashes it.

### The condition is one coefficient

A transform's inverse sends a destination point to a source point:

```
u = inverse[0]·x + inverse[1]·y + offset.0
v = inverse[2]·x + inverse[3]·y + offset.1
```

A destination *row* fixes `y` and lets `x` run. Its preimage lies in a band of
source rows exactly when `v` does not move as `x` does — which is to say
`inverse[2]` is nought. That is one coefficient, and `Mapping::horizontal`
reads it.

So a **scale** scans, a **move** scans, a **mirror** scans, and a **horizontal
shear** scans — that last one is the pleasing case, because it slides every row
sideways by a different amount and still reads one band, since what it moves is
`u` and a band is about `v`. A **rotation** does not, and neither does a
vertical shear: a rotated row's preimage is a slope, and a slope crosses every
row of the picture it lies in. `NotRowLocal` still exists and still means
exactly that.

### A row, not a row: the band

The refusal said *one row*, and one row is not quite what a resample reads.
Bilinear interpolates between the sample above and the one below, so it reads
two. An area filter integrates over the preimage of the whole destination
pixel, so a downscale of *k* to one reads about *k*. What every horizontal map
gives you is that those rows are **contiguous** and their number is a function
of the map rather than of the picture — a *band*.

`resample::band` answers with that range, and it answers **before** any row is
fetched. That ordering is the milestone: a caller asks what it will need, then
fetches exactly that. `Graph::gathered` stacks the band's rows into one frame,
`Picture::banded` teaches the sampler to read from it, and `resample_row` draws
one destination row.

`Picture` is the piece worth reading. Every sampler here asks two questions —
*is this position inside the picture* and *what is the sample there* — and a
band can answer the first exactly as a whole frame does, because it knows how
tall the picture is even though it does not hold it. Only the second changes,
and only outside the rows it holds, where it **refuses**. That refusal is
`RowOutsideBand` and it earns a status of its own: transparency is what a
source legitimately returns past its own edge, so a band that came back one row
short would draw a hole and the picture would look like a picture with a hole
in it. Nothing downstream could tell.

### A bound that is an argument

`MAX_BAND_ROWS` is sixty-four, and the number is not a limit somebody picked.
The row path exists so that an export never holds a frame; a band of *k* rows
holds *k* rows. Let *k* grow unbounded and a steep enough downscale makes the
band the frame, at which point scanning has bought nothing and is merely
slower. So a vertical downscale steeper than sixty-four to one refuses (R-1.3),
by name, and a caller that wants one renders whole frames — which is what it
would have been doing anyway. Bounded by a named constant and fallible, which
is R-5.2 for a thing that is not a `Vec`.

The bound bites at sixty-*three* to one, not sixty-four, and the reason is
worth writing down. A destination row's preimage under a one-in-sixty-four
shrink is `[0, 64)`, whose lower edge lands exactly on a row boundary, and the
band takes in the row that *begins* there. It has to: `area_at` visits that
row, finds an overlap of nought and skips it, and a band that did not hold a
row the sampler visits would refuse with `RowOutsideBand`. So the bound is a
bound on what the sampler **reads**, not on what it uses.

### Nothing to disagree about

The band and the samplers share their arithmetic, and that is deliberate in the
same way `carded` was in M8.42. `preimage_of` and `landing_of` are the two
functions that decide where a destination pixel lands, and `rows_under` calls
them rather than reimplementing them — with `x` fixed at nought, which is
exactly the property `horizontal` guarantees and the reason `rows_under`
refuses for any other map rather than answering something that happens to be
right for one column.

`resample` and `resample_row` likewise share `drawn`, one loop over one
destination row, so a scanned picture and a rendered one cannot differ by a
pixel: there is nothing for them to differ in. The test asserts it anyway, for
three maps and both filters, every row of each.

### An eighth guard deleted

`Graph::gathered` first checked every row it stacked against the description it
expected, the way every other row arm checks its source. Then the control:
`row_description` is a function of the node's description and not of the row,
so every row of one node carries the same one, and the check could never fire.
It is gone, and a row of the wrong *size* still refuses — at
`Frame::from_packed`, on the byte count. That is the eighth guard this project
has found and deleted for changing no answer.

### What the tests had to become

Five tests asserted `NotRowLocal` for a transform, and every one of them used a
translation or a half scale — which is to say every one of them now scans. They
became turns, built from the three-four-five triangle so the cosine and sine
are both rational and the map is an exact rotation rather than a scale wearing
one's name. And each grew a sibling in the other direction: a scan of a scaled
clip that agrees with the render of it, an export of a framed programme that is
the frames `render` makes, and a count showing a bilinear row reads **two**
source rows rather than the picture.

### What it unblocks

A framed programme can be exported. Scaling a shot and moving it is the
commonest thing a cutter does, and until now it forced the whole-frame path —
which on Phipia means it did not run at all. Nothing is left that `Graph::row`
refuses for being the operation it is, except a rotation and a subsampled
format, and both of those are refusals that should stand.

### What it cost

**No page, and the milestone's own code costs the image nothing.** 110 pages
both sides, `.text` 352,256 both sides. `band`, `rows_under`, `resample_row`,
`Graph::banded` and `Graph::gathered` are in none of the 543 symbols, because
the slate renders whole frames and never scans.

What moved is the arithmetic underneath. `bilinear_at` −2,055 and `area_at`
−1,005 folded into `drawn` +1,875, against `landing_of` +693, `bounds` +648 —
newly a function rather than inlined copies — and `preimage_of` +631;
`Picture::at` +105 for the band check, while `Picture::new` −107, `holds` −163
and `clamped` −278 inlined away entirely. Attributed bytes **+234**, all of it
absorbed by page padding, and two symbols *fewer* after a milestone that added
five functions.

**State: done.** Not done: a **caption sidecar** — WebVTT or SCC beside the
reel, for a programme whose captions should stay switchable; a **`Frame` built
from a window**, the last allocation on the row path; **burning over a span**
rather than at an instant; a caption on a **nest or a title**; and a **rotation
scanned in strips**, which is the honest version of the refusal above — a
turned row's preimage is a slope, and a slope has a bounded band per *column
range*, so a strip is to a turn what a band is to a scale.

## M8.45 — A frame built from a window

*Requires nothing new.*

Named as "not done" four milestones running, always with the same clause: *the
last allocation on the row path, and the one the 76 KiB number will eventually
turn on.* The number is `PHIP-03`'s — twelve image pages, four stack, two heap,
one anonymous, seventy-six kibibytes for a whole program. One row of a
1,920-wide RGBA picture is 7,680 bytes, so a path that keeps three of them
alive to composite two has spent nearly a third of the address space on
copies of something it already had.

### What was actually happening

`Frame` owns its samples, and the only ways in and out were
`Frame::from_packed(&[u8])` and `Frame::to_packed() -> Vec<u8>`. Both **copy**.
So every node on the row path copied its inputs out to look at them and copied
its output back in to return it, and a chain of *n* nodes copied 2*n* rows to
produce one.

`composite::over` is the plain case. It took two frames, packed each into a
fresh buffer, built a third, and packed that back into a frame: three buffers
alive at once, two of them holding bytes the frames were already holding.

### Lend, take, hand back

Three methods, and the shape is the whole design.

- **`Frame::packed(&self) -> Cow<'_, [u8]>`** — *lend*. A frame whose samples
  are already one packed run hands out a slice of them. Nothing is copied and
  nothing is allocated.
- **`Frame::from_owned(description, Vec<u8>)`** — *take*. A buffer that has
  been filled becomes a frame without moving a byte.
- **`Frame::into_packed(self) -> Vec<u8>`** — *hand back*. The frame is
  consumed, so there is nobody left to lend to, and the buffer comes out as it
  went in.

Together they make a cycle: a window is filled from storage, taken as a frame,
used, handed back as a window, and refilled. One allocation for a whole reel.
The only thing left that grows with the picture is the *output* buffer of a
node that computes new pixels, and that is the frame's own storage rather than
a copy of somebody else's.

### Why `Cow` and not a refusal

A planar frame's planes are three separate buffers and can never be one slice.
This project's habit is to refuse rather than repair, and here that habit is
wrong: the two answers are *the same bytes*. Refusing would make every caller
write a two-arm branch that reconstructs exactly what [`Cow`] already is, and
correct programs would start failing for a reason that is about cost.

So `packed` is total, and the thing a caller might need to *decide* on is
exposed separately: **`Frame::is_packed`** says which answer is coming, before
it comes. A caller on a small machine can ask; a caller that only wants the
bytes need not.

### Measuring it, which is the part that took the thinking

"It does not copy" is not a property a test can see by comparing bytes — a copy
has the same bytes. That is the entire difficulty, and it is the same shape as
M8.43's empty card, which composited to identical pixels.

So the tests compare **addresses**. A window is filled, its `as_ptr` recorded,
wrapped as a frame, and taken back: the address must be the one it started
with. `Vec::as_ptr` is safe, stable, and exact — a copy would land somewhere
else, and a test that compared contents would pass either way.

That is done at both ends of the real chain, not only in the unit test:

- a `Library` that hands out a row built from its own window, and the address
  surviving `Graph::row` to the caller;
- a `Sink` that records the address of every append, and four rows arriving at
  the sink at the four addresses they were read into.

Between them the claim is not "fewer copies" but *these bytes, this address,
from the window to the file*.

### A ninth guard deleted

`from_owned` began with a length check. The control passed: `Frame::new`
already compares the plane's length against its geometry and refuses with
`PlaneSizeMismatch`, and the planar path's `from_packed` does the same — the
same status, one step later, with no work in between. Gone, and the control
moved to `Frame::new`'s check, which is the one doing the job.

### What it cost

**One page, for sixty-four bytes**, and the arithmetic is the interesting part.
`.rodata`'s content went from 28,624 bytes to 28,688 — the offset table is 144
bytes on both sides and did not move, so the sixty-four are literals and panic
locations from two new functions. Seven pages is 28,672. The content had been
sitting **48 bytes below** that boundary and now sits **16 bytes above** it,
and a section the linker script pads to a page turns sixteen bytes into four
thousand and ninety-six.

`.text` is 352,256 both sides. Attributed bytes +71: `Frame::from_owned` +374
and `Frame::packed` +176, against `Look::apply` −159, the slate's `picture`
−146, `masked` −81, `resample` −78, `render_rows` −62 and `premultiply` −61 —
which are the callers that stopped copying, paying for the two new functions
almost exactly.

The image is not what this milestone is about. What it buys is at run time, and
the plainest statement of it is the one above: a 1,920-wide row through
`composite::over` had three buffers of 7,680 bytes alive at once and now has
one.

While measuring it, three sentences near the top of the platform contract still
said the image was 106 pages. They had been stale for several milestones; they
say 111 now.

**State: done.** Not done: a **caption sidecar** — WebVTT or SCC beside the
reel; **burning over a span** rather than at an instant; a caption on a **nest
or a title**; a **rotation scanned in strips**; and the **`Vec<Plane>` itself**,
which is one allocation of one element per frame and the only one left on the
row path that does not grow with the picture — an array of at most three would
remove it, and would touch every constructor here.

## M8.46 — A caption sidecar

*Requires nothing new.*

The reel has carried its own transcript since M8.40, and that is the right
place for it: one file, one digest, one thing to hand somebody. A **sidecar**
is the other thing an edit room wants — the same words as a separate file, in a
format that is not ours, so captions stay switchable, a player that has never
heard of `SPRW` can still show them, and a caption editor can open them without
a converter.

### WebVTT, and why not SCC

SCC is *the* broadcast caption format and it is the wrong first one. It is
byte-oriented, tied to 29.97 and drop-frame, and carries its text as CEA-608
control codes — a format about a **decoder** rather than about words. WebVTT is
text; its cues are written in order with nothing to seek back to; its
timestamps are wall-clock. All three suit a writer that streams and a machine
that cannot hold the file it is writing.

### The timestamps are wall-clock, and that is the milestone

This project already has a `Timecode`, and reaching for it here would have been
the natural mistake. Timecode **labels** a frame; WebVTT wants the moment the
frame is shown, and at 29.97 those are different numbers.

Thirty thousand frames makes both divisions exact, so the gap is visible with
nothing rounded:

```
shown at : 30,000 × 1001 ÷ 30 = 1,001,000 ms
labelled : 30,000 × 1000 ÷ 30 = 1,000,000 ms
```

One part in a thousand, always the same direction — **3.6 seconds an hour**,
which is a line of dialogue. A sidecar timed from timecode would start correct
and end wrong, which is the worst way for a file to be wrong.

So a cue's time comes from `Instant::seconds`, exact in rationals, and only the
last step rounds. Working the hour case exactly turned up something better than
the drift: an hour of 29.97 is `3600 × 30000 ÷ 1001` frames, and **that is not a
whole number**. The remainder is 108 — and 108 is exactly how many labels
drop-frame timecode skips per hour, two a minute except every tenth. The two
facts are the same fact, and the test asserts both halves of it.

### What rounding preserves, and the one thing it costs

Ties round up, which is the ordinary convention and not the interesting part.
The interesting part is what survives:

- **Monotone.** Rounding a monotone function is monotone, so two positions in
  order stay in order and two captions disjoint in ticks are disjoint in
  milliseconds. Nothing checks that — it is a property of flooring. Which
  means `Spotter::cue`'s ordering refusal is about a *caller* handing cues in
  the wrong order, never about this arithmetic, and the doc comment says so.
- **Not injective**, and that is the cost. Two ticks under a millisecond apart
  become one number, so a caption shorter than half a millisecond rounds to no
  duration. That is refused (R-1.3) rather than written, because a cue nobody
  can see is a file nobody can tell is wrong. At 48 kHz the first length that
  survives is twenty-four ticks — half a millisecond, which the tie rounds up.

### Escape three things and refuse two, and the line between them

`&`, `<` and `>` are markup in a WebVTT payload and are **escaped**, because
WebVTT has a spelling for them and "R&D" and "5 < 6" are things people say. A
caption format that refused them would be a caption format nobody could use.

A blank line and `-->` are **refused**, because WebVTT has no spelling for
either. A blank line separates cue blocks, so text containing one would
silently become two cues with the second malformed. `-->` is what a timing line
is made of. Both would produce a *valid file that says something else*, which
is the failure mode a format writer has to care about most. A carriage return
counts as a line ending too, so `\r\n\r\n` and `\r\r` are refused with the
blank lines.

A single newline is allowed, and deliberately: two lines is what a caption
normally is.

### One buffer, and no trailer

`Spotter` is `Winder`'s shape — `begin`, then a record at a time — and it holds
one buffer of `MAX_CUE_BYTES`, refilled per cue, so a sidecar of sixteen
thousand cues allocates once. That bound is derived in its own documentation
rather than picked: 21 for the identifier, 66 for the timing line, 11 for the
voice tag, 640 for the text — a hundred and twenty-eight characters at five
bytes, because `&` is the one byte that becomes five — and 6 to close. 744,
rounded to 768. A test writes the worst cue and checks the arithmetic.

There is no `finish`, and the reason is worth stating. A reel ends with a
trailer, so an interrupted reel refuses to open. A WebVTT file ends whenever it
ends: **an interrupted sidecar is a shorter valid sidecar** and nothing in the
format can tell. That is not something to fix in the format. What makes it safe
is where the bytes go — the scratch slot, verified, committed atomically like
everything else (R-9.4).

### One projection, not two

`export::spot` and `export::export` call the same `spoken_over`. A sidecar that
projected the captions a second time would be a sidecar that could disagree
with its reel, and two files disagreeing about what was said is worse than one
file with no captions at all. The control mutates the sidecar's span by one
frame, and the test that fails is the one that reads both files back.

It takes a `Sink` rather than a `Slot`, which is R-1.2 taken seriously: the
storage seam has three slots, a sidecar is not one of them, and a widening is a
new contract rather than a convenience. Where a sidecar goes is the caller's
decision.

### What it cost

**No page, and the page before it paid for this one.** 111 both sides, `.text`
352,256 both sides. `.rodata`'s content went 28,696 → 28,888 — 192 bytes, every
one absorbed by the page M8.45 opened, which had 3,880 bytes of headroom and
now has 3,688.

Not one symbol of `vtt` is in the image: the slate exports reels and does not
spot sidecars. Attributed bytes +92, of which `IoStatus`'s derived `Debug` is
+66 for five new variants. The 192 are those five variant *names* — 73 bytes of
text — and their entries in the formatter's table.

And the five refusal sentences cost **nothing at all**. `IoStatus::describe` is
in none of the 545 symbols: the image never formats a refusal, it returns one.

**State: done.** Not done: **reading** a sidecar, which is a parser and
therefore its own milestone with its own bounds against a hostile file (R-11.2)
— writing was the milestone that had the arithmetic in it; **SCC**, for the
broadcast path, which needs CEA-608 and drop-frame; **cue positioning**, since
WebVTT can say where on screen a caption sits and `MAX_LINES_ON_SCREEN` is
currently the only answer; **burning over a span** rather than at an instant; a
caption on a **nest or a title**; and a **rotation scanned in strips**.

## M8.47 — The span burn

*Requires nothing new.*

M8.43 gave `burn` an instant, and an export of a programme called it once a
frame. That is the wrong shape, and the cost is not subtle. Each call walked
every track, every item and every clip, and inverted each clip's retime for
every caption of its asset. A twenty-clip sequence of sixty-four-caption assets
over a twenty-four-thousand-frame reel asks **thirty million questions** to
answer what a single walk answers in a thousand.

### The set on screen is a step function of time

That is the whole observation, and everything else follows from it. What is
being said changes only where a caption starts or stops. So a span falls into
stretches over which the answer is *constant*, and there are at most `2k + 1`
of them for `k` captions — **a number with nothing to do with how many frames
the span holds**.

`Reading::over` projects once, records the moments the answer changes, and
answers any instant by finding which stretch it is in. Twenty frames of a
programme with two captions is three stretches, and the test says so.

### No stretch stores its own lines

The obvious representation gives each stretch the captions it holds, and it
does not fit. `k` captions across `2k + 1` stretches is `O(k²)` entries — at
`MAX_CAPTIONS_SHOWN` of 512 that is half a million, four megabytes, on a
machine given seventy-six kibibytes.

What a `Reading` holds instead is the captions once and **one tick per edge**.
A stretch is maximal-constant, so a caption covers all of it exactly when it
covers its first instant — which means a stretch's lines can be recomputed by
one pass over the captions and need never be stored. That is `O(k)`, and it is
the same trick M8.45 used for a different reason: the thing that grows is the
thing you refuse to keep.

### What the two projections agree about, and what they do not

They agree about **which words are on screen**, frame for frame, and the test
checks every frame of a two-clip span where one clip runs at half speed.

They differ about the *ranges*, and it is worth knowing why, because it looked
like a bug for a minute. `captions_over` clips each answer to the window it was
asked about. An instant projection therefore reports a sentence as one frame
long; a reading reports it as the stretch of the span it covers. **The
reading's is the more informative of the two**, and it makes `burn` strictly
better: two speakers at once are now ordered by when each of them actually
started rather than by which track they came off, because under the old
projection every caption's `from` was the frame's own start and the sort key
was doing nothing.

### `burn` stopped asking the project anything

Its signature lost a project, a sequence and a transcript, and gained a
`&Reading`. It is now pure in the way the rest of the planner is: it asks
nothing of the project, so nothing about it can be stale, and it cannot be the
thing that makes an export slow.

Burning is still per frame, because each frame is its own graph. What stopped
being per frame is the question.

### What it cost

**Fifteen bytes.** 111 pages both sides, `.text` 352,256 both sides, 545
symbols both sides — and exactly one of them moved: `ModelStatus`'s derived
`Debug`, +15 for the one new variant. `.rodata` grew 16 bytes, that variant's
name. Nothing else in the image changed by a byte, because `Reading` is in none
of the 545 symbols: the slate renders a frame at a time and never projects a
span.

It is the cheapest milestone the growth table holds and the one with the
largest saving behind it, which is the right way round and is not a
coincidence: the saving is a change of *shape*, and a shape costs nothing to
carry.

**State: done.** Not done: **building the card once a stretch** rather than
once a frame, which the stretches now make possible and which needs a graph
that outlives one frame; **reading** a sidecar back; **SCC**; **cue
positioning**; a caption on a **nest or a title**; and a **rotation scanned in
strips**.

## M8.48 — A turn scanned in strips

*Requires nothing new.*

`NotRowLocal` said a resampled row "needs more than a band of input rows", and
called that a statement about the *operation*. It was overclaiming, and finding
that out is the milestone.

### The width was always the missing term

A destination row's preimage under an affine map is a segment of the source.
When the map takes horizontals to horizontals the segment lies flat, so the
whole row reads one short band. When it does not — a rotation, a vertical
shear — the segment has a slope, and **a segment of slope `m` across `w`
columns crosses about `m · w` rows**.

That `w` is the term the old refusal left out. A turn's row is too tall for a
band *across the whole picture*; across a narrow enough part of it, it is not.
So the row is drawn in **strips**, and a strip is a range of columns and not a
different arithmetic: the pixels are identical, and where the boundaries fall
changes nothing.

The test says it in numbers. The three-four-five turn about the centre of a
two-hundred-square picture has `v(x, y) = −3(x − 100)/5 + 4(y − 100)/5 + 100`,
so at row 100:

```
whole row, x in {0, 200} : floor(40)  ..= floor(160.8) = a band of 121
half a row, x in {0, 100} : floor(100) ..= floor(160.8) = a band of  61
```

Sixty-four is the bound. Same map, same row, same arithmetic — a different
width.

### The width tunes itself, and correctness does not depend on it

`Graph::banded` starts optimistic, at the whole row, and halves whenever
`resample::strip` says the band would be too tall, keeping whatever width
worked for the strips after it. A scale pays one probe a row; a turn pays about
fourteen once. Neither has to be told which it is.

That is only safe because correctness does not depend on the width at all, and
that is the property worth stating: a strip is a range of columns. Nothing
about a pixel changes with which strip it fell in.

### What it costs, honestly

A turn **re-reads**. Neighbouring strips have overlapping bands, so a row drawn
in strips fetches more source rows than the picture has. How many is not up to
the slicing: `w` columns read about `m · w` rows and there are `width / w`
strips, so the total is about `m · width` however wide the strips are. That is
the number of rows the row's preimage genuinely crosses, so **no
row-at-a-time evaluator can read fewer**. Strips reach the floor; they do not
add to it.

Reading fewer means not working a row at a time. Tiles — a band of output rows
by a strip of columns, so one source band serves many output rows — is the
design that does better, and it is recorded as not done rather than pretended
away.

### What the refusal turned out to mean

`NotRowLocal` is now only about **subsampled formats**, and there it is exactly
right and irreducible: one chroma row of a 4:2:0 frame serves two luma rows, so
a luma row is not independent of its neighbour and no slicing changes that.

`BandTooTall` is what is left for a transform, and it is a question about *one
row of one map* rather than about an operation. It survives only where
narrowing cannot help — a vertical downscale, where a map that takes
horizontals to horizontals reads the same band however much of the row is asked
for. The export test that used to say "a programme that cannot be scanned" now
says it with a shrink of one in two hundred and fifty-six, and had to be given
a two-hundred-row picture to say it at all: the band is clamped to the picture,
so a shrink of any steepness over a three-row frame reads three rows.

### A tenth thing deleted

`Mapping::horizontal` was M8.44's answer to "can this be scanned", and after
strips nothing in the crate asks it. It is gone. What it meant is asserted
directly instead — that a flat map's band is the same however much of the row
is asked for — which is a stronger statement than the predicate was.

### And a guard that stayed

A control found that `rows_under`'s check for a strip of no columns changes no
answer: `Graph::banded` never makes an empty strip and nothing else called it
with one. It stayed, for the reason M8.42 kept `Run::plane_row`'s bound — it is
**public**, and a public function that silently answers a meaningless question
is a hazard however well its callers behave. It got a direct test instead.

### What it cost

**The image got smaller**, which no milestone that added a capability has
managed before. 111 pages both sides, `.text` 352,256 both sides, 545 symbols
both sides; attributed bytes **−82** and `.rodata` −8.

None of it is the feature: `strip`, `rows_under`, `resample_strip` and
`Graph::banded` are in none of the 545 symbols, because the slate renders whole
frames. Every byte is the shared arithmetic underneath — `preimage_of` −54 for
taking a range of columns rather than building each corner from `x + dx`,
`drawn` −39 for taking a strip rather than a width and a row, `resample` −27,
against `area_at` +38 for passing `x, x + 1`. Generalising a function made it
smaller, because the general form does less work per corner.

**State: done.** Not done: **tiles**, which is how a turn stops re-reading;
**building the card once a stretch**; **reading** a sidecar back; **SCC**;
**cue positioning**; and a caption on a **nest or a title**.

## M8.49 — Tiles

*Requires nothing new.*

M8.48 made a turn scannable and said plainly what it cost: **a turn re-reads**.
Drawn a row at a time, neighbouring strips have overlapping bands, so the same
source rows are fetched again for every destination row — about `m · width`
rows for each of `height` rows. That milestone recorded tiles as the way to do
better. This is that.

### One band, many rows

A **tile** is a rectangle of the destination: a range of columns *and* a range
of rows. Its preimage is one parallelogram, so it has one band — fetched once,
and drawn from once per row of the tile. The re-reading falls by a factor of
the tile's height.

`preimage_of` had already been generalised twice for this. M8.44 gave it a
pixel; M8.48 gave it a range of columns, which is what made a turn drawable at
all; this gives it a range of rows, which is what makes it affordable. The two
generalisations buy different things and the doc comment says which.

### The measurement, worked out by hand

The three-four-five turn about the centre of a hundred-and-sixty square gives
`v(x, y) = −3(x − 80)/5 + 4(y − 80)/5 + 80`, so a tile of `w` columns and `h`
rows spans `3w/5 + 4h/5` in `v`. The width halves from 160 until the band fits
in sixty-four:

```
together, h = 16 : w = 160 -> 96 + 12.8 = 108.8, too tall
                   w =  80 -> 48 + 12.8 =  60.8, which fits
  columns   0..80 : v spans 73.6 .. 134.4 -> rows 73..=134 = 62
  columns 80..160 : v spans 25.6 ..  86.4 -> rows 25..= 86 = 62
                                                    total = 124

apart, h = 1     : w =  80 -> 48 +  0.8 =  48.8, which fits
  two strips of about fifty rows, about a hundred a row, sixteen rows
  is about 1,600 -- 1,594 once the rows near the edges clamp
```

**124 against 1,594.** Both produce identical pixels, which is why counting is
the only way to see it, and the test asserts both numbers.

### What the flat case turned out to be

The second test was written to say "and tiles do nothing for a map with no
slope". It failed, and the failure was the better result.

A bilinear move down by one pixel reads two source rows per destination row,
and consecutive rows' bands **overlap by one**. Three rows apart read 1 + 2 + 2
= 5; together they read 3. Two rows saved out of five, on a map with no slope
at all. So the saving is not about turns: it is about consecutive rows sharing
source, which nearly every map does. Turns are only where it is dramatic.

### One buffer a row

A rectangle is not contiguous in a row-major picture, so `resample_tile` hands
back **one buffer per destination row** rather than one buffer with a stride.
That is not a compromise: it is the shape the caller wants anyway, because what
it does next is hand those rows to a sink one at a time. The count has to match
the tile's height and a mismatch is refused — fewer buffers would silently drop
rows and more would leave gaps, and both look like a picture.

### `Graph::rows`, and what it promises

The band form of `Graph::row`. For `Node::Transform` it tiles; for everything
else it is the rows stacked, because every other node here is row-local and
there is nothing to share. It makes **no promise** about being cheaper — the
saving is a property of the map, not of the method — and the two measurement
tests are the two answers.

There is no bound on how many rows to ask for, and that is deliberate. A band
of `h` rows is `h` rows of memory, and how much memory there is is the
caller's to know. `MAX_TILE_ROWS` says how tall a tile *may* be; nothing says
how tall it should be.

The export takes the largest tile the renderer admits, because it is the one
caller that walks a whole picture in order: it pays the memory once and gets
every row of the saving. The winder still takes rows, and that is the point —
what changed is how many are computed at once, not how many are held for
writing. Every read of the store still stays inside one window, and the test
that says so is unchanged.

### What it cost

**No page.** 111 both sides, `.text` 352,256 both sides; `.rodata` +40 and
attributed +277, all absorbed by the page M8.45 opened, which now has 3,808
bytes of headroom.

Not one tile function carries a name in the image: `tile`, `rows_under`,
`resample_tile`, `Graph::rows`, `Graph::banded` and `rows_of` are in none of
the 547 symbols. The two new symbols are the *shapes* the change needs rather
than the change itself — drop glue for `Vec<Frame>` +255, which the export's
row-splitting builds, and `RawVec<Vec<u8>>::grow_one` +110 for the
buffer-a-row a tile writes into. Against them, `Reel`'s drop glue −207 and
`preimage_of` −45 for taking a rectangle rather than a row of columns;
`resample` +110 and `area_at` +38 for passing one.

What it bought is not in the image at all. It is 124 against 1,594.

**State: done.** Not done: **a tile that is not a band of full rows** — the
column split is chosen per band and thrown away, so a caller that wanted a
rectangle of a picture rather than a run of its rows still cannot ask;
**building the caption card once a stretch**; **reading** a sidecar back;
**SCC**; **cue positioning**; and a caption on a **nest or a title**.

## M7 — Speed

*Requires `PHIP-04`, then `PHIP-10` and `PHIP-11`.*

The first milestone where performance is the goal rather than a consequence.
SIMD becomes legal, sealed leaves under R-3.4 become worthwhile, the job graph
starts running on more than one core, and the first C or C++ codec libraries
become candidates.

**Done when:** every leaf is bit-exact with its Rust reference across the
committed corpus and a fuzz campaign; the parallel job graph produces
hash-identical output at every core count (R-6.2); and each leaf's benchmark
clears R-3.4.3's threshold.

## M8 — Suite

*Requires nothing new.*

The parts that make it a tool rather than a demonstration: titles and text,
transitions, keyframed effects, masks, media management, EDL export, OTIO
interchange, and the interface that makes all of it reachable.

**Done when:** an EDL exported from Media Editor and re-imported reproduces the
same cut exactly; the interface's layout is deterministic and its damage
tracking is proven bounded (R-10.4).

## M9 — One

*Requires everything above.*

The first release. A release means: reproducible artefacts, pinned digests, the
complete evidence set, the source of every vendored dependency, the licence
record, the QEMU transcripts, the golden hashes, and an honest statement of
what Media Editor is not — in Phipia's tradition of saying exactly where the
boundary is.

## Working in parallel

Two tracks run at once, and they only meet at the milestones that name a
capability:

- **The Phipia track** builds the capability ladder: `PHIP-01`, `PHIP-02`,
  `PHIP-03`, `PHIP-05`, `PHIP-06`, `PHIP-07`, then `PHIP-08`, `PHIP-13`,
  `PHIP-04`,
  and finally `PHIP-10` and `PHIP-11`.
- **The Media Editor track** builds M2 first, because it needs nothing, then M3
  onwards as capabilities land.

M2 being independent of the platform is not an accident of planning. It is the
reason the language law puts Rust in charge of the model: the majority of this
application can be finished and proven before the operating system is ready to
run it.
