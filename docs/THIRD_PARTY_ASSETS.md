<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Third-party sources and visual assets

Phipia's build is offline and deterministic. The exact third-party source
files used by the runtime and UI are committed and licensed beside the code or
assets. Runtime sources are pinned to exact upstream Git objects or release
archives. Visual sources are converted by host tools and then parsed through
bounded Rust formats before C draws them.

## ext4plus

The sole ext4 implementation candidate is the official
[`arihant2math/ext4plus`](https://github.com/arihant2math/ext4plus) repository at
commit `ec7e8443e474376977bb752cde370762226a5a50`, Git tree
`a4aea888632546b2bbfbefa97b43ca6c8f945fc8`. The exact `no_std` source tree,
README, MIT license, and Apache-2.0 license are retained under
`vendor/ext4plus/`. Phipia selects the MIT terms for GPL-3.0-only distribution;
both upstream notices remain available.

The local manifest removes workspace inheritance and development-only inputs;
the local lock resolves that manifest to the repository's exact offline Cargo
mirror, matching the kernel lock. The implementation source is otherwise
pinned to the recorded tree apart from reviewable Phipia deltas. The accepted
runtime profile is writable only through Phipia's retained mutation stage and
ordered JBD2/NVMe executor. Upstream does not implement journaled writes by
itself; the local delta supplies the checksummed transaction, recovery,
checkpoint, revocation, retry, and power-cut-tested durability boundary.
`vendor/ext4plus/PHIPIA-PORT.md` records that boundary and the exact feature
configuration.

## BearSSL

TLS uses BearSSL 0.6 from the official
[`bearssl.org` Git repository](https://www.bearssl.org/gitweb/?p=BearSSL;a=summary),
annotated tag object `7d8e767e79bb1750345e571ec89cca1da13b52df`,
commit `8ef7680081c61b486622f2d983c0d3d21e83caad`, and Git tree
`3d0709034c2b5eb735d43ff639411ac31e76153b`. The retained `inc/` and `src/`
trees are byte-for-byte upstream files. BearSSL's MIT license and README are
preserved under `vendor/bearssl/`.

Phipia disables BearSSL's hosted entropy and time adapters and its optional
SSE2, AES-NI, and POWER8 implementations. The SDK wrapper supplies native
entropy, validated realtime, trust anchors, a canonical DNS hostname,
monotonic transport deadlines, and a bounded TLS 1.2 cipher profile. The
cryptographic primitives and protocol state machine are unmodified.

## zlib

The compression source is the official zlib 1.3.2 release archive from
[`zlib.net`](https://zlib.net/), SHA-256
`bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`.
It corresponds to annotated tag object
`216c70c020aa53f0c40920d155f808b6b59c9acb` and commit
`da607da739fa6047df13e66a2af6b8bec7c2a498` in the official repository.
The zlib license is retained verbatim.

Phipia carries the byte-exact public headers and nine-source `Z_SOLO` core for
bounded in-memory deflate/inflate and checksums. Hosted gzip-file adapters and
the allocation-backed `compress*` convenience API are excluded.
`vendor/zlib/SOURCE-MANIFEST.sha256` pins each retained upstream file, while
`vendor/zlib/PHIPIA-PORT.md` defines the freestanding build and allocator
contract. The reproducible SDK installs this profile as the static `libz.a`.
Phipia's bounded native shared-object loader is separate; zlib itself is not
shipped as a Phipia DSO. Authenticated Phipia DSOs can share immutable RX
physical pages across processes, but the current zlib SDK artifact remains the
reproducible static `libz.a` described above.

## SDL 2

The multimedia compatibility layer is the official SDL 2.32.10 source from
[`libsdl-org/SDL`](https://github.com/libsdl-org/SDL) at commit
`5d249570393f7a37e037abf22cd6012a4cc56a71` (tag `release-2.32.10`). The
retained `include/` and `src/` trees and upstream zlib license are committed
under `vendor/sdl2/`; exact retained-tree and license hashes are recorded in
`vendor/sdl2/UPSTREAM-COMMIT.txt`.

The byte-exact upstream `test/testdrawchessboard.c` application used by the
signed-install proof is retained under `apps/upstream-sdl-chess/`. Its source
URL, Git blob, SHA-256, license, and bounded Phipia harness are recorded in the
adjacent `UPSTREAM.md`.

Phipia adds an explicitly selected `__PHIPIA__` configuration and native
Phipia video/input, PCM audio, pthread/futex, timer, and preference-filesystem
backends. Disabled subsystems and the evidence boundary are recorded in
`vendor/sdl2/PHIPIA-PORT.md` and `docs/SDL.md`. No Linux compatibility layer or
SDL dummy video/audio backend is used.

## Inter

The graphical shell uses `InterVariable.ttf` from the official Inter repository
at commit `353b61b9f4430d5f420d56605a6e7993e0941470`. Inter is distributed under
the SIL Open Font License 1.1; the exact license is committed as
`assets/fonts/Inter-LICENSE.txt`.

`tools/rasterize-inter-ui.py` converts printable ASCII at the pinned size into
the committed alpha atlas and proportional metrics. The ordinary build does
not need Pillow and does not parse TrueType: `tools/make-ui-font-asset.py`
packs those committed intermediates into SUF2, whose exact metrics, length,
fingerprint, glyph ranges, and alpha data are validated before installation.

## Lucide

The thirty-three Settings, Canvas, Store, search, and window-control
pictograms come from the
official Lucide repository at commit
`23f9abc4ed0146cffededd3d7f94c1018bfdf693`. The selected SVG sources and
Lucide ISC license are committed under `assets/icons/lucide/`.

`tools/rasterize-settings-icons.py` composes those pictograms into one coherent
4×3 glossy category sheet. The sheet is committed, so CairoSVG is a development
regeneration dependency only; a normal Phipia build remains self-contained.
`tools/rasterize-canvas-icons.py` parses the selected SVG path, arc, circle, and
rounded-rectangle geometry with only Python's standard library, then applies
bounded 4× coverage sampling to produce the checked `SCI1` alpha resource used
by the native Canvas package. Phipia rasterizes the pinned Lucide search
geometry with bounded integer supersampling at its two small display sizes,
avoiding both a jagged hand-drawn glyph and a new kernel image decoder.
The same bounded sampler renders the pinned Lucide X, square, and minus
geometry over the close, maximize, and minimize controls.
`tools/rasterize-store-icons.py` composes the selected Lucide navigation marks
into the committed monochrome Store sprite; CairoSVG and Pillow are required
only to regenerate that source sprite, never by the ordinary build.
`tools/verify-ui-assets.py` pins every selected SVG, the license, and the
committed raster resources by SHA-256.

## 3d-dock

Phipia's Dock interaction and glass-shelf geometry are a native fixed-point
port of [`saudaljuaid/3d-dock`](https://github.com/saudaljuaid/3d-dock) at
commit `8ab14d0c372ab797475e49b8a658d54f30f706bc`. The original implementation is
written in C with Cairo and X11. Phipia preserves its raised-cosine hover
curve, pointer-anchored layout, eased panel width, press squash, decaying
bounce, trapezoidal shelf flare, warped reflection strips, running lights, and
tooltip fades, while replacing those hosted dependencies with bounded Q16.16
math and direct cached-framebuffer drawing. Only Phipia's eight applications are
present. The upstream MIT license is committed as
`docs/third-party/3d-dock-LICENSE`.

## Phipia photographic scenes

The fourteen wallpaper sources are real photographs downloaded from Unsplash
and used under the [Unsplash License](https://unsplash.com/license). The exact
cropped 1024x768 PNGs are committed; the original source pages are:

- [Purple galaxy](https://unsplash.com/photos/a-purple-and-blue-space-filled-with-stars-NYwZhS4afQc)
- [Star field](https://unsplash.com/photos/a-galaxy-surrounded-by-stars-in-space-Rw7H7ELqJS4)
- [Milky Way lake](https://unsplash.com/photos/milky-way-over-mountain-lake-A2N0wk8k70g)
- [Milky Way reflection](https://unsplash.com/photos/milky-way-and-mountains-reflected-in-a-serene-lake-HUYPJupBvwE)
- [Forest waterfall](https://unsplash.com/photos/a-waterfall-in-a-forest-DIFY95lLyzU)
- [Waterfall valley](https://unsplash.com/photos/scenery-of-waterfalls-9NgKOxVY4wM)
- [Desert dunes](https://unsplash.com/photos/a-group-of-sand-dunes-in-the-desert-HEf0fKgJA1Q)
- [Aurora](https://unsplash.com/photos/aurora-borealis-jwIk4Z3Msi4)
- [Aurora fjord](https://unsplash.com/photos/aurora-borealis-above-mountain-and-body-of-water-l9cneQNE03Y)
- [Golden mist forest](https://unsplash.com/photos/misty-forest-landscape-with-soft-golden-light-tkoA1sWwHUg)
- [Yosemite mist](https://unsplash.com/photos/forest-covered-by-mist-xPmdw_RliUA)
- [Alpine lake](https://unsplash.com/photos/alpine-lake-reflecting-mountains-under-clear-blue-sky-4vGDNyXH8n0)
- [Tropical sunset](https://unsplash.com/photos/tropical-coastline-with-palm-trees-at-sunset-o_X5J_5wsYQ)
- [Ocean cliffs](https://unsplash.com/photos/rocky-cliffs-overlook-calm-ocean-at-sunset-DGDl6JQpgBw)

They are packed without another resize into the deterministic RGB565 SPW3
album used by Desktop wallpaper selection. The 16-bit source path avoids the
visible palette dithering of the earlier RGB332 prototype while keeping
PNG/DEFLATE outside the kernel trust boundary.
