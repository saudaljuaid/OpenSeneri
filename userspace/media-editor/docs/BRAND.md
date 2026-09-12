<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Media Editor identity

`Media Editor` is the only public name of this application. Public prose uses
`Media Editor`; paths, command-line keys, symbols, crate names, and build
artefacts use `media-editor`; preprocessor guards and build environment
variables
use `MEDIAEDTO`. `Media Editor Slate` is the historical name of the first
workspace, part of Media Editor and not a separate product.

The application is part of the Phipia project and says so. It never claims to
be an operating system, and Phipia never claims to be an editor.

## The canonical mark

The mark is the clapperboard supplied by the project owner: an open film slate
seen at a slight angle, in near-black with soft shading, with white diagonal
stripes across the raised clapper stick and across the top edge of the slate
body, white horizontal rule lines dividing the writing area, a vertical divider
in the second row, and three small grey pins on the hinge plate. It sits on a
transparent field with rounded corners and no border.

[`assets/media-editor-logo.png`](../assets/media-editor-logo.png) is the source
of
truth. It is the exact supplied 1335×1178 transparent RGBA image, 742,944
bytes, with SHA-256:

    C5D706B274132B5FCAF0BB016D0DA56DDD1DC54B417709364874AD1A58611EB5

The mark was supplied under another filename and renamed once, into the path
above, which is the only name it will ever have here.

Media Editor has no PNG decoder and will not gain one for its own mark. When the
interface exists, a deterministic build tool converts this file into a bounded
runtime stream that safe Rust validates before anything draws it, exactly as
`tools/make-logo-asset.py` does for Phipia's pebble, and both the source digest
above and the tool's output digest are pinned by `make verify`.

The rules on the mark are Phipia's rules, and they are absolute. Do not redraw,
trace, recolour, crop, mirror, rotate, flatten its transparency, add type to it,
place it in a frame or tile, or substitute a visually similar clapperboard.
Public uses preserve its aspect ratio.

Media Editor's clapperboard and Phipia's pebble may appear beside one another
with
clear space between them. They are never merged into a combined mark.

## The palette

The mark supplies no interface palette. Media Editor's palette exists to serve
one
requirement that overrides taste:

**Chrome around a picture must not bias judgement of that picture.** Every
surface a frame is judged against is strictly neutral — equal red, green, and
blue — so nothing in the interface tints what the editor is looking at. This is
not a style preference; it is the reason colourists work in grey rooms.

| Role | Value | Note |
| --- | --- | --- |
| Viewer surround | `#1A1A1A` | Neutral. Nothing else may appear directly against a frame. |
| Application background | `#141414` | Neutral. |
| Panel face | `#232323` | Neutral. |
| Raised panel | `#2C2C2C` | Neutral. |
| Rule and outline | `#3A3A3A` | Neutral. |
| Primary text | `#E8E8E8` | Neutral. |
| Secondary text | `#9A9A9A` | Neutral. |
| Timeline track base | `#1F1F1F` | Neutral. |
| Teal accent | `#4F837F` | Selection and focus. Phipia's teal. |
| Gold accent | `#C4A44E` | Attention and unsaved state. Phipia's gold. |
| Green accent | `#598561` | Completion. Phipia's green. |
| Red accent | `#A55050` | Refusal and record. Phipia's red. |
| Violet accent | `#705984` | Markers. Phipia's violet. |

Accents come from Phipia's palette: the two
programs are one project and should look like it. Accents belong to controls,
never to the viewer, and never inside the picture area. No gradient, no glow,
no translucency over a frame, no coloured overlay a user could mistake for
something in the image.

Phipia's warm white `#F7F6F0` and slate-violet desktop `#595976` are First
Light's, not Media Editor's. They appear in documentation and never in the
application.

## Typography and interface language

The first interface uses a validated bitmap font, exactly as Phipia does, and
gains a shaped-text path only when titles require one. Type in the chrome is
small, dense, and legible at a glance; type in a title is whatever the editor
chose, rendered faithfully.

## Voice

Short, direct, and specific. Sentence case. No exclamation, no marketing, no
platform claims, and no vocabulary borrowed from a competitor's manual.

Proof words — `PASS`, `READY`, `ONLINE`, `VERIFIED` — belong in transcripts and
diagnostics, never in front of the editor. An error names what was refused and
what is still true: "the media file ends mid-frame; the timeline is unchanged"
rather than "an error occurred".

Never claim a capability the release does not have. Phipia's project status
section is the model: state the boundary plainly, and let the work speak.

## Public surfaces

The identity applies to the repository name and description, release titles,
the application's own title and about panel, transcripts, panic diagnostics,
documentation, crate names, ABI symbols, artefact names, and any capture
published from a real run. A surface carrying an older or invented name is a
defect, not a detail.

## Verification

`make lint` enforces the naming rules in this document. When the interface
exists, `make verify` will pin the source digest above, the runtime encoding
digest, and the branded symbol and artefact names, and a QEMU scenario will
compare the drawn pixels against the decoded stream, as Phipia's boot proof
already does for the pebble.

Until then the rule that matters is the negative one: nothing claims a mark it
does not have, and no build ships a drawn version of this file that a tool did
not produce from it.
