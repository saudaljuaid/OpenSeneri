<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Platform contract

Media Editor runs on Phipia. This document records what Phipia provides today,
what Media Editor proved by experiment, and the numbered capabilities Phipia
must
grow before the *whole* editor can exist on it. It is written against **Phipia
2.1.0** at commit `8fe1817`, read rather than remembered.

Nothing here asks Phipia to become a general-purpose operating system. Each
request is shaped the way Phipia already works: a typed boot-ledger stage, a
measured profile with an allowlist, an installed proof, and a negative control
capable of failing.

### This document was wrong, and the correction is the interesting part

Every version of this document until now was written against **v1.1.0** and
said, in its own first paragraph and in the charter and in the README, that
*Phipia cannot run Media Editor*. That was true when it was written and stopped
being true without anybody here re-reading Phipia.

**Phipia runs Media Editor today.** First Environment — Phipia's graphical shell
—
ships a Media Editor editing workspace beside its Files, Notes and Terminal
apps:
it opens and saves a project on the read-write FAT32 data volume, imports
24-bit BMP frames from it, holds clips on a timeline with a playhead, trims a
selected clip against a one-second minimum, and writes `EXPORT.BMP` back out.
Phipia mirrors this repository's source under `userspace/media-editor/` at an
upstream commit named in its README.

**That mirror needs re-pointing, and this is where it is written down.** Phipia
2.1.0 names upstream commit `70295ebc08a1825452f7c08256aac14270f4cc7b`. This
branch's history was rewritten afterwards — every commit message carried a
trailer that had to come out — so that hash is no longer reachable from
`engineering-foundation`. The same commit, same tree, same author and date, is
now `61ae0e2e36108159abdff8910a9e371cf71674d2`, and its subject is unchanged:
*"app: a project opens when the drive is not mounted"*. A rewrite is cheap
inside one repository and expensive across two, which is worth knowing before
the next one.

So the honest statement is narrower than the one this document made, and it is
the narrow one that is worth keeping: **the Rust freestanding image does not
fit Phipia's Ring 3 process layout.** The workspace that runs today is written
in C inside `src/kernel/ui.c`, where the memory it uses is the kernel's rather
than a program's 76 KiB. The numbered capabilities below are what the *image*
needs, and several of them have since arrived — each is marked with what it is
now, against the source.

A version number in a document is a claim about somebody else's repository, and
this one went eleven releases stale. It is stated at the top so that the next
person to read Phipia can see at a glance what this was checked against.

## What Phipia provides today

Read from the source, not from expectations — `saudaljuaid/phipia` at `8fe1817`:

| Facility | State in 2.1.0 | Source |
| --- | --- | --- |
| Ring 3 execution | Private address spaces and checked ELF64 loading, at fixed per-profile page layouts | `src/kernel/process.c`, `include/phipia/paging.h` |
| Application ABI | No *generally stable* native userspace ABI. An experimental versioned native boundary exists for networking — ABI version 1, authenticated process generation, checked user ranges, bounded transfers — beside a Linux compatibility boundary admitting measured `echo`, `uname` and interactive `cat` | `include/phipia/network_syscall.h`, `docs/LINUX_SYSCALL_ABI.md` |
| Storage | **FAT32** on emulated NVMe: a read-only system volume and a separate **read-write** data volume, with nested directories, file growth and truncation, random access, rename, deletion and clean persistence | `src/kernel/fat32_fs.c` |
| Memory to a program | 12 image pages, 4 stack, 2 heap, 1 anonymous — **76 KiB**, at the widest layout any profile is admitted at. `PAGING_PROCESS_EXPECTED_MAX_PAGES` is 24, which is 96 KiB | `include/phipia/paging.h` |
| Framebuffer | Kernel-owned, mapped write-combining, presented through a cached surface with damage tracking | `src/kernel/framebuffer.c`, `src/kernel/surface.c` |
| Input | Kernel-owned PS/2 keyboard and three-byte pointer, consumed by First Environment | `src/kernel/keyboard.c`, `src/kernel/pointer.c` |
| Time | `clock_monotonic_ns()` from a calibrated TSC, cross-checked against the ACPI PM timer — and now reachable from a program, through `NETWORK_SYSCALL_QUERY_TIME` | `src/kernel/clock.c`, `include/phipia/network_syscall.h` |
| Entropy | Bounded random bytes to a program, through `NETWORK_SYSCALL_RANDOM` | `include/phipia/network_syscall.h` |
| Threads | Kernel threads with guarded stacks and preemption, capacity 8; no userspace threads | `include/phipia/thread.h` |
| Cores | One. Phipia states plainly that it has no SMP | `docs/ARCHITECTURE.md` |
| Audio | None. No driver, no device, no mixer, no clock domain | — |
| GPU | None. No accelerator of any kind | — |
| Networking | virtio-net with Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP and HTTP/1.1 — and Media Editor still wants none of it | `src/kernel/virtio_net.c` |
| Floating point and SIMD | Not enabled and not preserved. Re-checked at 2.1.0 and unchanged. See below | measured |

### The floating-point finding

**Re-checked against 2.1.0 and unchanged**, which is worth saying because
everything around it in this document moved. `OSFXSR`, `OSXSAVE`, `fxsave` and
`xsave` appear nowhere in Phipia's source or headers, and the kernel is still
built `-mno-sse -mno-sse2 -msoft-float -fno-tree-vectorize`.

Phipia never sets `CR4.OSFXSR` or `CR4.OSXSAVE`, never executes `fxsave`,
`xsave`, or any of their partners, and saves no x87 or SSE register state at
interrupt entry, at the syscall boundary, or across a context switch. Its build
rejects any floating-point, MMX, SSE, or AVX instruction found in the kernel
image, and its BusyBox profiles are audited for exercised SIMD instructions and
refused if any appear.

The consequence for an editor is not small. Video and audio work is arithmetic,
and every unit of that arithmetic must currently be scalar and software-float.
Media Editor's first milestones are therefore written to be correct rather than
fast, and the capability that changes this — `PHIP-04` below — is the single
highest-value item Phipia can add for Media Editor's sake.

### The memory finding

A Phipia program is mapped 76 KiB of address space today, at the widest layout
any profile is admitted at; the constant that bounds a process is 24 pages, or
96 KiB. One 1920×1080
frame in 8-bit RGBA is 8,294,400 bytes: about 106 times that entire envelope.
One 4096×2160 frame is 33.75 MiB, about 455 times it. A one-second cache of HD
frames at 24 fps is 190 MiB.

An editor is a memory system with a user interface attached. `PHIP-03` and
`PHIP-12` are consequently as load-bearing as the ABI itself.

## What Media Editor proved by experiment

These are measured results from this repository's toolchain investigation, not
predictions. They fix the shape of the first native program.

**The stock bare-metal Rust target cannot be used.** `x86_64-unknown-none`
declares `position-independent-executables: true` and
`static-position-independent-executables: true`, and a freestanding binary
built for it links as `Type: DYN (Position-Independent Executable file)`.
Phipia's ELF validation refuses interpreter, dynamic, relocation, PIE,
executable-stack, and W+X shapes. A stock-target binary is refused before it is
ever mapped.

**A conforming shape is reachable on the stable toolchain.** Building for
`x86_64-unknown-none` with `-C relocation-model=static -C code-model=large`,
linking with GNU `ld` through `-no-pie`, a fixed-address script, and
`--orphan-handling=error`, produces:

```text
Type:                              EXEC (Executable file)
Entry point address:               0x400001000000
LOAD 0x0000400001000000 R
LOAD 0x0000400001000000 R E
LOAD                    RW
```

with no dynamic section, no relocation records, no undefined symbols, an empty
global offset table, and no MMX, SSE, or AVX instruction in the image. That is
exactly the shape `src/rust/linux_elf64.rs` accepts, and exactly the shape a
new native profile would be measured against.

**The large code model renames sections.** With `-C code-model=large` the
compiler emits `.ltext`, `.lrodata`, `.ldata`, and `.lbss`. A linker script
that names only the ordinary spellings silently drops executable code into a
read-only segment. `--orphan-handling=error` is what catches it — the same
lesson Phipia learned when a Rust static library first opened a gap between
data and bss.

**GNU `ld` is the supported linker.** `ld.lld` refuses `--orphan-handling=error`
unless `.symtab`, `.strtab`, and `.shstrtab` are named by the script. Phipia
links with GNU `ld`; Media Editor does the same rather than maintaining two
scripts.

**The image base is forced, and it forces everything else.** Phipia's
`PHIPIA_EARLY_PHYSICAL_LIMIT` is `0x100000000`: the kernel identity-maps the
whole low 4 GiB, supervisor-only, in every address space. A user image cannot
live below that line, so it lives at `0x0000400001000000` — about 70 TiB —
where no address fits a 32-bit displacement. Three consequences follow, and
none of them is a preference:

- the large code model is required, because the default `kernel` model emits
  32-bit sign-extended references that cannot reach the image;
- link-time optimisation cannot be used against the precompiled `core` and
  `alloc`, which were built with a different code model: the bitcode merge
  fails outright;
- a small global offset table is unavoidable. The precompiled standard library
  is position-independent, and at this distance the linker cannot relax its
  GOT-relative accesses into direct ones. The table is 112 bytes, resolved
  completely at link time, and the audit proves the image still has no dynamic
  section and no relocation record.

The alternative to all three is rebuilding the standard library from source on
a nightly toolchain. Media Editor does not, for the same reason Phipia pins a
stable compiler: a toolchain requirement is a promise to everyone who ever
builds the thing.

**The image does not fit Phipia's Ring 3 layout.** The current build is 111
pages, 444 KiB in total, against the 76 KiB a program is mapped at the widest
admitted profile — and that is an image with no picture and no interface, whose
frames are sixteen pixels wide because that is what fits.

This is the sentence that used to read *"the image already exceeds what Phipia
can map"*, and the difference matters. Phipia maps plenty; what it does not yet
do is admit an arbitrary Ring 3 image at a page count of its own choosing. The
workspace in First Environment runs in the kernel, where this ceiling does not
apply. `PHIP-02` and `PHIP-03` are what the *image* needs, and until they exist
the first thing they unblock is not video; it is the program itself.

That number needed taking apart, and taking it apart changed what it means.
`make audit` now runs `tools/footprint.py`, which splits the footprint by
section and attributes every sized symbol to the crate that emitted it:

| Section | Pages | Share |
| --- | --- | --- |
| `.text` | 82.0 | 77.4% |
| `.rodata` | 7.0 | 6.6% |
| `.bss` | 17.0 | 16.0% |

Re-measured at this commit rather than carried forward. An earlier version of
this table had been left standing while the growth history below moved on
under it, which is the exact failure the history exists to prevent — a number
in a document is only evidence of what it was measured on.

**And it happened again, worse, for a reason the tool was hiding.** The tables
here went stale over several milestones, and could not have been refreshed:
`tools/footprint.py` read Rust's *legacy* mangling, the toolchain emits **v0**,
and so every symbol in the image fell into one bucket called `(unmangled)`
while the line underneath went on printing "attributed in total". A blind
reader still produces a table. It just has one row in it.

The reader now understands both manglings, refuses when more than five per
cent of the sized symbols carry a name it cannot parse, and checks itself
against five real symbols before it reads anything at all. The measured
unreadable share is **0.25%** — 965 bytes, every one a compiler intrinsic with
a C name (`memcmp`, `memset`, `__udivti3`), which carry no crate and never
will. The same 965 bytes it was when the reader learned v0: the share moves
because the image around it does.

**Seventeen of those hundred and eleven pages are one constant.**
`media_editor_rt::HEAP` is `HEAP_BYTES`, sixty-four kibibytes of static arena,
and it is a *reservation* rather than anything the program contains. Reading
the total as "the program has grown" — which every earlier entry in the table
below did — was wrong in a way that mattered: `.bss` has been seventeen pages
throughout, so **ninety-four** of the current hundred and eleven are code and
read-only data and every page the image has ever gained is one of those. The
arena never moved at all.

The rest, by crate, read from the mangling:

| Pages | Crate |
| --- | --- |
| 26.6 | `media-editor-model` |
| 23.4 | `media-editor-render` |
| 16.1 | `media-editor-rt` — the arena, almost entirely |
| 11.3 | `media-editor-io` |
| 6.0 | `media-editor-app` |
| 5.7 | `core` |
| 2.9 | `media-editor-core` |
| 2.6 | `media-editor-media` |
| 1.8 | `alloc` |
| 0.5 | everything else — `compiler_builtins`, the compiler's own shim crate, and the C-named intrinsics |
| 0.1 | `media-editor-abi` |

**The model is now the largest crate in the image**, and it took one milestone
to do it: 108,765 bytes against the renderer's 95,879. Two milestones ago the
two were a hundred and twenty-four bytes apart, which was worth writing down at
the time precisely because a crossing was one change away; the gap is now
nearly thirteen thousand.

The renderer has not shrunk — it has not moved by a byte since nesting. What
has happened is that eight of the last nine milestones were the *model*
learning to say something, and the renderer already knew how to draw it. A
ramp is the clearest case in the table: it changes which frame every clip
shows, and it cost `media-editor-render` nothing at all.

`media_editor_io::format::encode` is the largest single function at **13,988
bytes**, and `Edit::apply` is now within five hundred of it at 13,454 — the two
have swapped places twice in this document's life and are about to again. Then
`format::read_body` at 9,193, `timeline::plan` at 7,115, `Graph::evaluate` at
6,567, `Item::split` at 6,242, `Sequence::stack_at` at 5,989, `Project::apply`
at 5,850 and `Lut3D::look_up` at 5,232. `Item::split` was not on this list at
all two milestones ago.
`Face::stencil`, which held this list at 23,807 bytes, does not
appear on it at all any more — the face moved into read-only data and the
number went with it, which is the change the entry below records and which
this table had never been updated to show.

Some pages are not attributed at all — 9.2 of them: padding, literals, and
anything the symbol table does not carry a size for.

**The largest-function title has changed hands three times, and each move was
structural.** `Face::stencil` took it at
23,807 bytes, and the reason was structural rather than careless: the face was
a table of seventy-one glyphs written as literal coordinates *inside a
function*, so the compiler emitted every coordinate as an instruction that
stores it. Twenty-six lowercase glyphs had cost 7,888 bytes — a little over
three hundred each, for four to six rectangles of eight small integers.

As a `static` the table lives in `.rodata` at roughly its own size. `.rodata`
went from five pages to six and `.text` lost five, so the image went from 91
pages to **87**: four back, for a change that moved no coordinate.

That last clause is the part worth having a test for, and it already existed.
The specimen came out **byte for byte identical**, which is what makes this a
change to how the face is written rather than to what it says — and it is the
reason a face is kept as a picture at all.

`Edit::apply` is worth naming twice, and its history is the clearest thing in
this document about how little a single number says. It was 6,525 bytes when
this table was first written; it grew to 17,597 by M8.22, because it is a
match over every edit the model has and *every* feature that adds an edit adds
an arm to one function; and it is **8,003 today**, having lost more than half
its size in a single commit that only added a field.

That fall is not an optimisation anybody wrote. Past a certain `Clip` size the
compiler stops emitting an inline copy of a clip in each of that function's
arms and calls out instead — and the work does not vanish, it moves: the seven
helpers `apply` now calls took 7,381 of the 9,599 bytes straight back. The
title returned to `format::encode`, which has held it at 10,970 since.

The lesson the paragraph originally carried still holds and is now the smaller
half of it: a match over every edit is a function that grows with the feature
list, and it is a place to look when the image has to come down. The larger
half is that **its size is not a property of the code alone**, and neither is
any other single figure in this document.

**The heap is eighty-four per cent of what a Phipia program is given, on its
own.** That reframes the problem: the largest single question about this
image's size is not which crates it links, it is how much arena to reserve —
and that number was chosen before anything measured what the program uses.

How much that lever is worth is measured rather than estimated: halving
`HEAP_BYTES` to thirty-two kibibytes took the image from sixty-one pages to
**fifty-three** when that was measured, and `.text` did not move by a byte.
(Measured at that size rather than carried forward from the forty-two-page
image where it was first measured: what a change costs has no answer without
saying in which program.) Nothing here proposes
doing that — the arena is sized for a program nobody has run — but it says
exactly what the arena costs, which is the first thing anyone deciding will
want.

The use is not measurable today, and that is worth saying plainly rather than
guessing at. `BumpHeap` tracks a high-water mark, but `media-editor-rt` is
linked
only into the freestanding image, and that image has no Ring 3 path to run on
until `PHIP-01` and `PHIP-02` exist as a general facility. So the arena's actual
use is one of the things the first QEMU run of the *image* will report, and it
is on that run's list — the workspace that runs in First Environment today does
not use this arena, because it is not this program.

The growth history, kept because a footprint that moves without anyone noticing
is how a program stops fitting:

| Pages | What was added |
| --- | --- |
| 36 | the slate, the model, the reel, the mixdown |
| 38 | *unattributed* — see below |
| 40 | keyframed parameter curves, and the track opacity that reads them |
| 39 | *down* one, while gaining the fader ramp and sound automation |
| 42 | per-keyframe editing, which `Edit::apply` reaches and so the image links |
| 43 | a grade on a clip: the field, the edit, and the format that carries it |
| 43 | the grade's render node and its wiring: no growth at all, because nothing reached it |
| 60 | the slate rendering a picture, which links `media-editor-render` for the first time |
| 61 | conforming a sequence to an edit decision list — and **none of it is the module** |
| 63 | wipes: the shape rasteriser, reached this time, and all of it `.text` |
| 65 | soft edges: the moment arithmetic, reached for the same reason |
| 69 | masks: the shape on a clip, its edit, its format, and its rasterisation |
| 70 | one asset per digest, and a location hint beside it |
| 70 | offline media: the slate and the planner's question, for **no growth** |
| 75 | resampling and the transform that uses it, reached through the graph |
| 78 | motion: a framing that animates, and the re-basing a cut through one needs |
| 80 | rolling a cut and sliding an item, which `Edit::apply` reaches and so the image links |
| 80 | a face written from scratch, for **no growth** — nothing in the image sets type |
| 87 | the slate naming its media — **seven pages**, and all of it the face being *reached* |
| 89 | titles as media: the model, the format, the node, and the planner that chooses it |
| 91 | lowercase — twenty-six glyphs, and `Face::stencil` becomes the largest function in the image |
| 87 | the face moved into read-only data: **four pages back**, and the specimen byte for byte identical |
| 88 | a card of several lines, aligned — one page, all of it the block layout |
| 90 | a fade on a clip, and the transfer table the corrected compositor now builds twice |
| 91 | retiming: an exact rational speed on a clip, and the mapping every frame goes through |
| 92 | a title's colour, named in light and encoded through the frame's own table |
| 93 | the span of media a clip reads, which the library check now asks it for |
| 93 | a freeze, for **no growth**: a third value for a tag that already existed |
| 91 | an opacity a clip animates — **two pages back**, and not for the reason it looks like |
| 93 | a mask a clip animates: two pages back again, and for the mirror of that reason |
| 94 | an animated grade — one page on, while the largest function in the image *fell* 9,599 bytes |
| 94 | an exact turn on a mask and a framing, for **no growth at all**: 143 bytes net, inside `.text`'s own page padding |
| 95 | the point a framing acts about — 816 bytes, which is one page because the padding the last change fitted inside was spent |
| 96 | a razor and a merge across every track at once — 5,527 bytes, a third of it `Edit::apply`'s two new arms |
| 96 | a lift, and the drop that undoes it — 1,512 bytes, inside the page padding again |
| 98 | markers — 5,389 bytes, a third of it `Edit::apply`'s three new arms and a third the format's two halves |
| 102 | nested sequences — 16,384 bytes and the first four-page milestone since the slate learned to name its media |
| 103 | a speed that changes over a clip — 4,096 bytes, and `Playback` losing the niche that made a freeze free |
| 106 | a note that travels with the shot — 12,288 bytes, three new arms in `Edit::apply` and the first join that merges |
| 106 | the timeline scanned a row at a time — **the sixth flat milestone running**, and the first checked to the byte rather than to the page: every loadable section identical, only DWARF moved. The five rows this one does not have are the five before it, which also moved nothing |
| 106 | the export written a row at a time, and `SPRW` version three — **the seventh flat**, and the first where the total held still while 922 bytes moved *inside* it: `sprw::header` +826 against `encode` −773, `MemoryStorage::append` +406, a new panic path +200, `decode` +98 for the trailer arithmetic, and `IoStatus`'s formatter +87 for five new variants. `.text` is 335,872 both sides, so all of it fits in the page padding |
| 108 | sound in the reel and in the export — **two pages, and the run of seven flat milestones ends here**: +8,192 bytes, all `.text`, of which the symbols account for +6,158 and the rest is padding. `unpacked_sound` +994, `AudioBuffer::new` +817 — *newly reached*, because the slate decodes a reel and a decoder that can carry sound must be able to build a buffer even for a reel with none — `Reel::with_sound` +746, `decode` +661, `encode` +582, `packed_sound` +544, `read_header` +364, `header` +247, and `Reel`'s equality +144 for comparing it |
| 109 | captions anchored in the source, and a vault that serves sound — **one page**, +4,096 bytes, all `.text`, of which the symbols account for +3,640. `format::read_captions` +1,005, `MediaAsset::with_captions` +744, `MediaAsset`'s `clone` +551 now that an asset owns a vector, `encode` +490, `Caption::new` +391, its drop glue +250, `decode_payload` +186 — against `Project::add_media` **−518** and `refresh_nests` **−459**, which is the inlining threshold M8.21 first recorded firing again on a struct that grew. **The projection itself costs nothing**: nothing in the image reaches `offsets_showing` or `captions_over`, so the page is the *data* and not the feature |
| 110 | the transcript in the reel — **one page**, +4,096 bytes, all `.text`, symbols +4,014. `packed_captions` +1,031 and `unpacked_captions` +969 are the codec; `Reel`'s equality +904 against −819 and `with_sound` +685 against −746 are the same functions renumbered as the struct gained a field; `Reel::with_captions` +501. The image decodes a reel, so it links the codec whether or not any reel it reads says anything |
| 110 | the transcript read a window at a time — **no growth, and 101 bytes back**: `unpacked_captions` −64 for losing a bound that was not its, `caption::checked` −16 for losing a rule that was not about captions, `decode` −14, `with_captions` −7. The windowed reader itself contributes nothing, because the image decodes reels and does not spool them |
| 110 | row forms for every generator — **no page, +419 bytes**, and all of it is one rasteriser serving two shapes: `carded` +2,802 against `typeset` −2,377, `render_rows` +1,361 against `render` −1,372 (which is now a 21-byte delegator), `plane_rows` +785 against `plane` −736. Drawing a row costs 419 bytes because it is the same code drawing fewer rows |
| 110 | captions burned into the picture — **not one byte, and not one symbol**: 545 symbols before and 545 after, every one of them the same size, no name added and none removed. It is the strongest flat result the table holds, and the reason is that `burn` is a *step* rather than a property: it composites a card the graph already knew how to draw, over a picture the graph already knew how to make, so it adds no arm, no node kind and no rule. The slate never calls it, so the image never links it. What did move is presentation only — 10,772 of 28,672 `.rodata` bytes and 14,514 of 352,256 `.text` bytes, all of it pointers into a string pool the linker re-packed in a different order when the app crate's object changed. The set of strings is identical; only where each one sits moved |
| 110 | the banded transform — **no page, and the milestone's own code costs nothing**: 110 both sides, `.text` 352,256 both sides. `band`, `rows_under`, `resample_row`, `Graph::banded` and `Graph::gathered` appear in none of the 543 symbols, because the slate renders whole frames and never scans. What moved is the arithmetic underneath, now shared so that a band and the pixels drawn inside it cannot disagree: `bilinear_at` −2,055 and `area_at` −1,005 folded into `drawn` +1,875, against `landing_of` +693, `bounds` +648 — newly a function rather than inlined copies — and `preimage_of` +631; `Picture::at` +105 for the band check, while `Picture::new` −107, `holds` −163 and `clamped` −278 inlined away entirely. Attributed bytes **+234**, every one of them absorbed by page padding, and two symbols *fewer* after a milestone that added five functions |
| 111 | a frame built from a window — **one page, for sixty-four bytes**, and the arithmetic is worth writing down because the page is not the cost. `.rodata`'s content went from 28,624 bytes to 28,688: the global offset table is 144 bytes on both sides and did not move, so the 64 are literals and panic locations from two new functions. Seven pages is 28,672, so the content had been sitting **48 bytes below** a page boundary and now sits **16 bytes above** it — and a section the linker script pads to a page rounds sixteen bytes up to four thousand and ninety-six. `.text` is 352,256 both sides, attributed bytes +71: `Frame::from_owned` +374 and `Frame::packed` +176 against `Look::apply` −159, `slate::picture` −146, `masked` −81, `resample` −78, `render_rows` −62 and `premultiply` −61, which are the callers that stopped copying. **The image is not what this milestone is about**: what it buys is at run time, where a 1,920-wide row through `composite::over` used to have three buffers of 7,680 bytes alive at once and now has one |
| 111 | a caption sidecar — **no page, and the page before it paid for this one**. 111 both sides, `.text` 352,256 both sides. `.rodata`'s content went 28,696 → 28,888, +192 bytes, every one of them absorbed by the page M8.45 opened — which had 3,880 bytes of headroom and now has 3,688. **Not one symbol of `vtt` is in the image**: the slate exports reels and does not spot sidecars. Attributed +92, of which `IoStatus`'s derived `Debug` is +66 for five new variants, `slate::picture` +19 and `timeline::plan` +7. The 192 bytes are the five variant *names* — 73 bytes of text — and their entries in the formatter's table. **The five refusal sentences cost nothing at all**, because `IoStatus::describe` is in none of the 545 symbols: the image never formats a refusal, it returns one |
| 111 | the span burn — **fifteen bytes, and one of them is a comma**. 111 both sides, `.text` 352,256 both sides, 545 symbols both sides, and exactly *one* of them moved: `ModelStatus`'s derived `Debug`, +15 for one new variant. `.rodata` +16 bytes for that variant's name. Nothing else in the image changed by a byte, because `Reading` is in none of the 545 symbols — the slate renders a frame at a time and never projects a span. The cheapest milestone the table holds, and the one with the largest saving behind it: on a 24,000-frame reel of a twenty-clip sequence the projection goes from thirty million questions to a thousand |
| 111 | a turn scanned in strips — **the image got smaller**, which is the first time a milestone that added a capability has. 111 both sides, `.text` 352,256 both sides, 545 symbols both sides; attributed bytes **−82** and `.rodata` −8. Nothing about strips is in the image — `strip`, `rows_under`, `resample_strip` and `Graph::banded` are in none of the 545 symbols — so every byte of the change is in the shared arithmetic underneath: `preimage_of` −54 for taking a range of columns instead of building each corner from `x + dx`, `drawn` −39 for taking a strip instead of a width and a row, `resample` −27, against `area_at` +38 for passing `x, x + 1`. `Mapping::horizontal` was deleted and cost nothing, because the image never linked it |
| 111 | tiles — **no page**, 111 both sides and `.text` 352,256 both sides; `.rodata` +40 and attributed +277, all absorbed by the page M8.45 opened, whose headroom is now 3,808 bytes. Not one tile function carries a name in the image — `tile`, `rows_under`, `resample_tile`, `Graph::rows`, `Graph::banded` and `rows_of` are in none of the 547 symbols, inlined or unreached. The two symbols that are new are the shapes the change needs rather than the change itself: drop glue for `Vec<Frame>` +255, which the export's row-splitting builds, and `RawVec<Vec<u8>>::grow_one` +110 for the buffer-a-row a tile writes into. Against them `Reel`'s drop glue −207 and `preimage_of` −45 for taking a rectangle instead of a row of columns; `resample` +110 and `area_at` +38 for passing one. **What it bought is not here at all**: sixteen rows of a 160-wide turn read 124 source rows together against 1,594 apart |

**A total that moves has a location.** M8.23 took `Clip` from 416 bytes to 440
and the image from 93 pages to 94 — and `Edit::apply`, the largest function in
the image, **fell 9,599 bytes** over the same commit. The threshold effect
M8.21 first recorded fired again: past a certain size the optimiser stops
emitting an inline copy of a clip in each of that function's many arms.

What no earlier entry had done is ask where the bytes went. They went into the
functions it now calls: `refade` +1,257, `remotion` +1,245, `reshape` +1,194,
`slip` +1,077, `regrade` +1,009, `remove` +962 and `retime` +637 — **7,381** of
the 9,599, straight back. The optimiser did not delete the work when it stopped
inlining it; it relocated it.

So a footprint that falls when a struct grows is a relocation rather than a
saving, and the entries below that read such a fall as one were reading a total
where a location was the fact. The tool has printed the largest symbols since
`make audit` learned to; subtracting two of its runs is all this took.

**Two pages back, from a milestone that only added things.** M8.21 put an
opacity curve on a clip, an edit to set it, two functions and a lane in the
file — and the image *fell* from 93 pages to 91. `media-editor-model` lost 7,376
bytes and `Edit::apply` alone lost 3,447 of them.

Nothing in the diff explains that, so it was tested rather than explained. On
the **previous** commit, with none of M8.21's code, a dummy `[u64; 3]` added to
`Clip` — twenty-four bytes of nothing, exactly what the opacity field costs —
takes the image to **91 pages** and `Edit::apply` to 16,476 bytes. The saving
was bought by the clip crossing 320 bytes, not by anything the milestone added:
past that size the optimiser stops emitting an inline copy of a clip in each of
`Edit::apply`'s many arms and calls out instead.

So a struct getting *bigger* made the program smaller. The seventy-six
kibibytes are the program's, not the struct's, and this is the clearest case
yet that the two are not the same measurement — and that a page count credited
to the change that happened to be in flight is a page count credited to the
wrong thing.

And the very next milestone put both pages back. M8.22 gave a clip a second
`Option<Motion>` — 72 bytes, three lanes of curve — taking `Clip` from 344 to
**416**, and the image returned to 93 pages. So the relationship is not
monotone either: 320 → 344 saved two pages and 344 → 416 spent them. There is
one threshold effect at one size, not a trend to lean on, and the only way to
know which side of it a change lands on is to build the image and look.

**The same pair again, at seven times the size.** The face was written in one
commit and cost **nothing** — no symbol of it appeared in the image, because
nothing in the image set type. One commit later the planner names a `Legend`
on an offline slate, `Graph::evaluate` calls `font::caption`, and the image
went from 80 pages to **87**. `Face::stencil` alone is 15,919 bytes: it is a
table of forty-five glyphs built with literal coordinates, and the compiler
emits every one of them as code.

That is nine per cent of a Phipia program's whole address space spent on being
able to write eight characters on a slate, and it is the clearest case yet for
the split this section keeps arriving at. The face is exactly the kind of thing
an editing program loads *when it needs it* — and the image cannot, today,
because there is no `PHIP-03` to load it with.

**And the converse, one commit later.** Wipes cost two pages, every byte of
them `.text`, and `media-editor-render` went from 11.2 pages to 13.0. The
difference from the entry above is not the size of the code — the shape
rasteriser is smaller than the conform module — it is that `Graph::evaluate`
*calls* it, and the slate calls the graph. The same two facts, measured twice
in opposite directions, are one fact: **the image is what something in it
reaches.**

**A module the image never calls costs it nothing, and that was measured, not
assumed.** Conforming added a module to `media-editor-io`, and the image grew by
exactly one page — all of it `.rodata`, none of it `.text`. Removing the module
and rebuilding gives a byte-identical image; removing the twelve refusal
strings it added to `IoStatus::describe` is what takes the image back to sixty
pages. `media_editor_io`'s attributed code did not move by a byte, in either
direction.

So the earlier note that `--gc-sections` changes nothing was right and for a
better reason than it gave: unreached code in an rlib is never pulled in, so
section collection has nothing left to collect. What the image links is what
something in it *calls* — which is why the grade's render node cost nothing
until the slate reached it, and why `media-editor-render` cost seventeen pages
the
day it did.

The corollary is the uncomfortable one: a refusal *string* is not free. Twelve
of them is a page, and this project writes one line of prose for every way each
format can be refused. That is a deliberate trade — R-7.3 says a reader that
cannot name what was wrong cannot be trusted to have checked the others — but
it is now a trade with a number on it.

**The image did not render until it did, and that was worth seventeen pages.**

For most of this branch's life `media-editor-render` had no symbol in the image
at
all — not the graph, not the compositor, not the colour pipeline, not the
lookup tables. The slate exercised the model, the reel, the frame pool and the
test patterns and never called `timeline::render`, so the half of the project
that renders was untested on the target and absent from every footprint
recorded here.

The slate renders now, and `.text` went from 22 pages to 39.

That is the honest number for a program that does what this one is for, and it
is three times the budget rather than twice it. It also settles two claims
written here, both of them mine and both wrong at the time.

The thirty-eight-page row used to say the growth was "the timeline rendering
through the graph, which reaches every node kind and so links the whole colour
pipeline". It cannot have been: linking the graph costs seventeen pages, not
two, and the crate was not linked at all. The row says unattributed now.

And `Node::Look` was deferred for a commit on the reasoning that it would cost
about two pages. In the image as it then stood it cost *nothing*, because
nothing reached it. In the image as it stands now, `Lut3D::look_up` is 5,230
bytes and is linked whether or not anything grades — which is about a page and
a third, and near enough to the original guess.

Both versions of that sentence were wrong, and in the same way: **"what does
this cost" has no answer without "in which program".** The estimate was not
wrong about the code. It was wrong about whether the code was reached, and
nothing but the symbol table could have said.

The row that goes down is the useful one. A whole feature went in — a ramping
fader, a second automation lane, the model and the format behind them — and the
image got *smaller*, because two long functions were split and one duplicated
body became a shared step. Growth is not proportional to features; it is
proportional to distinct code the program reaches, and the same pass that adds
a feature can pay for it by removing repetition.

That is not an argument for relying on it. It is an argument for measuring
after every change rather than assuming the direction, which is now what
`make audit` does.

**The trajectory is the thing to watch, not any single row.** The two real
answers stay what they were. `PHIP-03` is the one that fixes it. Splitting the
program so the freestanding image links only what it starts with is the one
that does not need Phipia — and the breakdown above is what that decision has
been missing, since it says which crates are actually worth splitting off and
that the arena is a larger question than any of them.

The waveform summary, the peak file and the lookup tables cost nothing, which
is worth knowing: unreferenced code is not pulled out of an `rlib`, so a module
the freestanding image never calls does not reach the image. What grows the
footprint is code the program *reaches*.

`--gc-sections` was tried and changes nothing: the linker script places
sections explicitly, so there is nothing for the collector to decide. Getting
this number down means either splitting the program so that the freestanding
image links less of it, or `PHIP-03`. The second is the answer; the first is
what to do if `PHIP-03` is slow.

Two tools read the image independently and agree on the total: `elf-audit.py`
sums the loadable *segments*, `footprint.py` sums the loaded *sections*. They
arrive at 172,032 bytes by different routes, which is a cross-check nobody had
to write.

These are not predictions. `make image` produces the artefact, `make audit`
checks it against R-13.4 and R-13.6, `tools/audit-control.py` proves the audit
can refuse by mutating the image two ways and requiring both to fail, and
`make reproducible` builds it twice into different directories and compares the
bytes.

## The capability ladder

Each item is a request to Phipia, in dependency order. Priority is what it
blocks, not how hard it is. "Measured shape" is the narrowest version that
unblocks the milestone — deliberately smaller than a general facility, because
that is how Phipia grows.

**Five of these have moved since the list was written**, and each says so under
its own heading rather than being quietly deleted: `PHIP-01` and `PHIP-02` in
part, `PHIP-05`, `PHIP-08` and `PHIP-14` in full. A request that has been
answered
is worth keeping with its answer beside it, because the shape Phipia actually
built is more informative than the shape this document asked for.

### PHIP-01 — Native application ABI

*Blocks: everything. Priority: first.* **Part of this exists at 2.1.0.**

Phipia has an experimental native syscall boundary of exactly the shape asked
for below — its own entry, its own versioned request and response records, its
own status enum, authenticated process generations, checked user ranges,
bounded transfers and timeouts — and it is not the Linux boundary. What it
carries is networking, time and entropy rather than an application ABI, and
Phipia states plainly that it has no *generally stable* native userspace ABI.
So the pattern is proven and the surface is not general yet, which is a much
better position than this document assumed.

A native Phipia syscall surface distinct from the Linux compatibility boundary:
its own entry, its own numbering, its own errno space, its own allowlist, its
own installed proof. The Linux boundary is a measured compatibility artefact
and must not become Media Editor's ABI — widening it to fit an application would
destroy the property that makes it trustworthy.

Measured shape: a syscall entry that authenticates process, generation, CR3,
CPL3 entry, and executable range exactly as `linux_syscall.c` does, with an
initial allowlist of `exit`, `write_console`, and `monotonic_ns`.

### PHIP-02 — Loading an application image that is not a pinned fixture

*Blocks: every milestone after the first. Priority: first.* **Partly moved.**

Phipia validates ELF64 in Rust and loads into private address spaces, so the
checking half of this exists. What has not moved is the part that matters here:
an image is still admitted at a *fixed page layout named per profile* in
`paging.h`, so a program's size is a constant somebody wrote down rather than
something the loader computes. That is the half a developed editor needs.

An editor is developed, so its image changes on every commit. Phipia needs a
way to admit an image by *shape* —
validated ELF64, `ET_EXEC`, static, non-PIE, W^X, bounded segment count, known
base — with the checksum pinned per release rather than per build.

Measured shape: a `media-editor` profile whose ELF contract is fixed and whose
digest is a release input, plus a negative control proving a malformed image is
refused at each named stage.

### PHIP-03 — General anonymous memory

*Blocks: any frame buffer, any decode, any cache. Priority: first.*

A userspace call that maps N anonymous RW/NX pages at a kernel-chosen address
inside the process address space, and one that releases them. Bounded by a
per-process policy maximum, refused past it, released in full at teardown, and
counted by the same resource census the current proofs use.

Measured shape: `map_anonymous(pages) -> address`, `unmap_anonymous(address,
pages)`, initial maximum 64 MiB, guard pages on both sides of each region.

### PHIP-04 — Floating point and SIMD state

*Blocks: all real media performance. Priority: highest value per unit work.*

`CR4.OSFXSR` and `CR4.OSXSAVE` enabled after a CPUID check, an `XSAVE` area per
process and per kernel thread, save and restore at the syscall boundary and at
context switch, and an installed proof that a user program's SSE2 register
contents survive an interrupt and a preemption. Only then may Media Editor's
build
stop passing `+soft-float`.

Measured shape: SSE2 first, AVX2 as a separate later profile, each with a
negative control that corrupts the save area and observes the named refusal.

### PHIP-05 — Userspace time

*Blocks: playback, scheduling, profiling.* **Done at 2.1.0.**

Asked for as "`clock_monotonic_ns()` exposed to a program, with the same
monotonicity guarantee `clock.c` already proves, and no other clock". Phipia
built exactly that, as `NETWORK_SYSCALL_QUERY_TIME` on the native boundary —
one clock, the kernel's own, reached through a checked request record. The only
thing to note is where it lives: a program gets the time by asking the
*networking* surface, which is the accident of what was built first rather than
a statement about time.

### PHIP-06 — Userspace framebuffer surface

*Blocks: any user interface. Priority: second.*

A program obtains a mapped RW/NX pixel surface of a fixed geometry and asks the
kernel to present a damage rectangle from it. The kernel keeps ownership of the
framebuffer, validates the rectangle against the surface, and copies. No shared
mapping of device memory into Ring 3, no compositor, no windows.

Measured shape: `surface_acquire(width, height) -> address`,
`surface_present(x, y, width, height)`, one surface per process, geometry fixed
at acquire, refusal on any rectangle not fully inside it.

### PHIP-07 — Input events to a program

*Blocks: interaction. Priority: second.*

A bounded event queue a foreground program may drain: key transitions with
scancodes and modifiers, pointer motion and button transitions. Ownership is
explicit and revocable, exactly like the `linux cat` foreground contract
already is, so the shell can take input back.

### PHIP-08 — Writable storage

*Blocks: saving a project.* **Done at 2.1.0, and done larger than asked.**

This was the largest single request in this document, and it was written when
the filesystem was a read-only FAT16 proof with three frozen root entries. It
asked for two steps: a bounded single-file rewrite first, a real directory and
allocation path later.

Phipia skipped straight to the second. There is now a **FAT32** implementation
with a read-only system volume and a separate read-write data volume: nested
directories, file growth and truncation, random access, rename, deletion and
clean persistence, with the pointer-free metadata validated in Rust and the
mutation, allocation and teardown owned by C. First Environment's Media Editor
workspace already opens and saves a project on it and writes `EXPORT.BMP` back.

What the *image* still lacks is a way to reach that from Ring 3 — the write
path is the kernel's, not a program's — which is `PHIP-01` rather than this.

**M8.32 answered the other half of it.** Phipia's filesystem is now stated in
Media Editor's own types, name rule for name rule, bound for bound, in
`media-editor-io::phipia` — so a name a person types is refused at the moment
they
type it rather than at the moment they save, and refused for the reason Phipia
would refuse it. Beside it is `media-editor-io::vault`, which is what those
bounds
require rather than what a filesystem would prefer:

> A directory holds **sixty-four** entries, so a hundred photographs cannot be
> a hundred files. A name is **eight and three**, so even if they could be,
> they could not keep their names. So the media library is **one** of Phipia's
> files with a store inside it — content-addressed, sealed, and carrying
> sixty-two bytes of name where the filesystem carries eleven.

**M8.37 widened the seam a second time, and this is the R-1.2 note that
widening requires.** `Storage` gained `append`: extend the scratch slot by
these bytes, starting it if it is empty. It is what `read_at` was to reading,
one milestone later and in the other direction — a reel this build writes is
bounded at 512 MiB against a program mapped 76 KiB, so a `write` that takes the
whole file in one slice is off by four orders of magnitude, and a file extended
a row at a time is not.

Nothing in `PHIP-08` had to move for it. Phipia's FAT32 already has "file growth
and truncation" as well as "random access", so both the appending shape and the
write-at-an-offset shape were available, and **appending is the weaker of the
two**. That was the choice: a writer that cannot seek backwards cannot damage
what it has already written, which is a property rather than a discipline. It
is also why `SPRW` moved its digest to a trailer in the same milestone — a
format that has to patch its own header is a format that needs the stronger
capability, and it needed it for no reason.

`append` takes no slot, which the other four methods all do. There is exactly
one place a save is assembled, and an operation that could name the committed
project or the committed vault would be an operation that could write a live
file in place — the one thing the whole protocol exists to prevent. Refusing it
at run time would be strictly worse than not being able to ask.

**M8.33 connected it to the seam, and one of Phipia's numbers decided the
shape.** A file holds sixteen mebibytes; a program is mapped seventy-six
kilobytes. Reading a whole vault on the target is off by **220×**, so the seam
grew a ranged read and the vault grew a `Catalogue` that holds a count, a
payload length and nothing else. That is not a concession to a small machine —
it is the same decision Phipia's own bitmap reader already made one layer down,
issuing random row reads through the filesystem rather than holding a picture.

Which is worth stating as a general finding about this platform: **`PHIP-03`
would make a great deal of this easier and none of it is waiting for it.** The
memory ceiling is real, and an application that reads its own material in
bounded windows is an application that fits — today, in the seventy-six
kilobytes that exist, rather than in the sixty-four mebibytes that are on a
list.

That is not a workaround. It is what content addressing was already for: the
model refers to media by digest, and a store keyed by digest is a store the
render graph can read from with nothing in between translating.

### PHIP-09 — More than one process, and process lifetime

*Blocks: decoder isolation, background render. Priority: fourth.*

Today one program runs at a time and the kernel tears it down before the prompt
returns. Isolating a decoder — the single most attackable component in an
editor — needs a second address space alive at the same time, with a channel
between them.

### PHIP-10 — Userspace threads

*Blocks: parallel decode and render. Priority: fourth.*

Kernel threads exist with capacity 8. A program needs its own, or a way to be
scheduled on several.

### PHIP-11 — SMP

*Blocks: real-time playback of anything demanding. Priority: fifth.*

Phipia is single-core. Video decode and render scale with cores more cleanly
than with anything else. Until this exists, Media Editor's job graph is written
to
be *order-independent and deterministic* so that the day cores arrive, nothing
in the model has to change.

### PHIP-12 — A large address space and many mappings

*Blocks: 4K work, long timelines. Priority: fifth.*

`PHIP-03` with a much larger policy maximum, many regions, and a mapping table
that is not a fixed 24-entry array.

### PHIP-13 — Audio output

*Blocks: audio monitoring, therefore editing. Priority: fourth.*

Phipia has no audio anything — re-checked at 2.1.0, and the one item on this
list that has not moved an inch in eleven releases.

The smallest useful device is an emulated Intel HDA or AC'97 output stream
with a ring buffer, a period interrupt, and a presentation clock a program can
query. Audio is the hardest real-time contract
in the application, and Phipia's existing DMA ownership discipline is exactly
the right foundation for it.

### PHIP-14 — Entropy for a program

*Blocks: dithering, cache salting.* **Done at 2.1.0.**

Asked for as "exposing a bounded `random_bytes(n)`", and that is what
`NETWORK_SYSCALL_RANDOM` is: bounded random bytes into a checked user range,
with degraded-state reporting rather than a silent fallback. It arrived for
networking's sake, which is how most of this list will be answered.

### PHIP-15 — Faults reported to a program

*Blocks: surviving a bad media file. Priority: fourth.*

A decoder that faults should terminate a bounded unit of work, not the editor.
Without process isolation or a fault channel, Media Editor's only defence is
that
every parser is written in safe Rust — which is the reason
[`ENGINEERING_RULES.md`](ENGINEERING_RULES.md) makes that non-negotiable.

### PHIP-16 — GPU

*Blocks: full-quality real-time playback and effects. Priority: last, and
possibly never.*

An editor can be excellent on the CPU alone; several were. This is recorded so
the render graph keeps a device-agnostic seam, not because it is planned.

### PHIP-17 — IOMMU

*Blocks: safe capture hardware. Priority: last.*

Phipia states plainly that a bus-mastering device can reach all physical
memory. That is acceptable for emulated fixtures and unacceptable for capture
hardware. Recorded for completeness.

## What Media Editor does in the meantime

Nothing in the ladder blocks the work that matters most early, because most of
an editor is pure logic over data:

- the time model, the project model, the timeline, and undo/redo are pure and
  are developed and tested on the host with no operating system at all;
- every parser is pure, bounded, and fuzzable on the host;
- the render graph, colour pipeline, and mixer are pure functions from typed
  inputs to typed outputs;
- only presentation, storage, input, and audio touch the platform, and each is
  behind one narrow seam named in [`ARCHITECTURE.md`](ARCHITECTURE.md).

That is the whole reason the language law puts Rust in charge: the majority of
this application can be correct long before the platform can run it.
