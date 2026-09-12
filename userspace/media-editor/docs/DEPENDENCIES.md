<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Open-source map

This table records the open-source components evaluated for Media Editor and the
current verdict for each.

Listing a component does not make it a dependency. Adoption requires the import
gate in
[`DEPENDENCY_POLICY.md`](DEPENDENCY_POLICY.md), which re-verifies every claim
made here against the vendored source.

## How to read this

### Tier — can it run on Phipia?

| Tier | Meaning |
| --- | --- |
| **T0** | Runs today's shape: `no_std`, no allocator required or `alloc` only, no operating-system services, no floating-point SIMD requirement. |
| **T1** | Needs Media Editor's own runtime: a heap, a time source, and tens of megabytes. Available once `PHIP-01`, `PHIP-03`, and `PHIP-05` exist. |
| **T2** | Needs a Phipia capability that does not exist. The blocking `PHIP-nn` is named. |
| **T3** | Cannot be used: licence, hosted assumption, size, or it duplicates something Media Editor must own. |

### Verdict

| Verdict | Meaning |
| --- | --- |
| **Adopt** | Intended for the tree when the related feature is implemented. |
| **Candidate** | Likely, pending an evaluation the policy defines. |
| **Watch** | Revisit when a platform capability or the project's needs change. |
| **Reference** | Used on the host to build fixtures or check conformance; never linked into a shipping artefact. |
| **Reject** | Will not be used. The reason is given. |

### Licence compatibility

Media Editor is GPL-3.0-only, like Phipia. That fixes what may be linked in.

| Compatible | MIT, BSD-2-Clause, BSD-3-Clause, ISC, Zlib, Apache-2.0, MPL-2.0, LGPL-2.1-or-later, LGPL-3.0, GPL-2.0-or-later, GPL-3.0, BSL-1.0, CC0-1.0, Unlicense |
| --- | --- |
| **Incompatible** | GPL-2.0-only, Apache-1.1, CDDL, EPL, SSPL, BUSL, "non-commercial", "research only", the Fraunhofer FDK AAC licence, any proprietary SDK EULA |
| **Case by case** | AGPL-3.0 (combinable with GPLv3 but imposes obligations Media Editor does not want), any licence with an added patent or field-of-use clause, any dual licence whose free half is the incompatible one |

Two one-way rules matter and are easy to get wrong: Apache-2.0 code may be
combined into a GPLv3 work but not a GPLv2-only one, and an LGPL or MPL
component becomes governed by the GPL when linked here. Neither direction
reverses.

**Every licence in the tables below is a claim to be re-verified from the
vendored source at import time.** Registry metadata is not evidence.

---

## 1. Runtime foundation

The `no_std` floor everything else stands on.

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `core`, `alloc`, `compiler_builtins` | Rust | MIT / Apache-2.0 | T0 | Adopt | Ships with the toolchain. `alloc` only once the allocator exists. |
| `libm` | Rust | MIT / Apache-2.0 | T0 | Adopt | Phipia has no libm and no FPU contract. Every transcendental goes through this. |
| `bytemuck` | Rust | Zlib / MIT / Apache-2.0 | T0 | Adopt | Safe plain-old-data casts for pixel and sample buffers. |
| `zerocopy` | Rust | BSD-2-Clause / Apache-2.0 / MIT | T0 | Candidate | Stronger parsing story than `bytemuck`; evaluate the two, adopt one, not both. |
| `bitflags` | Rust | MIT / Apache-2.0 | T0 | Adopt | Typed flag sets without hand-rolled masks. |
| `arrayvec` | Rust | MIT / Apache-2.0 | T0 | Adopt | Fixed-capacity vectors, which is what R-1.1 wants everywhere. |
| `heapless` | Rust | MIT / Apache-2.0 | T0 | Adopt | Bounded queues, maps, and SPSC channels with no allocator at all. |
| `smallvec` | Rust | MIT / Apache-2.0 | T0 | Watch | Only if a measured allocation pattern justifies it. |
| `hashbrown` | Rust | MIT / Apache-2.0 | T1 | Adopt | `HashMap` without `std`. Iteration order is never allowed to reach output (R-4.5). |
| `spin` | Rust | MIT | T0 | Candidate | Single-core today makes most locks unnecessary; revisit at `PHIP-11`. |
| `static_assertions` | Rust | MIT / Apache-2.0 | T0 | Watch | Rust 1.79+ `const` blocks cover most uses without a dependency. |

## 2. Memory allocation

Media Editor must bring its own allocator: Phipia gives pages, not a heap.

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `rlsf` | Rust | MIT / Apache-2.0 | T1 | **Adopt (leading)** | Two-Level Segregated Fit: O(1) bounded allocation and free, which is what R-5.3 needs for the audio path. |
| `talc` | Rust | MIT | T1 | Candidate | Small, fast, `no_std`. Good fallback if `rlsf` disappoints. |
| `linked_list_allocator` | Rust | MIT / Apache-2.0 | T1 | Watch | Simple and well understood, but fragmentation behaviour is wrong for media buffers. |
| `buddy_system_allocator` | Rust | MIT | T1 | Watch | Predictable, wasteful at media block sizes. |
| `dlmalloc` | Rust | MIT / Apache-2.0 | T1 | Reject | Larger and less predictable than the alternatives for this workload. |
| `slab`, `slotmap`, `thunderdome` | Rust | MIT / Zlib / MIT | T1 | Adopt (`slotmap`) | Stable generational identifiers for every object in the project model. |

## 3. Project model, identity, and history

The heart of the application, and the part most tempting to take from a
library. Mostly it should not be.

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `slotmap` | Rust | Zlib | T1 | Adopt | Generational keys make a deleted clip's identifier unusable rather than dangerous. |
| `rpds` | Rust | MPL-2.0 | T1 | Candidate | Persistent structural sharing turns undo into "keep the old root", which is exactly R-9.2. |
| `im` | Rust | MPL-2.0 | T1 | Watch | Same idea, larger, `std`-leaning. Evaluate against `rpds`. |
| `imbl` | Rust | MPL-2.0 | T1 | Watch | Maintained fork of `im`. |
| `undo` / `redo` crates | Rust | MIT / Apache-2.0 | T3 | Reject | An edit model is the application's core semantics. It is written here (R-12.7). |
| `petgraph` | Rust | MIT / Apache-2.0 | T1 | Candidate | Only for the render graph's topology checks, never for the timeline. |

## 4. Serialisation and the project file

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `serde` | Rust | MIT / Apache-2.0 | T1 | Adopt | `no_std` with `alloc`. Derives only; no runtime reflection. |
| `postcard` | Rust | MIT / Apache-2.0 | T1 | Adopt | Deterministic, compact, `no_std`, and suitable for an explicitly versioned schema. |
| `ciborium` (CBOR) | Rust | Apache-2.0 | T1 | Watch | Self-describing, useful for metadata sidecars, larger than `postcard`. |
| `rmp-serde` (MessagePack) | Rust | MIT | T1 | Watch | No advantage over `postcard` here. |
| `serde_json` | Rust | MIT / Apache-2.0 | T1 | Candidate | Needed only for interchange with FCPXML/OTIO tooling, not for the native format. |
| `toml` | Rust | MIT / Apache-2.0 | T1 | Watch | Configuration only, if configuration ever becomes a file. |
| `xmlparser` | Rust | MIT / Apache-2.0 | T0 | Candidate | `no_std` pull parser; the right size for FCPXML import. |
| `roxmltree` | Rust | MIT / Apache-2.0 | T1 | Candidate | Read-only DOM over `xmlparser`, if a tree is genuinely needed. |
| `quick-xml` | Rust | MIT | T1 | Watch | Faster and larger; only if the pull parser proves insufficient. |

## 5. Hashing, integrity, content addressing

Everything in the media pipeline is keyed by content (R-8.5), so this is a hot
path, not a formality.

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `blake3` | Rust | CC0-1.0 / Apache-2.0 | T0 | Adopt | Content identity for media, cache keys, and frame hashes. Fast even scalar. |
| `sha2` (RustCrypto) | Rust | MIT / Apache-2.0 | T0 | Watch | SHA-256 is Phipia's vocabulary for pinned artefacts and Media Editor speaks the same one — but it is already implemented here, checked against the published vectors, so this is now a replacement to be justified rather than a gap to be filled. |
| `crc` | Rust | MIT / Apache-2.0 | T0 | Adopt | Container and chunk checks where a format specifies CRC. |
| `crc32fast` | Rust | MIT / Apache-2.0 | T1 | Watch | Its value is the SIMD path, which is blocked until `PHIP-04`. |
| `xxhash-rust` | Rust | BSL-1.0 | T0 | Watch | Only if a profile shows BLAKE3 dominating a cache lookup. |

## 6. Compression

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `lz4_flex` | Rust | MIT | T1 | Adopt | Frame and waveform caches: decompression speed matters more than ratio. |
| `miniz_oxide` | Rust | MIT / Apache-2.0 / Zlib | T1 | Adopt | Pure-Rust DEFLATE; required by PNG and by several containers. |
| `ruzstd` | Rust | MIT | T1 | Candidate | Pure-Rust Zstandard decoder for archived project payloads. |
| `zstd` (reference) | C | BSD-3-Clause / GPL-2.0 | T2 | Watch | Needs a libc; revisit only if `ruzstd` is too slow and the C ABI cost is worth it. |
| `zlib` / `zlib-ng` | C | Zlib | T2 | Reject | `miniz_oxide` removes the reason to carry C here. |

## 7. Numerics and DSP primitives

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `libm` | Rust | MIT / Apache-2.0 | T0 | Adopt | See section 1. Determinism across machines is the reason, not just availability. |
| `fixed` | Rust | MIT / Apache-2.0 | T0 | Candidate | Fixed-point paths where exactness beats range, especially before `PHIP-04`. |
| `num-rational` | Rust | MIT / Apache-2.0 | T0 | Reject | The time model is core semantics and is written here with checked arithmetic (R-4.8, R-12.7). |
| `num-traits`, `num-integer` | Rust | MIT / Apache-2.0 | T0 | Candidate | Only if generic numeric code proves genuinely necessary. |
| `glam` | Rust | MIT / Apache-2.0 | T0 | Adopt | Transform maths for the compositor; `no_std` with `libm`. |
| `nalgebra` | Rust | Apache-2.0 | T1 | Reject | Far more linear algebra than a 2D compositor needs. |
| `kurbo` | Rust | MIT / Apache-2.0 | T1 | Candidate | Bézier maths for motion paths, masks, and keyframe curves. |
| `microfft` | Rust | MIT / Apache-2.0 | T0 | Adopt | Fixed-size `no_std` FFT for scopes and audio analysis. |
| `rustfft` | Rust | MIT / Apache-2.0 | T2 | Watch | Needs `std` and wants SIMD; revisit after `PHIP-04`. |
| `realfft` | Rust | MIT | T2 | Watch | Same, and only meaningful with `rustfft`. |
| `KFR` | C++ | GPL-2.0-or-later / commercial | T2 | Reject | A whole DSP framework where sealed leaves are wanted. |

## 8. Containers: demux and mux

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `symphonia` (format crates) | Rust | MPL-2.0 | T1 | **Candidate (leading)** | Pure-Rust ISO-BMFF, MKV, OGG, WAV, AIFF, CAF readers. Its `std` usage must be audited crate by crate before adoption. |
| `mp4parse` | Rust | MPL-2.0 | T1 | Candidate | Mozilla's hardened ISO-BMFF metadata parser; battle-tested against hostile input. |
| `mp4` crate | Rust | MIT | T1 | Watch | Simpler and less proven than the two above. |
| `matroska-demuxer` | Rust | MIT | T1 | Candidate | If MKV support is wanted before `symphonia` is adopted. |
| L-SMASH | C | ISC | T2 | Watch | Small, clean ISO-BMFF muxer; a real option for export if a Rust muxer is not written. |
| GPAC | C | LGPL-2.1-or-later | T2 | Reject | Enormous, hosted, and far past what a muxer needs to be. |
| Bento4 | C++ | GPL-2.0 / commercial | T2 | Reject | GPL-2.0-only in its free form is incompatible with GPL-3.0-only. Verify before reconsidering. |
| libmatroska / libebml | C++ | LGPL-2.1 | T2 | Watch | Only if a Rust MKV muxer proves impractical. |
| FFmpeg `libavformat` | C | LGPL-2.1-or-later | T2 | Reference | Needs a libc, threads, and files. Used on the host to build and check fixtures; never linked. |

## 9. Video decode

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `dav1d` | C + asm | BSD-2-Clause | T2 (`PHIP-04`) | Candidate | The best AV1 decoder there is. Its value is its assembly, so it is worth nothing before SIMD exists. |
| `rav1d` | Rust | BSD-2-Clause | T2 (`PHIP-04`) | Watch | A Rust translation of `dav1d`; watch its maturity, it would remove a C boundary entirely. |
| `libaom` | C | BSD-2-Clause + AOM patent | T2 | Watch | Reference correctness, not production speed. |
| `libvpx` | C | BSD-3-Clause | T2 | Watch | VP8/VP9 ingest for older material. |
| `openh264` | C++ | BSD-2-Clause | T2 | Candidate | Permissive H.264 decode; Cisco's patent position applies to their binaries, not to source built here. |
| `libde265` | C | LGPL-3.0 | T2 | Candidate | HEVC decode; compatible, and smaller than the alternatives. |
| `vvdec` | C++ | BSD-3-Clause-Clear | T2 | Watch | VVC support is outside the current codec scope. |
| `zune-jpeg` | Rust | MIT / Apache-2.0 | T0 | Adopt | Motion-JPEG and still JPEG in safe Rust. A real T0 decoder available today. |
| FFmpeg `libavcodec` | C | LGPL-2.1-or-later | T2 | Reference | Same as `libavformat`: the fixture and conformance oracle, not a runtime dependency. |
| Proprietary camera SDKs (BRAW, R3D, X-OCN) | — | proprietary | T3 | Reject | R-2.5 and R-12.2. Not negotiable. |

## 10. Video encode

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `rav1e` | Rust | BSD-2-Clause | T2 (`PHIP-04`, `PHIP-11`) | **Candidate (leading)** | Pure Rust, deterministic by design, and it removes a C boundary from the export path. |
| `SVT-AV1` | C | BSD-3-Clause-Clear + AOM patent | T2 | Watch | Faster at scale, and much more platform machinery. |
| `x264` | C + asm | GPL-2.0-or-later | T2 | Candidate | H.264 delivery is what the world still asks for. Licence is compatible; the assembly needs `PHIP-04`. |
| `x265` | C++ + asm | GPL-2.0-or-later | T2 | Watch | Same shape, larger, slower to justify. |
| `libaom` (encode) | C | BSD-2-Clause + patent | T2 | Watch | Reference encoder; too slow for delivery. |
| `vvenc` | C++ | BSD-3-Clause-Clear | T2 | Watch | VVC export is outside the current codec scope. |
| `libjpeg-turbo` | C + asm | IJG / BSD-3-Clause / Zlib | T2 | Watch | Only if JPEG export becomes hot enough to leave Rust. |

## 11. Intermediate and mezzanine codecs

The codec an editor actually lives in. Ingest is transcoded to a mezzanine
format once, and everything downstream reads that.

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| FFV1 | — | spec is open; FFmpeg's implementation is LGPL-2.1-or-later | T0/T1 | **Adopt (own implementation)** | Lossless, intra-only, range-coded, fully specified in RFC 9043. Writing this in safe Rust is achievable and gives Media Editor a mezzanine format it owns end to end. |
| Uncompressed planar (`SPRW`) | — | Media Editor's own | T0 | Adopt | The very first format: no entropy coding at all, so the pipeline can be proven before any codec exists. |
| ProRes (FFmpeg implementation) | C | LGPL-2.1-or-later | T2 | Watch | Reverse-engineered, and the name is Apple's trademark. Usable for ingest compatibility, never as Media Editor's own mezzanine. |
| DNxHD / DNxHR (FFmpeg implementation) | C | LGPL-2.1-or-later | T2 | Watch | Same shape as ProRes, with a cleaner specification history. |
| CineForm / SMPTE VC-5 | C | Apache-2.0 (GoPro SDK) | T2 | Candidate | Permissively licensed wavelet intermediate; a genuine alternative to writing FFV1. |
| `exr` (OpenEXR) | Rust | BSD-3-Clause | T1 | Adopt | The right container for high-dynamic-range frame sequences and for golden-frame evidence. |
| `dpx`/`cineon` | — | format is documented | T0 | Candidate | Trivial to read and write, and it is what film scans arrive as. |

## 12. Still images and frame sequences

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `png` | Rust | MIT / Apache-2.0 | T1 | Adopt | With `miniz_oxide`. Also the format the reference captures use. |
| `zune-jpeg` / `zune-image` | Rust | MIT / Apache-2.0 | T1 | Adopt | Fast, safe, actively developed. |
| `jpeg-decoder` | Rust | MIT / Apache-2.0 | T1 | Watch | The older option; evaluate against `zune-jpeg` once. |
| `tiff` | Rust | MIT | T1 | Candidate | Common in post-production deliverables. |
| `exr` | Rust | BSD-3-Clause | T1 | Adopt | See section 11. |
| `image` | Rust | MIT / Apache-2.0 | T1 | Reject | A convenience umbrella that pulls a large tree for formats Media Editor does not want. Depend on the format crates directly. |
| `libspng` | C | BSD-2-Clause | T2 | Reject | The Rust `png` crate removes the need. |
| `libpng` | C | PNG Reference Library License v2 | T2 | Reject | Same. |
| JPEG XL (`libjxl`) | C++ | BSD-3-Clause | T2 | Watch | Interesting for stills; not an editing format. |
| JPEG XL in Rust (`jxl-oxide`) | Rust | **verify** | T1 | Watch | At least one Rust JPEG XL implementation is copyleft in a way that needs checking before it is considered. |

## 13. Camera RAW

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `rawloader` / `rawler` | Rust | LGPL-2.1 | T1 | Candidate | Pure Rust, covers most stills cameras, compatible licence. |
| LibRaw | C++ | LGPL-2.1 / CDDL-1.0 | T2 | Candidate | Take the LGPL half only. The CDDL half is incompatible and must never be the one vendored. |
| dcraw | C | public domain-ish, unclear | T3 | Reject | Provenance too unclear to satisfy R-12.2. |
| Adobe DNG SDK | C++ | proprietary-ish | T3 | Reject | R-12.2. |

## 14. Audio decode and encode

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `symphonia` (codecs) | Rust | MPL-2.0 | T1 | Candidate | FLAC, MP3, AAC-LC, Vorbis, PCM, ADPCM in safe Rust, from one project. |
| `claxon` | Rust | Apache-2.0 | T1 | Candidate | A focused, very well-tested FLAC decoder if `symphonia` is not adopted whole. |
| `hound` | Rust | Apache-2.0 | T1 | Adopt | WAV read and write. The first audio format Media Editor needs, and the simplest. |
| `lewton` | Rust | MIT | T1 | Watch | Vorbis decode; superseded by `symphonia` for most purposes. |
| `libopus` | C | BSD-3-Clause | T2 | Candidate | Opus is the right delivery codec for a GPL project; the reference implementation is excellent. |
| `opus` / `audiopus` bindings | Rust | MIT / Apache-2.0 | T2 | Candidate | Only alongside `libopus`. |
| `libFLAC` | C | BSD-3-Clause | T2 | Watch | Only if a Rust encoder proves inadequate. |
| `flacenc` | Rust | Apache-2.0 | T1 | Candidate | Pure-Rust FLAC encoding for archival exports. |
| LAME | C | LGPL-2.0-or-later | T2 | Watch | MP3 export is a compatibility obligation, not a desire. |
| Fraunhofer FDK AAC | C | FDK licence | T3 | **Reject** | Licence is incompatible with GPL. This is the single most common licensing mistake in media software and Media Editor will not make it. |
| `exhale` (xHE-AAC) | C++ | verify | T2 | Watch | Licence and patent position both need checking before this is even a candidate. |

## 15. Audio DSP

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `dasp` | Rust | MIT | T0 | Adopt | Sample types, frame types, and conversions with no allocator. The vocabulary the mixer is written in. |
| `rubato` | Rust | MIT | T1 | Adopt | Asynchronous and synchronous sample-rate conversion in pure Rust, with a stated filter — which R-4.9 requires. |
| `speexdsp` resampler | C | BSD-3-Clause | T2 | Watch | Small, fast, well understood; a fallback if `rubato` is too slow scalar. |
| `libsamplerate` | C | BSD-2-Clause | T2 | Watch | Same role, larger. |
| `signalsmith-stretch` | C++ | MIT | T2 | **Candidate (leading)** | Header-only, high quality time-stretch and pitch-shift. This is exactly the shape R-3.4 admits C++ for: a sealed leaf with a Rust reference beside it. |
| Rubber Band | C++ | GPL-2.0-or-later / commercial | T2 | Candidate | The best-known open time-stretcher; compatible, but a much larger surface than the leaf above. |
| SoundTouch | C++ | LGPL-2.1 | T2 | Watch | Older, simpler, lower quality. |
| `ebur128` | Rust | MIT | T1 | **Adopt** | EBU R128 loudness. Delivery specifications are written in LUFS; an editor without this cannot deliver. |
| `libebur128` | C | MIT | T2 | Watch | The C original, if the Rust port ever falls behind. |
| `biquad` | Rust | MIT / Apache-2.0 | T0 | Adopt | EQ and filter primitives, `no_std`. |
| `fundsp` | Rust | MIT / Apache-2.0 | T1 | Watch | An elegant graph DSP library; overlaps Media Editor's own audio graph, so probably R-12.7. |
| LADSPA / LV2 hosting (`lilv`) | C | ISC | T2 (`PHIP-09`) | Watch | Third-party audio plugins are a post-1.0 conversation, and they need process isolation first. |

## 16. Colour management

Colour is where an editor earns or loses professional trust.

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| Colour pipeline maths | Rust | Media Editor's own | T0 | **Adopt (own)** | Primaries, transfer functions, matrices, and range handling are small, exactly specifiable, and central. R-8.2 makes them a core type, not a dependency. |
| `palette` | Rust | MIT / Apache-2.0 | T0 | Candidate | `no_std` colour-space types; useful for the interface, probably not for the pipeline. |
| `kolor` | Rust | MIT / Apache-2.0 | T0 | Watch | Similar, smaller, less maintained. |
| `lcms2` (Little CMS) | C | MIT | T2 | Candidate | ICC profile handling for still-image ingest. ICC is not the pipeline; it is an import concern. |
| OpenColorIO | C++ | BSD-3-Clause | T2 | Watch | The industry's configuration standard. Its dependency tree (YAML, expat, pystring) is the problem, not its licence. Reading OCIO configs is the interesting part and can be done natively. |
| ACES transforms (AMPAS) | CTL / data | AMPAS modified BSD | T0 | Candidate | The transforms are specifications and data. Implementing them as matrices and LUTs in Rust is the right adoption. |
| `dcv-color-primitives` | Rust | MIT | T2 (`PHIP-04`) | Watch | YUV↔RGB with SIMD. Value arrives with `PHIP-04`. |
| 3D LUT formats (`.cube`, `.3dl`) | — | documented | T0 | Adopt (own) | A few hundred lines of bounded parsing; a natural fuzz target. |

## 17. Rasterisation, compositing, vector graphics

Phipia has no GPU, so this is a CPU story for the foreseeable future.

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `tiny-skia` | Rust | BSD-3-Clause | T1 | **Adopt (leading)** | A port of Skia's CPU raster pipeline in safe Rust, `no_std` with `alloc`. It is the single highest-value graphics dependency available to this project. |
| `zeno` | Rust | MIT / Apache-2.0 | T0 | Candidate | Very small `no_std` path rasteriser, if `tiny-skia` proves too large. |
| `lyon` | Rust | MIT / Apache-2.0 | T1 | Watch | Tessellation matters for GPU rendering, which does not exist here yet. |
| `embedded-graphics` | Rust | MIT / Apache-2.0 | T0 | **Adopt (early)** | Primitives, text, and framebuffer targets with no allocator. A direct route to an initial native interface. |
| `resvg` / `usvg` | Rust | MPL-2.0 | T1 | Candidate | SVG import for titles and graphics, rendering through `tiny-skia`. |
| `kurbo` | Rust | MIT / Apache-2.0 | T1 | Candidate | See section 7. |
| Skia | C++ | BSD-3-Clause | T2 | Reject | An enormous hosted build system. `tiny-skia` exists precisely to avoid this. |
| Cairo | C | LGPL-2.1 / MPL-1.1 | T2 | Reject | Hosted, and the MPL-1.1 half complicates what should be simple. |
| Blend2D | C++ | Zlib | T2 | Reject | Its speed comes from a runtime JIT. A JIT means writable-executable memory, which Phipia refuses by construction. |
| AGG | C++ | BSD-ish / GPL | T2 | Watch | Unmaintained, but the algorithms are still a good reference. |

## 18. Text: fonts, shaping, layout

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `ttf-parser` | Rust | MIT / Apache-2.0 | T0 | **Adopt** | `no_std`, zero-allocation, and it refuses malformed fonts rather than repairing them. A font file is hostile input (R-11.1). |
| `fontdue` | Rust | MIT / Apache-2.0 | T1 | Candidate | Glyph rasterisation with a small, readable implementation. |
| `ab_glyph` | Rust | Apache-2.0 | T1 | Watch | Similar role. |
| `swash` | Rust | MIT / Apache-2.0 | T1 | Candidate | Shaping and rasterisation together; larger, and very capable. |
| `rustybuzz` | Rust | MIT | T1 | Candidate | A port of HarfBuzz. Required the moment titles must support a script more complex than Latin. |
| `cosmic-text` | Rust | MIT / Apache-2.0 | T2 | Watch | Full layout stack, but it assumes a hosted environment today. |
| HarfBuzz | C++ | MIT | T2 | Watch | Only if `rustybuzz` falls behind. |
| FreeType | C | FTL or GPL-2.0-or-later | T2 | Watch | Both halves are compatible; the Rust stack removes the need. |
| Bitmap fonts (BDF, PSF) | — | per font | T0 | Adopt (early) | Phipia already ships a validated bitmap font path. The first interface reuses that idea rather than a shaping engine. |

## 19. Subtitles and captions

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| SRT, WebVTT | — | documented | T0 | Adopt (own) | Small, bounded, and a natural fuzz target. |
| libass (ASS/SSA) | C | ISC | T2 | Candidate | The reference implementation for styled subtitles; pulls FreeType, FriBidi, and HarfBuzz with it. |
| `subparse` | Rust | MIT / Apache-2.0 | T1 | Watch | Convenience only. |
| CEA-608/708 | — | documented | T1 | Watch | Needed for broadcast delivery, not for editing. |
| IMSC / TTML | — | W3C specifications | T1 | Watch | XML-based; the parser rules in section 11 apply. |

## 20. Editorial interchange

An editor that cannot exchange a cut with another editor is a toy.

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| OpenTimelineIO | C++ / Python | Apache-2.0 | T2 | **Candidate (leading)** | The industry's interchange model, permissively licensed. Start as a host-side converter; a native Rust reader is a later, well-defined project. |
| EDL (CMX 3600) | — | documented | T0 | Adopt (own) | Forty years old, text, and exactly specified. A bounded parser and writer. |
| FCPXML | — | Apple's schema | T1 | Candidate | XML import via `xmlparser`. Read support has real value; write support has real risk of misrepresenting a cut. |
| AAF | C / C++ | **verify** | T2 | Watch | The reference SDK's licence is not obviously GPL-compatible and must be established before any work starts. Ardour's `libaaf` is the more promising route. |
| MXF (`libMXF`, `bmx`) | C++ | BSD-3-Clause | T2 | Candidate | Broadcast delivery wrapping, permissively licensed. |
| XML / AAF from other NLEs | — | — | T1 | Watch | Every one of these is hostile input under R-11. |

## 21. Timecode and broadcast metadata

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| Timecode | Rust | Media Editor's own | T0 | **Adopt (own)** | Drop-frame arithmetic is where editors are wrong most often. It is thirty lines of exact rational maths and a thousand lines of tests, and it belongs here (R-4.8). |
| `vtc` | Rust | MIT | T0 | Reject | See above; R-12.7. |
| SMPTE ST 12-1, ST 2059 | — | standards | — | Reference | Read them; do not depend on anything claiming to implement them for you. |
| BWF / iXML metadata | — | documented | T1 | Candidate | Production audio carries scene and take metadata that a good editor uses. |

## 22. Parallelism and scheduling

Everything here is blocked on `PHIP-10` and `PHIP-11`, and is listed so the job
graph is designed for it now rather than retrofitted later.

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| Media Editor job graph | Rust | own | T0 | Adopt (own) | Deterministic, order-independent, testable serially today and parallel later (R-6.2). |
| `rayon` | Rust | MIT / Apache-2.0 | T2 (`PHIP-10`) | Watch | Work stealing makes reduction order non-deterministic unless the reduction shape is fixed. R-4.4 would have to be enforced above it. |
| `crossbeam-queue` | Rust | MIT / Apache-2.0 | T2 | Candidate | Lock-free queues with `alloc`; the right primitive for ownership transfer between stages. |
| `heapless::spsc` | Rust | MIT / Apache-2.0 | T0 | Adopt | Single-producer, single-consumer queues need no allocator and no locks. |
| `loom` | Rust | MIT | host | Candidate | Exhaustive concurrency model checking, on the host, when concurrency exists. |

## 23. Acceleration

| Component | Language | Licence | Tier | Verdict | Notes |
| --- | --- | --- | --- | --- | --- |
| `wgpu` | Rust | MIT / Apache-2.0 | T2 (`PHIP-16`) | Watch | Needs a driver, a queue, and a memory model Phipia does not have. |
| `naga` | Rust | MIT / Apache-2.0 | T2 | Watch | Only alongside `wgpu`. |
| Vulkan / Mesa | C | MIT | T2 | Watch | Writing a Vulkan driver is a Phipia project, not a Media Editor one. |
| Mesa `llvmpipe` / `lavapipe` | C | MIT | T2 | Reject | It brings LLVM and a runtime JIT into the address space. See Blend2D. |
| Hand-written SIMD kernels | Rust / C++ | own | T2 (`PHIP-04`) | Adopt (later) | The realistic acceleration path: sealed leaves under R-3.4, bit-exact with their Rust references. |

## 24. Testing, fuzzing, and proof

Host-side tools. None of these are linked into a shipping artefact.

| Component | Licence | Verdict | Notes |
| --- | --- | --- | --- |
| `cargo test` / `cargo-nextest` | MIT / Apache-2.0 | Adopt | Nextest for per-test isolation and stable reporting. |
| `proptest` | MIT / Apache-2.0 | **Adopt** | R-9.2's undo/redo property is a property test or it is nothing. |
| `arbitrary` | MIT / Apache-2.0 | Adopt | Structured input generation for fuzzing typed formats. |
| `cargo-fuzz` + `libfuzzer-sys` | MIT / Apache-2.0 / NCSA | **Adopt** | R-11.3 requires a fuzz target per parser. |
| `honggfuzz-rs`, `afl.rs` | Apache-2.0 | Watch | Second engines for the parsers that matter most. |
| `insta` | Apache-2.0 | Candidate | Snapshot review for text-shaped outputs such as EDL. |
| `criterion` | MIT / Apache-2.0 | Adopt | R-3.4.3's 1.3× threshold needs a benchmark that can be believed. |
| `divan` | MIT / Apache-2.0 | Watch | Lighter alternative to `criterion`. |
| `miri` | MIT / Apache-2.0 | **Adopt** | Runs the ABI crate's `unsafe` under an interpreter that catches what review misses. |
| `kani` | Apache-2.0 / MIT | Candidate | Bounded model checking for the arithmetic in the time model. |
| `loom` | MIT | Watch | See section 22. |
| QEMU | GPL-2.0-only (the emulator) | **Adopt (tool)** | Acceptance environment. A tool that is run, never linked — so its licence does not reach Media Editor. |

## 25. Build and supply chain

| Component | Licence | Verdict | Notes |
| --- | --- | --- | --- |
| `rustc`, `cargo` | MIT / Apache-2.0 | Adopt | Pinned in `rust-toolchain.toml`. |
| `cargo vendor` | MIT / Apache-2.0 | **Adopt** | R-12.1: everything in the tree, built offline. |
| `cargo-deny` | MIT / Apache-2.0 | **Adopt** | Licence, advisory, duplicate, and source gates in CI. |
| `cargo-vet` | MIT / Apache-2.0 | Candidate | Records who audited which dependency version. |
| `cargo-geiger` | Apache-2.0 / MIT | Candidate | Counts `unsafe` per dependency, which is R-12.5's budget. |
| `cbindgen` | MPL-2.0 | Candidate | Generates the C header for the ABI crate so both sides cannot drift. |
| `reuse` | Apache-2.0 / GPL-3.0-or-later | Candidate | Checks every file carries an SPDX identifier. |
| GNU `ld`, `objdump`, `nm`, `readelf` | GPL-3.0-or-later | Adopt (tool) | The ELF audit in R-13.4 is these tools. Phipia already depends on them. |
| GCC / Clang | GPL-3.0-or-later / Apache-2.0 with LLVM exception | Adopt (tool) | For the C shims and any C++ leaf. |
| `clang-format`, `clang-tidy` | Apache-2.0 with LLVM exception | Adopt | R-15.1. |
| Python 3 standard library | PSF-2.0 | Adopt (tool) | Fixture and asset builders, matching Phipia's practice. No third-party Python. |

## 26. Reference media

Test footage is a dependency with a licence like any other.

| Source | Licence | Verdict | Notes |
| --- | --- | --- | --- |
| Generated test patterns | Media Editor's own | **Adopt** | Deterministic bars, ramps, sweeps, timecode burn-in, and pathological frames. Reproducible from a script, exactly as Phipia builds its fixtures. |
| Xiph.org test media | CC-BY / public domain, per clip | Adopt | The standard uncompressed sequences the codec world measures against. |
| Blender open movies | CC-BY | Candidate | Real footage with clear provenance, for timeline and playback work. |
| Netflix / AOM test sets | per clip, mostly CC | Watch | Verify each clip individually; the sets are not uniformly licensed. |
| FFmpeg FATE samples | mixed, several unclear | Reject | Provenance is not uniform enough for R-12.2. |
| Anything from the internet without a licence file | — | **Reject** | Including "it is obviously fine". |

## 27. What Media Editor writes itself

Not because writing is fun, but because these are either the product's core
semantics (R-12.7) or things no library can supply to a freestanding target.

| Component | Why it cannot be a dependency |
| --- | --- |
| The Phipia runtime: entry, allocator policy, panic path, syscall shims | There is exactly one operating system this runs on and no library targets it. |
| The C ABI boundary crate | It is the definition of the platform contract. |
| The time model: rational time, timebases, drop-frame timecode | Core semantics, and the place editors are most often wrong. |
| The project model, timeline, and edit operations | This *is* the application. |
| Undo/redo | It is the algebra of the edit model, not a data structure. |
| The project file format | Long-term custody of the user's work (R-9.3, R-9.4). |
| The media cache and frame pool | Bounded, content-keyed, and specific to this pipeline (R-5.4, R-8.5). |
| The render graph and scheduler | Determinism requirements no general scheduler makes (R-4.4, R-6.2). |
| The colour pipeline | Small, exactly specifiable, and central to trust. |
| The audio mixer's control plane | Real-time constraints under R-5.3 that no general library respects. |
| The interface toolkit | No usable toolkit exists for a freestanding target with one kernel-owned framebuffer. |
| Waveform and thumbnail overviews | Derived, cached, and specific to this model. |
| The mezzanine codec (`SPRW`, then FFV1) | Owning the format end to end is the point. |
| Build, fixture, and verification tooling | Following Phipia's practice exactly. |

## 28. Rejected outright

| Component or class | Reason |
| --- | --- |
| Fraunhofer FDK AAC | Licence incompatible with the GPL. |
| Any GPL-2.0-only component | Incompatible with GPL-3.0-only. |
| Any CDDL, EPL, SSPL, or BUSL component | Incompatible. |
| Proprietary camera and codec SDKs | R-2.5, R-12.2. |
| Anything requiring a JIT (Blend2D, `llvmpipe`) | Requires writable-executable memory, which Phipia refuses by construction. |
| Anything requiring dynamic linking | Phipia has no dynamic linker, and R-13.4 forbids the shape. |
| Anything requiring a network at build or run time | R-12.1, and Phipia has no network. |
| An umbrella crate pulling a tree for one feature (`image`) | R-12.3. |
| A library that duplicates the product's core semantics | R-12.7. |
| Telemetry, crash reporting, auto-update, licence checking | The charter's non-goals. |

## 29. Candidate dependency sets

Everything above is a survey. This is the actual shopping list, and it is
short on purpose.

| Area | Dependencies |
| --- | --- |
| **Native interface** | `libm`, `bytemuck`, `arrayvec`, `heapless`, `embedded-graphics`. |
| **Project model** | `slotmap`, `hashbrown`, `rlsf`, `serde`, `postcard`, `blake3`, `sha2`. Host-only: `proptest`, `cargo-fuzz`, `criterion`, `miri`, `cargo-deny`. |
| **Frame pipeline** | `tiny-skia`, `ttf-parser`, `hound`, `dasp`, `lz4_flex`, `miniz_oxide`, `png`. |
| **Editing and export** | `zune-jpeg`, `rubato`, `ebur128`, `biquad`, `microfft`, `exr`. |

Twenty-two libraries to a working cut. Every one of them is safe Rust, every
one is compatible with GPL-3.0-only, and not one of them requires a capability
Phipia is not already asked for in
[`PLATFORM_CONTRACT.md`](PLATFORM_CONTRACT.md).

### What the current implementation uses

The current tree implements the time and project models, edit journal,
generational store, bounded containers, allocator, runtime, SHA-256, project
format, frame types, cache, test patterns, and `SPRW` format without third-party
dependencies.

Candidates in the tables remain candidates until they pass
[`DEPENDENCY_POLICY.md`](DEPENDENCY_POLICY.md). C and C++ libraries must also
meet R-3.4: a Rust reference implementation, bit-exact comparison, measurements,
and an ABI boundary in the designated crate.
