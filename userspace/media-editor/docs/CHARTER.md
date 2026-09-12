<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Media Editor charter

Media Editor is a professional non-linear video editor written for Phipia and
for
nothing else. Phipia is a freestanding x86_64 operating system built from first
principles; Media Editor is the application that gives it a reason to exist
beyond
its own boot proofs.

The destination is a Final Cut Pro class editing suite: a timeline that holds a
real programme, playback that does not stutter, colour that is defensible,
audio that is sample-exact, titles that look drawn rather than typed, and an
export whose bytes are reproducible. That is the destination. This repository
starts at the beginning of the road, and this document says exactly where the
beginning is.

## Where the beginning is

This paragraph used to open *"Phipia v1.1.0 cannot run Media Editor"*. It was
true when it was written and it is not true now, and leaving it standing while
Phipia grew eleven releases underneath it is the exact failure the verification
document keeps recording about numbers. So it is corrected here rather than
deleted, because where the beginning was is part of the road.

**Phipia 2.1.0 runs a Media Editor workspace**, in its First Environment shell,
opening and saving projects on a read-write FAT32 volume. What Phipia cannot
yet run is *this* program: the freestanding Rust image has no Ring 3 path of
its own, because an image is admitted at a fixed page layout named per profile
and the widest of those is 76 KiB.

What is still missing for the image is narrower than the original list, and
each item is tracked in the platform contract: no generally stable native
application ABI, no loader that computes a program's size instead of being told
it, no userspace memory service, no audio device, no second core, no userspace
access to the framebuffer or to input, and no architectural guarantee that a
Ring 3 program may execute a single SSE instruction. That last one has not
moved in eleven releases and is still the largest single fact about writing
media software here.

So Media Editor's first work is not a timeline. It is:

1. a complete and honest map of what the application will be built out of;
2. a set of engineering rules strict enough that the map stays true;
3. a numbered list of capabilities Phipia must grow, each stated in Phipia's
   own vocabulary of measured profiles, ledger stages, and negative controls.

Those three are the contents of this repository today. Nothing here claims to
run, because nothing here runs yet. See
[`PLATFORM_CONTRACT.md`](PLATFORM_CONTRACT.md) for the capability ladder and
[`ROADMAP.md`](ROADMAP.md) for the order the work is done in.

## Native to Phipia, and only Phipia

Media Editor is not portable software that happens to have a Phipia build. It is
a
Phipia application.

- There is no portability layer, no `#ifdef` for another operating system, no
  POSIX assumption, and no abstraction whose second implementation would be
  Linux. Host builds exist only to test pure logic; they are evidence, never a
  shipping target.
- Where Phipia lacks a capability, the answer is to specify and measure that
  capability in Phipia, not to emulate it inside Media Editor.
- Where Phipia's contract is narrower than a library expects, the library is
  adapted or refused. The contract is not widened to suit a dependency.

This is a constraint chosen on purpose. An editor that owns its operating
system can make promises a portable editor cannot: exact frame timing, exact
memory behaviour, a render that is bit-identical between runs, and a failure
mode that is a named refusal rather than a stall.

## What the application owns

| Concern | Language | Why |
| --- | --- | --- |
| Project model, timeline, media library, undo/redo, pipeline coordination, UI state | Rust | The whole application is untrusted-input-shaped and lifetime-shaped. This is the default and the majority. |
| The single C ABI boundary to Phipia, and later to external codec libraries | C ABI | A boundary exists because Phipia's application ABI is C-shaped, not because a second language is desirable. |
| Tiny freestanding shims where the boundary is instruction- or register-shaped | C | A shim is small enough to read in one sitting and holds no state of its own. |
| Sealed inner loops that a measurement proves need it | C++ | Only after a correct Rust implementation exists to be measured against, and bit-exact with it. |

[`ENGINEERING_RULES.md`](ENGINEERING_RULES.md) is the normative version of that
table and of everything else. The table is a summary; the rules govern.

## Inherited law

Three habits are taken from Phipia unchanged, because they are why Phipia is
worth building an application on.

**Bounded contracts.** A feature is a stated shape with stated maxima. Widening
it is a new contract with new evidence, never an edit to an old allowlist.

**Evidence over assertion.** A green run proves the checked contract under
recorded conditions and nothing more. A screenshot shows presentation, not
correctness.

**Refuse instead of repair.** Truncated, overlapping, wrapped, ambiguous, or
otherwise malformed input is refused by name. Nothing is guessed, patched, or
partially published.

## Deliberate non-goals

Media Editor does not aim at, and will refuse work toward: cross-platform
support,
a stable third-party plugin ABI before the application itself is stable, POSIX
compatibility, network features of any kind, telemetry or analytics, cloud
services, proprietary codec SDKs, or any format whose only implementation is
under a licence incompatible with GPL-3.0-only.

## Documents

| Document | Contents |
| --- | --- |
| [`ENGINEERING_RULES.md`](ENGINEERING_RULES.md) | The normative rules. Every other document defers to it. |
| [`DEPENDENCIES.md`](DEPENDENCIES.md) | The open-source map: every component considered, its licence, and its verdict. |
| [`DEPENDENCY_POLICY.md`](DEPENDENCY_POLICY.md) | How a dependency enters the tree, and how it leaves. |
| [`PLATFORM_CONTRACT.md`](PLATFORM_CONTRACT.md) | What Media Editor needs from Phipia, and what is proven to work today. |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | The planned crate map and the shape of the data model. |
| [`ROADMAP.md`](ROADMAP.md) | Milestones, smallest first. |
| [`VERIFICATION.md`](VERIFICATION.md) | What counts as evidence. |
| [`BRAND.md`](BRAND.md) | The mark, the palette, the naming, the voice. |
| [`GLOSSARY.md`](GLOSSARY.md) | Editing vocabulary, defined exactly enough to implement. |

Media Editor is licensed under [GPL-3.0-only](../LICENSE), the same licence as
Phipia.
