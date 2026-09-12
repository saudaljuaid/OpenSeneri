<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Verification

Media Editor takes Phipia's position on evidence without softening it: a green
run
proves the checked contract under its recorded conditions, and nothing more. It
does not imply an untested format works, an untested machine boots, or a
performance number holds anywhere else.

## Gates

The commands below are the whole contract. Each milestone adds targets; none
removes one.

```sh
make lint           # repository hygiene: whitespace, tabs, headers, links, width
make check          # rustfmt and clippy::pedantic, warnings denied
make test           # host tests: unit, property, golden, and boundary
make image          # the freestanding program image
make audit          # R-13.4 and R-13.6 on the linked ELF, and its control
make reproducible   # two clean builds into different directories, compared
make verify         # all of the above
```

Two more arrive with what makes them possible: `make qemu-tests` with `PHIP-01`,
and a dependency gate with the first dependency.

Fuzzing has arrived early, in the form the rules require rather than the tool
they name. `cargo-fuzz` needs a network to vendor; until the gate can run, the
project file's decoder is swept deterministically instead — every byte of a
real file changed to five values, every prefix, every extension, and several
hundred thousand bytes of seeded garbage, all in `make test`. When
`cargo-fuzz` arrives it inherits the corpus; it does not start one.

`make verify` is the one that has to be believed, so it is the strictest. It
rejects: any compiler warning; any `clippy::pedantic` finding; unformatted
code; an undefined symbol; a relocation record; an unexpected linker section; a
global offset table past its bound or pointing outside the image; a PIE or
dynamic image; a W+X segment; an executable stack; an image based anywhere but
the agreed address; any floating-point, MMX, SSE, or AVX instruction while
`PHIP-04` is outstanding; and two clean builds that differ by a single byte.

It also runs `tools/audit-control.py`, which mutates a copy of the image into a
position-independent one and into one with an SSE instruction in its text, and
requires the audit to refuse both by name. The audit is therefore shown capable
of failing on every run rather than on the day someone remembers to check.

## What counts as evidence

**Acceptance is a QEMU scenario on Phipia.** Host tests are supporting
evidence. A behaviour that has only ever been observed on the host is not a
behaviour Media Editor claims — which is why nothing in this repository claims
the
program runs. It claims the image has a shape Phipia accepts, which is a
different and much smaller statement, and it is the one the audit proves.

**A scenario is bounded and named.** Each has a stable identifier, a required
serial transcript, an expected guest exit value, and a fixture built by a
committed deterministic tool. Fixtures are ordinary local files attached
read-only to emulated devices; host-device passthrough is never used as
evidence, exactly as Phipia requires.

**A screenshot shows presentation, not correctness.** Pixel comparisons are
used for what they can prove — that the drawn output has not changed — and a
one-pixel mutation must fail the comparison.

**A performance claim is a committed benchmark plus a machine profile.** A
number without both is an anecdote.

## Negative controls

Every invariant carries one, and the procedure is Phipia's: make one isolated
temporary mutation that violates the invariant, run the narrowest relevant
gate, observe the refusal by its name, and restore the source. The mutation is
never committed; the fact that it was performed, and the exact refusal it
produced, belongs in the pull request.

Examples this project will need:

| Invariant | Mutation | Expected refusal |
| --- | --- | --- |
| Undo restores exactly (R-9.2) | Drop one field from an edit's inverse | Property test fails on a specific generated sequence |
| Saves are atomic (R-9.4) | Interrupt the write after the first block | Previous file intact, named error, no partial file |
| Renders are deterministic (R-4.1) | Reorder one reduction | Golden hash mismatch |
| Frames are fully described (R-8.2) | Drop the transfer function from one frame | Named refusal before any conversion |
| Caches cannot go stale (R-8.5) | Omit the parameter set from the key | Test observes a wrong-parameter hit |
| The image shape is conforming (R-13.4) | Remove `-no-pie` | ELF audit fails on `Type: DYN` |
| Resources are released (R-5.6) | Skip one pool return | Census mismatch at teardown |
| A parser is bounded (R-11.2) | Raise one limit | Fuzz target finds the allocation immediately |
| A save verifies itself (R-9.4) | Drop the read-back comparison | The corrupting-storage test fails, and only that one |
| A file is digested (R-9.3) | Skip the digest check | The single-byte mutation sweep accepts a changed file |
| A reel's description is covered (R-8.2) | Digest only the samples | The sweep accepts a changed transfer function, which silently changes every frame |
| A transfer curve is the standard's (R-8.3) | Move sRGB's breakpoint by a factor of ten | Three tests fail by name: the reference values, the round trip, and monotonicity |
| Luma weights sum to the scale | Round green independently instead of taking the remainder | BT.2020's weights no longer sum, so a white field would not measure as white |
| A gamut conversion happens (R-8.3) | Skip the matrix | The saturated-colour test fails, and **only** that one |
| A node's identity covers its inputs (R-8.5) | Leave the input out of the digest | Two tests fail: the identity itself, and order-independence, because a shared cache then returns the wrong frame |
| Alpha association is stated (R-8.2) | Drop the check that a format with alpha must name one | Three tests fail: both description tests and the forged-file test, because an untagged frame becomes constructible |
| Alpha association is part of identity (R-8.5) | Leave the alpha tag out of the frame digest | Straight and premultiplied frames collide in the pool, and the golden digests move |
| A mezzanine carries the association (R-8.2) | Write a constant alpha tag | Nine tests fail, because every reel then describes something it does not hold |
| Compositing happens in light (R-8.3) | Add code values instead of decoding first | Four tests fail, including the hand-computed pixel, which lands 47 code values too bright |
| `over` trusts nothing (R-8.2) | Drop the premultiplied check from `over` | The dark-fringe test fails: relabelled straight samples are composited |
| A conversion carries coverage (R-8.2) | Write a constant alpha byte again | Three tests fail, one of them the round trip that used to assert the bug |
| A scope derives its matrix (R-8.3) | Use a fixed BT.601 matrix whatever the frame says | The green-box test fails and the axis test does **not** — the axes are matrix-independent by construction, which is why the green box is also asserted |
| A blank frame is black (R-8.2) | Fill it with zeroes again | Two tests fail: zero is below the legal floor of a limited-range luma plane, and is saturated blue-green in a chroma one |
| An EDL out point is exclusive (R-11.3) | Add one to the span | Two tests fail; without this every clip in every imported file is one frame long |
| `FCM` is stateful (R-11.3) | Read only the first one | Two tests fail, both about a file that changes mode halfway |
| Two statements of drop-frame must agree (R-1.3) | Trust the punctuation and ignore the `FCM` line | The conflict test fails: a contradictory file is read rather than refused |
| Keyword matching is not suffix matching | Match `DROP FRAME` before `NON-DROP FRAME` | Twelve tests fail, because every non-drop file is read as drop-frame |
| The pan law keeps power constant | Send the fraction itself rather than its square root | Three tests fail, including the centre, which lands 3 dB out |
| Clipping is reported (R-7.2) | Clip the sample but not the report | Two tests fail; the mix is unchanged and the mixer has stopped saying so |
| A sum rounds away from zero | Truncate towards zero instead | The half-scale mix test fails: every quiet passage drifts towards silence |
| Unity is exact — **control passed, claim was wrong** | Remove the zero-decibel fast path | *Nothing failed.* `pow` computes `exp2(y·log2(x))`, and at `y = 0` that is exactly one, so the shortcut was never load-bearing. The comment claiming it was is corrected, and a test now checks the general path reaches unity on its own |
| Higher tracks are on top | Composite the stack top first | Three tests fail, including the hand-computed half-covered pixel |
| A gap is transparent (R-9.1) | Let a gap stand in for its track's first clip | Two tests fail: an upper track with sparse material blanks out everything beneath it |
| A playhead reads the right source frame | Drop the clip's source start from the sum | Two tests fail; without them the whole clip plays the wrong material, not one frame of it |
| A sample position is the floor | Round to nearest instead | **Only one** test fails — the one asserting the definition. The tiling test passes either way, because rounding is monotone too, and the comment claiming tiling was the reason for the floor is corrected |
| A frame's sample count is not constant | Divide the span by the frame count | Two tests fail: at 29.97 the blocks stop tiling and stop being contiguous |
| A fader reaches the mix | Mix every track at unity | Three tests fail, including the muted track, which is heard |
| A fader survives a save (R-9.3) | Write unity for every track | Two tests fail; a delivered mix would arrive flat |
| An edit's inverse is where it was (R-9.2) | Return unity as the inverse | Undo moves the fader to a default rather than back |
| A faded layer stays premultiplied (R-8.2) | Scale the coverage and leave the colour | The opaque-dissolve test fails — and the white-to-black one does *not*, because black has no colour to scale. Two tests, two mutations, neither redundant |
| A dissolve is a real mix at every frame | Let the fraction reach nought and one | Two tests fail: the first and last frames of every dissolve repeat their neighbours |
| A dissolve uses handles (R-9.1) | Clamp each side to its own in and out points | The handle test fails; both ends of every dissolve would freeze on a frame |
| Every input is checked (R-6.1) — **control passed, gap was real** | Report only one of `Over`'s two inputs | *Nothing failed.* The identity computation reaches both inputs and refuses first, so two mechanisms enforce one rule — and the rule was resting on the one nobody had tested. A test now names both sides, and the same mutation fails it |
| A cache key covers the frame wanted (R-8.5) | Leave the tick out of a source node's identity | The caching test fails: every frame of a clip returns the first one |
| Media is named by content (R-8.5) | Name it by the project's track index instead | Eight tests fail, because every layer fetches the wrong footage |
| Opacity is part of identity (R-8.5) | Leave it out of a fade's identity | A dissolve caches its first frame and shows it for the whole transition |
| The octant table is a table | Replace it with a parity trick | Six tests fail, starting with a right angle's sine at nought — which is how the table came to be written out in the first place |
| An angle reduces by masking | Reduce with a remainder instead | The odd-and-even test fails: a remainder keeps the sign of a negative angle, so every angle before nought reflects rather than wrapping |
| The series is long enough — **control passed, and the reason is recorded** | Cut it from nine terms to five | *Nothing failed.* The result narrows to thirty-two fractional bits and five terms is already inside that. Nine is set for the wide value, not the narrow one, and the comment now says so |
| The K-weighting runs | Pass the signal through untouched | Three tests fail, both compliance cases among them |
| The high-pass runs — **control passed, gap was real** | Drop the second filter stage | *Nothing failed.* Every test used a 1 kHz tone, where that stage costs 0.03 dB. A 50 Hz test now exists, where it costs 3.9, and the same mutation fails it |
| The standard's offset is applied | Drop the -0.691 | Three tests fail; every reading sits 0.7 units high |
| Channels are summed, not averaged | Divide by the channel count | Four tests fail, including the one that pins a stereo pair at 3.01 units above mono |
| A block is four steps of overlap | Make it three | Five tests fail, both compliance cases among them: a block divided by a block's length must hold a block's energy |
| A peak survives every zoom | Average the extremes going up, as a naive downsample would | Two tests fail: the single click in half a second of silence is gone by the second level, and the widest zoom no longer agrees with the samples |
| A block holds two sides, not a magnitude | Store the reach and mirror it | Four tests fail, including the one that asserts a block reports samples that are actually in it |
| The mean square does not compound with zoom | Fold stored means instead of exact sums | The within-one bound fails at the levels above zero, where a floor of floors has drifted |
| The last block is divided by what it holds | Divide it by a full block's width | Two tests fail: three samples at a steady level read quieter than a full block of the same |
| Levels are folded within a channel | Pair across the channel boundary | Two tests fail — but only after the fixture was given an odd block count. See below |
| The pyramid reaches a single block | Stop one level early | Five tests fail, the file-shape check among them |
| Dependencies run downward | Add one from `media` up to `io` | `make lint` names the crate, the layer it is in, and the layer it reached for |
| Dependencies do not run sideways | Add one from `media` to `model`, its own layer | The same refusal: sideways is a violation, because two crates in a layer depending on each other are one crate that has not admitted it |
| The declared layers are the real ones | Restore the order the diagram used to draw | Three findings, which is the proof the old diagram was wrong rather than differently drawn |
| Every crate is declared | Drop `io` from the layer block | Two findings: the crate is in the tree and not the layers, and something depends on a crate that is not declared |
| A summary file is checked | Drop the digest comparison | The byte sweep and the shape test fail |
| A summary's digest covers its header | Hash the blocks only | Four tests fail, and a probe named the cost exactly: byte 8 and bytes 32 to 63 become undetectably editable — the sample rate, and the digest of the sound the summary is *of* |
| A summary's length follows from its header | Accept bytes past the shape | The trailing-bytes test fails: a summary's length is derived, so extra blocks are a disagreement rather than padding |
| Reserved fields are refused when set | Read them and ignore them | Twelve reserved bytes are accepted, one test names each |
| The block size obeys the summary's own rule | Check it against nought only | The header test fails on a block size of a hundred, which is not a power of two |
| A curve holds past its ends | Continue the slope before the first keyframe | The holding test fails: a parameter set to reach 100% arrives at more than that outside the keys that describe it |
| A curve inverts its horizontal Bézier | Use the time fraction as the parameter directly | The bent-handle case fails: eleven sixteenths along a span is where `t = 1/2` lives, and skipping the inversion reads it as eleven sixteenths |
| The inversion includes an exact hit | Make the bisection comparison strict | Three tests fail; every eased value lands one dyadic short |
| The segment search is not off by one | `<` for `<=` on the midpoint | Two tests fail, including the one that asks a curve for its own keyframes |
| Ease handles stay inside their span | Accept any horizontal | The refusal test fails; a folded curve has more than one value at an instant |
| The ease rounds to nearest — **control passed, gap was real** | Round towards nought | *Nothing failed.* The rule was stated in the code and pinned by no test. A case where the exact value is 1/10, which is 104857.6 parts in 2^20, now exists and the same mutation fails it |
| The interpolation form matters — **control passed, and the reason is recorded** | Write `from(1-f) + to·f` instead of `from + (to-from)f` | *Nothing failed*, correctly: with exact rationals the two forms are identical. The comment says as much and says why the habit is kept anyway — in fixed point they are not |
| Automation multiplies a dissolve | Drop the track's opacity at a transition | The dissolve-under-automation test fails: two things decide a layer's opacity there and either alone throws the other away |
| An opacity saturates rather than exceeding | Leave the curve's overshoot unclamped | The overshoot test fails; a track reads as more than fully opaque |
| An opacity curve reaches the file | Write the absence of one instead | Two tests fail — and, importantly, *not* the round-trip tests. See below |
| An unknown interpolation is refused | Read it as a hold | The unknown-tag test fails: an ease would become a step while the file still said ease |
| A sound track has a fader, not an opacity | Accept one on either kind | The refusal test fails |
| A fader ramp's interval is half open | Close it, arriving on the last sample | Three tests fail: the last sample lands on the target it should stop one step short of |
| A fader ramp actually moves | Hold the block at its starting value | Four tests fail, the tiling test among them |
| A fader ramp runs the right way | Swap its ends | Six tests fail; a fade down goes up |
| A moving fader still reports clipping | Count clipped samples as nought | Three tests fail, including the one that mixes a rising ramp against a full-scale source |
| The mixdown ramps within a frame | Hold each frame flat at its starting gain | Three tests fail: a fade over eight thousand samples takes four distinct values |
| Mute wins over automation | Let the curve speak for a muted track | The mute test fails; a track turned off comes back on because somebody drew a fade on it |
| An automated fader saturates | Leave the curve's overshoot unclamped | The saturation test fails — and only after a fixture existed that could overshoot. See below |
| The level curve reaches the file | Write the absence of one instead | The lane test fails — and only after the fixture animated *both* lanes. See below |
| The two lanes are written in order | Swap them | Six tests fail, the canonical-encoding test among them |
| Removing the last keyframe turns the lane off | Leave the lane on with an empty-looking curve | Two tests fail: undoing the first keyframe leaves a flat curve nobody asked for |
| A moved keyframe lands in its new place | Re-insert it at the index it came from | The reordering test fails; the curve is no longer in time order |
| A keyframe cannot land on another | Drop the collision check | The collision test fails: two values at one instant is the same nothing as none |
| A lane belongs to one kind of track | Accept either lane on either kind | The lane test fails |
| Setting a keyframe hands back the old value | Hand back the new one | The journal's own check fails it as `HistoryInconsistent`, which is what that check is for |
| Tetrahedral is not trilinear | Make it call the other one | Two tests fail; twenty-nine of thirty-nine greys pick up a tint |
| Each tetrahedron has the right vertices | Give one branch the wrong ones | Two tests fail, the continuity one among them — but only after that test existed. See below |
| A sample picks the right tetrahedron | Swap two of the six orderings | Two tests fail |
| A colour outside the table is held at its edge | Drop the clamp | The saturation test fails |
| The top of the range reaches the far corner | Read it as the start of the last cell | Four tests fail |
| The cube is indexed red-fastest | Transpose it | The identity test fails; a table that changes nothing changes everything |
| A `.cube` file is written red-fastest | Transpose the writer | Two round-trip tests fail against a fixture asymmetric in all three axes |
| A `.cube` domain is checked | Read the lines and drop the refusal | The domain test fails: a table authored for another input range would be the wrong look on every pixel |
| A one-dimensional table is refused | Skip the key like any other | The refusal test fails; a curve's samples would be read as a cube's |
| An over-range sample is not clamped | Clamp it into nought to one | Two tests fail; a highlight sent above white on purpose is flattened |
| A decimal is read to nine places | Keep three | Four tests fail, including the round trip, because the digits no longer survive |
| Samples come after the size | Accept them before it | The no-size test fails |
| A look is applied in the encoding it was made for | Feed any frame to the table | The encoding test fails; a show LUT would run on the wrong space with nothing crashing |
| A look needs straight coverage | Accept premultiplied | The coverage test fails: a non-linear function on premultiplied samples computes `f(ac)` where `a·f(c)` was wanted |
| Coverage is carried, not written | Write 255 into the alpha byte | The keyed test fails — the same mistake that made every keyed frame a solid rectangle in the conversion path |
| A look needs three colour channels | Accept any format | The format test fails |
| Colour channels are normalised as colour | Normalise them as chroma | Two tests fail, including the one where an identity table must change nothing |
| The footprint tool sees the arena | Halve `HEAP_BYTES` | It reports 34 pages rather than 42, with `.text` unmoved — which is also the measurement of what the arena costs |
| The footprint counts what is loaded | Leave `.bss` out of the loaded sections | It reports 25 pages, under-counting by the whole arena |
| A short lookup table file is refused | Pad it out to a full cube | Three tests fail, both sweeps among them |
| A fourth number on a sample line is refused | Ignore it | The sample-line test fails |
| A lookup table's lines are bounded | Remove the bound | Two tests fail |
| The documents' counts match the tree | Put a crate's count one behind | `make lint` names the crate, what it says, and what the tree holds |
| Every crate with tests states a count | Delete one row's count | It names the crate and says its row states none |
| The README's total matches | Put it one behind | It names both numbers |
| The README's control count matches | Claim two hundred | It names both numbers |
| A new test cannot land undocumented | Add one and change nothing else | Two findings, one per document, which is the case this exists for |
| A split gives both halves the look | Rebuild the tail from scratch | Two tests fail, including the one that says join is the inverse of split |
| Differently graded clips do not join | Stop comparing the grades | The join test fails; one look is discarded without saying so |
| A grade reaches the file | Write "no grade" for every clip | The grade round-trip fails |
| An unknown grade flag is refused | Read it as no grade | The flag test fails; a look is dropped while the file still says there is one |
| A grade reaches the layer stack | Report `None` on every layer | The stack test fails; a look nothing can apply |
| A look node names which look | Leave the digest out of its identity | The edited-grade test fails; the second render is answered out of the pool with the first |
| A look's identity is more than its samples | Leave out the interpolation, then the encoding | The identity test fails either way: a table read two ways is two looks |
| The timeline puts a grade in the graph | Ignore the clip's grade | Two tests fail — but only after they existed. See below |
| A graded layer is fetched straight | Fetch it premultiplied | Two tests fail; a look refuses the frame the compositor wants |
| A graded layer is re-associated | Leave it straight | Two tests fail; the render does not end premultiplied |
| The slate's picture depends on when it is rendered | Move the playhead one frame | The golden fails — but only after a fade existed. See below |
| The slate's picture depends on the fade | Never apply the track's opacity | The golden fails |
| Higher tracks are on top, on the target too | Swap the two layers | The golden fails |
| A capture's checksum covers the chunk name | Cover the data only | The CRC test fails against a checksum computed from the polynomial |
| A stored block carries its complement | Write the length twice | The stream test fails on the one redundancy a stored block has |
| Adler-32 uses the right modulus | Take it modulo 65,536 | The wrapping test fails — but only after a fixture existed whose sums reach a modulus at all |
| Every scanline is its own | Write the first row for all of them | The same test fails — but only after a fixture existed whose rows differ |
| A capture's rows are unfiltered | Claim a filter the rows do not use | Two tests fail |
| A capture refuses premultiplied coverage | Write it as though it were straight | The coverage test fails |
| The record timecode is the order, not the event number | Sort by the number | The renumbered-and-reversed test fails — but only after a fixture existed whose numbers disagree with the record |
| A label is recounted at the rate the caller stated | Take the parser's frame number as it stands | The round trip fails, by a quarter, for every list at 24 |
| A dissolve opens half its length, rounded down | Round it up | The odd-dissolve test fails: the picture moves by a frame |
| A dissolve gives the outgoing clip its tail back | Leave the clip as the event wrote it | The outgoing clip comes back twelve frames short |
| A dissolve's incoming clip starts early into its handles | Write its in point as the cut | The event's source in point is twelve frames late |
| A reel and its comment are two statements about one source | Do not compare them | The disagreeing-reel test stops refusing |
| Two sources may not share a reel name | Accept the second | The collision test stops refusing |
| Picture is written before sound | Never notice the order | The out-of-order test stops refusing |
| A trailing gap is reported as left behind | Do not count it | The trailing-gap test reports nothing lost, and the cut still comes back short |
| An out point is exclusive | Write one frame more | The source and record spans are each a frame too long |
| Coverage is an area, not a sample at the centre | Test the pixel's centre instead | The partial-column test fails: a quarter becomes nothing |
| The shoelace area is halved | Leave the sum doubled | Every whole pixel reads two |
| The closed form subtracts what runs past both sides | Subtract only the first | The two implementations stop agreeing |
| The closed form measures from the corner the normal points from | Always measure from the top left | The same, at every orientation with a negative coefficient |
| Quantising rounds half away from zero | Truncate | Half of 255 reads 127 rather than 128 |
| A coverage plane runs across before it runs down | Fill it column by column | The reading-order test fails |
| A mask scales the coverage with the colour | Scale the three colour channels only | The masked frame claims more colour than its coverage allows |
| A coverage plane is one byte per pixel | Do not check its length | A plane of the wrong size stops being refused |
| A wipe leaves both of its clips whole | Fade the incoming one as a dissolve would | The incoming clip shows through the outgoing one on the covered side |
| The wipe travels with the incoming layer | Report no wipe at all | Every wipe renders as a dissolve |
| A track's automation multiplies a wipe too | Apply the transition alone | A wipe inside a fade stops being faded |
| The file says which kind of transition | Write every kind as a dissolve | A wipe comes back a dissolve |
| An unknown transition tag is refused | Read anything unrecognised as a dissolve | A resealed file with a tag of two stops being refused |
| The renderer applies the wipe | Ignore the layer's wipe | A wipe and a dissolve become the same picture |
| A wipe's edge starts where its direction points from | Sweep from the far corner instead | The wipe covers everything at nought and nothing at one |
| A wipe is reported as left behind | Do not count it | A list that lost the edge reports nothing lost |
| The band is weighted by where in it each part lies | Weight the whole slab at a half | A pixel inside the band stops being the ramp at its centre |
| The first moments are area times centroid | Subtract the two coordinates instead of adding them | The same test fails |
| The band is centred on its edge | Start it at the edge instead of half a band before | A soft edge and its complement stop summing to one |
| The file carries a wipe's softness | Write nought for every wipe | A third comes back as a hard edge |
| A softness outside its range is refused | Do not check it | A softness of three halves stops being refused |
| The renderer is told the softness | Pass nought | Every soft wipe renders hard |
| A concave outline is refused | Follow whichever way the last corner turned | An arrowhead stops being refused |
| Corners in a line enclose nothing | Accept a polygon that never turns | Three collinear points become a mask |
| A mask survives a rebuild | Drop it in `with_source` | A slip loses the shape |
| A convex region's winding is measured, not assumed | Take one direction as given | The same shape given the other way round inverts — and this now guards the resampler too, which shares the constructor |
| An inversion flips the byte, not the shape | Ignore the flag | A mask and its inversion stop summing to full coverage |
| A mask's corners are fractions of the frame | Read them as pixels | A half-by-half rectangle stops covering a quarter |
| The file carries a clip's mask | Write the absent flag for every clip | A shape does not come back |
| A file's mask goes through the model's constructor | Build a rectangle instead | A file describing an arrowhead stops being refused |
| The renderer applies the mask | Ignore the layer's shape | Nothing is taken away |
| One asset per digest | Insert every asset unconditionally | The same content added twice becomes two identifiers |
| One digest cannot be two lengths | Accept whichever arrived first | A contradiction stops being refused |
| A hint is not part of what makes an asset the same | Overwrite the record's hint on a second add | Opening a file rewrites a project nobody edited |
| Moving a hint gives back the one it replaced | Return nothing | The caller cannot put it back |
| A hint that says nothing is refused | Accept no bytes at all | An empty hint looks like an answer |
| The file carries a location hint | Write the length and drop the bytes | A hint does not come back |
| A file listing one asset twice is refused | Let the reader fold them together | Clips indexing the second record point at the first |
| A planner asks whether media is there before naming it | Always name the source | An unavailable source is fetched, and the slate lands in the cache under the picture's key |
| An offline slate's period is a fraction of the frame | Fix it at sixteen pixels | The slate is a solid colour on a small frame |
| An offline slate runs diagonally | Vary along one axis only | It becomes bars, which a programme may contain |
| Each test pattern has its own identity | Give two the same tag | Five patterns produce four identities |
| Resampling happens in linear light | Average the code values | A checkerboard of 64 and 192 reads 188 rather than 146 — but only after a fixture whose values are not the transfer's fixed points |
| The weights are the exact overlap areas | Leave out the division by the footprint | A flat field stops surviving a scale |
| Bilinear samples at pixel centres | Sample at the corner | The picture shifts half a pixel up and left |
| Outside the source contributes nothing | Clamp to the nearest edge pixel | The edge column smears outwards instead of stopping |
| A bilinear sample beyond the last centre clamps | Let it vanish | The last column of an enlarged picture fades out |
| A map with no inverse is refused | Refuse it as something else | The singular-map test stops naming its refusal |
| Resampling requires premultiplied samples | Accept straight ones | A keyed edge picks up a dark fringe |
| A transform that flattens the picture is refused | Never check the determinant | A scale of nought stops being refused |
| A mirror is not a refusal | Refuse a negative determinant too | Flipping a shot stops being possible |
| The identity is still, and a move is not | Ignore the offset | A move of a hundredth reads as no move |
| A transform survives a rebuild | Drop it in `with_source` | A slip loses the framing |
| A transform acts about the frame's centre | Act about the corner | A halved clip lands in the top-left quarter |
| A transform that moves nothing adds no node | Add one anyway | The identity is resampled |
| A mask is in frame coordinates, after the transform | Mask before moving | The mask travels with the clip |
| The file carries a transform's filter | Write area for every one | Bilinear comes back as area |
| A file's transform goes through the model's constructor | Build a fresh one instead | A file describing a flattening transform stops being refused |
| A motion is measured from the clip's own start | Measure it from the programme | A shot moved down the timeline arrives mid-push-in |
| The stack resolves the motion at all | Hand out the base framing | Every animated clip holds its first frame's size |
| A split re-bases the tail's animation | Carry it across unchanged | The move restarts at every cut |
| A shift keeps the keyframes that land before nought | Drop them as out of range | The tail's opening frames flatten to a hold |
| A join checks two present animations line up | Accept any two | Joining keeps the first's move and discards the second's |
| A join refuses an animation on only one side | Accept those too | Half an animation survives a join that had no business happening |
| An overshooting ease is caught before it becomes a framing | Drop the guard in `moved_by` | A scale that dips below nothing becomes a framing rather than a refusal |
| A scale keyframe at nought or below is refused | Refuse only `i64::MIN` | A keyframe at nought flattens the picture onto a point |
| A motion that animates nothing is refused | Hold it | An empty motion reads as an animated clip |
| An absent scale lane reads one, not nought | Read nought | A clip that animates only its position vanishes |
| The base's filter survives the motion | Force area | Every animated enlargement gets the reduction filter |
| The format's three lanes are written in the order it reads | Swap two | A saved push-in comes back as a move, or is refused outright |
| A motion needs a framing to animate | Accept a clip with none | A project holds an animation with nothing to scale |
| The framing cannot be taken off an animated clip | Allow it | The same state by the other door |
| A motion counted another way is refused when it is set | Compare a lane with itself | The render finds out instead of the editor |
| A file cannot animate a clip with nothing to animate | Skip the check | A file produces a project no sequence of edits could |
| A motion tag this build does not read is refused | Read it as no animation | A future file's animation is silently dropped |
| A roll moves the incoming clip's in point too | Leave it where it was | The cut moves and the same frame is still under it |
| A roll gives to one side what it takes from the other | Give half | Every roll changes how long the programme is |
| A roll that eats a side is refused | Floor the length at one tick | A roll past a clip's end silently deletes it |
| A roll cannot reach before its media begins | Floor the in point at nought | A roll asks for frames that were never shot |
| A roll names the cut by the item after it | Never refuse a boundary of nought | The subtraction goes round the houses |
| A slide leaves the item it slides alone | Lengthen it too | Sliding a shot re-times it |
| An item needs a neighbour on the side it slides away from | Never refuse an item at the start | The subtraction again |
| A trim re-checks the dissolves it moved | Trust the check that ran when it was drawn | A trim leaves a transition the track cannot perform |
| A dissolve needs a handle as well as a length | Ask for no handle | A roll leaves a dissolve starting before its clip's media |
| A refused trim writes nothing | Write first, check after | Half a cut nobody made |
| A trim checks the dissolve against what it is about to write | Check against what is there now | The check passes and then the write breaks it |
| A roll's inverse turns the sign round | Take its absolute value | Undoing a roll rolls further the same way |
| A slide's inverse turns the sign round | Hand back the same number | Undoing a slide slides again |
| A glyph's pieces do not overlap | Widen `E`'s top bar over its stem | The sum stops being the union's area |
| The rasteriser refuses a face whose pieces overlap | Widen `F`'s top bar the same way | A pixel covered twice, drawn as if once |
| A glyph's area is the shoelace sum halved | Do not halve it | Every area is twice what it is |
| A crossbar is computed from the strokes it meets | Take the wrong edge of one | `A`'s bar overshoots its own leg |
| A character this face cannot set is refused | Hand back the first glyph | A slate prints a message it was not given |
| The pen advances between glyphs | Advance by nothing | Every letter is drawn on the one before |
| The advance is wider than the box | Narrow it to the box | Adjacent letters start sharing pixels |
| A run is placed where it was asked | Ignore the vertical origin | Every line of a caption lands on the first |
| The size scales the glyph | Scale only across | Type is one em tall at every size |
| Type has to be some size | Accept nought | A run at no size is drawn rather than refused |
| A run longer than this draws is refused | Accept any length | The bound is a comment |
| A piece's bounding box contains the piece | Take one more pixel off it | The right-hand column of every stroke is dropped |
| A run is as wide as its advance, not its box | Measure by the box | Every measured caption comes up short |
| Type is premultiplied in light, not in code values | Write the coverage into the colour | Every edge of every caption is too dark by what the curve bends |
| The brief caption is tried when the long one will not fit | Give up after the first | A proxy slate says nothing rather than saying the digest |
| A caption's words are in its identity | Hash whether each is empty | Two offline clips share one slate |
| A caption's length is in its identity | Hash the bytes alone | Two captions that concatenate the same collide |
| A caption is centred on its ink, not on its advance | Measure by the advance | Every caption sits left of centre by half a side bearing |
| A caption keeps its margin | Use the whole width | Type runs into both edges of the frame |
| A caption below the floor is not set | Set it at any size | A slate's caption is a grey smear claiming to be information |
| A caption of nothing is not set | Divide by its width anyway | A refusal where a blank frame was asked for |
| A caption is capped so it does not become a billboard | Take whatever fits the width | Two characters six hundred pixels tall, mostly off the bottom |
| An offline slate says which media is missing | Draw the bare pattern | Three milestones of "cannot say which" |
| The slate names the media by its own digest | Name every clip as zeroes | Every offline clip is the same clip |
| Every part of a title is in its name | Hash the words and the size only | Two cards in two places become one asset |
| A title's words are in its name | Hash whether it is empty | Every card in a project is the same card |
| A title with nothing to say is refused | Hold it | A picture that draws nothing calls itself a title |
| A title longer than this describes is refused | Take any length | The bound is a comment |
| Type has to be some size | Refuse only `i64::MIN` | A card at no size, saved and reopened |
| A title is named by its own description | Give every one the same digest | Every card in every project collides |
| A title has nowhere to be | Accept a location hint | Somebody relinks a card to a file |
| Relinking goes through the asset's own refusal | Swallow it in the library | The guard by the other door |
| A title is drawn rather than fetched | Ask the library for it | Every card renders as an offline slate |
| A title's words are in its node's identity | Hash none of them | A programme with two cards shows the first twice |
| A title's size is a fraction of the height | Take it off the width | A card set for 16:9 shrinks when delivered 4:3 |
| A title is placed where it was told | Read one fraction for both axes | Every card lands on a diagonal |
| A line is centred on the point it was given | Take the cap line as the centre | Every line sits half an em low |
| The file says which kind of media an asset is | Write every asset as a recording | Every title comes back as a missing file |
| A title's words survive the file | Write them as `A`s | The card reopens saying something else |
| A title's place survives the file | Swap the two fractions | The card reopens somewhere else |
| A title is named by what it says, in the file too | Skip the check | An edited file repoints every clip of a card |
| A file cannot say a title is somewhere | Skip the check | A project no sequence of edits could produce |
| A media source tag this build does not read is refused | Read it as a recording | A future file's titles become missing files |
| A title's words have to be text | Replace what is not | A file's bytes become a card that says something else |
| A lowercase body sits on the x-line | Start `c` a half-unit higher | The x-height is whatever each letter felt like |
| A descender reaches the descender line | Stop `p` one short | A word with a `p` in it sits on a different line |
| An ascender reaches above the x-line | Cut `l` down to the x-line | `l` and `i` become the same letter |
| A capital sits on the baseline | Lift `L` off it | Capitals and lowercase stop sharing a line |
| Nothing hangs below the descender | Claim one half-unit less | The metric stops describing the face |
| A line leaves room for the descender above it | Set the spacing at the em | Every `g` goes through the `A` below it |
| The face sets lowercase at all | Name `a` as `A` | A face for slates, not for names |
| Lowercase pieces do not overlap either | Widen `m`'s shoulder over its stem | The sum stops being the union's area |
| The specimen shows the whole repertoire | Add a glyph nothing sets | A letter nobody ever looks at |
| The specimen is the face that is committed | Move `o` half a unit | The picture and the face disagree |
| An `A` keeps its crossbar | Compute it and drop it | An `A` is two legs and no bar |
| A `1` keeps its flag | Compute it and drop it | A `1` is a bare stem |
| The crossbar is computed from the legs it meets | Pass one leg twice | The bar collapses onto itself |
| A glyph counts the piece it computes | Count only its strokes | The count and the drawing disagree |
| The table is the whole face | Drop the space from it | A space stops being a character |
| A glyph's strokes are its own | Give every glyph the first one's | Every letter is a space |
| A block stacks its lines at the face's line spacing | Stack them at the em | A descender lands on the cap below and the card is refused |
| A block's lines are stacked at all | Advance by nothing | Every line of a card on top of the first |
| A block straddles the point it was placed at | Hang it below | A two-line card drops half a block when a line is added |
| A block is as wide as its widest line | Take the last line's width | Alignment measures against whichever line came last |
| Left puts a line at the block's left edge | Put it at the right | Left and right are the same alignment |
| Right puts a line at the block's right edge | Put it at the left | The same, from the other side |
| One line is set where one line used to be | Make the block an em taller | Every card in every older project moves |
| A card where every line is blank is refused | Hold it | A picture that draws nothing calls itself a title |
| A card of more lines than this describes is refused | Take any number | The bound is a comment |
| The alignment is in the card's name | Hash them all the same | Three alignments, one asset |
| Each line's length is in the card's name | Hash the bytes alone | Two cards whose lines concatenate the same collide |
| Every line of a card survives the file | Write only the first | A card reopens saying less |
| A card's alignment survives the file | Write centred for all | Every card reopens centred |
| An alignment tag this build does not read is refused | Read it as centred | A future file's cards are silently re-set |
| The alignment a card was given is the one it is set in | Cross the wire in the planner | The model and the picture disagree |
| Every line of a card reaches the node | Hand over the first | The project holds a card the render does not draw |
| A fade in rises from nothing | Start it one tick along | A fade from black starts a shade above it |
| A fade out falls to nothing on the last frame | Fall to nothing on the frame after | One frame of picture at the end of every fade to black |
| Where the two fades meet the smaller wins | Take the larger | A clip faded both ways is at full in the middle of both |
| Outside the clip a fade is nothing | Read it as full | A dissolve's handles show material the fade has not reached |
| A clip nobody has faded is up at every instant | Do not short-circuit a fade of nought | Every clip in every project fades |
| Fades that together outlast the clip are refused | Hold them | A clip that is never fully up |
| A trim re-checks the fades on the clip | Skip it | A trim silently re-times somebody's fade |
| The stack carries where the fade is going | Repeat this frame's value | Every block of a fade is flat, and the fade is a staircase |
| A gap cannot be faded | Report no fades and change nothing | An inverse that claims to have done something |
| A clip's fade reaches the picture | Multiply by one | The model holds a fade the render ignores |
| The fade and the opacity go into one node | Use two | Two roundings and two cache entries for one picture |
| A clip's fade reaches the sound | Never scale the samples | A picture that fades over sound that does not |
| A faded buffer ramps across the block | Hold the first value | The fade is a staircase at the block rate |
| A fade rounds away from zero in both directions | Round both up | A direct-current offset, which is a thump at every cut |
| A faded layer's colour is scaled in light | Scale the code value | A dissolve between two identical pictures sags in the middle |
| A masked layer's colour is scaled in light | Scale the code value | Every soft edge is darker than the picture either side of it |
| A masked sample stays under its own coverage | Clamp only to one | The frame stops being premultiplied and `over` refuses it |
| A clip's fades survive the file | Write them the other way round | A saved fade in comes back a fade out |
| A fade tag this build does not read is refused | Read it as no fades | A future file's fades are silently dropped |
| A speed maps the offset onto the media | Take the offset as the media position | A clip at half speed shows every frame once |
| The mapping floors rather than rounds | Round to nearest | A pull-down lands on the next frame from halfway through each one |
| The size decides how far and the sign decides which way | Add for a reverse as well | A reversed clip runs forwards out of its in point |
| A reversed clip mirrors its forward twin | Let the speed's sign into the distance | `100, 99, 99, 98` against a forward `100, 100, 101, 101` |
| A speed of nothing is refused | Accept it | A clip that shows one frame forever and consumes no media |
| A reverse that would read before its media is refused | Accept it | A negative source tick, discovered at the frame instead of at the edit |
| A cut through a retimed clip goes through its own mapping | Add the offset to the in point | The tail of a split drifts by the speed's worth of frames |
| Two clips at different speeds do not join | Ignore the speeds | A join makes one clip out of two that play at different rates |
| The source end is past what the clip consumes | Return the in point plus the length | A clip at double speed claims half the media it eats |
| Sound cannot yet be retimed | Accept a speed on an audio track | A pitch nobody resampled, silently |
| A gap has no media to play at any speed | Answer real time for one | A speed set on nothing, reported as applied |
| The playhead asks for the retimed frame | Add the offset to the in point | The layer stack hands the renderer the un-retimed frame |
| A dissolve's two layers ask for the retimed frame as well | Add the offset in the transition arm | A retimed shot jumps for exactly the length of the dissolve |
| A speed survives the file exactly | Write real time instead | A saved slow-motion clip comes back at full speed |
| Real time costs one byte to say so | Always write the whole rational | Sixteen bytes a clip for the fact that nothing was retimed |
| A speed tag this build does not read is refused | Read it as real time | A future file's retiming is silently dropped |
| A file's speed goes through the model's constructor | Take the file's word for it | A file holding a frozen clip loads into a model that refuses to make one |
| An ink channel above full light is refused | Check only the low end | A colour brighter than white, refused three layers on with nothing naming the card |
| An ink channel below no light is refused | Check only the high end | A colour that subtracts, which is not an ink |
| The bounds include their own ends | Make both comparisons strict | Black type and white type are the two colours refused |
| Every channel is checked, not just the first | Loop over the red alone | A green above full light passes |
| A card is white until somebody colours it | Default to black | Every card written before an ink existed goes black |
| The ink is part of what a card is | Leave it out of the digest | The same words in two colours are one asset, and whichever drew first wins |
| All three channels are in the digest | Absorb the red three times | Two inks differing only in blue are one asset |
| Colouring a card keeps everything else about it | Re-align it on the way past | A right-aligned block re-flows when somebody picks a colour |
| The ink is a fraction of light rather than a byte | Pack the byte directly | Half of full light comes out nought instead of 188 |
| The table is this frame's and not a default one | Build it from full-range sRGB | A limited-range card's mid-tones are spelled in the wrong range |
| The three channels are packed in order | Reverse them | Red type comes out blue |
| A caption asks the table for white too | Hard-code 255 | A limited-range slate refuses its own caption by name |
| The ink is in the cache key | Leave it out | A red card is served from a white card's cache entry |
| The planner carries the ink across | Send white regardless | The model holds a colour the picture never shows |
| An ink survives the file exactly | Write nought for every channel | A saved card comes back black |
| White costs one byte to say so | Always write three rationals | Forty-eight bytes an asset to say nothing was chosen |
| An ink tag this build does not read is refused | Read it as white | A future file's colour is silently dropped |
| A file's ink is refused rather than corrected | Fall back to white | A file holding an impossible colour loads as a card nobody wrote |
| A span puts its low end first | Always return the in point first | A reversed clip's range reads backwards to whoever checks it |
| A span ends at the last frame the clip shows | End one past it | Every clip claims one frame more media than it reads |
| The library asks what the clip reads, not how long it is | Use the in point and the timeline length | A clip at double speed reads twice its length unchecked |
| The far end of the media is checked at all | Drop the comparison | A clip reads past its asset, discovered at the frame |
| The last frame of the media is inside the bound | Stop one frame short | A clip that ends exactly at the last frame is refused |
| Retiming reaches the library check | Skip the edit | Setting a speed walks around the bound |
| A clip arriving already retimed reaches it too | Skip the insert | A paste, a file or an undo walks around it instead |
| A slip is checked on the clip it would produce | Check the clip as it was | Moving the in point past the end is accepted |
| A lengthening is checked on the clip it would produce | Check the clip as it was | A trim past what a retimed clip can read is accepted |
| A freeze shows one frame at every offset | Take the speed to be one | A still plays |
| A freeze consumes the frame it shows | End at the in point | A clip that shows a frame reads none of it |
| A freeze consumes one frame and not two | End one further along | Every still claims a frame it never shows |
| Freezing keeps everything else about the clip | Drop the grade | A still loses the look somebody put on it |
| Two stills join only when they hold the same frame | Join any two | Two stills of two different shots fuse into one |
| A still cut in two joins back together | Ask the moving question of a freeze | Join stops being the inverse of split |
| A still and a moving clip are two shots | Ignore the playback | A join keeps one playback and drops the other silently |
| A freeze is applied rather than ignored | Hand back the clip unchanged | The edit reports success and nothing is held |
| A speed is applied through the same door | Hand back the clip unchanged | Retiming stops working, freezing does not |
| The inverse gives back the playback it replaced | Always say real time | Undoing a freeze on a fast clip leaves it at real time |
| Sound is refused a freeze as well as a speed | Refuse only speeds | A held block of samples, which is a tone at the block rate |
| A freeze survives the file | Write it as real time | A saved still comes back playing |
| A frozen tag is read back as a freeze | Read it as no playback at all | The tag is written and dropped on the way back |
| A freeze writes no number after its tag | Write a rational too | The frame it holds, said twice, in two places that can disagree |
| An absent curve reads as fully opaque | Read it as nothing | Every clip nobody animated disappears |
| The curve is read from the clip's own start | Read it three ticks along | An animation that plays early, by a margin nobody can see |
| An overshooting ease is clamped at the read | Hand back what the curve said | A layer past full coverage, refused by the compositor |
| A curve counted another way is refused | Accept it | An animation read at the wrong frames for a whole clip |
| A cut re-bases the animation onto the tail | Carry it unchanged | The animation restarts at every cut |
| Two animations that do not line up do not join | Ignore the curves | Two shots fuse and one animation is dropped silently |
| Two halves of one animation do join | Refuse any animated pair | Join stops being the inverse of split |
| The animation is applied rather than ignored | Hand back the clip unchanged | The edit reports success and nothing animates |
| The inverse gives back the curve it replaced | Always say none | Switching an animation off throws away the shape somebody drew |
| Sound has no opacity to animate | Accept a curve on a sound clip | A coverage set on a quantity that is a logarithm of amplitude |
| A gap has nothing to reveal | Answer none for one | An animation set on nothing, reported as applied |
| The clip's own animation reaches the layer | Send the track's alone | The model holds an animation the picture never shows |
| A title's animation reaches the layer like any other clip's | Send one instead | A card that will not fade, though every shot does |
| A clip's animation survives the file | Write no curve | A saved animation comes back gone |
| The centroid weighs by area rather than by corner | Average the corners | A trapezoid balances where nothing is |
| The centroid divides by six times the area | Divide by four | Every shape scales about a point outside itself |
| A shape scales about its own middle | Scale about the frame's corner | A vignette slides toward the corner while it grows |
| A scale of one and a move of nothing change nothing | Add one to the scale | A shape nobody animated is twice the size |
| A scale at or below nothing is refused | Accept it | A shape collapsed to a point, or turned inside out |
| The move is added after the scale | Drop it | A sweep that grows both ways instead of one |
| The animation reaches the shape | Hand back the shape unmoved | The model holds an iris the picture never opens |
| The animation is read from the clip's own start | Read it three ticks along | A shape that opens early, by a margin nobody can see |
| An animation with no shape is refused | Accept it | An animation of nothing, which no edit could produce |
| A cut re-bases the shape's animation | Carry it unchanged | The iris restarts at every cut |
| Two shape animations that do not line up do not join | Ignore them | Two shots fuse and one animation is dropped |
| Two halves of one shape animation do join | Refuse any animated pair | Join stops being the inverse of split |
| Taking the shape off an animated clip is refused | Allow it | An animation left with nothing to scale |
| The shape's animation is applied rather than ignored | Hand back the clip | The edit reports success and nothing animates |
| The layer carries the resolved shape | Carry the unresolved one | The renderer would need a clock, and a clock makes a cache key lie |
| The shape's animation survives the file | Write none | A saved iris comes back still |
| A file animating a shape that is not there is refused | Read it as none | A file's animation is silently dropped rather than named |
| A grade nobody animated is all the way on | Read an absent curve as nought | Every look in every project written before a strength existed turns off |
| The arrival is read from the clip's own start | Read it three ticks along | A look that arrives early, by a margin nobody can see |
| An overshooting ease is clamped at the read | Hand back what the curve said | A strength past the table's own output, which is arithmetic rather than a grade |
| A strength needs a grade to be the strength of | Accept one on an ungraded clip | Two tests fail, in two crates: the edit stops refusing and so does the file, which is one guard reached by two doors |
| A strength counted another way is refused | Compare a curve's timebase with itself | An arrival read at the wrong frames for a whole clip |
| Taking the grade off an animated clip is refused | Let it through | An arrival left with nothing to arrive at |
| A cut re-bases the arrival onto the tail | Shift it by nothing | Two tests fail: the look restarts arriving at every cut, and join stops being the inverse of split |
| Two arrivals that do not line up do not join | Answer yes to any pair | Two shots fuse and one arrival is dropped silently |
| The layer carries the resolved strength | Report a full grade whatever the curve says | Two tests fail, in two crates: the model holds an arrival the picture never shows |
| The strength is in the node's identity | Leave it out of the digest | A look coming on over a shot is served from one cache entry for its whole arrival |
| The planner carries the strength across | Send a full grade regardless | The wiring between the model and the node, which is the seam both sides' tests leave uncovered |
| A strength outside none to all is refused | Check only the low end | A picture on the far side of a table that was never sampled there |
| The bounds include their own ends | Make both comparisons strict | Fifteen tests fail: none of it and all of it are the two strengths every project actually holds |
| The mix reaches the pixels at all | Apply the look flat and drop the result | Four tests fail; the model holds an arrival and the renderer applies the whole look |
| The mix happens in code values, not in light | Decode both sides, mix, and encode again | Half a look on a mid-grey lands on **92** rather than 64 — the exact number the test derives by hand from the sRGB curve, from the other space |
| The file carries a clip's grade strength | Write no curve | Three tests fail, and — as ever — **not** the round trips, which compare a round trip against another round trip |
| The strength is written after the grade — **control passed, claim was decoration** | Swap both the write and the read | *Nothing failed.* The refusal lives in `Clip::with_grade_strength` at the end of the builder chain and cannot see what order the bytes arrived in. The comment claiming the reader "holds both" is corrected to say the order is a convention |
| The half-angle formula is the half-angle formula | Add the square instead of subtracting it | Seven tests fail across three crates, the hand-derived quarter turn among them |
| The sine's numerator is twice the parameter | Multiply by one | A quarter turn stops being a quarter turn |
| A pair off the unit circle is refused | Compute the sum and drop the comparison | A scale is accepted wearing a rotation's name |
| The rotation turns rather than reflecting | Add where the matrix subtracts | Five tests fail, the corner arithmetic and the left-multiplication among them |
| A shape turns about its own centroid | Turn about the frame's corner | Nine tests fail; a vignette swings across the picture instead of rotating in place |
| The shape's turn is applied at all | Hand the corners to `Turn::NONE` | The model holds an iris that turns and a picture that does not |
| The turn acts on the left of the framing | Take the rows instead of the columns, which is `M·R` | A mirrored clip turns the other way |
| The framing's turn is applied at all | Leave the linear part alone | Three tests fail, the planner's seam among them |
| The turn lane counts towards being an animation | Leave it out of the emptiness check | Five tests fail: a motion that only turns is refused as empty |
| An absent turn lane reads no turn | Read its neutral as one | Seven tests fail; every clip that animates only its scale starts turning through a right angle |
| The turn lane is shifted by a cut | Carry it across unchanged | The turn restarts at every cut and join stops being the inverse of split |
| The file carries a motion's turn lane | Write the absent lane | Two tests fail, and — as ever — **not** the round trips |
| The writer's lane order is the reader's | Read the turn where the down lane is written | Four tests fail, the canonical-encoding test among them |
| A preimage is a parallelogram, not its bounding box | Replace it with the exact box around it | **One** test fails: the tilted turn. Every axis-aligned test passes, and so do *both* quarter-turn tests — which is the measurement that a right angle cannot test rotation |
| The default pivot is the centre | Make it the corner | Three tests fail across three crates; every framing ever rendered slides to the lower right |
| An animated clip keeps the pivot its framing was given | Rebuild through the constructor, which starts from nothing | The pivot returns to the centre half way through a push-in — the third field to find this trap, after the grade and the motion |
| The pivot is not part of being still | Require it to be the centre before skipping the node | A clip with a pivot and no move is resampled to compute the picture it already had |
| The pivot is where the map acts about | Scale it by the other axis | Two tests fail; the fixed point is not fixed on any picture that is not square |
| The pivot reaches the map at all | Use the centre once inside the renderer | Three tests fail, the folding measurement among them |
| The pivot is in the node's identity — **control passed, gap was real** | Absorb the across component twice | *Nothing failed.* The anchor was in the digest and no test asked it to be, so two pivots differing only *down* the frame collided. A test now names both axes, and the same mutation fails it |
| The pivot's two axes are written in order | Swap them | Three tests fail, the canonical-encoding test among them |
| The file carries a transform's pivot | Write nought for both | Three tests fail, and — as ever — the named-field test is the one that had to exist |
| A pivot cannot be folded into the move in fractions | Fold it, in the only space a model could | A diagonal map agrees exactly and a rotation on an 8×4 picture does not, which is the whole argument for the field |
| The footprint reader knows the mangling the compiler emits | Refuse to parse v0 | It names the three symbols it misreads and exits non-zero, rather than filing the whole image under one row |
| A length prefix is not the name's first character | Drop v0's separator from the pattern | The one crate whose name begins with an underscore reads one character short |
| A symbol with no crate says so | File a C name under `core` | The self-check names `memcmp` |
| A column publishes every cut or none | Split each track as it is reached | A refused razor leaves a cut behind on the track it could cut |
| A column publishes every heal or none — **control passed, gap was real** | Join each track as it is reached | *Nothing failed.* The cut had an atomicity test and the heal had none. One now exists, and the same mutation fails it |
| A cut's inverse is a heal over the same set | Hand back an empty set | Undoing a razor leaves the whole column cut |
| A merge's inverse is the cut that made it | Hand back an empty set | Undoing a merge leaves the tracks fused |
| A blade on a cut is not a cut | Name a track whose boundary is already there | The blade names tracks with nothing to cut, and the razor refuses at the moment somebody drags it |
| A track under a dissolve is left out of the blade | Stop asking about transitions | The set names a track `Track::split` refuses |
| A merge heals a cut rather than any boundary | Name any boundary at the instant | Two different shots that abut are fused and one of them is lost |
| A set names a track it was given and no other | Ignore an index past the bound | A set silently drops a track, and a cut and its inverse describe different edits |
| A set that names nothing is refused | Accept it | An edit that changes nothing takes a place in the history and claims, on undo, to have put something back |
| A set iterates in index order | Iterate it backwards | Two tests fail; an edit's effect starts depending on how its set was built |
| A lift leaves a hole and moves nothing | Remove the item instead | Six tests fail; the lift is an extract, and everything after it moves |
| The hole is as long as the shot | Make it one tick shorter | Four tests fail; the programme changes length |
| A gap cannot be lifted | Accept one | An edit that changes nothing takes a place in the history |
| An item a transition touches cannot be lifted | Drop the check | A dissolve is left mixing a gap, refused at the frame instead of at the edit |
| The transition check is the narrow one — **control passed, fixture was blind** | Ask "at or after" instead | *Nothing failed.* The fixture lifted an item *after* the dissolve, where both checks agree. A case that lifts at the head of a programme with a dissolve at its end now exists, and the same mutation fails it |
| A shot goes back only into the gap it left | Drop the check | A shot is dropped into a slot something else happened to |
| The gap has to be the right length as well as a gap | Check only that it is a gap | A shot is dropped into a gap of the wrong length |
| The inverse of a lift carries the shot | Hand back the shot without its look | Undo puts back a clip nobody graded |
| Two notes at one instant are refused | Accept the second | Three tests fail; neither note can be named, moved or removed |
| Notes are kept in time order | Insert at the end regardless | Two tests fail; the list stops being the timeline |
| A note before the programme is refused | Accept it | Two tests fail, in the model and in the file: a note on nothing |
| A marker's text bound is a bound | Raise it a thousandfold | A hostile file talks its way past it |
| The bound counts characters, not bytes | Count bytes | A note of legal length in a language that spells wider is refused |
| The inverse of a removal carries the text | Hand back an empty note | Undo puts back a marker that has forgotten what it said |
| A refused move puts the note back | Leave it removed | A move that did not happen deletes the note it was moving |
| A note does not move when an item ripples | Slide every note by the trim | The note moves away from the thing it is about |
| The file carries a sequence's markers | Write an empty list | Four tests fail, and — as ever — **not** the round trips |
| A note's text has to be text | Read it lossily | A file's bytes become a note that says something else |
| A sequence cannot contain itself (R-5.5) | Let the walk revisit a sequence already on its path | Two tests fail; a project holds a nest that renders forever |
| The nesting walk is depth-limited | Drop the bound | A chain deeper than the named maximum is built |
| A nest says how long its sequence is *now* | Leave the asset saying what it said when it was made | Three tests fail; the library states a length no sequence is |
| Every clip's source span is rechecked after an edit | Ignore what the check says | A clip is left reading past the end of what it reads |
| A refusal undoes the refresh as well as the edit (R-1.4) | Undo only the edit | The library keeps the length a refused edit produced |
| A check that runs after the edit undoes it (R-1.4) | Return the refusal without undoing | Two tests fail; a refused edit leaves the nested sequence shortened |
| A nest is named by which sequence it is | Hash the domain tag alone | Four tests fail across two crates; two nests of two sequences collide |
| Only a recording can carry a location hint | Ask whether it is a title instead of whether it is a recording | A nest is relinked to a file |
| A nest's sequence index resolves against the headers read first | Resolve it one place early | Two tests fail; a nest comes back naming the wrong sequence |
| The reader recomputes a nest's digest rather than believing it | Believe the file's | Two tests fail; a file gives a nest a digest that is not its sequence's |
| A file cannot give a nest a location | Accept the hint | A loaded project holds a nest that claims to be a file |
| Sequence bodies are written innermost first | Write them in header order | A valid file is refused halfway through loading |
| Once each, and every one — **control passed, the test was missing** | Accept a second body for one sequence | *Nothing failed.* The guard had no test behind it; `a_file_giving_one_sequence_two_bodies_is_refused` now splices one body's header index over the other's, and the same mutation fails it |
| A nest is composited onto **nothing**, not onto black | Start it from a blank frame | The empty half of a nest blanks out the track beneath it |
| The two nothings are two nodes and two cache keys | Evaluate an empty frame as a blank one | A nest stops being transparent where it is empty |
| A nested clip reads the nest at its own offset | Read it at the programme's instant | Two tests fail; the nest shows the wrong frame of itself |
| A straight run integrates to a trapezium | Take the fraction as nought, so the segment is a rectangle of its first value | Seven tests fail; every ramp reads its media at the speed it started at |
| The trapezium takes the *mean* of its two ends | Take their sum | Six tests fail; a ramp consumes twice the media it should |
| A hold's area is the value it leaves, not the one it arrives at | Integrate towards the next keyframe | A held keyframe ramps |
| The curve is held before its first keyframe | Charge nothing before it | Four tests fail; a ramp written in the middle of a clip reads nothing until it starts |
| The curve is held past its last keyframe | Charge nothing after it | Seven tests fail; a clip longer than its ramp stops consuming media |
| The area is measured from tick nought | Measure it from the first keyframe | Four tests fail; a cut through a ramp puts the tail where the head never was |
| An ease has no exact area | Integrate it as a hold | The cube root the inversion rounds becomes a frame of drift |
| A ramp may not stop | Accept a keyframe at nought | A ramp holds a frame without saying so |
| A ramp may not turn around | Accept a sign change | A ramp reads part of its media twice, where the span check does not look |
| An ease is refused over the *whole* curve — **control passed, the test was missing** | Accept one | *Nothing failed.* The clip's own guard only ever caught the harmless case: an ease the clip walks through is refused by `area_to` anyway, and one past its last tick was refused by nothing. `a_ramp_whose_ease_lies_past_the_clip_is_still_refused` now pins the case a later trim would have turned into a refusal at a distance |
| A ramp is checked against the start of its media | Drop the check | A backwards ramp reads below where its media begins |
| A ramp is counted in its clip's own timebase — **control passed, the guard went** | Drop `with_ramp`'s timebase check | *Nothing failed*, and nothing could: `source_span` asks `area_to` at a tick in the clip's timebase four lines later, which refuses with the same status. A guard whose absence changes no answer is a guard no test can hold, so it was deleted and the control now mutates `area_to`, where the invariant actually lives |
| A ramp's source position is the **area**, not a multiple | Multiply by the speed at the offset instead | Four tests fail; a ramp reads at its first keyframe's speed throughout |
| The size of the travel is taken before the floor | Floor the signed travel | Two tests fail; a reversed clip stops showing the frames its forward twin shows |
| A cut re-bases the ramp | Carry it unchanged | The tail restarts the ramp at the cut |
| A join asks whether the ramps line up, not whether they match | Compare them directly | Two tests fail; every cut through a ramp stops joining back |
| Sound cannot be ramped | Refuse only a freeze | Two tests fail; sound plays at the wrong pitch rather than saying it cannot |
| The file carries a ramp's curve | Write no keyframes | Four tests fail; a saved ramp comes back as something else |
| A ramp comes back through the model's own door | Keep the plain clip when the model refuses | Three tests fail; a file holds a ramp no sequence of edits could produce |
| A ramp with no keyframes is refused | Read it as no ramp at all | A file says there is a ramp and gives no curve |
| The planner asks the clip where it has got to | Add the offset to the in point | A ramped clip fetches the frame at the programme's own offset |
| Two notes on one shot at one offset are refused | Accept the second | Two tests fail; neither note can be named, moved or removed |
| A note before the shot starts is refused | Accept it | A shot carries a note before its own first frame |
| A note is counted in its shot's own timebase | Drop the check | A note in another rate is read as if it were this one |
| A shot's notes are kept in time order | Append regardless | The list stops being the timeline |
| A clip's marker bound is a bound, and a small one | Raise it a thousandfold | Two tests fail; the per-clip budget the bound exists for is gone |
| A note **at** the cut goes to the tail | Give it to the head | Two tests fail; the note is on the half that no longer holds its frame |
| A cut hands the notes past it to the tail | Drop them | Two tests fail; a cut deletes every note past it |
| The tail's notes are re-based onto its own start | Carry them unchanged | Two tests fail; the notes sit where they sat in the original |
| A join merges the notes rather than keeping the head's | Keep the head's | Three tests fail; a join deletes every note on the tail |
| A join moves the arriving notes on by the head's length | Leave them where they are | Three tests fail; the tail's notes land on top of the head's |
| A trim carries the notes it hides | Drop the ones past the new end | A trim becomes a delete, and pulling the shot back out does not bring them back |
| A note comes off saying what it said | Hand back an empty one | Two tests fail; undo puts back a note that has forgotten what it said |
| A refused move puts the note back | Publish the removal before the addition | A move that did not happen deletes the note it was moving |
| A gap has nothing to leave a note on | Refuse with another status | An edit that changes nothing takes a place in the history |
| The file carries a shot's notes | Write an empty list | Four tests fail, and — as ever — **not** the round trip of a project without any |
| A shot's notes come back through the model's own door | Skip the ones the model refuses | Two tests fail; a file holds a clip no sequence of edits could produce |
| A clip's note list is read against the *clip's* bound | Read it against the sequence's | A file gives one shot thousands of notes |
| The name subset is Phipia's, not a superset | Accept every printable byte | A name with a space reaches the kernel and is refused there instead |
| A name is uppercased | Keep it as typed | Two tests fail; `clip.bmp` and `CLIP.BMP` become two pieces of material |
| Eight and three are bounded **separately** — **control passed, the test was missing** | Bound their sum instead | *Nothing failed.* Both fixtures were twelve characters, which the sum refuses too. A base of nine and an extension of two — eleven between them, which a sum accepts and Phipia does not — now pins it |
| A name has one dot, and not at either end | Refuse only the second | A Unix hidden file is accepted as a name |
| A path is relative to one mount | Drop the check | A leading separator is read as an empty first component |
| A path cannot climb above its mount | Let the pop fail silently | A path of dot-dots reaches outside the volume |
| The path bounds are Phipia's | Raise the component bound a hundredfold | A path longer than the mount accepts is built and refused at the kernel |
| A bitmap's rows are padded to four bytes | Drop the padding | A picture whose width is not a multiple of four comes back sheared |
| A bitmap's rows run bottom-up | Read them top-down regardless | Two tests fail; every imported photograph is upside down |
| A bitmap's channels run blue, green, red | Read them in order | Three tests fail; every photograph has its red and blue swapped |
| *Unsupported* is a different answer from *malformed* | Accept every bit depth | A palette bitmap is called broken rather than unread |
| A bitmap is bounded as Phipia bounds one | Raise both bounds eightfold | A picture past what Phipia reads is imported here and not there |
| A height with no positive counterpart is refused | Drop the `i32::MIN` case | A top-down height of `i32::MIN` negates into itself |
| The seal covers the index as well as the material — **control passed, the test was missing** | Seal only the header | *Nothing failed.* Every hostile-file test resealed, and the byte sweep's refusals all came from other checks. `a_file_whose_name_is_edited_is_refused` now changes a name to another legal name, which nothing but the seal can notice |
| A vault's spans run end to end — **control passed, the test was missing** | Accept a gap | *Nothing failed.* No test built a hostile *file*; the sweep's mutations were caught by the digest recomputation instead. Five resealed-file tests now exist, and this mutation fails the first |
| A vault recomputes what its material is — **control passed, the test was missing** | Believe the file's digest | *Nothing failed*, for the same reason. A resealed file with one payload byte changed now fails it |
| The same material twice is refused | Accept it | A vault holds two entries whose digests are equal |
| A vault fits in one of Phipia's files | Double the payload bound | A vault is built that the filesystem cannot store |
| A vault holds no more material than its bound — **control passed, the test was missing** | Drop the count check | *Nothing failed*, for the same reason. A resealed file claiming 257 items now fails it |
| The unused tail of a name field is zero — **control passed, the test was missing** | Accept anything there | *Nothing failed*, for the same reason. Two files decoding alike and differing byte for byte is the end of a canonical format, and a resealed file with a tail byte now pins it |
| *Absent* and *unreadable* are different answers | Report a damaged file as a missing one | Somebody is sent looking for a drive that is mounted |
| A shelf answers only for material it holds | Always say yes | The planner is told a vault holds material it does not |
| A shelf hands back the frame the tick names | Clamp to the last one | A photograph shows its one frame at every tick of a clip |
| A ranged read is short at the end, not refused | Refuse past the end | Every reader carries an arm for a condition that is not an error |
| A commit names its destination | Always commit into the project | Fourteen tests fail; saving a vault overwrites the project |
| A commit into the scratch slot is refused — **control passed, the guard was doubled** | Accept it | *Nothing failed.* There were two guards and the early one caught nothing the match arm did not. One refusal, one place — the same finding `Clip::with_ramp`'s timebase check produced, and the same answer |
| A vault is committed into the vault slot | Commit into the project's | Nine tests fail; storing material destroys the project |
| A vault's write is read back before it is committed — **control passed, the tests were missing** | Skip the comparison | *Nothing failed.* The project's save had R-9.4's four-fault battery and the vault's had none at all. `a_vault_survives_a_save_that_fails_at_every_step` now drives each fault in turn and checks the last good vault still opens **and verifies** after every one |
| A catalogue checks its header against the slot | Drop the length agreement | A truncated vault opens and reads past its own end |
| A catalogue bounds the count before it uses it | Drop the bound | A file claiming thousands of items is opened |
| An entry is read at the offset its position names | Always read the first | Six tests fail; every entry after the first comes from the wrong place |
| An entry past the count is refused | Accept it | Two tests fail; a read past the index returns whatever material sits there |
| Material is found past the index | Put the payload at the header | Eight tests fail; material is read out of the index |
| Material stops at the end of its own span | Fill the whole buffer | Two tests fail; a photograph runs into the next photograph |
| A verification recomputes the seal | Drop the comparison | `open` deliberately does not look, so nothing does |
| A verification recomputes every item's digest | Drop the comparison | Material that is not what its entry says passes |
| A verification checks the spans run end to end | Drop the comparison | A file with a gap in it verifies |
| Every byte past the header goes into the seal | Drop one byte a window | Three tests fail; a chunked walk is exactly where an off-by-one lives |
| A catalogue reads through ranged reads only | Count a ranged read as a whole one | The catalogue loads the file it exists not to load |
| A frame is found by arithmetic, not by search | Step one byte a frame | Five tests fail; every frame after the first comes from the wrong place |
| The frames begin after the header | Start the payload at nought | Five tests fail; every frame is read ninety-six bytes early |
| A frame position past the reel is refused | Accept it | A read past the last frame returns whatever follows it |
| A spool checks the extent is the length its header says | Drop the check | A truncated reel opens and reads past its own end |
| A spool and a decode read **one** header | Drop the frame bound from the shared reader | Three tests fail; two doors into one format, and one of them weaker |
| A plane's row is found past every plane before it | Start every plane at nought | The chroma of a planar frame is read out of its luma |
| A row is found by its own stride | Ignore the row | Two tests fail; every row of every frame is the first row |
| A plane a format does not have is refused | Accept it | A read of the second plane of an RGB frame runs off the end |
| A row past a plane's height is refused | Accept it | A row past the picture returns the next frame's first row |
| A destination shorter than a row is refused | Fill what fits | A caller draws half a row with no indication (R-1.4) |
| A verification covers the description as well as the samples | Seal only the samples | Two tests fail; a flipped transfer function turns every frame into a different picture |
| A verification walks every sample | Drop one byte a window | Two tests fail; a damaged sample verifies |
| A material is one entry's bytes, not the slot's | Read from the start of the slot | A reel in a vault is read from the start of the file |
| A material's length is its entry's | Answer with the largest number there is | A reel in a vault looks longer than it is and its trailing check passes |
| The stacks hand back what they have, described as it is | Answer whatever was asked for | A conversion sits inside the cache key of the source |
| The stacks read through the catalogue only | Count a ranged read as a whole one | Two tests fail; the render loads the file the whole chain exists not to load |
| A row is **one** row of the frame it belongs to | Make a row the whole height | Six tests fail; the two evaluators stop being one picture |
| A row past the frame is refused | Drop the check | Two tests fail; a row past the end wraps |
| A subsampled format has no row | Accept one | One chroma row serves two luma rows, so the chroma is guessed |
| A transform is not row-local | Walk through it | Two tests fail; a resampled frame is built from the wrong rows |
| *Not row-local* and *no row form* are different refusals | Answer both with the first | Somebody cannot tell whether to wait for a version or change the question |
| A library that cannot serve rows refuses rather than loading a frame | Default to fetching the frame and slicing it | A row-at-a-time renderer does exactly what it exists to avoid, invisibly |
| A mask is placed against the **whole** frame | Place it against one row | Two tests fail; a shape stretched over one row is a different shape |
| A wipe is placed against the whole frame | Place it against one row | A wipe travels across one row rather than across the picture |
| A row of a mask is **that** row of it | Always rasterise the first | Two tests fail; every row of every mask is its first |
| A row of a wipe is that row of it | Always rasterise the first | Every row of every wipe is its first |
| A whole mask is still every row of it | Drop the last row | Two tests fail; the frame form and the row form come out of two rasterisers |
| A source's *row* is checked against what was asked for | Drop the check | A library hands back a row of something else and it is composited |
| A transform's geometry walk needs no refusal — **control passed, the guard went** | Walk through a transform in the geometry walk | *Nothing failed*, and nothing could: a transform resamples into its source's own description, so walking through gives the right geometry, and `row` refuses the transform itself a moment later with the same status. Third guard this project has found that changes no answer, and the third it has deleted |
| The stacks serve a row from **one** plane row | Read a whole frame and slice it | The largest single read becomes a frame rather than a row |
| The stacks serve the row asked for | Always serve the first | Every row of a shot is its first row |
| The stacks serve the frame the tick names | Always serve the first | Every frame of a shot is its first frame |
| A scan asks whether the plan can be scanned **before** it opens | Drop `row_local` from `Scan::open` | Two tests fail; a framed clip and an offline slate both open a scan and refuse at the first row |
| Asking first refuses a transform | Walk through the transform in `row_local` | Two tests fail; a framing is told it scans and refuses at the row |
| Asking first refuses a generator with no row form | Answer `Ok` for a pattern, a card and a legend | A title is told it scans and refuses at the row |
| Asking first walks *through* a decoration | Answer `Ok` at a fade, a look, a mask or a wipe without walking its input | A fade over a framing is told it scans |
| Asking first walks **both** layers of a composite | Check the top layer only | A framed layer beneath another one is told it scans |
| Asking first refuses a description that has no rows | Look at node kinds and not at the descriptions they carry | A 4:2:0 blank, empty and source are each told they scan |
| Asking first refuses the target a conversion writes | Check only what a conversion reads | A chain ending in 4:2:0 is told it scans because its source is straight |
| A scan's rows are described one row high | Report the whole frame's description as the row's | Three tests fail; a caller assembling rows builds a frame of the wrong height |
| A scan is as tall as the programme | Report one row fewer | Four tests fail; a caller assembles a short picture and the row past the end is accepted |
| The row a caller asks for is the row it renders | Always render row nought | Every row of a scan is the top of the picture |
| A library with no row form refuses rather than loading a frame | Default `Library::row` to fetching the whole frame and slicing it | The default quietly loads eight megabytes to hand back six thousand bytes |
| A reel's trailer is checked | Read the trailer and never compare it | Three tests fail; a corrupt reel decodes |
| The digest covers the header as well as the samples | Hash only the payload | Eight tests fail; a changed transfer tag reads as sound |
| A reel that ends before its trailer is refused | Expect a file the length of its payload | Thirteen tests fail; a recording cut short reads as a reel |
| The streaming reader expects a trailer too | Leave `TRAILER_BYTES` out of `Spool::open`'s arithmetic | Sixteen tests fail; the cheap door admits what the expensive one refuses |
| `verify` stops before the trailer | Walk to the end of the extent | Four tests fail; the digest is hashed into the answer it is compared against |
| A reel wound is the reel encoded | Write a different version byte from the winder | Three tests fail; two writers of one format produce two files |
| Every row is hashed as well as written | Append without updating the digest | Three tests fail; a reel's trailer is the hash of its header alone |
| A row out of order is refused | Take whatever row arrives | A caller's off-by-one becomes a file whose pictures are wrong and whose digest is right |
| A row is described the way the reel is | Drop the description check | A whole frame is written where one row belongs |
| A reel short of rows cannot be closed | Write the trailer whenever asked | A three-frame header is written over one frame of samples |
| A planar reel is not wound | Accept any plane count | Three planes are written as though rows and planes interleaved the same way |
| A reel is wound onto nothing | Drop the empty-sink check | Two tests fail; a reel's header lands after whatever was there |
| A row advances one and rolls into the next frame | Roll over one row late | Six tests fail; the winder asks for a fifth row of a frame four rows high |
| An append extends rather than replaces | Replace the scratch slot on every append | Two tests fail; a reel becomes its own last row |
| A sink counts what it has written | Never advance the count | A second reel is wound onto the tail of the first |
| An export commits only the file it wrote | Drop the trailer-against-digest check | A verification pointed at another sound reel passes for it |
| An export walks what actually landed | Check the trailer and skip the walk | A row damaged after it was hashed is committed |
| An export starts the scratch slot from nothing | Leave the last save staged | A reel is wound onto the tail of whatever was there |
| An export moves the playhead | Render the first instant every time | Two tests fail; every frame of the reel is the first one |
| An export writes as many frames as the span holds | Declare one frame more than the span | Five tests fail; the header outruns the samples and the reel will not close |
| A reel's sound is as long as its pictures | Drop the length check on `with_sound` | A second of sound is accepted against a tenth of a second of picture |
| The sound bound is a bound, not an average | Make the ceiling the floor | Four tests fail; every reel whose span started late is refused |
| The sound floor is a floor | Make the floor the ceiling | Five tests fail; a reel one sample under the average is refused |
| The format's sample arithmetic is the mixer's | Count at half the rate | Four tests fail; the format and `floor_into` disagree at every frame |
| A header that half declares sound is refused | Accept a rate with no channels | A resealed header names a rate and nothing to play it in |
| A silent reel declaring samples is refused | Ignore the sample count when there is no sound | A resealed header claims ten thousand samples it does not have |
| A reserved byte of the sound description is refused | Skip the six reserved bytes | A future version's field reads as nothing |
| Sound is written after every picture | Take a block before the pictures are done | A block of samples lands in the middle of a frame |
| A block of sound out of order is refused | Take whatever block arrives | Frame nine's sound is written under frame four |
| A block is a length one frame could cover | Drop the block-length bound | A block of any length is written and the total comes up short |
| A block is in the rate and channels declared | Drop the rate and channel check | A mono block is written into a stereo reel |
| A reel short of sound cannot be closed | Close whatever has arrived | A four-block reel is closed after two |
| A reel with no sound takes none | Infer a description from the block handed over | A silent reel writes samples nothing in its header describes |
| Samples are interleaved across channels | Write each channel in turn | A block cannot be written until the take ends, which is what streaming avoids |
| The reader walks to a block rather than multiplying | Multiply by one block's length | The reader drifts after the first frame, because blocks are 1601 or 1602 |
| The sound section begins where the pictures end | Look one frame earlier | The last frame's picture is read as sound |
| A reel's length counts its sound | Expect a file with no samples in it | Every reel that carries sound is refused as having trailing bytes |
| An export declares the samples its span holds | Count from the frame count alone | The declared count is one out for every span that does not start on a sample |
| An export mixes each frame at its own instant | Mix the first instant every time | Five tests fail; every block of sound is the first frame's |
| An export reports what the mix did | Keep the last block's count rather than the sum | A clip in frame one is forgotten by frame two |
| A frame **covers** a source range rather than sampling one | Make a frame show only its own tick | Three tests fail; at triple speed two thirds of a transcript falls between frames |
| A held frame still covers one tick | Give a frame whose neighbour has not moved on an empty range | Six tests fail; slow motion and a freeze show no words at all |
| A reversed clip's coverage runs the other way | Cover upwards on a clip that reads downwards | A reversed clip's words land in the wrong places |
| The search knows which way the clip reads | Search every clip as though it ran forwards | A reversed clip is bisected against a descending sequence |
| The bisection finds the **first** offset | Step the low bound two at a time | Eleven tests fail; a caption appears somewhere inside its word |
| An empty or backwards source range shows nothing | Drop the guard and search anyway | Two tests fail; an empty range inside a fast clip's stride reports a frame |
| One voice cannot say two things at once | Ignore the voice when checking overlap | Two captions of one speaker over one tick are both drawn |
| Two voices may say two things at once | Refuse any overlap | A conversation cannot be transcribed |
| Captions that meet do not overlap | Compare the half-open ranges inclusively | Ten tests fail; a contiguous transcript is refused |
| A caption that ends before it begins is refused | Drop the range check | A backwards caption is stored and never shown |
| An asset carries no more captions than the bound | Drop the count check | A transcript that cannot fit is accepted into a file read whole |
| A projection is clipped to the span asked for | Report every caption whole | A one-frame query reports a ten-frame caption |
| A caption is offset by where its clip begins | Add nought instead of the clip's start | Two tests fail; every caption lands as though its clip began at nought |
| A gap moves what follows it | Let a gap contribute nothing to the running position | Two tests fail; a ripple does not move the words |
| A nested caption is lifted through the clip that reads it | Pass the nest's own range straight out | A caption inside a nest is reported at its position inside the nest |
| A query past the bound is refused, not truncated | Return what was found at the bound | A partial transcript is handed back looking whole |
| A transcript survives a save | Read the captions and discard them | Two tests fail; the words are written and never read back |
| A hostile transcript is refused at the model's door | Drop the check in `with_captions` | Two tests fail; a file holds two captions of one voice over one tick |
| A run of samples reads only the blocks it spans | Find the block by comparing strictly | A run reads and discards a block it does not span |
| The mixer names media by what it is | Resolve the digest from the first asset rather than the layer's | Every clip mixes the wrong recording's sound |
| A reel's transcript comes after every picture and sample | Write the words among the pictures | Four tests fail; a section whose length varies sits in front of one whose offsets are arithmetic |
| The streaming reader's length counts the transcript | Leave the caption bytes out of `Spool::open` | Three tests fail; every reel that says something is refused as having trailing bytes |
| The loading reader's length counts the transcript | Leave them out of `decode` | The cheap door and the expensive one admit different files |
| The winder's own length counts the transcript | Leave them out of the size bound | A reel past the format's limit is written |
| A header that disagrees about its transcript is refused | Drop the count-against-length check | A reel claims three captions in nought bytes |
| Only the captions over the range asked for come back | Return every record | Three tests fail; a projection is handed a whole transcript |
| The transcript begins where the *sound* ends | Begin it where the pictures end | A reel with sound reads its samples as words |
| A caption out of turn is refused | Take whatever caption arrives | The wrong words go in the file with a right digest |
| A caption past the declared length is refused | Ignore the byte count while writing | The section overruns its header and a reader walks off the end |
| A reel short of words cannot be closed | Close whatever arrived | A three-caption header is written over one caption |
| A reel that declared no transcript takes none | Accept words a header never described | A silent reel writes bytes nothing accounts for |
| An exported reel's words are rebased to its own nought | Keep their timeline positions | A reel beginning at frame ten carries captions at frame twenty |
| A projection asks for the stretch its clip reads | Ask for the whole recording | A source with a thousand captions builds all of them per clip |
| A held transcript answers only what overlaps | Return every caption of the asset | The range a source is asked for means nothing |
| A record straddling a window boundary is carried | Clear the buffer before each refill | The partial record is dropped and everything after a boundary is garbage |
| A partial record at a window's end is not read | Read a caption the buffer does not hold entire | A length is read as a picture and whatever follows is called text |
| A window is refilled *behind* what it carried | Read over the carry rather than after it | The next window overwrites the remainder it was meant to complete |
| What a window consumed is dropped and the rest kept | Keep the consumed bytes | Two tests fail; every window re-reads what the last one gave back |
| A section ending inside a record is refused | Return what was found instead | A header saying more captions than the bytes hold passes |
| A project file's caption bound is the project file's | Apply it to every container | A reel of sixteen thousand captions is refused by a bound about a different file |
| A pattern row is placed against the whole picture | Place it against a frame one row high | Two tests fail; bars stop being eighths of the width |
| A pattern row is the row asked for | Always draw the first | Two tests fail; every row of a pattern is its first |
| A pattern row past the bottom is refused | Drop the bound | A row past the end is drawn against arithmetic that happens to work |
| A row of type is placed against the whole picture | Lay the run out against a frame one row high | A card is set at a different size in a different place |
| A row of type is the row asked for | Always rasterise the first | Two tests fail; every row of a card is its first |
| A row of type past the bottom is refused | Drop the bound | A row past the end is rasterised against nothing |
| A card and a row of a card are set by one function | Draw a whole card in the row form | The two forms disagree about where a letter falls |
| A legend is laid out against the frame it sits on | Lay it out against the row handed over | A legend on a row is placed against a frame one row high |
| A generator says up front that it can be scanned | Keep refusing them in `row_local` | An offline programme is refused before a scan begins |
| Nothing said is nothing drawn | Build the card anyway | An empty card is composited over every frame of every programme |
| More caption lines than fit on screen are refused | Drop the bound | Five speakers become four lines and one person is not heard |
| Two speakers come out in a fixed order | Sort without the voice | The block reshuffles itself between frames |
| A caption block sits where a caption goes | Place it a quarter down | The words are drawn across the middle of the picture |
| The burned card goes over the programme | Composite the programme over the card | Three tests fail; the picture hides the words |
| Only a horizontal map has a band | `Mapping::horizontal` answers `true` for every map | Five tests fail; a rotation is scanned and every row of it is drawn from one band of a slope |
| A turn is turned away before a row is spent | Drop `row_local`'s check of the map | One test fails; the refusal arrives at the first row instead of at `open` |
| The band holds every row the sampler visits | `rows_under` returns `bottom - 1` for an area filter | Three tests fail; the last row of every area band is missing and the sampler reaches past it |
| A bilinear row reads the sample below as well as the one above | `rows_under` returns `(top, top)` for a bilinear filter | Three tests fail; a bilinear scan reads one row and interpolates towards itself |
| A band taller than its bound is refused | Multiply `MAX_BAND_ROWS` by 1024 at the check | Two tests fail; a downscale of any steepness is admitted |
| Outside the band is not outside the picture | `Picture::at` answers `None` for a row outside the band | One test fails; a short band draws a hole and it looks like a picture with a hole in it |
| A band is the same picture cut down | Drop the check that the band describes the source | One test fails; a band of another picture is resampled as though it were this one |
| A window wrapped as a frame is not copied | `from_owned` builds through `from_packed` | Three tests fail; the buffer is copied into a fresh plane and the address changes |
| A frame hands its buffer back rather than repacking it | `into_packed` always repacks | One test fails; the buffer comes back at a new address |
| `packed` lends when the bytes are already packed | Always answer `Cow::Owned` | Three tests fail; every reader copies a frame out to look at it |
| A padded frame is not one packed run | `run` ignores the stride | One test fails; a padded frame lends its padding as though it were picture |
| A planar frame is not one packed run | `run` takes the first plane instead of the only one | Five tests fail; a planar frame lends one plane as the whole picture |
| A plane must hold what its geometry claims | Drop `Frame::new`'s length check | One test fails; a window of the wrong size becomes a frame whose rows run off the end of it |
| The winder appends the row it was given | `Winder::row` packs the row into a fresh buffer | One test fails; the sink is handed a copy at a different address |
| A cue's time is wall clock, not a frame label | `milliseconds` reads the tick at the nominal rate | Six tests fail; the sidecar drifts 3.6 seconds an hour at 29.97 |
| A millisecond is rounded, not truncated | Add nought instead of a half before flooring | Four tests fail; every timestamp is early by up to a millisecond |
| A cue that rounds to nothing is refused | Compare `to < from` instead of `to <= from` | One test fails; a caption shorter than a millisecond becomes a cue of no duration |
| Cues are written in order | Drop the check against the last cue's start | One test fails; a cue starting before the one before it is written anyway |
| A blank line inside a cue would end the cue | Require a carriage return **and** a blank line | One test fails; a caption with a blank line becomes two malformed cues |
| An arrow inside a cue would read as a timing line | Look for `--->` instead of `-->` | One test fails; a caption containing an arrow is written as a timing |
| Markup characters are spelled out | Escape each character to itself | Two tests fail; a caption saying R&D is written as markup |
| A sidecar is the same projection as its reel | Spot one frame less of the span than the reel carries | One test fails; the two files disagree about what was said |
| A reading is cut at every caption's edges | Record a caption's start and not its end | Two tests fail; a stretch spans a moment the words changed in |
| The span's own ends are edges | Drop the span's start | Ten tests fail; a span nobody speaks in has no stretch at all |
| Edges are put in order | Drop the sort | Four tests fail; the stretches overlap or run backwards |
| A repeated edge is not a stretch of no frames | Drop the dedup | Two tests fail; two captions beginning together leave a stretch of nought frames |
| A stretch is the gap that begins at the last edge before the instant | Drop the `- 1` after `partition_point` | Eight tests fail; every instant gets the stretch after the one it is in |
| A reading answers only for its own span | Drop the bounds check | One test fails; an instant outside the span is answered with the nearest stretch |
| A reading answers only in its own timebase | Drop the timebase check | One test fails; a position in another rate is answered as though its ticks meant the same |
| A line covers the stretch it is given for | Compare `from <= to` instead of `from < to` | Four tests fail; a caption that has finished is still on screen |
| A strip's band is measured across its own columns | Measure every strip across one column | Six tests fail; the band is short for all but the first column of each strip |
| A bilinear strip looks at both of its ends | Sample the first column twice | One test fails; a strip's band misses the rows its far end reads |
| A strip too tall is narrowed rather than refused | Never halve the span | One test fails; a turn wide enough to need strips refuses instead of being sliced |
| The whole row is covered by its strips | Advance past one extra column between strips | One test fails; a column between two strips is never drawn |
| A strip draws the columns it was given | Draw from column nought in every strip | One test fails; the picture repeats across the row |
| A band taller than its bound is refused | Multiply `MAX_BAND_ROWS` by 1024 at the check | Three tests fail; a downscale of any steepness is admitted |
| A strip of no columns is refused | Drop the empty-range check in `rows_under` | One test fails; a strip from a column to itself is answered for |
| A subsampled format still has no row | Drop the subsampling check in `row_description` | Four tests fail; a 4:2:0 frame is scanned a luma row at a time |
| A tile's band spans all of its rows | Measure the band across the tile's first row | Two tests fail; every row but the first reaches past the band |
| A bilinear tile looks at all four of its corners | Sample the top edge twice | One test fails; a tile's band misses the rows its lower edge reads |
| Each row of a tile is drawn at its own row | Draw every row of the tile as its first | One test fails; the band repeats down the picture |
| A tile writes one buffer a row | Drop the count check | One test fails; mismatched buffers are accepted and rows go missing |
| A band of a turn is drawn as one tile per strip | Measure each tile across one row | Two tests fail; each row fetches its own band and the saving is gone |
| A band of a row-local node is its rows stacked | Stack the first row every time | Eight tests fail; a band is the first row repeated |
| An empty band is refused rather than answered | Drop the range check in `Graph::rows` | One test fails; an empty or backwards range is answered |
| The export writes every row of every band | Drop the last row of each band | Seventeen tests fail; the reel is short of rows |

### A rule stated in a comment is not a rule

The curve's ease rounds half away from zero, and the code says so, and the
reason is good: the compositor rounds the same way, so a fade drawn by a curve
and a fade drawn by the compositor agree at the point where they meet.

Rounding towards nought instead failed nothing. Every test happened to use a
value that was already a multiple of the precision, so the two rules gave the
same answer everywhere the suite looked, and the rule that mattered was held up
by a sentence.

The fix was to find a case where the rules disagree and pin it: a tenth is
104857.6 parts in 2^20, so nearest is 104858 and towards nought is 104857. That
took deriving one value by hand, which is the price.

The general shape: **a rule that only a comment states is a rule that will
change without anybody noticing.** Rounding direction, tie-breaking, saturation
versus wrapping, the end a half-open range excludes — each is easy to write
down, easy to be right about, and easy to have no test for, because the obvious
test cases are the ones where it makes no difference.

### The field that detects staleness can itself go stale

The summary file's digest covers its header as well as its blocks, and it would
have been easy to hash only the blocks — they are the data, after all. A probe
against that version named the cost precisely rather than plausibly:
thirty-three header bytes become undetectably editable. Byte 8 is the sample
rate, so a 48 kHz summary reads as 44.1 and every block silently covers a
different span of time. Bytes 32 to 63 are the digest of the sound the summary
is *of* — the field whose whole purpose is to let a reader see that the summary
is stale.

A staleness check that can itself go stale is not a check. The general form:
**a field that exists to detect corruption has to be inside whatever detects
corruption**, and "it is only metadata" is the argument that puts it outside.

The number came from running the mutation and listing which bytes were accepted,
not from reasoning about which ought to be. Reasoning would have found the rate
and quite possibly stopped there.

### A drawing is not a check

The architecture document drew `media-editor-io` beneath `media-editor-media`
while the manifest had it depending on `media-editor-media`, and had done for
as long as
there was an `io` crate. Nobody was misled, because nobody consults a diagram
to find out what compiles — which is the point. The diagram was not wrong in a
way that hurt; it was wrong in a way that *could not be found*, because nothing
read it.

Every other rule in this project is enforced by something that runs. The
layering was the exception, and the exception is where the drift was.

So the layers are now declared in a fenced block that `tools/layering.py`
parses, next to the diagram that renders them for a reader. The check refuses
any dependency that does not run strictly downward, and it refuses a crate that
is in the tree but not the block. Restoring the old order produces three
findings, which is the evidence that the old drawing was wrong and not merely
drawn differently.

The general rule this is an instance of: **a document that states a fact about
the code should be readable by the code.** Prose for the reasoning, a
machine-readable block for the fact, and one check that they agree.

That check found a bug in itself on its first run, which is worth recording
too. It read `[[bin]] name = "media-editor"` as the package name of the image
crate and reported a crate missing from the layers. It was right to: the
finding was real, the cause was the reader rather than the tree, and a check
that had happened to skip that manifest would have passed and told nobody.

### A fixture can be too tidy to break

The control that pairs summary blocks across a channel boundary failed nothing
at first. The mutation was real and the test was aimed at exactly it — but the
fixture was eight blocks long, so every level had an even count, every block had
a partner within its own channel, and the boundary was never reached. Eleven
blocks halve to six, three, two, one: two odd counts, two blocks with no
partner, and the same mutation fails immediately.

Powers of two are the natural length to reach for and are the one length that
cannot exercise a halving's remainder. The same applies to a frame count that
divides the sample rate, a buffer that is a whole number of blocks, and a
sequence whose clips all start on the second. **A fixture whose dimensions
divide evenly tests the easy half of every function that divides.**

That the mean-square test caught it anyway is luck, not coverage: its fixture
happened to be thirty-four blocks long. A test that catches a bug it was not
aimed at is a reason to go back and fix the test that was.

### A round trip proves less than it looks like it proves

Skipping the gamut matrix was not caught by the conversion's round-trip tests,
because a matrix skipped in both directions is also a round trip. It was caught
only after a test was added that asserts what a *single* conversion must do —
BT.709's red sits well inside BT.2020, so expressing it there needs green and
blue as well, and a pipeline that skipped the matrix hands back pure red.

The general lesson, which applies to every symmetric operation in this project:
a round trip checks that two functions are inverses, not that either is
correct. Both need a test that looks at one direction on its own.

A second instance, found later and worth adding here rather than starting a new
heading. Dropping the opacity curve from the project file entirely — writing
"no automation" for every track — failed neither of the round-trip tests:
neither `a_project_survives_a_round_trip` nor `the_encoding_is_canonical`.
Both compare a round trip against *another
round trip*, so a field the writer never writes is missing from both sides and
they agree perfectly. It was caught only by a test that names the field, reads
it back, and checks the values it holds.

So: a round-trip test cannot see a field the format has forgotten. Every
field a format carries needs a test that names it.

**And then it happened again, one commit later.** A sound track's level curve
was added to the format, and dropping it from the writer failed nothing — for
exactly the reason written above, which had been written above at the time. The
fixture animated the picture track and not the sound one, so the lane the
mutation removed was a lane no test ever put anything in.

Writing a lesson down is not the same as applying it. The operational form,
which is narrower and harder to skip: **when a format gains a field, the
fixture gains a value for it in the same commit** — otherwise the sweeps cover
bytes that are never written, and every test agrees about a field that is not
there.

### A fixture that does not vary along the axis under test

Two controls on the capture writer failed nothing, and both had the same cause
in a new shape.

Writing every scanline as the first one broke nothing, because the fixture was
a horizontal ramp — whose rows are all identical. The comment above it even
said "a test pattern differs everywhere", which is true across and false down.

Taking Adler-32 modulo 65,536 rather than 65,521 broke nothing either, because
the fixture was thirty-nine bytes and neither accumulator ever reached a
modulus. The test computed the checksum independently, from its definition, and
still could not tell the two apart — an independent computation over data that
does not exercise the difference is not an independent check.

The general form, and the third time this project has met it: **a fixture that
does not vary along the axis under test cannot see a change to that axis.** It
was powers of two hiding a halving's remainder, then a static composite hiding
the instant, and now identical rows hiding a row index and small sums hiding a
modulus. The question to ask of every fixture is not "is this valid input" but
"does this input move when the thing I am testing moves".

It has since been met a fourth time, in the fourth shape: the conform suite's
test that an importer reads the record timecode rather than the event number
handed the events over backwards and passed with the sort mutated to use the
numbers — because reversing a list without renumbering it leaves the numbers
ascending with the record, so the two orders agree and either sort gives the
same answer. The fixture varied along the *order* axis and not along the axis
that separates the two readings of it. The events are now renumbered to agree
with the wrong order, so the numbers are a complete, self-consistent account of
a cut that runs the other way, and the control fails.

### A control has to mutate the branch the test takes

The control for "a mask is in frame coordinates, applied after the transform"
mutated the `None` arm — the one taken when there is *no* mask. The test has a
mask, so it took the other arm, and the control passed while proving nothing.

It is a small mistake with a general shape: **a control on a `match` must
change the arm the gate's fixture actually reaches**, and which arm that is is
worth checking rather than assuming from the line that looked relevant. The
same applies to any conditional — an `if` whose `else` a test never enters is
not covered by mutating the `else`.

Cheap to catch, because this is exactly what a control passing means. It is the
third distinct reason a passing control has turned up so far: a fixture that
cannot see the axis, a claim that was decoration, and now a mutation aimed at
dead ground.

### Black and white cannot tell light from code values

The resampler's linear-light test halved a checkerboard of black and white and
asserted 188. The control that replaced the decode with a straight division
**passed**, and the reason is that nought and one are exactly the fixed points
of a transfer function: `decode(0) = 0` and `decode(255) = 1` either way, so
their mean is the same number in both spaces.

At 64 and 192 the answers separate — 146 against 128 — and the control fails.
This is the sixth meeting of the fixture rule and the most specific form of it
yet: **a test of a non-linear function must use values where the function is
not the identity**, and for a transfer curve those are the mid-tones, which are
exactly the values a lazy fixture avoids because black and white are easier to
type.

### An interrupted control harness leaves a wrong build behind

A control edits the source in place and restores it afterwards. When the
process died between those two steps, the mutation stayed — and the next thing
to run was a *wrong build that looked like a real one*. The symptom was a test
failing for a reason that made no sense against the source as read, which cost
more time than the bug it was chasing.

The harness now restores on the way out however the process ends, including a
kill. The general form: **any tool that mutates a working tree has to restore
from a handler rather than from the next line**, because the next line is
exactly what does not run.

### A fallback inside a cached computation poisons the cache

A source node's identity covers the media, the tick and the description — not
whether the file was reachable. So a node that fell back to an offline slate
while evaluating would store that slate under the real picture's key, and serve
it after the drive came home.

This is worth stating as a shape rather than as one bug, because it applies to
every cached pure function: **anything a computation reacts to must be in its
key, or must be decided before the computation is named.** The second is nearly
always the better answer — availability changes, and putting it in the key
would make every cache entry useless the moment a drive was unplugged.

The test that holds it renders twice through *one* pool, absent then present,
and requires the second render to be the picture. A test that used a fresh pool
each time would pass with the bug in place.

### A slate has to read at the size it is drawn

The offline pattern's stripes were sixteen pixels apart, which is a solid
colour on a frame narrower than sixteen pixels — and that is exactly the size
the freestanding image renders at, and exactly the case where a slate being
mistaken for a shot of a red wall matters. A four-pixel test found it.

The period is now a fraction of the frame. The general form: **a pattern whose
job is to be recognised has to be defined relative to the picture, not in
pixels**, because the picture's size is not something it gets to assume.

### A theorem can be true of everything it was tested on and false

Conform's claim — *if the export leaves nothing behind, the round trip is
equal* — was stated three milestones before the case that breaks it was tried.
Two identifiers naming one digest: the export reports nothing lost, the import
resolves both clips to whichever identifier it finds first, and the sequence
comes back pointing at one of them.

Every test of that theorem passed, and none of them built a project holding the
same content twice, because nothing in the fixtures ever did that and nothing
in the model prevented it. The theorem was not wrong about anything it tested.
It was wrong about the world it assumed.

What found it was reading a **document** — a block in `ARCHITECTURE.md` that
described a media asset as carrying a location hint the type did not have.
Checking that claim led to looking at what the type *did* have, which led to
asking what makes two assets the same, which is the question the theorem
depends on and had never been asked.

The operational form: **a theorem's assumptions are worth writing down
separately from the theorem**, because the tests will only ever exercise the
world the fixtures build, and the assumption is exactly the thing no fixture
thinks to violate.

And the second form: prose is worth *checking* like a claim. This project has
a tool that refuses when the documented test counts disagree with the tree, and
nothing at all that notices when a diagram describes a type that does not
exist. Reading one carefully found a bug that four hundred tests did not.

### A picture, kept beside its hash

This document has said since its first version that a reference frame is stored
beside its hash *so that a failure can be looked at, not just counted*. For a
long time it could only be counted, and the reason was honest — the only frame
the freestanding image composites is sixteen pixels wide.

`tests/golden/reference.png` is the first one that is a picture: 320×180,
colour bars underneath, a ramp and a flat colour meeting at a soft wipe, both
inside a six-sided mask. On a mismatch the test writes what it actually
rendered beside the reference and names both paths. That is the difference a
picture makes over a digest: a hash says something changed, two files say what.

And its first version showed **nothing of what it claimed to show**. Both sides
of the wipe rendered the same test pattern, so the feathered edge the capture
existed to demonstrate was perfectly invisible — the same fixture lesson, for
the fifth time, in the place most likely to go unnoticed because the image
still *looked* fine. The capture now asserts that the two sides of the wipe
differ and that a band of partial values lies between them, so a reference that
stops demonstrating its subject fails rather than being quietly admired.

### Measured against the previous commit, not against reasoning

The slate's golden moved again when a clip gained a mask flag, and the obvious
explanation — one byte per clip, three clips, three bytes — was checked in a
**git worktree at the previous commit**: one clip grew by one byte, three by
three, seven by seven.

The check mattered more than the last two times, because this commit changed
both the version *and* the payload, and an earlier attempt to isolate them by
reverting only the version constant proved nothing — the mask bytes were still
being written, so both builds produced the same file. A "before" has to be a
real before.

### A control that passes can be worth more than the control was

The soft edge's zero case delegates to the hard path rather than dividing by a
band of nothing. The control for it — delegate to a band a thousandth of the
travel wide instead — **changed nothing**, and the reason is the interesting
part: at that width every pixel is already fully in or fully out, so the two
planes agree byte for byte.

Which means the delegation is a *convenience* rather than a patch over a
discontinuity, and the soft path **converges** on the hard one rather than
jumping to it. That is a stronger and more useful statement than the control
was trying to make, so it became a test — with a second half asserting that a
band wide enough to see *does* differ, so the comparison is not measuring a
function that ignores its argument.

This is the second time a passing control has been the finding rather than a
failure of the fixture (the first is two sections below, on claims that turned
out to be decoration). The two cases are different and both are worth knowing:
sometimes a control that changes nothing means the claim above it is empty, and
sometimes it means the claim is true for a better reason than the one written
down.

### A design rejected for a reason that was wrong

The shape rasteriser left soft edges out and recorded why: "the area weighted
by a ramp rather than a plain area", which is "a much larger case analysis".
That was written with conviction and was simply false.

The integral of an affine function over a polygon is its area times its value
at the polygon's centroid — the definition of a centroid, not a result about
one. The ramp is affine, the region is the pixel square clipped by two parallel
half-planes, and the clipper already existed. It is two clips and a moment.

Worth recording because a **reason** attached to a deferral is a claim like any
other, and nothing in this project checks the reasons. A test can fail; a
paragraph explaining why something was not built cannot. The operational form:
when a deferral's reason is technical rather than about priorities, it is worth
five minutes of actually trying, because the cost of being wrong is a feature
that never gets built and a document that confidently says why.

### A refusal behind a checksum needs a resealed fixture

`SPRJ` refuses an unknown transition tag, and the first test for it mutated the
tag byte in a real file and asserted the refusal by name. It could never have
seen it: the format's digest covers the payload, so a mutated byte is refused
as a **digest mismatch** before a single field is parsed — which is exactly
what the byte sweep two sections above asserts, on purpose.

A test for a field-level refusal in a digested format has to **reseal** the
file: change the byte, recompute the payload digest, write it back into the
header. That is not a contrivance — it is the case that matters, because a file
whose digest agrees with its contents is one something produced deliberately,
and that is precisely when the field checks have to hold.

The general form: **a check that sits behind an integrity check cannot be
tested by corruption.** Every format here that carries a digest has this
property, and every future test of one of its interior refusals needs the same
treatment.

### A golden that moves for a reason worth checking

Bumping `SPRJ` to version seven moved the slate's golden transcript. The
obvious explanation — the version byte is in the header and the slate's digest
covers the whole file — is also the correct one, but it was *checked* rather
than assumed: encoding one transition-free project under both versions produces
files of the same length that differ in exactly one byte, at offset four.

Worth the two minutes because the alternative explanation was live. The same
commit changed how transitions are written, and a golden that moved because the
payload changed in some way nobody had characterised would look identical from
here. Updating a golden is the one moment where a test stops being able to tell
you anything, so the reason has to be established before the number is
replaced, not after.

### A test over black cannot tell light from code values

The wipe's first test put white over black and asserted that the edge pixel
lands above the code-value midpoint, because mixing in linear light is
brighter. It read exactly 128 — the midpoint — and the assertion was wrong
twice over.

Over black the bottom layer contributes **no light at all**, so the result is
just the masked top layer encoded again, and the linear-light answer and the
code-value answer are the same number. This project's notes already carried
that finding once, about a compositing test; making it again in a new module
is what an already-recorded lesson costs when it is recorded as a fact about
one test rather than as a rule.

The rule: **a test whose background is black cannot distinguish compositing in
light from compositing in code values.** Put something with light in it
underneath.

And the direction was wrong too. Over a mid-grey the linear answer is 154 and
the code-value answer is 192 — linear is the *darker* one here, the opposite of
what the white-over-black intuition predicts, because stored 128 is only 0.216
of full light. The test now asserts the number, derived by hand, rather than an
inequality derived from an intuition.

### Two claims that were not load-bearing, established by trying

Both are in the rasteriser and both were written as though they mattered.

That the boundary line **belongs** to the region: making the test strict
changes no coverage anywhere, because a line has no area and the clipper puts
back as a crossing point exactly the corner the strict test would have dropped.

That the early exit when a polygon is clipped below three vertices **guards**
anything: the shoelace sum over two vertices is already nought, and clipping an
empty polygon yields an empty one, so removing it changes no answer. It is a
real optimisation — a sixty-four edge mask would otherwise keep clipping
nothing for every pixel outside it — and it is now labelled as one.

Neither is a bug. Both were comments claiming more than the code delivers, and
the only reason either was found is that a control was written for it and the
control passed. **A control that changes nothing is not always a bad fixture;
sometimes it is a true report that the claim above it is decoration.** The
outcome is the same either way: something has to change.

### A round trip that does not go through the file is not one

`conform::import` read the frame numbers off the timecodes as they arrived and
was wrong by a quarter for every list at 24, because `edl::parse` labels every
timecode at thirty — it has no rate to use and will not invent one, which
M3.5 states plainly. The label is four numbers; what they count in is told.

Every test that handed the exported list straight back to the importer passed.
They were not round trips. A value compared with itself agrees about everything,
including the parts of it that were never written down, and the rate is exactly
such a part: it lives in the `Timecode` and not in the file. Only the tests
that went **export, write, parse, import** could see it, and they failed
immediately.

So: a round trip is verified through the serialised form, never through the
in-memory value the writer produced. The same rule the project file already
follows for a different reason — a `SPRJ` test that skipped the bytes would
never test the bytes — restated here for the reason that bit: the bytes are
where the information is *lost*, and testing around them tests nothing.

### A golden hash of something that does not change

The slate began rendering a picture and reporting its digest, which is the
golden render hash `## Golden output` asks every render to carry and which
nothing here had. The first version pinned a project whose two clips both ran
the whole span — so every instant composited identically, and a control that
moved the playhead a frame broke nothing.

A golden hash of a static result pins the arithmetic and says nothing about the
inputs it claims to name. The fix was to put a *fade* on the upper track, which
makes the picture a function of the instant and, incidentally, puts the curve
arithmetic in the image: exact rationals, an interpolation, and a fixed-point
opacity reaching the compositor, on the target rather than only on the host.

The general form, which applies to every golden in this project: **a golden
output is only evidence for the inputs it actually varies with.** Naming an
input in the comment does not make the hash depend on it.

### The same lesson, one commit later

The heading below — that a number recorded repeatedly acquires a story nothing
checks — was written, and then the same mistake was made immediately.

`Node::Look` was deliberately *not* built in one commit, on the reasoning that
it would cost the image about two pages: `evaluate` would reach the lookup
table code, and the freestanding image links `evaluate`. That reasoning was
stated confidently and was wrong in both halves. The image links neither
`evaluate` nor any other symbol from `media-editor-render` — the slate exercises
the model, the reel, the pool and the test patterns, and never renders. When
the node did land, the footprint did not move by a byte.

The same check that found it also falsified the platform contract's
explanation of an earlier two-page rise, which said the same thing about the
same crate.

Writing a lesson down does not install it. What installs it is doing the thing
the lesson says at the moment it applies — and the moment it applied here was
a sentence beginning "it would cost", which is the shape a guess takes when it
is about to be recorded as a fact. **An estimate about a measurable quantity,
in a project that measures it on every build, is a decision not to run the
tool.**

### A wiring nothing tested, and a fixture that hid why

The graph gained a `Look` node, the model gained a grade, and `timeline::plan`
was taught to put one in front of the other. Deleting that wiring entirely
broke **no test**. The node had tests, the model had tests, and the join
between them had none — which is the seam that always lacks them, because both
sides look covered.

Writing the missing test found the real fault underneath. A look refuses
premultiplied coverage, and the timeline renders premultiplied, so a graded
clip could not render *at all*. The two want opposite things for good reasons:
`over` is only correct on premultiplied samples, and a non-linear function on
premultiplied samples computes `f(ac)` where `a·f(c)` was wanted.

The fix is to fetch a graded layer **straight**, grade it, and associate it
afterwards — which loses nothing, because the frame was never premultiplied.
Unpremultiplying one that had been would; that is why `Look::apply` refuses
rather than doing it quietly.

And the fixture had been hiding the collision. `Flat::frame` ignored the
description it was asked for and always answered with its own — harmless while
every layer was fetched identically, and exactly wrong the moment one was not.
One test depended on that fault deliberately, to prove the graph refuses a
source that answers a different question; every other test depended on it by
accident. The lie is now a field on the fixture, set only where it is the
subject.

**A fixture that ignores one of its inputs is a fixture that cannot see a
change to that input.** It reads as simplification and behaves as a blind spot.

### `new` is the only place allowed to start from nothing

Adding one field to `Clip` broke it in three places, and they were three
spellings of the same thing: `Item::with_duration` rebuilt a clip field by
field, `Item::split` built its tail with `Clip::new`, and `Edit`'s slip built a
whole new clip from three of the old one's fields. Every one of them silently
dropped the new field.

Two were found by reading and the third by a test, which is two more than
should have needed finding. The patch-each-site fix would have left the trap
armed for the next field.

What removes it: **a constructor starts from nothing, and everything else
changes one field.** `with_grade`, `with_source` and `with_duration` each take
`..*self`, so a field added tomorrow travels through all of them without
anybody remembering. `Clip::new` is the one place that begins empty, which is
correct there and nowhere else.

The general form, for any value type that gains fields over time: count the
places that *rebuild* it. If there is more than one, the next field will be
dropped by all but the one that gets remembered.

### A number kept by hand is a number that goes stale

Every crate's test count in the architecture table, and the total and the
control count in the README, were maintained by hand for the first five hundred
tests. None of them was ever wrong, and that is luck rather than process:
nothing checked them, and the only reason they stayed right is that updating
them was on a mental list.

`tools/counts.py` now reads them and refuses a disagreement — the same bargain
as `layering.py`, and for the same reason. It made one sentence in the README
change from words to digits, which is a small loss of prose for a fact the code
can read, and that trade is the whole idea: **prose for the reasoning, a
machine-readable fact beside it, one check that they agree.**

The count is static — `#[test]` attributes rather than a run — and that is
checked rather than assumed: at the commit that added the tool, the static and
runtime counts matched exactly for all nine crates. `make verify` runs the
suite anyway, so a divergence would show up there as a different total.

### A sweep that had to mutate inside the field's alphabet

The `.cube` parser had no sweep at all — the three binary formats each have
two, the edit decision list has one, and this project's own rules say a parser
without a target does not ship. That gap was found by going and looking, not by
anything failing.

Writing the sweep then produced three failures, and all three were the
*assertion* being wrong rather than the parser.

The sharpest one: a text sweep that replaces the `0` of `0.0` with a space
gives ` .0`, which splits to `.0`, which is still nought. Different text, same
number — and the sweep read that as "this byte carries nothing", which is what
it is meant to report for a byte the reader dropped. The mutation has to stay
inside the alphabet of the field it is mutating. A digit changed to a
*different digit* always changes the number it spells, and the claim is sharp
again.

The general form: **a text sweep that mutates outside a field's alphabet
measures the lexer's leniency rather than the parser's completeness.** Binary
formats do not have this problem, which is why the technique transplanted
badly.

### A format that cannot detect its own truncation

The same sweep found something about `.cube` rather than about the reader. A
prefix that stops before the last sample line is refused — the cube comes up
short and the count says so. A prefix that stops *inside* the last line can
still spell three numbers: `200.0 0.0 200.0` cut to `200.0 0.0 2` is three
numbers, and cut to `200.0 0.0 200.` is three numbers with the same values.

No reader could do better. `.cube` carries no length and no digest, so a file
truncated inside its final number is indistinguishable from a valid file
somebody authored differently.

Every format this project writes itself carries both, and this is exactly why:
`SPRJ`, `SPRW` and `SPPK` refuse *every* prefix, and this one cannot. That is
the argument for a length and a digest stated as a measurement rather than as a
principle — an interchange format without them has a class of corruption that
is undetectable by construction, and the test now says where the line falls
instead of asserting something convenient.

### A number that was being read as the wrong thing

The image's footprint has been recorded after every change — thirty-six, then
thirty-eight, forty, thirty-nine, forty-two — and read each time as "the
program has grown". Taking the number apart showed that reading was wrong in a
way that mattered: **sixteen of the forty-two pages are one constant**, the
static arena in `media-editor-rt`, which is a reservation rather than anything
the
program contains. The code went from twenty pages to twenty-six over the same
period, and sixteen pages never moved.

Worse, the conclusion drawn from the number — that the answer is to split the
program so the image links fewer crates — was aimed at the smaller half. The
arena alone is eighty-four per cent of what a Phipia program is given.

Nothing here was a *bug*: every measurement was correct and every entry in the
table was true. What was wrong was the sentence wrapped around them, and no
test can catch a wrong sentence about a right number. What catches it is
breaking the number down and looking, which `make audit` now does on every run.

The general form: **a single number that gets recorded repeatedly acquires a
story, and the story is not checked by anything that checks the number.** When
a figure is worth tracking it is worth decomposing at least once, because the
decomposition is what says whether the story is about the thing that is moving.

### A check that forces the architecture to move first

Adding a `.cube` reader meant `media-editor-io` depending on
`media-editor-render`,
and both were layer three — so the layering check refused it. That refusal is
the whole value of having written the check: the alternative is quietly adding
a sideways edge because a file needed one, which is how a layering becomes a
drawing again.

The right answer was not to work around it. `io` is the format layer for every
domain crate, so it belongs above all of them, and it moved to its own layer
with `app` and the image above it. The document changed, then the manifest, in
that order — which is the order the check enforces and the reason it is worth
enforcing.

A rule that only fires when it is inconvenient is the only kind of rule that
does anything.

### A bounding box is not a shape

A test called `every_tetrahedron_is_reached_and_none_of_them_is_wrong` was
written for exactly one mutation: giving one of the six tetrahedra the wrong
vertices. That mutation passed it.

The test checked that each result lands within the range its cell's eight
corners span. A tetrahedron given the wrong vertices still interpolates between
corners of the same cell, so it stays inside that box — the check was true of
the bug as well as of the fix. What a wrong vertex set actually breaks is
*continuity*: the six tetrahedra meet on the planes where two fractions are
equal, both branches either side must agree on that plane, and a wrong vertex
set puts a step in the surface. In a grade that is a hard edge through a smooth
gradient, which is the artefact tetrahedral interpolation is chosen to avoid.

So the test that catches it walks a line across all three planes and asserts the
result never jumps. The lesson generalises past this case: **a containment check
is usually satisfied by the bug as well as the fix.** "Within range", "not
negative", "sums to one" — each is worth having and none of them distinguishes
a correct computation from a plausible wrong one. The distinguishing property is
almost always a *relationship* — continuity, monotonicity, an identity, an
exactly known value — rather than a bound.

The bounding-box test is kept, because it does catch the mutation that swaps two
orderings. Both are needed and neither is the other.

### A guard whose refusal nothing triggers

The automated fader is clamped to the fader's own travel, and removing the
clamp failed nothing. Not because the clamp is idle: without it an overshooting
curve produces a `FaderOutOfRange` *refusal* rather than a wrong number, so the
mutation would have been caught the moment any fixture asked for a value past
the end stop. None did. Every curve in the suite stayed inside the travel, so
the guard was never approached from the side it guards.

This is the same shape as a fixture too tidy to break, but it fails in a
quieter way: a missing clamp does not give a wrong answer, it gives an error,
and an error nothing triggers looks exactly like an error nothing needs. **A
guard is only checked by an input that reaches it**, and for a saturating guard
that means a fixture that deliberately exceeds the bound — here an ease
overshooting to 30.375 decibels on a fader that stops at 24.

### Check the status of the thing you are checking

Four separate times now, a check reported success because it was reading the
wrong thing.

A mutation was made and nothing failed — because the mutated code did not
compile. A hook was tested with a ninety-five character line of `x` — which the
prose rule deliberately exempts, since a line with no spaces is an unsplittable
URL. And `git commit | tail -3` inside an `if` reported success — because a
pipeline's status is its *last* command's, so the `if` was testing `tail`.

Each time the surface reading was "the guard does not work", and each time the
truth was "the check did not reach the guard". They fail identically from the
outside, which is what makes this worth its own heading.

A fourth, from a different direction. Four controls were run over the mixer
with `cargo test -p media-editor-audio`, and one of them appeared to leave a
test
untouched that it should plainly have broken. It had not: `cargo test` stops
after the first *test binary* that fails, so the later binaries never ran at
all. The mutation was real, the test was right, and the report was reading a
run that had stopped early.

`--no-fail-fast` is the flag, and printing how many tests *ran* is the check on
the check — the same discipline as reading test counts rather than failure
counts. A control over a crate with several test binaries is not a control
until every one of them has run.

The habit that catches all four: **say what you expect to see, then look for
that**, rather than looking for the absence of a complaint. An expected failure
should be seen failing, with its message read. An expected exit status should be
captured from the command that produces it, not from whatever the shell ran
last. And a test input should be checked against the rule's own exemptions
before it is trusted to trigger the rule.

### A control that does not compile is a control that did not run

Twice now a mutation has been made, the suite has come back with nothing
failing, and the honest reading was "the invariant is not checked" — when the
truth was that the mutated code never built. `warnings = "deny"` turns an
unused binding into an error, and a mutation that removes a use of something
removes it from a whole file.

So a control is only a control once its build has been seen to succeed. In
practice: keep the mutated code compiling (`let _ = &thing;` is enough), and
read the *test counts* rather than only the failures, because "no failures" and
"no tests ran" look identical through a filter.

That is not a hypothetical. One control in this table passed for a real reason —
the zero-decibel fast path in `Gain::factor` turned out not to be load-bearing —
and it would have been indistinguishable from a control that never ran, if the
build had not been checked.

### A test whose name overstated its fixture

M8.10's join check is a `match` over two clips' animations: neither, both, or
one. A control that made two *present* animations always agree changed nothing,
and the test it was gated on was called
`two_differently_animated_clips_do_not_join` — which is what it ought to have
been testing and is not what it did. The fixture animated the tail and left the
head alone, so the only arm it ever reached was the one-sided one.

The lesson is not "mutate the arm the fixture takes" — that one is already
written down two sections up and was met again here anyway. It is that the
**name** said so first. A test named for two animated clips that only animates
one is a discrepancy readable without running anything, and it was sitting in
the file while the control was being written.

The test now runs all three cases and the invariant carries two controls, one
for each arm that can wrongly say yes.

### The seventh fixture that did not vary

A flat colour, scaled, is the same flat colour. The first version of the test
claiming M8.10 needed no renderer change rendered a 4×4 flat red frame through
two different framings and compared them — and it would have passed just as
happily if the framing had never been applied at all, because there was nothing
in the picture for a framing to move.

Seven times now, in different crates, with different subjects. The pattern is
always the same shape: the fixture is chosen for how simply it states the
*subject*, and simplicity in the subject usually means uniformity in the thing
being measured. The remedy that has worked every time is to put the *negative*
in the same test — render a third framing that must come out different — so
the fixture proves it can tell before it is asked to.

That third assertion is now the first one in the test, deliberately: if the
fixture cannot distinguish, the failure should say so rather than letting two
vacuous comparisons pass underneath it.

### Two mechanisms enforcing one rule, for the third time

`Track::roll` refused a boundary past the last item, and so did `Track::item`
a line later. The control that removed the first one failed nothing, because
the second produced the same refusal with the same name — and the test could
not tell which had spoken.

The redundant half is gone. What is left is the half that is not a duplicate
of anything: a boundary of nought would take `boundary - 1` round the houses,
and no later check stands in for that. Both trims now carry a control that
removes it, and both fail.

This is the third time the pattern has appeared — after `Over`'s two inputs
and the zero-decibel fast path — and it is starting to look less like an
accident than like the ordinary result of writing a guard and then writing the
code under it. **A guard that duplicates one further in is not defence in
depth; it is a test that cannot fail.**

### A mutation that changes nothing is not a control

Distinct from a control that *passes* — that is a finding about the code. This
was a finding about the mutation. The control for "a slide leaves the item it
slides alone" replaced the slid item with `shortened_from_the_front(index, 0)`,
which shortens by nothing and moves the in point by nothing, so it hands back
the item it was given. The suite was right to accept it.

Reading the verdict as "the invariant is unchecked" would have been wrong; so
would reading it as "the invariant holds". The only correct reading is that
nothing was tried. A control has to be checked for *being a change* before its
result means anything — which, in practice, means writing the mutation and
then asking what value it produces rather than only what line it replaces.

### An inverse a test never asked for

`a_slide_is_its_own_inverse` applied `+by` and then `-by` and compared with the
original. It passed with the edit's inverse mutated to hand back `by` unchanged
— because the test never used the inverse. It computed one.

Two properties, and only one of them was being tested: *sliding back undoes a
slide* is arithmetic, and *the edit knows how to undo itself* is the journal.
There is now a separate test that goes through `undo`, and the control fails
it.

### A control that found slack rather than a hole

The font's per-piece bounding box exists to skip work: a run of forty
characters is two hundred convex pieces and a pixel touches a handful, so each
piece is asked about a pixel only if it could possibly cover it. The bound was
written the obvious way — round the near edge down, the far edge up.

The control took one pixel off the far edge and **changed no answer**. Which
meant the bound had a pixel of slack in it: a piece ending at 6.875 has its
last ink in pixel 6, and one ending at exactly 7.0 also has its last ink in
pixel 6, because pixel 7 is the square from 7 to 8. Rounding up is right for a
*coordinate* and wrong for the *pixel containing* one.

Nothing was incorrect — a generous bound only costs work — and the finding is
still worth having, because the control's job is to establish that a line is
load-bearing and this one established that a character of it was not. The bound
is tight now, and the control takes one pixel off *that*, and fails.

### A generalisation asserted one size too far

A stroke in this face is two of sixteen units, so at an em of `n` pixels it is
`n/8` pixels wide. The test asserting that nothing is solid below eight pixels
to the em looked like arithmetic and was **false at seven**.

Two strokes that abut make a region thicker than either of them, and a pixel
inside the corner where a stem meets a bar is covered by both. The stroke width
bounds what one *piece* can fill, not what the *letter* can — and the fixture,
which runs the whole repertoire rather than one glyph, found the corner the
reasoning had not.

The test now asserts what was measured (four pixels to the em, nothing solid)
and the note says why the tidier claim is not available.

### A separator can stand in for a length, until there are three fields

The legend's identity covers two captions, and the test for it set two legends
whose captions *concatenate* to the same bytes — the collision a
length-prefixed encoding exists to prevent.

Removing the length prefix failed that test, as it should. Weakening it to a
constant instead — one byte, always the same — **passed**. With exactly two
fields and a fixed separator between them, the separator itself lands in a
different place and the streams differ.

Which is true and is not the property. The prefix is there so the encoding
stays unambiguous when a third field arrives, and a control that only proves
"something separates them" would go on passing through the change that breaks
it. The control removes the prefix entirely, which is the mutation that
matches the claim.

### A ceiling with no test under it

The caption's size is the smaller of what fits the width and a fraction of the
height. Removing the second failed nothing: every fixture was a caption long
enough that the *width* decided, so the ceiling never bound.

It exists for the opposite case — two characters across a wide frame fit at an
enormous size — and there was no fixture like that. There is now: "OK" on a
480-pixel frame, with the inked rows counted. That is the sixth or seventh time
the same shape has appeared, and the shape is always "the fixture does not
reach the branch", not "the branch is wrong".

### A test that pinned the wrong number and drew the wrong moral

This file has said twice already that **a test over black cannot tell light
from code values**. The third time it cost a real bug, and the bug had been
sitting inside the test written to catch it.

`composite::faded` and `composite::masked` scaled a premultiplied layer's
colour in *code values*. The module's own header has said since its first
version that "a premultiplied sample is the encoding of `light × coverage`,
not the encoded value scaled by coverage" — so the convention was written
down, and two functions broke it, and nothing noticed.

Nothing could. Every fixture faded a layer that was **black**: the dissolve
goldens, the wipe, the slate. Nought times anything is nought, and the two
arithmetics agree there exactly. What found it was a fixture that had never
existed — a dissolve between two *identical* pictures, which has to be that
picture and instead sagged by twenty-eight code values in the middle.

The part worth keeping is what it did to the wipe test. That test exists
**specifically** to pin the difference between light and code values. It
asserted 154 for the edge pixel, with a comment reading: "the linear answer is
the *darker* one — the opposite of what a white-over-black intuition predicts,
which is why this asserts the number rather than a direction." Every clause of
that is the bug talking. The linear answer is 205 and it is the *brighter* one.

So the test's own stated moral — assert the number, never the direction — was
right, and was not enough. Asserting a number derived from the code you are
testing is asserting that the code does what it does. The number has to be
derived from the *definition*, by hand, in the comment, which is what both
tests now do and what the slate's `picture red` has done all along — and that
is why `picture red` moving from 73 to 98 could be checked rather than
accepted.

### A size the exemption argues from, caught going stale

`tests/size.rs` exists because `Item`'s `large_enum_variant` exemption argues
from a number: a clip is 288 bytes. Adding two fades made it 336, and the test
failed with "either shrink it or rewrite the argument, but do not leave the two
disagreeing".

Shrinking it was the better answer, and finding that out was the point. Two
`Duration`s carry a timebase each, and a fade's timebase is the clip's — so
they were the same fact written three times and three facts to keep agreeing.
Stored as ticks the clip is 304 bytes, the invariant is gone, and the accessors
still hand back `Duration`s counted the clip's way.

A number nobody measures is a number that goes stale; a number a test measures
is a design review that arrives on time.

### A guard that could not be made to fail, for the fourth time

`Clip::source_at` opened with an arm for real time — return the in point plus
the offset, rather than multiply by a speed that happens to be one. Its
control replaced the condition with `false` and the gate passed, because a
speed of one takes `floor(offset x 1) = offset` and the general path answers
identically on every input.

The tempting reading is that the control was badly aimed. It was not: there is
no input that separates the two arms, so no gate could hold that guard, and a
guard no test can hold is a guard that will be edited one day by someone who
has no way of finding out they were wrong. It went, and the doc comment now
says it went and why — because the next person to notice the multiply will
have the same idea.

Fourth time, after `Track::roll` and `Track::slide`'s bounds checks and the
transform's invertibility check. The shape is always the same: a guard placed
for clarity in front of a path that already handles the case.

### An anchor that was a substring of the one it meant

The layer stack asks a clip for its media position in two places — the
ordinary arm and the dissolve arm — and they sit at different indentations.
The control for the ordinary one anchored on
`"            let source = clip.source_at(offset)?;"`, twelve spaces, and the
harness reported **two matches**: the dissolve arm's line is the same text at
twenty spaces, and contains the twelve-space version as a substring.

The harness caught it, which is the whole reason it counts matches rather than
replacing the first. But the fix was not just a longer anchor. Two call sites
had one control between them, and the dissolve arm — where a retimed shot
would jump for exactly the length of a transition, and nowhere else — had
never been tested at all. It has its own gate now, and its own control.

An anchor that matches twice is usually a harness complaint. It is sometimes a
coverage report.

### A control that does not compile, for the third time

`let along = offset;` in place of `floor_of(size x offset)` left `floor_of`
with no callers, and `warnings = "deny"` makes an uncalled private function a
build error, so the gate never ran and the harness said `did not build`. The
mutation had to keep the function alive — `floor_of(Rational::new(offset, 1)?)`
— which is a narrower and better control anyway: it isolates the *speed*
rather than the speed and the flooring together.

Recorded already, twice, and worth recording again because the failure mode is
invisible from the mutation: nothing about `let along = offset;` looks like it
will delete a function.

### A bug asserted before it was measured

The ink milestone was written believing it fixed something: type was packed as
`u8::MAX`, 255 is an illegal code value in limited range, so a limited-range
title had been writing an illegal sample. That sentence went into the module
header, two test comments and a test *name* —
`an_illegal_code_value_is_no_longer_what_a_card_writes` — before anything had
been run against the old code.

The control refused to fail. Putting the hard-coded byte back left both
limited-range assertions passing, because `premultiply` re-encodes through the
frame's own table on the way out and `encode` searches only the legal codes.
The illegal byte never survived to anything anybody could see.

Measuring properly took two more steps and found something the prose had not
considered. The fault is real for a **slate caption** and absent for a
**card**, and the difference is the coverage plane: a card's letters come from
a hard-edged stencil, so every sample is full light at full coverage or none at
none and the clamp catches it; a caption is antialiased, and a partly covered
pixel premultiplied from 255 claims more light than its coverage allows, which
`checked_premultiplied` refuses by name. A limited-range slate was not drawing
a slightly wrong caption. It was failing with `NotPremultiplied`.

Three things worth keeping:

- The test that was going to carry the claim now asserts its own premise — the
  stencil has no soft edge — instead of resting on it. A test that depends on
  a fact it does not state is a test that stops meaning what its name says the
  day the fact changes.
- A test named after a fix has to be checked against the code it fixes. This
  one held before and after, which is fine for a statement about encoding and
  fatal for a statement about a bug, and only its name knew the difference.
- The pattern is the wipe test's from M8.17, one turn earlier in the loop:
  there the number came out of the code, here the *fault* came out of the
  reasoning. Neither is measurement. A control is what tells the difference,
  and it is worth writing the control before the prose it is going to check.

### A parameter that was the same thing as another, until it was not

`Project::check_source` took a media identifier, an in point and a **length**,
and computed `in point + length` as the range a clip reads. That was exactly
right for eleven milestones, because a clip read one frame of media per frame
of timeline and the two numbers were the same number.

M8.18 made them different. A clip at double speed reads twice its length, and
nothing noticed: the guard kept comparing the timeline length against the
asset, so a clip could be retimed until it read past the end of its media and
the refusal arrived at the frame that fetched it rather than at the edit that
caused it. `Edit::SetClipSpeed` was not in `validate`'s match at all.

Two things made it invisible. The retiming milestone wrote a guard for the
*other* end — `with_speed` refuses a reverse that would read before its media
begins — and having written one guard, the pair felt thought about. And every
fixture in the suite was a clip well inside a long asset, so nothing was near
either end. That is the seventh fixture problem in a new costume: a clip a
hundred frames into nine thousand cannot see a bound at either end.

The remedy is not a second guard. It is to stop passing the *ingredients* of a
question and pass the thing that can answer it. `check_source` now takes the
clip the edit would produce and asks it for `source_span`, which cannot fall
behind the mapping because it is the mapping. **A caller that recomputes a
callee's arithmetic is a copy that has to be kept in step, and nothing tells
you the day it stops being.**

The same argument in one line: the four edits that can widen what a clip reads
all build the clip first and hand it over, rather than each assembling a range
out of the fields it happens to be changing.

### A page count credited to the change that happened to be in flight

M8.21 added an opacity curve to a clip, an edit to set it, two functions and a
lane in the file — and the image *fell* two pages. The sentence that wanted to
be written was "the milestone paid for itself", and it would have been wrong.

Nothing in the diff explains a saving, so it was tested rather than explained.
On the previous commit, with none of the milestone's code, a dummy `[u64; 3]`
in `Clip` — twenty-four bytes of nothing, exactly what the new field costs —
took the image from 93 pages to **91** and `Edit::apply` from 20,210 bytes to
16,476. The saving was bought by the clip crossing 320 bytes, past which the
optimiser stops emitting an inline copy of a clip in each of `Edit::apply`'s
arms and calls out instead.

Two things worth keeping:

- **A control does not have to be a mutation that breaks something.** This one
  changed nothing about behaviour and everything about attribution: it asked
  "would this number have moved anyway?" and the answer was yes. Every
  footprint entry in this project's history is a claim about *cause*, and only
  this kind of experiment can check one.
- A struct getting **bigger** made the program **smaller**, which is the
  opposite of what every earlier footprint note in this repository assumed
  while reading a total. The seventy-six kibibytes belong to the program, not
  to the struct, and the two are not the same measurement.

### "Did not build" is not "did not fail", and the harness now says which

Fourth time a control's mutation left a binding unused and `warnings = "deny"`
turned that into a build error, so the gate never ran. The harness printed
`did not build` on its second line and `PASSED` on its first — and `PASSED`
is what got read, three times in one run.

The mutations were fixed the usual way, by keeping the binding used. The
harness was fixed too: a mutation that never compiled now prints **`NOBUILD`**
rather than `PASSED`, because those are different findings and only one of them
is about the code under test.

Recorded as its own entry rather than folded into the earlier ones, because the
earlier lesson was "write mutations that compile" and this one is "a report
that can be misread will be". The first is advice; the second is a fix.

### A claim about rounding that the control would not support

`mix` interpolates `from + s·(to − from)` rather than `from·(1 − s) + to·s`,
and the comment above it said the first form is chosen because "only the first
is exact at the ends" — a `Fixed` multiplied by one comes back unchanged, so a
strength of one gives back the look byte for byte, which is the property every
project written before this milestone depends on.

Every clause of that is true except the word **only**. Both forms are exact at
both ends, because multiplying by nought and by one are each exact, and a sweep
of sixty-five strengths against every code value finds **zero** disagreements
at either end. What the sweep does find is 59,520 disagreements in the middle,
each of exactly one unit in the last place, enough to move a quantised byte in
about one case in sixty — and the chosen form lands nearer the exact rational
109,382 times against 43,084, equal otherwise.

So the form is load-bearing, and it is load-bearing for a *smaller* reason than
the one written down, and the exactness the milestone actually rests on belongs
to the multiply rather than to the arrangement.

What is worth keeping is when this was found. The comment was written first,
the control was designed second, and designing it was what exposed the claim —
before the control was ever run, because writing "and the other form does not"
requires deciding what would fail. This project has recorded twice that a
control is what tells measurement from reasoning; this is the cheaper version
of the same lesson: **a control does not have to be executed to falsify a
sentence, only specified.**

### A claim that was decoration, found by a control that changed nothing

The project file writes a clip's grade strength *after* its grade, and the
comment gave a reason: the reader is then "holding both when it has to refuse a
strength with no look to be the strength of" — the same argument the motion's
position after the transform makes.

Swapping both the write and the read broke nothing. The refusal does not happen
at the read at all: `read_item` collects its fields and then builds the clip
through the model's own constructors, and `Clip::with_grade_strength` is the
last link in that chain. It cannot see what order the bytes arrived in, and the
grade is already on the clip by the time it runs whichever way round the file
is written.

The order is a convention worth keeping — a reader comparing the two halves of
the format should find the same shapes in the same places — and it is not a
mechanism. The comment now says so. This is the third entry of this kind, after
the rasteriser's two, and the shape is always the same: **a reason attached to
a line is a claim, and the only thing in this project that checks a claim is a
control.**

### A threshold effect that moves cost rather than removing it

M8.21 recorded a surprise: adding a field to `Clip` made the image *smaller*,
because past 320 bytes the optimiser stops emitting an inline copy of a clip in
each of `Edit::apply`'s many arms. M8.22 recorded the mirror when the clip grew
again. Both were measured, and both were read as the whole story.

This milestone took the clip from 416 bytes to 440 and the symbol table shows
what those entries could not. `Edit::apply` **fell 9,599 bytes** — and its
helpers rose: `refade` by 1,257, `remotion` by 1,245, `reshape` by 1,194,
`slip` by 1,077, `regrade` by 1,009, `remove` by 962 and `retime` by 637, which
is **7,381** of it back. The optimiser did not delete the work when it stopped
inlining; it moved the work into the functions it now calls.

So a footprint that falls when a struct grows is not a saving, it is a
relocation, and reading the total is what hides that. The general form, which
is this project's own note about a single number turned one turn further:
**a total that moves has a location, and the location is a different fact from
the movement.** The tool that prints the largest symbols already had the
answer; nothing had subtracted two of its outputs before.

### The one rotation that cannot test rotation

The area resampler's `area_at` has said since it was written that an affine map
"sends a square to a parallelogram, so this is exact and has four corners
however the picture is turned". Nothing had ever turned a picture, so the
clause was prose. The obvious fix was a quarter turn of a small picture, and it
is a good test: a right angle sends the pixel grid onto itself, so the answer
must be an **exact permutation** of the pixels with no blending anywhere, and
four quarter turns must give back the picture byte for byte. Both hold.

Neither exercises the parallelogram. A right angle takes the unit square to the
unit square, so every preimage in that test is still **axis-aligned**, and the
tilted path the comment describes is never entered.

The control is what said so, and said it exactly. Replacing the four-corner
preimage with the exact axis-aligned box around it — a change that is a no-op
whenever the preimage is already axis-aligned — fails **one** test out of two
hundred and thirty-five: the one that turns by the three-four-five angle, where
the box has area 49/25 against the parallelogram's one. Every axis-aligned test
passes. Both quarter-turn tests pass.

The general form, and this project's eighth meeting with it: **a fixture chosen
because it makes the answer exact has usually chosen the case where the code
under test does not run.** The exactness that made a quarter turn attractive —
the grid maps onto itself — is precisely the property that keeps it out of the
branch. The tilted test asserts something weaker about each pixel (a flat field
survives, so the weights sum to one) and something much stronger about the
code, and both tests are kept because neither is the other.

### A rotation that is exact, which a comment had ruled out

`transform.rs` has said since M8.9 that there is no rotation in this model
because "a sine and a cosine are not exact, and a project whose framing
depended on them would drift". Every word of that is true of an **angle** and
none of it is true of a **rotation**.

The rational points on the unit circle are dense, and the tangent half-angle
substitution reaches all of them: `cos = (1 − t²)/(1 + t²)`, `sin = 2t/(1 + t²)`
is a rational pair for every rational `t`, with `cos² + sin² = 1` exactly and a
determinant of exactly one. So a turn composes without drifting, preserves area
exactly, and returns to where it started after four quarter turns — none of
which a floating-point rotation can claim.

This is the deferral-reason lesson again, one heading further on from the soft
edge that was left out because the case analysis "would be much larger" and
turned out to be two clips and a moment. The shape repeats: **a reason attached
to a deferral is a claim, and a claim about arithmetic is worth five minutes of
trying.** The cost of being wrong is a feature that never gets built and a
document that confidently explains why.

What the reason *did* get right is worth keeping too, because it is why the
type stores the point rather than the parameter: `t` reaches every rotation
except the half turn, which is `(−1, 0)` and sits at infinity. A type that
stored the parameter could not turn a picture upside down. The parameter is
what a **curve** holds, because a curve needs somewhere unbounded to live.

### A field in a digest that nothing asked to be there

The anchor went into `Node::Transform`'s identity as a matter of course — every
parameter of a node is in its identity, that is what the rule says. The control
absorbed the *across* component twice, so two pivots differing only **down** the
frame would collide in the cache, and **nothing failed**.

The field was right and the coverage was absent, which is the same verdict as
"a guard whose refusal nothing triggers" reached from the opposite direction:
there, a correct clamp was unreached; here, a correct digest was unasked. Both
look identical from outside — a control that passes — and both mean a line of
code is being trusted rather than checked.

The test that closes it varies **each axis separately**, because the mutation
crossed out exactly one of them and a fixture that moved the pivot diagonally
would have caught nothing.

### The eighth fixture, written while fixing the seventh

That test's first version drew `TestPattern::Bars` and asserted that three
pivots give three different pictures. Two of the three comparisons were
vacuous: bars are constant **down** the frame, so moving the pivot down changes
no pixel, and the assertion that the anchor's second component matters passed
without the anchor's second component mattering.

The circumstance is what makes it worth recording. This was a test being
written *for a control that had just found a coverage gap*, by someone who had
written the phrase "a fixture that does not vary along the axis under test"
into this file twice that day — and the fixture still did not vary along the
axis under test. A checkerboard fixed it.

**Knowing the rule does not apply the rule.** What applies it is asking, of
every fixture, the question the rule is made of: *does this input move when the
thing I am testing moves?* — and asking it after the fixture is chosen rather
than before, because the fixture is always chosen for how simply it states the
subject.

### A page boundary is a step, and two milestones landed either side of it

M8.24 added 143 bytes of code and the image did not move: 94 pages before and
after. M8.25 added 816 and the image went to 95. Neither number is a surprise
on its own and the pair is the useful thing: `.text` is padded to a page
boundary, so a milestone's cost in *pages* depends on how much slack the last
page happened to have when it arrived, not only on what it added.

Which means "no growth" and "one page" are the same measurement reported at
different offsets, and neither is a statement about the change until the byte
count is beside it. Every entry in the footprint table now carries one.

### A reader that had gone blind, and still printed a table

`make audit` runs `tools/footprint.py`, which splits the image by section and
attributes every sized symbol to the crate that emitted it. The per-crate table
in the platform contract comes from it, and this project's own note about that
number says a figure worth tracking is worth decomposing at least once.

It had stopped decomposing anything. The reader knew Rust's **legacy**
mangling — `_ZN`, then length-prefixed path components — and the toolchain
emits **v0**, so `crate_of` returned `(unmangled)` for every symbol in the
image. The table still printed. It still added up. The line under it still
said "attributed in total". It had one row.

Nothing failed, because nothing could: the output is a table, a table with one
row is a table, and no check compared it against anything. The contract's
per-crate figures had been unreproducible for as long as this had been true,
and the largest-function list beside them was quoting `Face::stencil` at 23,807
bytes — a symbol that no longer exists at that size, in a section it no longer
lives in.

Three things now hold it:

- the reader understands **both** manglings, because which one a build uses is
  the toolchain's decision and not the tool's;
- it **checks itself** against five real symbols before it reads the image at
  all — including one whose crate name begins with an underscore, which is the
  case that carries v0's length separator and which this reader got wrong on
  its first attempt, filing `__rustc` as `___rust`;
- it **refuses** when more than five per cent of the sized symbols carry a name
  it cannot parse. The measured share is 0.27%, all of it compiler intrinsics
  with C names, and the bound is twenty times that on purpose: the failure it
  exists to catch is not a drifting number, it is a cliff — the day the
  mangling changes, that share goes from a quarter of a per cent to a hundred
  in one commit.

The general form, and it is the sharpest version this project has met of a
shape it already knew: **a tool that reports a shape rather than a verdict can
fail without failing.** `layering.py` refuses. `counts.py` refuses. `elf-audit`
refuses, and `audit-control.py` proves on every run that it can. `footprint.py`
printed, and printing is not checking. Every tool that produces a number needs
a condition under which it declines to.

### One direction tested, the other assumed

The column cut and the column heal are the same shape: work out the whole
answer in a pass that touches nothing, then publish it in a pass that cannot
fail. The cut got a test for that — a set naming one track that can be cut and
one that cannot, and an assertion that the refusal leaves the first track
exactly as it was.

The heal got the reasoning and not the test. Its control, which collapses the
two passes into one and heals each track as it is reached, changed **no
answer** — because no fixture ever asked a merge to refuse partway.

The tempting reading is that the heal is safe anyway: it removes an item
rather than adding one, so nothing allocates, so what could go wrong. That is
an argument about *allocation* answering a question about *atomicity*, and the
two are different: `Item::join` refuses a pair that is not one item cut in two,
and a column that healed three tracks and refused the fourth would be a merge
nobody could undo in one step.

The general form is narrower than "test both directions", because the two
directions here were not symmetric in the author's head — one felt like it
needed the care and the other felt free. **When two operations are built from
one argument, the argument has to be tested at both ends, and the end that
feels safe is the one that will not be.**

### An exemption that expired on schedule

`Edit` carried `#[expect(clippy::large_enum_variant)]` with a paragraph
explaining why one variant is so much bigger than the rest: `InsertItem` holds
an item, because that is what an insert is and what makes undo work, and
boxing it would be an infallible allocation R-5.2 forbids.

Adding a lift gave the enum a **second** variant holding an item. The lint
compares the largest against the next largest; with two the same size there is
no outlier, so it stopped firing — and because the exemption was written as an
`expect` rather than an `allow`, the build failed with "this lint expectation
is unfulfilled".

That is the whole reason the distinction exists, and it is worth recording as a
success rather than as an incident. The paragraph beside the attribute had said
so in advance: *"an `expect` rather than an `allow` so that the day the
difference falls back under the threshold the build says so instead of carrying
a stale exemption."* It did, on the exact day, without anybody remembering.

**Prefer the annotation that expires.** `allow` is a claim that never has to be
true again; `expect` is a claim checked on every build. The same shape as
`tests/size.rs` pinning a number a doc comment argues from, and as
`counts.py` reading the totals the README states — each is a sentence a machine
can re-read.

The ceiling in `tests/size.rs` stayed, because it was the half doing the work
all along: it bounds what the whole history costs, and no lint was ever going
to notice that.

### A claim about somebody else's repository, checked by nothing

Every number this project asserts about *itself* is held by something that
runs: `counts.py` reads the totals the README states, `tests/size.rs` pins the
byte counts a doc comment argues from, `footprint.py` refuses when it stops
understanding the mangling, and every invariant carries a control.

The platform contract asserts numbers about **Phipia**, and nothing held any of
them. It was written against Phipia v1.1.0 and said, in its own opening and in
the charter and in the README, that *Phipia cannot run Media Editor*. Phipia
reached 2.1.0. It grew a read-write FAT32 volume, a versioned native syscall
boundary carrying time and entropy, and a Media Editor editing workspace in its
own shell — and this repository went on saying the opposite for eleven
releases, in three documents and one crate's module header.

The failure is the same shape as the two the footprint tables record, with one
difference that makes it worse: a stale footprint table is a number nobody
re-measured, and a re-measurement is one command away. A stale claim about
another repository is a number nobody could re-measure without going and
reading that repository, which nothing in this build does or can do.

**What changed as a result.** The platform contract now states the Phipia
version *and commit* it was read against, at the top, in bold, as a basis
rather than as background; the capability table's column says which version it
describes; and the five numbered requests Phipia has since answered are marked
answered, with what Phipia actually built quoted beside what this document had
asked for. Keeping the request next to its answer is deliberate: Phipia built
`PHIP-08` larger than it was asked and built `PHIP-05` and `PHIP-14` for
networking's sake rather than for this project's, and both of those facts
predict how the rest of the list will be answered.

**What has not changed** is the one finding that still holds: no `OSFXSR`, no
`OSXSAVE`, no `fxsave`, no `xsave` anywhere in Phipia's source, and a kernel
still built `-mno-sse -msoft-float`. Re-checked at 2.1.0. It is the reason
every number in this program is an exact integer, and it is the only item on
the list that has not moved in eleven releases.

There is no automated control for this one, and saying so is better than
implying otherwise. The nearest thing to a control is the version and commit at
the top of the document, which at least makes the staleness *visible* to the
next person who reads Phipia — the same trick the growth history uses, and the
same trick `expect` uses in place of `allow`.

### Six controls that passed, and the one gap behind five of them

Twenty-three controls went with the vault and six of them failed to fail. That
is the worst run in this document and the most useful, because five of the six
were one mistake made once.

**No test had ever built a hostile file.** Every vault test encoded a vault and
decoded it back. That exercises the encoder and the decoder against each other
and proves they agree — which they would also do if both were wrong. What it
never produces is a file *no encoder would write*, and four invariants existed
only against those: the spans running end to end, the digest recomputation, the
item bound, and the zero tail of a name field. Each mutation was caught by some
other check first, so each control passed while the invariant it was aimed at
had nothing behind it.

The project file's tests have had a `resealed` helper since M4 for exactly this
reason: a mutated byte is refused as a broken seal long before the field it
changed is looked at, so a hostile-file test has to recompute the seal to reach
anything. The vault's tests did not have one. They do now — six of them, and
the same mutation that passed against each invariant now fails it.

**The lesson is about the shape of a suite, not about a bug.** A round-trip
test and a hostile-file test look similar and prove opposite things: one says
the two halves agree, and only the other says either half is right. A format
without the second has a decoder whose refusals are decorative, and the way to
find that out is a control, which is what happened.

### A seal that covers something already checked

The sixth was different and is worth keeping separate. The vault's seal covers
the header as well as the index and the material, and mutating that half
changed no answer — because every field in those twenty-four bytes is
independently checked: the magic, the version, both reserved words, the count
against its bound, and the payload length against the file's own length. A
mutation there is refused by the field, not by the seal.

That is the same finding `Clip::with_ramp`'s timebase check produced, and it
got the opposite treatment, deliberately. The ramp's guard was deleted because
nothing would ever make it load-bearing. This one stays: it is one call, and
the day this layout gains a *descriptive* field — the way `SPRW`'s header
carries a transfer function, where a flipped bit turns every frame into a
different picture with nothing to notice — the redundancy stops being
redundant. What changed is the control, which now mutates the half of the seal
that is doing work, and the test behind it, which changes a name to another
**legal** name of the same length. Nothing but the seal can see that.

Two identical findings, two different answers, and the difference is whether
the redundancy is structural or accidental. Writing down which is which is the
part that stops the next reader deleting the wrong one.

## Fuzzing

Every parser has a target, and the list of parsers is long: the project file,
every container, every codec bitstream, subtitle formats, LUT files, fonts,
EDL, XML interchange, and the metadata inside all of them.

- Targets run in CI on every change to their parser, briefly, and on a schedule
  for longer.
- Every crash becomes a committed regression test with the input minimised.
- A corpus is committed and grows; it is never reset.
- A parser without a target does not ship (R-11.3).

## Golden output

Rendering is verified by hash, not by eye (R-4.10).

- A golden test names its project, its inputs, its settings, and the SHA-256 of
  its output.
- The same render on a different machine, at a different core count, in a
  different run order, must produce the same hash.
- A golden hash changes only in a commit that says why, shows the visual
  difference, and has been reviewed as an intended change.
- Reference frames are stored as OpenEXR or PNG beside the hash so that a
  failure can be looked at, not just counted.

`media_editor_io::png` is what writes them: eight-bit grey, colour, or colour
and
coverage, no interlacing, no palette, and no compression at all — a legal zlib
stream of DEFLATE *stored* blocks, so every byte of the file can be read by
hand. It refuses premultiplied coverage rather than guessing, because a PNG
stores straight colour and writing a premultiplied frame as though it were
straight is a picture darker than the one rendered, worst exactly at the edges
somebody is looking at.

It was checked against an independent decoder rather than only against itself:
Python's `zlib.decompress` and `zlib.crc32` read a capture back and the pixels
came out identical to the frame. A checksum verified only by the code that
wrote it is a checksum that agrees with itself.

**No reference is committed yet, and that is deliberate.** The only golden
render today is sixteen pixels by nine, which is not a picture and tells a
person nothing when it changes. The writer exists so the rule can be satisfied
the moment there is a frame worth looking at, and `PHIP-03` is what makes one.

## Pull-request evidence

Every pull request states:

- the exact commands and CI workflows run;
- the scenario, transcript lines, or test names that establish the change;
- the negative control performed and the refusal it produced;
- any emulator, toolchain, or platform limitation that applies;
- the most credible failure the checks do not cover.

"None" is not an acceptable answer to the last one.

## Release evidence

A release publishes, from the release commit: the reproducible artefacts and
their digests, the vendored source of every dependency, the licence record, the
dependency manifest, every QEMU transcript, the golden hashes, the fuzz corpora
state, the benchmark results with their machine profile, and a plain statement
of what the release does not do.

A binary is not published unless those records match — the same rule Phipia
applies to its measured BusyBox profiles, for the same reason.
