<!-- SPDX-License-Identifier: GPL-3.0-only -->

<h1 align="center">Media Editor</h1>

<p align="center">
  <strong>A professional non-linear video editor, native to Phipia.</strong><br>
  Not a port, not a portable application with a Phipia build — an editor that
  owns its operating system.
</p>

<p align="center">
  <a href="LICENSE"><img
    src="https://img.shields.io/badge/license-GPL--3.0--only-595976"
    alt="GPL-3.0-only"></a>
  <img
    src="https://img.shields.io/badge/status-foundation-18181C"
    alt="foundation stage">
</p>

## Status

Foundation stage. The parts of an editor that need no operating system are
being built and proven now; the parts that need one are waiting on Phipia.

[Phipia](https://github.com/saudaljuaid/Phipia) is at **2.1.0**, and it runs a
Media Editor workspace today — in its First Environment shell, opening and
saving
projects on a read-write FAT32 volume, importing BMP frames, trimming clips on
a timeline and exporting a frame back out.

What Phipia cannot yet run is *this* program. The freestanding Rust image has
no Ring 3 path of its own: an image is admitted at a fixed page layout named
per profile, and the widest of those is 76 KiB against an image of 424 KiB.
There is still no generally stable native application ABI, no userspace memory
service, no audio device, no second core, and no architectural guarantee that a
Ring 3 program may execute a single SSE instruction — which is why every
milestone below is written in exact integers.

An earlier version of this paragraph said Phipia *cannot run Media Editor*. It
was
true when written and went eleven releases stale;
[the platform contract](docs/PLATFORM_CONTRACT.md) records the correction and
which of its numbered requests Phipia has since answered.

**What exists and is proven, today:**

- exact rational time, timebases, half-open ranges, and drop-frame timecode,
  swept over every frame of a whole day at eight rates — twenty-nine million
  round trips, each of which must name its own frame back;
- the project model — sequences, tracks, clips, gaps, and eight edit operations
  — where overlapping items are unrepresentable rather than merely rejected;
- undo and redo as an algebra, checked over two thousand generated editing
  sessions, with the check itself shown capable of failing;
- a project file that is versioned, length-prefixed and digested, so that
  **every single-byte change to a valid file is refused** — swept, not claimed
  — with SHA-256 written here and checked against the published vectors;
- a save that cannot lose the last one: encode, write scratch, read it back and
  compare, then commit. Each of its four failure modes is a test that requires
  the previous file to survive whole;
- frames that **cannot be built without a complete colour description** — no
  `Unknown` primary, no `Unspecified` transfer, no `Default` anywhere, so the
  untagged frame behind every washed-out export is not a value this program can
  construct — with plane arithmetic checked at every dimension in a range, and
  a frame with an alpha channel required to say whether it is straight or
  premultiplied, because that is the difference between a clean edge and a
  dark fringe;
- a bounded, content-keyed frame pool that evicts by use, deterministically,
  and refuses to let one key ever name two different frames;
- `SPRW`, an uncompressed mezzanine format whose byte sweep found a real
  integrity gap before it shipped, and closed it;
- a colour derivation computed in **exact rationals rather than floating
  point**, so a gamut converted to itself is the identity matrix and every row
  of a conversion sums to exactly one — with the derived BT.709 coefficients
  and the derived BT.709-to-BT.2020 matrix checked against the numbers the
  standards print;
- transfer functions — sRGB, BT.709, BT.2020, both pure gammas, ST 2084 and
  hybrid log-gamma — computed with **integer arithmetic and no libm**, because
  `pow` is not specified bit-for-bit by IEEE 754 and two machines with
  different maths libraries would otherwise export different pixels;
- sine and cosine measured in **turns rather than radians**, so reducing an
  angle to one revolution is masking bits and loses nothing — a quarter turn's
  sine is exactly one, and an angle ten thousand revolutions out gives the same
  answer bit for bit as the same angle at the origin, which no floating-point
  library can promise;
- histogram, waveform and vectorscope, which are counts rather than pictures,
  checked against expectations worked out by counting rather than by running
  the code — and on the vectorscope, against two properties that are exact:
  every grey sits precisely on the origin under every matrix, and full red sits
  at exactly +1/2 red-difference in BT.601, BT.709 and BT.2020 alike, because
  the coefficient cancels itself out of the definition;
- frame conversion in the order colour science requires — gamut changes happen
  **in linear light and nowhere else** — with a scaler and a chroma filter
  refused rather than guessed, because each is a decision with a name;
- a compositor that works **in linear light and nowhere else**, checked as the
  algebra `over` actually is — a transparent layer leaves the one beneath it
  bit-for-bit unchanged, an opaque layer hides it entirely, and the two ways of
  grouping three layers agree to within a single code value, so grouping clips
  into a compound cannot change the picture — with one pixel computed by hand
  to pin it: full white at half coverage over mid-grey is 205, where a
  compositor that adds code values gives 252;
- and a bug that compositor found in code that was already passing its tests:
  a colour conversion was writing 255 for every alpha byte, so any keyed frame
  that crossed a colour space came out a **solid rectangle**. The test that
  should have caught it converted opaque bars, which have nothing to lose;
- **a sequence rendered at an instant** — the spine of an editor, and the piece
  that makes everything above it do something. Which clip is on each track,
  which frame of it the playhead wants, and the layers composited bottom first
  onto opaque black. Three decisions are named rather than left implicit: higher
  tracks are on top, **a gap is transparent rather than black** so an upper
  track with sparse material does not blank out everything beneath it, and a
  track that has stopped is not a track full of black past its end;
- **dissolves**, which need no operator of their own: the model reports both
  sides of the cut, the outgoing at full opacity and the incoming at an exact
  fraction, so `over` computes the cross-fade — in linear light, so a
  white-to-black dissolve steps 231, 203, 170, 124 rather than the evenly
  spaced numbers a code-value fade would give, each worked out by hand;
- **its sound mixed over a span**, each track at its own fader — set by an
  edit, undoable, and saved, because a mix level that lived only in a function
  call would be a mix nobody could deliver — and where the interesting
  arithmetic lives: a
  frame at 29.97 is 1601.6 samples, so no frame holds a whole number of them
  and none ever will. A frame's samples are bounded rather than counted — each
  block is 1601 or 1602, each block's end is the next one's beginning, and over
  three hundred frames they sum to exactly 480,480, a number arrived at by
  arithmetic rather than by running the code;
- a render graph in which **a cycle is unrepresentable** and every node is
  identified by a digest over its inputs, evaluated in every order a scheduler
  could choose and checked to give the same answers — proven now, while it is
  nearly trivial, so it still holds when there is a second core. **The timeline
  renders through it**, so a pool kept between renders means scrubbing back
  over a cut decodes nothing again, and two sequences cutting the same footage
  share one cached frame — because the graph names media by what it *is*
  rather than by any project's index for it;
- **loudness as ITU-R BS.1770 defines it** — the measurement a delivery is
  actually judged against, rather than the peak meter that says almost nothing
  about it — checked against EBU Tech 3341's own compliance cases, which are
  *generated* rather than shipped: a −23 dBFS tone reads −23.0 LUFS and a −33
  one reads −33.0, both inside the tenth of a unit the standard allows;
- **a mixer's arithmetic**, computed with integers because Phipia preserves no
  floating-point state and because `pow` is not specified bit-for-bit anyway:
  zero decibels is **exactly** one, twenty decibels is **exactly** ten, and six
  decibels is asserted *not* to be a doubling — two full-scale sources trimmed
  six decibels each still clip, which is what the 6 versus 6.020599913
  difference actually costs. A constant-power pan holds `left² + right²` at one
  across the image, a signal against its own inverse nulls to precisely zero,
  and full scale is **reported rather than reached quietly**;
- **parameters that change over time** — the keyframes and Bézier eases every
  editor draws as two handles — held past their ends rather than extrapolated,
  because continuing the slope is how a parameter set to reach 100% at the end
  of a shot arrives at 340% two shots later. Linear is *exact*: a twenty-four
  frame ramp is `n/24` at frame `n`, thirds and sevenths included. The ease is
  where the honesty is: solving `x(t) = time` on a cubic needs a cube root,
  which is not rational, so there is no exact answer to find — the parameter is
  bisected to one part in a million, evaluated in 128-bit integers, and rounded
  once. The size of the approximation is stated and asserted rather than left to
  a floating-point library to decide differently on each machine. The first
  parameter to read one is a picture track's opacity, which *multiplies* what
  the clips on it are doing rather than replacing it — two things decide what is
  on screen during a dissolve inside a fade, and either alone throws the other
  away;
- **a fader that moves while the sound is playing.** A gain applied one frame at
  a time and held flat puts a step at every frame boundary — a buzz at the frame
  rate on any fast move. So the mixer takes a *ramp*, whose interval is half
  open: it starts at one gain and arrives at the next at the sample *after* its
  last, which is the next block's first. That is what makes consecutive blocks
  tile a fader move rather than repeat a value at every seam — and a repetition
  at a regular interval is a tone. Mute is not a fader position but a switch, so
  a muted track stays muted whatever is drawn on it;
- **the waveform a timeline draws sound from**, summarised once into a pyramid
  of blocks rather than re-reduced on every scroll — the lowest sample, the
  highest, and the mean square. Two numbers rather than one magnitude, because
  brass and speech and a kick drum genuinely lean one way and a mirrored
  drawing is a picture of a signal nobody recorded. The highest of two highests
  is the highest, so **one sample at the rails in half a second of silence is
  visible at every zoom** — a click cannot hide when you zoom out, and cannot
  be invented either. The one number that is not exact, the mean square, folds
  upward in 128-bit sums and divides once, so it is within *one* however far
  out you go rather than one per level. It stores as `SPPK`, whose header holds
  the digest of the sound it summarises — so a stale peak file is something you
  can *see*, rather than infer from a modification time that a copy, a restore
  or a clock change will happily lie about;
- **three-dimensional lookup tables**, the form every grade travels in —
  interpolated *tetrahedrally*, which is not a preference but a measured
  difference: on the neutral axis the four terms telescope to a straight run
  between the cell's diagonal corners, so **a grey stays grey exactly**, while
  trilinear mixes all eight corners and tints twenty-nine greys out of
  thirty-nine on the same table. Both are implemented, and trilinear is there
  to be failed by that test — a design decision with no test showing what the
  rejected option does is a preference rather than a decision. They arrive as
  `.cube` files, whose decimal text is read *exactly* — `0.123456` is
  `123456/1000000` — because going through a binary float would throw that away
  on the way in, in a project with no floating point anywhere else. Applied in
  the encoding the table was *authored for* — declared, not inferred, and a
  frame in another encoding is refused — and deliberately **not** in linear
  light, which is the opposite of the compositor and right for the same reason:
  apply an operation in the space its definition is written in;
- **CMX 3600 edit decision lists**, read and written — the interchange format
  every other system still speaks — with each of its four traps named by a test
  that fails when the trap is sprung: the exclusive out point, the stateful
  `FCM` line, the two disagreeing statements of drop-frame, and the eight
  characters a reel name gets. Every prefix of a valid event line is swept and
  must be refused;
- **a sequence conforms to one of those lists and comes back**, and the claim
  is a theorem rather than a hope: if the export leaves nothing behind, writing
  the list, parsing it and importing the result produces a sequence **equal** to
  the one that went in — by `PartialEq` on the whole value, so a field nobody
  thought to compare is compared anyway. What it cannot carry is counted and
  reported rather than dropped quietly, and the line between reporting and
  refusing is drawn where the frames are: a grade or a fader leaves a cut that
  is still correct and only looks wrong, while a second picture track written
  to the format's one video channel would be a different programme. Reel names
  are the source digest's first eight characters, with the whole of it in the
  comment, so two sources that would share one are refused rather than written.
  The importer read its frame numbers a quarter too fast until a test went
  through the *text* — the parser labels everything at thirty because the file
  cannot say, and a round trip that skips the file compares a value with
  itself;
- **shapes, rasterised by exact area** — the coverage of a pixel is the area of
  it that lies inside the shape, computed as a rational and quantised once,
  rather than sampled at the centre or at sixteen sub-positions. That is what
  keeps a wipe's edge sliding rather than crawling: the area under a line moves
  continuously as the line does and a sample does not. Checked three
  independent ways — a closed form the rasteriser never calls, agreeing pixel
  for pixel at six orientations; a rectangle's coverage against the product of
  its two overlaps; and the strongest, a relationship rather than a bound, that
  **a shape's coverage summed over the picture is the exact area it encloses**.
  A wipe then needs no compositing operator of its own, for the reason a
  dissolve does not: mask the incoming layer with the plane and put it `over`
  the outgoing one;
- **wipes, which are dissolves that spend their fraction differently** — the
  two are timed identically at the same cut, and a test compares their layer
  stacks frame by frame across a whole programme and requires them to agree
  about everything except what the fraction is *for*. A dissolve spends it on
  the incoming layer's opacity; a wipe carries it, and both clips stay whole
  because the incoming one is not half-faded but entirely there behind an edge.
  The direction is a **rational vector rather than an angle**, so straight
  across is `(1, 0)` and a true diagonal is `(1, 1)` rather than a rounding of
  forty-five degrees — an angle would need a sine and a cosine and neither is
  exact. Its length carries no meaning, which is a test, because a
  normalisation creeping in would need a square root. Their edges are hard or
  **soft**, and a soft one is exact too: the integral of an affine ramp over a
  polygon is its area times its value at the polygon's centroid, which is the
  definition of a centroid rather than a result about it — so a feathered edge
  is two clips and a moment rather than the "much larger case analysis" this
  project had written down and believed. Softness is a fraction of the wipe's
  travel, so it means the same thing at every size and angle;
- **masks on clips**, the same coverage machinery pointed at a clip instead of
  a transition. Corners are fractions of the frame, so a mask drawn on a proxy
  is the same mask on the finish. The winding is *measured* from the polygon's
  own area rather than demanded of the caller — getting it wrong inverts the
  mask, which is the most confusing failure a mask has — and an inversion flips
  the **byte** rather than the shape, so the two sides sum to exactly full
  coverage at every pixel. A concave outline is refused rather than quietly
  replaced by its convex hull, which would be a different shape drawn by
  nobody;
- **a reference capture that is a picture** — at
  [`tests/golden/reference.png`](tests/golden/reference.png), which is
  colour bars under a ramp and a flat colour meeting at a soft wipe, both
  inside a six-sided mask. On a mismatch the test writes what it actually
  rendered beside the reference and names both paths, because a hash says
  something changed and two files say what;
- **one asset per digest**, which is what content addressing already meant and
  what a stated theorem quietly depended on. Two identifiers naming one digest
  falsified conform's round trip — the export reported nothing lost and the
  import resolved both clips to whichever it found first. Adding the same
  content again now gives back the identifier it already has; the same bytes
  described two different ways is refused. Assets carry a **location hint**,
  which is bytes rather than text because a path is whatever the platform says
  it is, and relinking is that hint moving and nothing else — pointing a clip
  at different bytes is different media, and the digest says so;
- **a project that opens when the drive is not mounted** — a clip whose media
  is missing renders an offline slate rather than failing the whole frame. The
  fallback is in the *planner*, not the graph, and that is forced: a source
  node's identity covers the media, the tick and the description and **not**
  whether the bytes were reachable, so a node that fell back while evaluating
  would cache the slate under the real picture's key and serve it after the
  drive came home. The test renders twice through one pool. The slate is
  diagonal stripes whose period is a *fraction of the frame* rather than a
  pixel count, because a fixed period is a solid colour on a small frame, which
  is exactly where a slate must not look like footage;
- **resampling**, which scaling a clip needs and which is where "looks about
  right" hides the most. It happens in **linear light**, because an average
  only means something over quantities that add, and on **premultiplied**
  samples, because averaging straight ones across an edge is the dark fringe
  round every badly keyed title. The forward map is inverted exactly — a
  rational two-by-two inverse is a determinant and four divisions — and a map
  with no inverse is refused. Area weighting is the exact overlap and is right
  for reduction; bilinear is right for enlargement; **choosing is the caller's**
  rather than a heuristic keyed on the scale factor, which would change a
  picture's look the moment somebody dragged past 100%. The general
  parallelogram path is checked against a product of one-dimensional overlaps
  it never forms;
- **transforms on clips**, which is that resampler given something to move. The
  linear part is dimensionless and the move is in fractions of the frame, so a
  project cut on a proxy and finished four times larger keeps the framing
  somebody chose. It acts about the frame's **centre**, because scaling about
  the corner sends a picture sliding off the bottom right the moment a slider
  moves. There is no rotation-in-degrees — four rationals instead, for the
  reason the wipe's direction is a vector — so a half, a third, a mirror and a
  quarter turn are all exact. A **mirror is not a refusal**: it has a negative
  determinant, and it is the *zero* one that has no inverse. And a transform
  that moves nothing goes through no resampler at all, which a test asserts by
  counting nodes rather than comparing pixels;
- **motion**, which is that framing given a curve. M4.6 named "a scale that
  pushes in" in its opening line and then deferred it, on the grounds that a
  curve on an item would need a keyframe name surviving a renumbering — and it
  does not, because the curve goes **on the clip**, where there is no index to
  survive. It is measured from the clip's own start, so moving a shot down the
  timeline moves its push-in with it rather than re-timing it. Which means a
  **cut re-bases the tail**: keyframes before the cut go negative rather than
  being dropped, because a curve holds its first value before its first
  keyframe and dropping that pair would flatten a move already underway into a
  hold. Join is the exact inverse, and refuses two halves whose animations do
  not line up. The renderer did not change by one line: the layer stack hands
  out a *resolved* transform, so nothing below it ever learns that anything
  moves — a claim with a test, and a third render beside it at a different
  framing, because the first two would agree just as well if the framing were
  being dropped on the floor;
- **rolling a cut and sliding an item**, the two trims a track could not do.
  A roll moves a cut without changing how long the programme is, so nothing
  after the cut moves and nothing after the cut has to be moved back; a slide
  moves an item without changing the item at all, its neighbours giving and
  taking to make room. **Both are their own inverses** with the sign turned
  round, which is why neither edit has to remember what it replaced. A
  dissolve&#39;s two conditions are about exactly what a trim changes — how long
  each side is, and how far into its media the incoming one starts — so a trim
  re-checks the dissolves it moved rather than trusting a check that ran when
  somebody drew them. And **removing an item was already a ripple delete**: a
  track stores no positions, so there is never a hole to close;
- **a face, written from scratch**, because one could not be taken from
  anywhere: every outline format worth reading is a parser and a hinting
  engine, every free face is somebody else's licence, and a bitmap would have
  to be drawn again at every size. A glyph here is a handful of **convex pieces
  that touch but never overlap**, so its coverage is their *sum* — exact, by
  the rasteriser wipes and masks already use, with no reasoning about
  antialiasing at all. Disjointness is measured rather than trusted: for every
  pair of pieces in every glyph, the exact area of their intersection, which
  must be nought. And `quantise` refuses a coverage above full, so an
  overlapping face is refused rather than drawn wrong. The face is therefore
  **the same shape at every size** — a glyph at twice the size covers exactly
  four times the area — with no hinting and no grid to snap to. Capitals,
  digits, lowercase and enough punctuation for a timecode, a digest and a
  name. Capitals were **one** measurement — cap line to baseline and nothing
  else — and lowercase needed three more: an x-height the bodies sit on,
  ascenders that reach the cap line, and descenders that hang below the
  baseline. Those four numbers are not a comment: a test measures every glyph
  against them, so a letter that drifted off its own line would fail rather
  than merely look wrong. A character it
  cannot set is **refused by name** rather than drawn as a box, because a
  slate that prints a message it was not given is the one thing a slate must
  not do. The whole repertoire is committed as
  [`tests/golden/specimen.png`](tests/golden/specimen.png) and compared byte
  for byte, because every other test would pass on a face whose letters were
  the wrong letters. And the face is a **table**, not a program that builds
  one: written as construction code it was the largest single item in the whole
  image at 23,807 bytes — a coordinate in a function body is an *instruction
  that stores a coordinate*, and there are some two thousand of them — so it
  moved into read-only data and gave **four pages** back. The specimen came out
  byte for byte identical, which is the proof the change was a change to how
  the face is written rather than to what it says;
- **and the offline slate says which media is missing**, which is the sentence
  that stood in this file's risk section for three milestones. The digest
  rather than a file name, because the digest is what the clip refers to: a
  name is a hint that may have moved. A legend carries **two** captions — the
  whole sentence and the part that matters — because a caption on a proxy has
  a real choice to make and neither answer is right at both sizes: at 320
  across it reads `MEDIA OFFLINE 4F3C9A21`, at 160 just the digest, and below
  that **nothing at all**, because a slate whose caption is a grey smear has
  told the viewer something false about how much it knows. The type is
  premultiplied **in light**, through the same conversion every other layer
  goes through; writing the coverage byte into the colour channels is the
  obvious way to build white type and is too dark along every edge by exactly
  the amount the transfer curve bends;
- **titles, which are media** — not a new kind of item and not a property of a
  clip, but an asset a clip cuts from like any other. That is the whole design:
  trimming, rolling, sliding, splitting, joining, dissolving, grading, masking,
  moving and animating a title all work already, and not one of them had to be
  told what a title is. A title is **named by what it says** — its digest is
  the digest of its own description — so the same card in two projects is one
  asset, two clips of it share a cached frame, and changing a word makes a
  *different* asset rather than quietly changing every clip of it. It has
  nowhere to be, so it cannot be relinked and does not need to be; and it is
  never offline, so the library is never even asked whether it has one. Its
  colour is three fractions of **full light** rather than three code values: a
  byte is a number in an encoding and the same byte is a different colour in
  sRGB than in a linear working space, so the ink means the same thing
  everywhere and the frame's own table spells it — 255 in full range, 235 in
  limited, and half of full light is 188 rather than 128. A card says as many
  lines as it
  needs, aligned left, centred or right, and the two questions stay apart:
  where the *block* goes is the card's own place, and the alignment is only how
  the lines sit against one another — so moving a left-aligned card does not
  re-align it. The lines stack at the face's **own** line spacing rather than
  at the em: this face descends, and lines an em apart would put every `g` in
  one line through every `A` in the next — where the two would sum past full
  coverage and the card would be *refused* rather than drawn heavy;
- **a fade on a clip**, which is the gesture a cut cannot make. A dissolve
  sits at a cut and needs two clips; the first item of a programme has nothing
  before it, so until this there was no way to bring a programme up from black
  at all. It rises from **nought** on the clip's own first frame and falls back
  to nought on its last — a different question from a dissolve, whose fraction
  never reaches either end because a frame there would repeat a neighbour. A
  fade from black *is* the black. Where the two fades of one clip meet, the
  smaller wins; where a clip's fade meets a dissolve at its cut, they multiply.
  A trim shorter than the fades on it is **refused** rather than silently
  re-timing somebody's fade;
- and **a bug that fade found**. Compositing a faded or masked layer scaled its
  premultiplied colour in *code values* — which this module's own header has
  forbidden since its first version: "a premultiplied sample is the encoding of
  light × coverage, not the encoded value scaled by coverage". A dissolve
  between two **identical** pictures sagged by twenty-eight code values in the
  middle. Every test the project had faded a layer that was **black**, where
  nought times anything is nought and the two arithmetics agree — the third
  time that blind spot has cost something here, and this time it had corrupted
  a test written specifically to pin the difference: the wipe's edge pixel
  asserted 154 with a comment saying the linear answer was the darker one, and
  both the number and the moral were the bug talking. It is 205, derived by
  hand, and `picture red` moved from 73 to 98;
- **retiming**: a clip plays its media at an exact rational speed. It keeps its
  length on the timeline; what changes is how much media it consumes to fill
  it. A clip at `24/25` is the standard pull-down and a clip at `0.96` is a
  rounding of it that drifts a frame every twenty-five seconds — slowly enough
  that nobody notices until a delivery. The **size** of the speed says how far
  and the **sign** says which way, so a reversed clip shows exactly the frames
  its forward twin shows; flooring `offset × speed` directly would round the
  other way and give `100, 99, 99, 98` against a forward `100, 100, 101, 101`.
  A reverse that would read before its media is refused when the speed is
  *set*, a speed of nought is refused as a freeze by another name, and sound is
  refused at any speed but real time until there is a resampler to pitch it;
- **a freeze**, which retiming named while refusing to be it: a speed of nought
  "would show one frame forever and consume no media — a freeze, which is a
  different edit with a different name". The second half of that sentence is
  the design. A freeze does *not* consume no media: it consumes exactly one
  frame, and `floor(offset × 0)` cannot say so — it puts the source end at the
  in point, claiming a clip that shows a frame reads none of it. So playback is
  two cases, at a speed or frozen, and a still's span is a single tick — which
  is what lets it be **held past the end of its own media**, as a still should
  be. Two stills join when they hold the *same* frame, because a still cut in
  two is two stills of one frame and join is the exact inverse of split; a
  still beside a moving clip does not join even where the arithmetic lines up.
  Sound is refused a freeze for a sharper reason than a speed: a held frame of
  sound is a held *block* of samples, which is a tone at the block rate;
- **a clip that animates itself**. A fade is the quick answer — two lengths and
  a straight ramp — and this is the general one: a curve on the clip with
  whatever shape somebody drew, a hold, a linear run, an ease. The two
  **multiply**, like everything else here that decides what is on screen.
  Measured from the clip's own start, so a ripple moves the animation with the
  shot, and a cut **re-bases** the tail rather than restarting it. An
  overshooting ease is clamped at the read, exactly as a track's automation is,
  because a layer past full coverage is a frame the compositor refuses. Sound
  is refused an opacity — not because sound cannot fade, but because its
  loudness is decibels and an opacity is a coverage. And it animates a **title**
  with no code of its own, because a title is media and a clip cuts from it
  like any other: a card that fades up and pushes in is a clip with a curve and
  a motion;
- and **a page count that was credited to the wrong thing**. That milestone
  only added — a field, an edit, two functions, a lane in the file — and the
  image *fell* two pages. Rather than write "it paid for itself", the claim was
  tested: on the previous commit, twenty-four bytes of dummy padding in `Clip`
  produce the same two pages and the same 3.4 KB off `Edit::apply`. The saving
  was bought by the clip crossing 320 bytes, past which the optimiser stops
  copying a clip inline into each of `apply`'s arms. **A struct getting bigger
  made the program smaller**, which is the opposite of what every earlier
  footprint note here assumed while reading a total;
- **captions that track the edit**, which is the one feature here where the
  arithmetic had a wall in it. A caption is a range of the **media** — a source
  in-point and out-point — and where it lands in the programme is never stored:
  it is computed from whatever clips happen to be reading that stretch of that
  recording. So **not one line of `Edit` knows captions exist**, and cut,
  ripple, roll, slip, retime and undo all move them correctly with no
  bookkeeping to forget. Slip is the one that shows why: slipping changes
  *which* of the recording is shown without moving the clip, so the words change
  and their position does not — exactly backwards from a timeline-anchored
  caption. The cost is inverting the map a clip applies to time, and on a
  **ramp** that map is the integral of the speed curve — rectangles and
  trapeziums, so the
  integral is *quadratic* and its inverse wants a **square root**, which is not
  rational. The same wall the cubic ease hit from the other side. The way
  through is that a caption needs a **frame**, not a moment — and the frames
  of a clip are a monotone integer sequence, so the inverse is a lower bound
  found by
  halving, every comparison exact, twenty of them for a clip a million frames
  long. Not an approximation of the right offset: the right offset, without ever
  asking a question arithmetic cannot answer. A test walks every frame of every
  fixture and requires the bisection to agree at seven speeds and four ramp
  shapes. And a frame **covers** a source range rather than sampling a tick,
  which a test found in one line: at triple speed the frames show 100, 103, 106,
  so a caption over 104 and 105 lands on no frame and would have vanished —
  speed a clip up and two thirds of a transcript drops out silently. That one
  rule also dissolved three cases that looked special, including the freeze,
  whose own guard was deleted because the general rule already gave its answer;
- **sound in the export**, which is where a frame not holding a whole number of
  samples finally decides a *format*. A frame at 30000/1001 covers 1601.6
  samples, so interleaving picture and sound would make the offset of frame `k`
  depend on how many samples preceded it — and `SPRW` rests on frame `k` living
  at a computed offset. So the samples go in one run **after** every picture,
  which keeps both offsets exact and costs nothing to write, since both halves
  still go down in one pass. The samples are interleaved across channels where
  the pictures are one plane, and those look like opposite decisions and are the
  same one: **a streaming writer needs its bytes in the order they are
  produced.** A reel of `n` frames holds `⌊n·r⌋` **or** `⌈n·r⌉` samples, and
  which one depends on where the take was cut from — which a reel does not
  record — so the header carries the count and the reader checks the bound,
  exactly: three
  frames hold 4804 or 4805 and never 4803. Streaming the mix cost nothing
  because `mix` already walked a span frame by frame, so mixing one frame at a
  time gives the same samples by construction; one frame of 48 kHz stereo is
  12,816 bytes against 3,840,000 for ten seconds. And the export now returns
  what **clipped**, because the samples in the file are the clipped ones and
  they look exactly like samples somebody meant;
- **a sequence exported a row at a time**, which is the far end of a chain four
  milestones long and the first function here that renders a programme and puts
  it somewhere. A 1920×1080 RGBA frame is 8,294,400 bytes, so ten seconds of it
  at 24 frames is a reel of 1,990,656,000 against the 76 KiB a Phipia program is
  mapped — twenty-five thousand times the address space. The measurement that
  matters is not how many writes but **how large the largest was**: on a
  64-wide programme the export never hands storage more than **256 bytes** at
  once, which is one row and a third of one picture. `SPRW` moved to version
  three for it, and that is the argued part: the digest left the header for a
  trailer, because a digest in the header means *the first byte of the file
  cannot be written until the last byte of the payload is known* — version
  two's own encoder built the whole payload before it could assemble a file,
  which is a strange thing for a format meant to be what a recorder writes. It
  also makes a torn write structurally detectable, in sixty-four bytes rather
  than half a gibibyte of rehashing. The file is the same length either way,
  which was measured rather than assumed: the slate's reel is 528 bytes under
  both, and exactly one of thirty-four lines in the golden transcript moved. The
  seam gained an `append` and deliberately **not** a write at an offset —
  Phipia's FAT32 offers both, and a writer that cannot seek backwards cannot
  damage what it has already written. And R-9.4 now covers a save that takes
  fourteen steps rather than one, with a refusal driven through **every one of
  them**;
- **a sequence scanned a row at a time**, which is the first caller of the row
  form a person would actually write. `Scan::open` plans an instant and
  `Scan::row` renders one row of it, and it is a *type* rather than a function
  for a reason with a number on it: planning walks the stack, resolves every
  clip's media and builds a node per decoration, so doing it once a row would
  do it a thousand and eighty times to render one 1080-line frame — trading the
  allocation the row form exists to avoid for a worse cost in a different
  currency. A test's whole job is that number. The property above it is the one
  at the top of the program: **a scan's rows are the frame the render makes** —
  two layers, the upper one fading, composited over black, computed both ways
  and equal. `open` refuses a *programme* that cannot be scanned — a framing, a
  title, an offline slate — before a row is rendered, and it does **not** ask
  whether the library can serve rows: `Library::row` already refuses by
  default, so a flag beside it would be a second statement of the same fact
  free to disagree with the method, and there is no lateness to prevent, since
  every source is touched by row nought and a library that cannot serve rows
  refuses on the *first* row and never on the four hundredth. A guard whose
  absence changes no answer is one no test can hold, and this project has found
  and deleted three. So the boundary is written down and pinned instead: a
  library that serves whole frames only opens a scan, refuses its first row,
  and renders the same programme whole;
- **the render graph, a row at a time**, which closes the chain the three
  milestones before it built. One property does the work: for every node and
  every row, `row(id, y)` is **byte for byte** the *y*-th row of
  `evaluate(id)` — two evaluators that disagreed anywhere would be two answers
  to one question. A 1920×1080 RGBA frame is 8,294,400 bytes against the 76 KiB
  a Phipia program is mapped, so the whole-frame evaluator allocates a hundred
  and six times the program's address space **per node**; one row is 7,680.
  Two refusals, and the difference between them is the point: `NotRowLocal` is
  about the *operation* — a resampled row's preimage is a line, and only a
  linear map taking horizontals to horizontals makes it a row, which a rotation
  never does; a subsampled format is refused the same way for a sharper reason,
  since one chroma row serves two luma rows. `NoRowForm` is about *this build*,
  and somebody reading a refusal needs to know whether to wait for a version or
  change the question. The `Library::row` default **refuses** rather than
  fetching a frame and slicing it, because a default that quietly loaded eight
  megabytes to hand back six thousand bytes would make the whole thing look
  like it was working while doing exactly what it exists to avoid. Masks and
  wipes are placed against the *whole* frame — a shape stretched over one row
  is a different shape — so the rasteriser grew a range form and the frame form
  now goes through it. And the end-to-end measure is the one that decides
  whether a reader fits: not how many reads but **how large the largest was** —
  256 bytes on the row path against 1,024 on the frame path, differing by
  exactly the height of the picture;
- **a reel read a frame at a time, and a row at a time**, which is the ceiling
  the milestone before this one named and left standing. A reel this build
  writes is bounded at 512 MiB against the 76 KiB a Phipia program is mapped —
  **6,899×** what there is, so `decode` is a function that cannot be called on
  the target. A `Spool` holds a description, a rate and a count; frame *k*
  lives at `96 + k × packed_bytes`, which is arithmetic rather than a search
  because every frame in a reel shares one description and is therefore one
  size — a property the format argued for on other grounds and this is where it
  is cashed. `Spool::open` and `decode` now read the same header through the
  same function, because a streaming reader that admitted a file the loading
  one refused would be a second, weaker door into one format; a test drives
  four header mutations through both and requires the **same refusal** from
  each. And the honest part: streaming removes the *reel* from the arithmetic
  and leaves the *frame*, which at HD is 6,220,800 bytes — eighty times the
  program's map — so `plane_row` exists, and a row of a 1920-wide RGB picture
  is 5,760 bytes, seven per cent of that address space rather than eighty times
  it. Phipia's own bitmap reader already does exactly this, issuing random row
  reads rather than holding a picture: two programs arriving at one shape from
  opposite ends of one constraint. The chain is now four links each built to
  avoid holding the one above — catalogue, material, spool, frame — and the
  test that says so asserts the whole-slot read count is *unchanged* across an
  open, a search, five frame reads, a verification and four render queries;
- **the vault, through the storage seam** — and the connecting turned out to be
  three facts rather than one. A slot of its own, because material is large and
  changes rarely while a project is small and changes constantly, and writing
  sixteen mebibytes of photographs every time somebody trims a clip is a save
  protocol nobody can afford to run; so `commit` names its destination, and
  committing into the *scratch* is refused because that is where a save is
  assembled. And a **ranged read**, which is the whole point: one of Phipia's
  files holds sixteen mebibytes and a Phipia program is mapped seventy-six
  kilobytes, so reading a whole vault on the target is off by 220× — not a
  tight fit, not a thing to optimise later. Phipia's own bitmap reader already
  solved this one layer down by issuing random row reads rather than holding a
  picture, so the seam took the same shape, short at the end rather than
  refused because that is what `phipfs_read` does. A `Catalogue` then holds a
  count and a payload length and **nothing else**: an entry is one 112-byte
  read, a search is a bounded scan, material comes back a window at a time so a
  caller with less memory than a photograph can read one. `open` deliberately
  does **not** check the seal — that would mean reading every byte, which is
  what a catalogue exists to avoid — and the doc says so out loud; `verify` is
  the expensive answer, walked in windows so it runs on a file larger than
  memory. The two are tested against each other: a vault with one byte changed
  opens perfectly well and fails verification. Three controls passed — a guard
  that was doubled, a save-failure battery the project had and the vault did
  not, and a counter that could never count;
- **Phipia's filesystem, and somewhere to paste a photograph into**. Written
  after reading Phipia 2.1.0 rather than from memory, and the design is
  dictated by two of its numbers: a directory holds **sixty-four** entries, so
  a hundred photographs cannot be a hundred files; and a name is **eight and
  three** from a fifty-two character alphabet, so even if they could be, they
  could not keep their names — an editor that renames somebody's material to
  `IMG~0007.BMP` on import has destroyed the only thing that said what it was.
  So the media library is **one** of Phipia's files with a store inside it:
  `SSV1`, a 56-byte header, 112 bytes an entry, and the material end to end.
  Two hundred and fifty-six items, four times what a directory holds, and
  16,748,488 bytes of payload — not a chosen number but what is left of sixteen
  mebibytes once a full index is in the file, asserted at compile time so the
  day either bound moves the build says which. Spans run end to end so there is
  one file for a given store; every digest is **recomputed** rather than
  believed, because Phipia says plainly that FAT32 is not journaled and an
  interruption may leave a mismatch — so a damaged cluster becomes a named
  refusal instead of a photograph filed under somebody else's name. And a vault
  *is* a render library: material is keyed by digest, a clip refers to media by
  digest, a source node asks for a digest — so "the file is in the vault" and
  "the picture renders" are one fact. A photograph is a reel of one frame and
  footage is a reel of many, which is why one answer does for both. Beside it
  is a 24-bit BMP codec bounded exactly as Phipia's importer is, so a file
  Phipia accepts this accepts. Six of twenty-three controls passed first time —
  the worst run this project has had and the most useful: five were the same
  gap, that no test had ever built a *hostile file*, and the sixth found that
  the seal over the header is redundant because every field in it is already
  checked;
- **a note that travels with the shot**, which is the other half of the pair
  M8.28 opened. Same type as a sequence's marker, same text bound, same
  one-per-instant rule; what differs is what the instant is measured *from*. A
  sequence's names a position in the programme and stays where it was put; a
  clip's names an offset from the shot's own start, so it moves when the shot
  moves, survives a trim, goes into the bin with a lift, and is divided by a
  cut. Which one an editor wants is decided by what the note is about, and
  offering only one would be deciding that for them. The bound is **eight** a
  clip against four thousand a sequence, and the difference is arithmetic: a
  clip's bound is paid *per clip*, and one track of 65,536 items at sixty-four
  notes each would ask for hundreds of megabytes on a machine with
  seventy-six kilobytes. There is deliberately no bound on *where* a note may
  sit, because a trim is not a delete: shorten a shot and the notes on the part
  you hid come back when you pull it out again. A note **at** a cut goes to the
  tail, which is the half-open convention every span here uses — and that makes
  notes the first thing a **join** has to merge rather than keep, so
  `Item::join` stopped being one call to `with_duration` for the first time
  since it was written, and can now refuse on capacity rather than on
  contiguity;
- **a speed that changes over a clip** — a ramp, and the first **integral** in
  this program. Every other animated parameter is read by asking a curve for
  its *value*; a clip's position in its media is where it has got to, which is
  the **area** under the speed curve. So `Curve::area_to` exists, and it is
  exact because the two shapes it integrates are the two a rational can hold:
  a hold is a rectangle and a straight run is a trapezium. An **ease** is
  refused, and that refusal is the sharpest thing here — the area under a cubic
  Bézier is exactly rational over a *whole* segment, and finding the parameter
  at a tick *inside* one means solving a cubic, which is the cube root the
  ease inversion approximates to one part in a million. A clip is asked where
  it has got to at every tick, so the case that would be exact is never the
  case that is asked. A keyframe at nought is refused because that is a freeze;
  a sign change is refused because a ramp that turns around reads part of its
  media twice and puts the span check in the wrong place. A cut re-bases the
  ramp like the four animation lanes beside it — and *that* found a real bug:
  re-basing puts keyframes below tick nought, where the walk had been charging
  a held rectangle for a stretch that was inside a segment, which came out as a
  frame and a half of drift on the first tail that was asked. Two controls also
  passed: one found a guard that only ever caught a harmless case and now has
  the test for the case it uniquely covers, and one found a timebase check
  whose absence changed no answer at all, which was deleted;
- **nested sequences** — a sequence used as material, cut into another
  sequence and trimmed, graded, masked, framed and animated like anything else.
  `ARCHITECTURE.md` predicted it would be a kind of `Item`, and the prediction
  was wrong: a nest is a kind of **media**, which is why it arrived with no new
  machinery — the razor blades one, a dissolve mixes two, `Motion` animates
  one, and not a line of any of them knows what a nest is. It is named by which
  sequence it *is* rather than by what is in it, because a digest over the
  contents would change at every keystroke and repoint every clip that referred
  to it; the content gets to matter in the render, where a nested clip becomes
  a subgraph and its cache key is a function of everything inside. Its length
  is the one duration that is a fact about the *project*, so the project
  refreshes it after every edit — and a refused edit then has to put back both
  halves, which a control caught it doing only one of. It composites onto
  **nothing** rather than onto black: a programme is opaque, but material that
  is absent is absent, and a nest on V2 that blanked out V1 wherever it
  happened to be empty would be useless for anything but a full-length one.
  Neither walk recurses (R-5.5 names this structure by name) — the model's
  cycle check carries its own path and the planner's is a bounded stack — and
  the file grew a third phase, because a nest's record names a sequence and a
  sequence's body names media, so the knot unties only on the one thing a
  sequence needs before either: its timebase;
- **markers**, which `ARCHITECTURE.md` has listed as planned since its first
  version. A note at an instant, with text — the one thing in this model that
  exists purely for the person editing: nothing renders it and no clip is
  affected by one. It does **not** move when an item ripples, because a note
  reading "the sync drifts here" is about a place on the timeline and an
  unrelated shot getting longer must not move it away from the thing it is
  about. That is a property of the *absence* of code, so the control **adds**
  the behaviour — a trim that slides every note — and the test that pins the
  decision fails. One per instant, because two at one instant is the same
  nothing as none: neither can be named. The text bound counts **characters**,
  and the fixture that proves it is the bound's worth of `é`, which is longer
  in bytes than in characters and is the only input that can tell the two rules
  apart;
- **a lift, which is the other delete**. Removing an item and closing the gap
  has always been here; taking the shot and *leaving* the hole never was, and
  half the deletes anybody performs are that one. The choice is not taste, it
  is about the rest of the programme: sound cut to picture stays in sync
  through a lift and slides through an extract, and an editor offering only one
  is making that decision for the user without saying so. It composes with the
  razor into the commonest gesture there is. Its inverse carries the shot back
  and refuses any slot that is not still the gap it left. And a dissolve on
  either boundary the item touches refuses the lift — *either boundary it
  touches*, not "at or after", because a lift renumbers nothing and the coarse
  check would refuse a lift at the head of a programme because of a dissolve at
  its end. The commit also retired an exemption **on schedule**: `Edit` carried
  a `clippy::large_enum_variant` `expect` arguing why *one* variant is bigger
  than the rest, a second variant now carries an item, the lint stopped firing,
  and the build said so — which is exactly what the paragraph beside it had
  predicted;
- **a razor, and the merge that undoes it**. Cutting one item on one track has
  been here since the model had items; what was missing is the gesture — a
  blade dragged **down** the timeline cuts every track it crosses, and dragged
  back it heals every cut it made. The difference is not convenience, it is
  **undo**: four splits are four entries in the history, and undoing once
  leaves three cuts behind. So a column is one edit whose inverse is one edit,
  over a set of tracks the edit itself carries — a `u128`, one bit per track,
  against a bound of 128 tracks and a compile-time assertion that the two
  numbers are one number. The set is *passed* rather than recomputed, which is
  what makes the inverse exact: a blade does not cut a track whose material has
  stopped or whose cut is already there, and a heal that recomputed could
  undo a cut nobody made. Both directions are two passes — work the whole
  answer out touching nothing, then publish it — so a refused razor leaves
  nothing behind. A control found that the *heal* had that reasoning and no
  test, which is the second half of an argument going untested because it felt
  like the safe half;
- **the point a framing acts about**. The centre was a good default and a poor
  rule — a lower third swings in on its left edge, and M8.24 made that sharper,
  because a turn about the centre was suddenly the only turn there was. The
  interesting part is what could *not* be done: acting about `a` rather than
  the centre contributes `(a − c) − M(a − c)`, which is a translation, so it
  looks foldable into the move the model already passes. That is true in
  **pixels** and false in **fractions**, which is the only space the model has
  — the vector to the anchor scales per axis and a rotation does not commute
  with that — so the folding is exact for a scale and wrong for every
  rotation, which is the case it was added for. There is a test that folds it
  and compares: on an 8×4 picture a diagonal map agrees byte for byte and a
  three-four-five turn does not. And a control here found a real gap: the
  anchor was in the render node's cache key and nothing had ever asked it to
  be, so two pivots differing only *down* the frame collided;
- **a turn, and it is exact**. This model said since M8.9 that it has no
  rotation, because "a sine and a cosine are not exact, and a project whose
  framing depended on them would drift". True of an *angle*; false of a
  **rotation**. Put `t = tan(θ/2)` and `cos = (1 − t²)/(1 + t²)`,
  `sin = 2t/(1 + t²)` is a rational pair for every rational `t`, with
  `cos² + sin² = 1` exactly and a determinant of exactly one — and the rational
  points on the circle are *dense*, so a quarter turn, a three-four-five, and
  everything between them are all available with nothing approximated. Turns
  compose without renormalising, so a thousand of them is still exactly a turn;
  four quarter turns are the identity on the nose; and a picture turned four
  times through the resampler comes back **byte for byte**. The type stores the
  *point*, because `t` reaches every rotation except the half turn, which sits
  at infinity; the *curve* stores `t`, because a curve needs somewhere
  unbounded to live. One lane turns a mask about its own centroid and a framing
  on the left of its base transform — `R·M`, not `M·R`, which differ exactly
  when the framing mirrors. The renderer did not change by a line, and the
  image did not move by a page. And testing the resampler's own long-standing
  claim that it works "however the picture is turned" found the sharpest
  fixture lesson yet: a **quarter turn cannot test rotation**, because a right
  angle sends the pixel grid onto itself and leaves every preimage
  axis-aligned. The mutation that proves it fails one test in 235, and it is
  neither quarter-turn test;
- **a grade that comes on over a shot** — the last place a parameter was a
  value where it could be a curve, which is the phrase the two milestones
  before it both ended on. Not *which* look: a digest is not a quantity and two
  tables have nothing between them to interpolate. What animates is the
  **strength**, nought for the clip untouched and one for the look applied
  exactly as it always was — and one is what an absent curve reads, which is
  why every project written before this keeps its looks. The mix happens in the
  table's own **code values**, `c + s·(f(c) − c)`, for the reason that module
  has given since its first version: apply an operation in the space its
  definition is written in. That is the **opposite direction** from the
  compositor, which mixes only in light, and both follow from the one rule. It
  is a testable difference rather than a stated one: half a look taking a
  mid-grey to black lands on 64, and the control that moves the mix into light
  lands on 92.374 — both derived by hand from the sRGB curve before either was
  run. And the commit corrected two sentences it had written itself: that the
  interpolation's arrangement is what makes a full-strength grade exact (it is
  the multiply, and both arrangements are exact at the ends), and that the
  format's field order is what lets a file's refusal see the grade (it is the
  builder chain, and swapping both halves of the format breaks nothing). Both
  were found by controls — one by specifying it, one by running it;
- **a mask that animates** — an iris that opens, a vignette that breathes, a
  shape that sweeps a card on. A uniform scale and a move, not the corners: a
  corner that moved on its own could turn a convex outline **concave** part way
  through, and this build computes an exact area only for a convex one, so
  per-corner animation would mean a refusal arriving at a *frame* rather than
  at the edit. It scales about the mask's **own area centroid** — a trapezoid's
  corners average to `(1/2, 1/2)` and its area balances at `(1/2, 4/9)`, and
  scaling about the wrong one drifts a shape sideways while it grows. Which
  gives a **text reveal** out of the two lanes already there: a strip scaled by
  `s` and moved right by `(s − 1)/8` keeps its left edge at nought and sweeps a
  card on. And the same milestone put the two pages back — `Clip` 344 → 416,
  the image 91 → 93 — so the threshold above is one effect at one size, not a
  trend to lean on;
- **a title's colour, named in light**. Titles shipped white with an argument
  that was right — three bytes in a model that has never held a colour would be
  three bytes in *which encoding* — and a conclusion that was not. The way out
  is to store fractions of **full light** rather than bytes: the same ink is
  255 in a full-range frame and 235 in a limited-range one, and half of full
  light is 188 rather than 128, because sRGB bends. Each of those numbers is
  derived from the definition in the test that asserts it. And it found a real
  refusal: a slate caption is antialiased, so packing a hard 255 made every
  partly covered pixel claim more light than its coverage allowed, and a
  limited-range slate failed with `NotPremultiplied` rather than drawing
  something slightly wrong. A card, whose stencil has no soft edge, was never
  affected — which the tests now say, one each, rather than one claim covering
  both;
- **the program renders**, on the freestanding target, and says what came out.
  The slate composites two layers through a fade at frame 12 of a 24-frame rise
  and prints the SHA-256 of the result — a golden render hash over the layer
  stack, the plan, the graph, the compositor and the pool. `picture red` is 98,
  and every step of that is derived by hand: fading a premultiplied layer
  scales its coverage too, and `over` works in linear light. Until this
  existed, `media-editor-render` had **no symbol in the image at all** — half
  the
  project was absent from every footprint recorded, and linking it cost
  seventeen pages;
- a freestanding program image that links at Phipia's user base as a static,
  non-PIE `ET_EXEC` with no dynamic section, no relocations, and no SIMD, built
  twice into different directories and compared byte for byte.

1353 tests, no third-party dependencies, no `unsafe` outside the two crates
that are allowed it, and every rule this project wrote down is enforced by
something that runs. 770 invariants have been checked by
deliberately breaking the code and requiring the break to be caught; four of
those found real bugs, nine found gaps in the tests themselves, two found a
sentence claiming more than the code delivers, and one
named the exact cost of a shortcut that had looked harmless. They
are listed, with what each refusal looked like, in
[Verification](docs/VERIFICATION.md).

**What it is waiting for:** `PHIP-01` through `PHIP-08` in
[the platform contract](docs/PLATFORM_CONTRACT.md) — the capabilities Phipia
must grow, each written as a measured profile in Phipia's own vocabulary.

## The shape

| Concern | Language |
| --- | --- |
| Project model, timeline, file management, undo/redo, media pipeline coordination, UI state | Rust |
| The single boundary to Phipia's userspace ABI, and later to external codec libraries | C ABI |
| Tiny freestanding shims where the boundary is instruction- or register-shaped | C |
| Sealed inner loops, only after a correct Rust implementation exists to measure against and stay bit-exact with | C++ |

Two crates may contain `unsafe`. Every other crate forbids it by attribute.

## Documents

| Document | Contents |
| --- | --- |
| [Charter](docs/CHARTER.md) | What Media Editor is, what it refuses to be, and why it is Phipia-only |
| [Engineering rules](docs/ENGINEERING_RULES.md) | The normative rules |
| [Open-source map](docs/DEPENDENCIES.md) | Every component considered, with licence and verdict |
| [Dependency policy](docs/DEPENDENCY_POLICY.md) | How a dependency enters the tree, and how it leaves |
| [Platform contract](docs/PLATFORM_CONTRACT.md) | What Phipia must provide, and what already works |
| [Architecture](docs/ARCHITECTURE.md) | The planned crate map and data model |
| [Roadmap](docs/ROADMAP.md) | Milestones, smallest first |
| [Verification](docs/VERIFICATION.md) | What counts as evidence |
| [Brand](docs/BRAND.md) | The mark, the palette, the naming, the voice |
| [Glossary](docs/GLOSSARY.md) | Editing vocabulary, defined exactly enough to implement |

## Checks

```sh
make hooks          # enable the repository's pre-commit check
make lint           # hygiene: whitespace, headers, links, width; the crate
                    # layering, against the block in docs/ARCHITECTURE.md that
                    # declares it; and every test and control count the
                    # documents assert, against the tree
make check          # rustfmt and clippy::pedantic, warnings denied
make test           # the host suite
make image          # the freestanding program image for Phipia
make audit          # R-13.4 and R-13.6 on the linked ELF, its control, and
                    # a breakdown of where the image's pages actually go
make reproducible   # two clean builds, compared byte for byte
make verify         # all of the above
```

The gates that need an emulator arrive with the capabilities that make them
possible; they are specified in [Verification](docs/VERIFICATION.md) so that no
milestone can quietly land without them.

## Licence

[GPL-3.0-only](LICENSE), the same licence as Phipia. Every dependency must be
compatible with it, verified from vendored source rather than from registry
metadata.
