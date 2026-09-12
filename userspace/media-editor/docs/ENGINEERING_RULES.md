<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Media Editor engineering rules

These are the normative project rules. **MUST** and **MUST NOT** are absolute;
**SHOULD** requires an explanation in the pull request. Rule identifiers are
stable review references.

A rule may be waived only by a waiver committed as `docs/waivers/<id>.md`
naming the rule, the exact scope, the owner, the expiry date, and the test that
proves the waived condition stays contained. An expired waiver fails the build.

---

## 1. First principles

**R-1.1** Every contract is bounded. A feature is a stated shape with stated
maxima: how many, how large, how long, how deep. An unbounded loop, an
unbounded allocation, an unbounded recursion, and an unbounded wait are all the
same defect.

**R-1.2** Widening a contract is a new contract. It arrives with a new name,
new evidence, and its own negative control. It never arrives as an edit to an
existing allowlist, limit, or `match` arm.

**R-1.3** Refuse; do not repair. Truncated, overlapping, wrapping,
non-canonical, ambiguous, or self-contradicting input is refused by a named
error. Nothing is guessed, clamped, rounded into range, or partially accepted.

**R-1.4** No partial publication. An operation either completes and publishes
its whole result, or it fails and publishes nothing. There is no state in which
half of a frame, half of an edit, or half of a project file is visible.

**R-1.5** One owner. Every resource — a mapping, a buffer, a device, a
generation, a lock, a file — has exactly one owner at every instant, and
ownership transfer is an explicit operation with a name.

**R-1.6** Evidence over assertion. A claim about behaviour is worth what its
test is worth. A screenshot shows presentation, not correctness.

**R-1.7** Every invariant has a negative control: an isolated, temporary
mutation that violates it and makes the narrowest relevant gate fail by name.

**R-1.8** No hidden runtime. No garbage collector, no unwinder, no dynamic
loader, no implicit thread pool, no global constructor, no lazy singleton, no
work performed before `main` that the source does not show.

**R-1.9** The same project and inputs produce the same output bytes on any
machine, core count, or run order. See section 4.

**R-1.10** No code path may lose, silently alter, or make a saved project
unopenable. On failure, keep the last good state.

---

## 2. Phipia target

**R-2.1** Media Editor targets Phipia. Shipping code MUST NOT contain a
conditional, abstraction, or dependency whose purpose is another operating
system.

**R-2.2** Host builds exist only to run pure logic under test. A crate that
compiles for the host MUST compile for the host with the same source that runs
on Phipia — differing only by which crates are linked in, never by `#[cfg]`
that changes behaviour.

**R-2.3** No POSIX assumption. No file descriptors, no `errno` semantics, no
signals, no `fork`, no paths, no environment variables, no locale, no TTY,
unless and until Phipia defines that concept natively and measures it.

**R-2.4** When Phipia lacks a capability, the response is to specify it in
[`PLATFORM_CONTRACT.md`](PLATFORM_CONTRACT.md) and build it in Phipia. It is
never to emulate it inside Media Editor, and never to widen Phipia's measured
Linux compatibility boundary, which exists to be narrow.

**R-2.5** Media Editor MUST NOT link against a libc, a hosted runtime, or any
library that assumes one, in any shipping artefact.

---

## 3. Languages

### 3.1 Rust owns the application

**R-3.1.1** Rust is the default language. The project model, the timeline, the
media library, file management, undo/redo, media pipeline coordination, the
render graph, the colour pipeline, the mixer's control plane, and all UI state
MUST be Rust.

**R-3.1.2** All shipping crates are `#![no_std]`. `alloc` is permitted where
section 5 allows it. `std` MUST NOT appear in a shipping dependency graph.

**R-3.1.3** Every crate declares, at the top of its root module:
`#![deny(warnings)]`, `#![deny(unsafe_op_in_unsafe_fn)]`,
`#![deny(missing_docs)]`, and `#![forbid(unsafe_code)]` unless it is
`media-editor-abi` or `media-editor-rt`, the only two crates R-3.1.4 permits to
contain `unsafe`.

**R-3.1.4** `unsafe` is confined to the ABI crate and to the runtime crate.
Everywhere else it is forbidden by attribute, not by convention. Within those
two crates, every `unsafe` block MUST carry a `// SAFETY:` comment naming the
condition that makes it sound and who guarantees that condition.

**R-3.1.5** No panicking constructs in shipping code: no `unwrap`, `expect`,
`panic!`, `unreachable!`, `todo!`, `unimplemented!`, `assert!` family in
non-test code, no indexing that can be out of range, no integer division
without a proven non-zero divisor, no slicing by range without a checked
length. Use `get`, `checked_*`, and typed errors. `debug_assert!` is permitted
and MUST NOT be load-bearing. Assertions evaluated in a `const` context are not
runtime panics and are encouraged — R-3.2.4 requires them.

**R-3.1.6** Arithmetic on values derived from input MUST be explicit:
`checked_*`, `saturating_*`, or `wrapping_*` chosen deliberately and named in a
comment when the choice is not obvious. Implicit `as` casts that can truncate
or change sign are forbidden; use `try_into` with a typed refusal.

**R-3.1.7** Errors are typed enums per subsystem, in Phipia's style: one
`enum ...Status` with a variant per distinguishable refusal, `#[non_exhaustive]`
only where the crate is public, and a `&'static str` description function.
`Box<dyn Error>`, stringly-typed errors, and error types that lose the cause
are forbidden.

**R-3.1.8** Public API surfaces MUST be documented, including what they refuse
and under which conditions. `#![deny(missing_docs)]` enforces existence; review
enforces usefulness.

**R-3.1.9** Trait objects and dynamic dispatch are permitted in control paths
and forbidden in per-sample and per-pixel paths.

**R-3.1.10** No `build.rs` may read the network, read outside the crate
directory, or generate code from a source not committed to this repository.

**R-3.1.11** The Rust edition is 2024 and the minimum supported compiler
version is pinned in `rust-toolchain.toml`. Raising either is its own commit
with its own justification.

**R-3.1.12** `clippy::pedantic` runs with warnings denied. An allowed lint
carries an inline reason.

### 3.2 The C ABI boundary

**R-3.2.1** There is exactly one boundary crate, `media-editor-abi`. Every
`extern "C"` declaration, every raw pointer that came from outside Rust, and
every conversion from a foreign pointer into a slice lives there. Nowhere else.

**R-3.2.2** The boundary exists for two reasons only: Phipia's native userspace
ABI is C-shaped, and external codec libraries are C-shaped. A boundary crossing
for any other reason is refused.

**R-3.2.3** Values crossing the boundary MUST be `#[repr(C)]` with fixed-width
types. Forbidden across the boundary: Rust enums without an explicit
`#[repr(i32)]`, `bool` from foreign code without normalisation, bitfields,
`long`, `long double`, varargs, packed structures whose padding is not
declared, and any type whose size or alignment is not asserted on both sides.

**R-3.2.4** Every shared aggregate MUST be size- and offset-asserted at compile
time on both sides — `const _: () = assert!(...)` in Rust and
`_Static_assert` in C — exactly as `src/rust/abi.rs` does in Phipia. A mismatch
is a build failure, never a runtime surprise.

**R-3.2.5** Ownership does not cross the boundary. Rust allocates and frees
Rust memory; the foreign side allocates and frees its own. Where a foreign
library must write into a buffer, Rust supplies a bounded slice it already owns
and validates the complete destination range before the call.

**R-3.2.6** Results returned across the boundary SHOULD be pointer-free:
scalars, fixed-size records, or a status plus a count. A returned pointer must
have a documented lifetime shorter than the call that produced it, and MUST NOT
be retained.

**R-3.2.7** Callbacks from foreign code into Rust are forbidden unless the
callback is registered through the boundary crate, is `extern "C"`, cannot
unwind, cannot allocate, cannot re-enter the boundary, and is bounded in time.

**R-3.2.8** Every boundary function documents, in a `# Safety` section, exactly
what the caller must guarantee. A function whose safety condition cannot be
stated in two sentences is the wrong function.

**R-3.2.9** Boundary symbols are prefixed `media_editor_`. The kernel-facing
shim
symbols are prefixed `media_editor_sys_`. No other global symbol may be exported
from a shipping artefact.

**R-3.2.10** The boundary is versioned by a constant that both sides check at
initialisation. A mismatch refuses to start.

### 3.3 C is for tiny shims

**R-3.3.1** C is permitted for one purpose: a freestanding shim that exists
because the operation is instruction-, register-, or ABI-shaped and cannot be
expressed in Rust without more `unsafe` than the shim contains. Phipia's
syscall entry sequence is the canonical example.

**R-3.3.2** A shim MUST be tiny, and "tiny" is enforced, not felt: at most 100
lines of code per file excluding comments, at most 200 lines of C in the
repository in total until a waiver says otherwise, and no shim may call another
shim.

**R-3.3.3** A shim MUST NOT: allocate, hold state between calls, parse
untrusted bytes, implement a data structure, implement a policy, contain a
loop whose bound is not a compile-time constant, or use the preprocessor for
control flow.

**R-3.3.4** C is C11 freestanding, compiled with Phipia's flag set:
`-ffreestanding -fno-pie -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse
-mno-sse2 -msoft-float -fno-tree-vectorize -fno-asynchronous-unwind-tables
-Wall -Wextra -Werror -Wpedantic -Wshadow -Wundef -Wstrict-prototypes
-Wmissing-prototypes`. Divergence from Phipia's flags requires a waiver.

**R-3.3.5** No libc. No headers other than `stdbool.h`, `stddef.h`,
`stdint.h`, and Media Editor's own.

**R-3.3.6** Every shim has a Rust-side test that exercises it through the
boundary, and a negative control.

### 3.4 C++ is for sealed performance leaves

C++ is limited to measured inner loops and admitted codec libraries.

**R-3.4.1 (Rust first.)** No C++ may be written for a computation until
a correct, tested, pure-Rust implementation of that computation exists in the
tree. The Rust version defines the behavior.

**R-3.4.2 (Bit-exactness.)** A C++ leaf MUST produce output bit-identical to
its Rust reference for every input in a committed corpus plus a fuzz campaign.
The Rust reference remains in CI and defines the correct output. A mismatch
disables the C++ leaf.

**R-3.4.3 (Measurement or deletion.)** A C++ leaf MUST come with a committed
benchmark showing at least a 1.3× improvement over the Rust reference on the
target profile. A leaf that stops meeting its threshold — because the Rust
version improved, or the workload changed — is deleted at the next release.

**R-3.4.4 (Sealed.)** A leaf is a pure function: bounded inputs in, bounded
outputs out, no allocation, no I/O, no global state, no static storage with a
constructor, no callback into Rust, no knowledge of the project model. It
exposes `extern "C"` only.

**R-3.4.5 (Subset.)** Compiled with `-fno-exceptions -fno-rtti
-fno-threadsafe-statics -fno-use-cxa-atexit -ffreestanding -fno-unwind-tables`
and the same warnings-as-errors and no-SIMD-until-`PHIP-04` rules as C.
Forbidden: the standard library's allocating containers, `iostream`, `new` and
`delete`, exceptions, RTTI, virtual inheritance, templates that appear in the
ABI, thread-local storage, and any global whose initialisation is not a
constant expression. The build asserts `.init_array` is empty.

**R-3.4.6 (No C++ in the trust path.)** C++ MUST NOT parse untrusted bytes,
validate anything, own a lifetime, or make a decision. It transforms data whose
shape Rust has already proven.

**R-3.4.7** A vendored third-party C++ library is not a leaf and does not get
these exemptions; it is a dependency, governed by
[`DEPENDENCY_POLICY.md`](DEPENDENCY_POLICY.md), and it is wrapped so that no
C++ type ever appears in Media Editor's own code.

### 3.5 Assembly

**R-3.5.1** Assembly is permitted only where instruction selection is the
entire point: the process entry stub, the syscall sequence, and a CPU feature
probe. Each file states why a compiler cannot be trusted to emit the sequence.

**R-3.5.2** Every assembly routine has a portable path used by tests, and the
two are proven equivalent.

### 3.6 Nothing else

**R-3.6.1** No other language may appear in a shipping artefact. Python is
permitted for build and verification tooling only, matching Phipia's practice,
and MUST be deterministic and dependency-free beyond the standard library.

**R-3.6.2** No scripting language, expression evaluator, template engine, or
bytecode interpreter may be embedded in the application. A project file is
data, never a program. See R-11.4.

---

## 4. Determinism

**R-4.1** A render is a pure function of (project bytes, media bytes, render
settings). The same triple produces byte-identical output, on any machine, at
any core count, in any scheduling order, in any build with the same pinned
toolchain.

**R-4.2** Fast-math is forbidden. `-ffast-math`, `-funsafe-math-optimizations`,
`-ffinite-math-only`, and their Rust equivalents MUST NOT appear. Floating
point contraction is off: `-ffp-contract=off`. FMA is used only where written
explicitly.

**R-4.3** Denormal flushing is forbidden. `FTZ` and `DAZ` MUST NOT be set.

**R-4.4** Reductions over more than one element MUST have a fixed association
order that does not depend on thread count, work-stealing, or completion order.
A parallel reduction uses a fixed tree shape, decided by index, not by arrival.

**R-4.5** Iteration order over any associative container MUST NOT affect
output. Where order matters, iterate a sorted or index-ordered sequence.

**R-4.6** Randomness is seeded, explicit, and reproducible. Dither, noise, and
any stochastic process take an explicit seed derived from the project and the
frame index. No entropy source may reach a render.

**R-4.7** No wall-clock time, uptime, address value, pointer identity, or
allocation address may influence output.

**R-4.8** Time is rational, never floating point. A position on the timeline is
an exact rational in a stated timebase. Frame rates such as 24000/1001 are
represented as that fraction and never as `23.976`. Conversions between
timebases are exact or they are refused.

**R-4.9** Audio positions are integer sample counts in a stated sample rate.
Sample-rate conversion states its filter, its phase, and its rounding, and is
bit-exact across runs.

**R-4.10** Every render acceptance test compares a SHA-256 of the output, not a
perceptual metric. Perceptual metrics are diagnostics.

---

## 5. Memory

**R-5.1** Every allocation is bounded by a named policy constant, in Phipia's
style: a `const` with a comment saying why that number and what happens at it.

**R-5.2** Every allocation is fallible. A failed allocation returns a typed
refusal that the caller handles. Aborting on allocation failure is forbidden in
shipping code, which means the infallible `alloc` APIs are forbidden too:
`Vec::push` without a prior `try_reserve`, `Box::new`, `collect` into an
allocating container, and anything else whose failure path aborts. The
allocator's error hook exists to make a violation loud in tests, not to be
relied on in production.

**R-5.3** No allocation in a real-time path: the audio callback, the present
path, and any interrupt or event callback. These use pre-allocated arenas
sized at start-up.

**R-5.4** Frame and sample buffers come from typed pools with a fixed capacity
and an explicit eviction policy. Ad-hoc `Vec` growth for media data is
forbidden.

**R-5.5** Recursion over user-controlled structure is forbidden. Depth-limited
explicit stacks with a named maximum replace it — in the timeline, in nested
sequences, in effect graphs, and in every parser.

**R-5.6** At the end of every session, and at the end of every test, a resource
census MUST show every pool, mapping, buffer, and handle returned. This mirrors
Phipia's `*_resources_released()` checks and is enforced the same way.

**R-5.7** No memory may be reachable but unowned. No leak is acceptable "because
we exit anyway"; Media Editor is a long-running application and exits are rare.

**R-5.8** Zero-initialisation is explicit. Reading uninitialised memory is
unrepresentable in safe Rust and forbidden across the boundary.

---

## 6. Concurrency

Concurrency must not change model or render results.

**R-6.1** The unit of parallelism is a pure task in a job graph: typed inputs,
typed outputs, no shared mutable state, no ambient access to the model.

**R-6.2** A job graph's result MUST be independent of execution order,
concurrency level, and completion order. This is tested by running the same
graph serially and in every permutation a bounded scheduler can produce, and
comparing hashes.

**R-6.3** Shared mutable state requires a typed owner and an explicit
transfer. `unsafe impl Send`/`Sync` is forbidden outside the runtime crate.

**R-6.4** No locks on the render or audio path. Communication is by ownership
transfer through bounded queues.

**R-6.5** Every wait is timed and every timeout is a named refusal. There are no
infinite waits, anywhere, for any reason.

**R-6.6** Blocking the UI event loop for more than one frame interval is a
defect, not a performance issue.

---

## 7. Errors

**R-7.1** Failure is a value. A subsystem returns its typed status; it does not
panic, abort, log-and-continue, or return a default.

**R-7.2** A panic in shipping code is a proof failure. `panic = "abort"` is set
so a panic stops the process rather than unwinding through the boundary, and
any panic reaching a release build is a release blocker.

**R-7.3** A refusal names the condition. "Invalid input" is not a refusal;
"clip start is after clip end in track 3" is.

**R-7.4** No error is discarded. `let _ =` on a `Result` is forbidden;
handling means acting, recording, or explicitly and visibly ignoring with a
comment stating why that is safe.

**R-7.5** Recovery restores the last known-good state in full. A partially
applied edit is a bug of the same severity as data loss.

**R-7.6** Diagnostics are separate from the interface. Proof vocabulary —
`PASS`, `READY`, `ONLINE` — belongs in transcripts, not in front of the user.
This is Phipia's rule and Media Editor keeps it.

---

## 8. Media pipeline

**R-8.1** A frame is immutable once produced. Effects produce new frames.

**R-8.2** A frame carries its full description: geometry, pixel format, colour
primaries, transfer function, matrix, range, chroma siting, and pixel aspect.
An untagged frame is refused. There is no default colour space.

**R-8.3** Colour conversion is explicit and lossless-by-declaration: every
conversion states its input and output description and is refused if either is
unknown.

**R-8.4** A decoder produces exactly what the file says, or a named refusal. It
MUST NOT conceal errors, invent frames, repeat the previous frame, or silently
resynchronise.

**R-8.5** Every cache entry is keyed by a hash over (input content hash, full
parameter set, code version). Cache hits must match all three.

**R-8.6** Every pipeline stage declares its latency in frames or samples.
Playback alignment is computed from declarations, never measured and guessed.

**R-8.7** Dropped frames and audio underruns are counted, surfaced, and
reproducible in a test. Silent dropping is forbidden.

**R-8.8** No media decoding may run in the UI event loop.

---

## 9. Project data

**R-9.1** The project model is the single source of truth. Everything else —
UI state, caches, rendered frames, waveform overviews — is derived and
disposable.

**R-9.2** Every edit is a value: a typed, serialisable, invertible operation.
Undo applies the inverse; redo reapplies the operation. Undo/redo is a total
function over the model, tested by property: for any sequence of operations,
undoing all of them reproduces the initial model exactly, hash for hash.

**R-9.3** The project file format is versioned from its first byte, carries a
magic, a version, a length, and a digest, and refuses anything it does not
recognise. Forward compatibility is a promise only where the format says so.

**R-9.4** Saving is atomic and all-or-nothing. A save that is interrupted
leaves the previous file intact. This is proven by a test that interrupts it.

**R-9.5** Media is referenced by content identity — a digest — plus a location
hint, never by location alone. Moved media is re-linked by identity.

**R-9.6** The application never modifies source media in place.

**R-9.7** Autosave is a separate, additive journal. It never overwrites the
user's file.

---

## 10. Interface

**R-10.1** UI state is derived state. A widget may hold focus, scroll position,
and transient input, and nothing else. Anything a user would be upset to lose
belongs in the model.

**R-10.2** Rendering is a pure function of (model, UI state, geometry). No
drawing code reads a clock, a device, or a global.

**R-10.3** Nothing draws from an interrupt or event-delivery context. Events are
published; drawing happens in the application's own pass. This is Phipia's
`surface.c` discipline and Media Editor inherits it.

**R-10.4** Damage is tracked and bounded; a full-surface repaint is a fallback
that is counted and visible in diagnostics.

**R-10.5** Layout is deterministic for a given geometry. Overlap, overflow,
invalid focus, and duplicate identifiers are refused before activation, as
Phipia already refuses them.

**R-10.6** Copy is ordinary human language, sentence case, and short.

---

## 11. Hostile input

Treat every byte of media as hostile input.

**R-11.1** Every parser — container, codec bitstream, subtitle, project file,
font, image — MUST be written in safe Rust with `#![forbid(unsafe_code)]`, or
be a vendored library that section 12 admitted with explicit isolation.

**R-11.2** Every parser is bounded in memory, in time, and in output size
before it starts, and refuses input that would exceed any of the three.

**R-11.3** Every parser has a fuzz target, run in CI, with a committed corpus
and a committed set of crashers-turned-tests.

**R-11.4** A project file MUST NOT be able to cause code execution, a path
traversal, a network access, an unbounded allocation, or the loading of an
arbitrary library. There is no expression evaluator and no plugin path in the
format.

**R-11.5** Metadata is displayed as text, never interpreted. No markup, no
escape sequences, no format strings.

**R-11.6** Run untrusted decoders behind Phipia process isolation where the
platform seam supports it. R-11.1 still applies inside that boundary.

---

## 12. Dependencies

The detail lives in [`DEPENDENCY_POLICY.md`](DEPENDENCY_POLICY.md); these are
the rules it implements.

**R-12.1** Every dependency is vendored into the tree, pinned by exact version
and digest, and built offline. There is no build that reaches a network.

**R-12.2** Every dependency's licence MUST be compatible with GPL-3.0-only, and
the compatibility MUST be verified from the vendored source at import, not from
a registry field.

**R-12.3** A dependency that pulls `std`, a libc, a C++ standard library, a
build-time code generator that is not committed, or a transitive tree larger
than its value is refused.

**R-12.4** Every dependency has a named owner, recorded reason, and practical
exit plan.

**R-12.5** `unsafe` in a dependency is budgeted. The budget is recorded per
dependency and reviewed at every upgrade.

**R-12.6** Upgrades are deliberate, reviewed commits with their own evidence.
Automatic dependency updates are forbidden.

**R-12.7** A dependency that duplicates something Media Editor must own anyway —
the time model, the project format, the timeline — is refused. See
[`DEPENDENCIES.md`](DEPENDENCIES.md) section "What Media Editor writes itself".

---

## 13. Build

**R-13.1** One command builds everything from a clean tree, offline, with no
host state beyond the pinned toolchain.

**R-13.2** Two clean builds of the same commit produce byte-identical
artefacts, and CI proves it by building twice, from two different directories,
and comparing, exactly as Phipia's BusyBox profiles do. This requires
`--remap-path-prefix` for every compiler, a fixed `SOURCE_DATE_EPOCH`, no build
identifier, and no embedded host name, user name, or timestamp.

**R-13.3** Warnings are errors, in every language, in every crate, including
dependencies where the build system allows it.

**R-13.4** The shipping artefact MUST be a static, non-PIE `ET_EXEC` image with
separate read-only, execute, and write segments, no dynamic section, no
relocation records, no undefined symbols, and a non-executable stack.
`tools/elf-audit.py` asserts each of these on the linked ELF, and
`tools/audit-control.py` proves on every run that it is capable of refusing.

A global offset table is permitted only under all three of these conditions: it
is resolved completely at link time, every entry points inside the image, and
its size stays inside the bound the linker script asserts. Phipia identity-maps
the whole low 4 GiB for itself, so a user image lives above it, and at that
distance the linker cannot relax the precompiled standard library's
position-independent accesses into direct ones. The table that results carries
no relocation record and needs no loader, which is the property the original
rule was protecting; requiring it to be empty would instead require rebuilding
the standard library on a nightly toolchain, which costs more than it buys.
See [`PLATFORM_CONTRACT.md`](PLATFORM_CONTRACT.md).

**R-13.4.1** Link-time optimisation MUST NOT be enabled for the image while it
links against a precompiled standard library, because the two disagree about
the code model and the disagreement is a link failure rather than a silent
miscompilation only by luck. It may return with a from-source standard library
and its own evidence.

**R-13.5** `--orphan-handling=error` is mandatory. A section neither placed nor
discarded by the linker script is a build failure.

**R-13.6** No floating-point, MMX, SSE, or AVX instruction may appear in a
shipping image until Phipia provides `PHIP-04`. The build audits the
disassembly, as Phipia's does.

**R-13.7** Every generated asset and fixture has a pinned digest checked by the
build. A changed digest is a deliberate commit.

**R-13.8** Build products, toolchains, vendored archives' build outputs, and
editor state are never committed.

---

## 14. Verification

The detail lives in [`VERIFICATION.md`](VERIFICATION.md).

**R-14.1** Acceptance happens on Phipia under QEMU. Host tests are supporting
evidence and never acceptance.

**R-14.2** Every new invariant arrives with a negative control (R-1.7).

**R-14.3** Every parser arrives with a fuzz target (R-11.3).

**R-14.4** Every render path arrives with a golden-hash test (R-4.10).

**R-14.5** Every performance claim arrives with a committed benchmark and the
machine profile it was measured on.

**R-14.6** A pull request states the exact commands run, the transcript lines
that establish the change, and the most credible failure the checks do not
cover. "None" is not an acceptable risk statement.

---

## 15. Style and review

**R-15.1** Formatting is mechanical: `rustfmt` and `clang-format` with
committed configurations. Formatting is never discussed in review.

**R-15.2** Lines wrap at 80 columns in prose and documents, 100 in code.

**R-15.3** Names are ordinary words. Abbreviations are used only where the
domain uses them — `fps`, `pts`, `lut` — and are spelled out at first use in a
module's documentation.

**R-15.4** Comments say why, and name the refusal. A comment restating the code
is deleted in review.

**R-15.5** One logical change per commit. Imperative subject, at most 72
characters, prefixed by area, in Phipia's form:

```text
timeline: refuse an edit whose out point precedes its in point
abi: assert the frame descriptor layout on both sides
```

**R-15.6** No `TODO` without an issue reference. No commented-out code. No
dead code behind a flag that is never on.

**R-15.7** No force-push to a shared branch, no bypassing hooks, no merging a
red check.

**R-15.8** A reviewer's job is to try to break the claim, not to confirm it.

---

## 16. Prohibited features

Forbidden anywhere in shipping code: `std`; libc; dynamic linking; PIE;
exceptions; RTTI; garbage collection; global constructors; hidden threads;
`unwrap`/`expect`/`panic!`; unchecked indexing; unbounded allocation; unbounded
recursion; unbounded waits; fast-math; denormal flushing; order-dependent
reductions; wall-clock or address-derived behaviour; floating-point timecode;
network access; telemetry; auto-update; a plugin loader; an embedded
interpreter; code execution from project data; in-place modification of source
media; a portability layer; a POSIX assumption; proprietary SDKs; and any
dependency whose licence is incompatible with GPL-3.0-only.

---

## 17. Updating these rules

**R-17.1** This document changes by pull request, with the reason stated and
the affected code updated in the same change or in a linked follow-up that is
committed before the release.

**R-17.2** A rule removed must say what replaced it. Rules are never silently
dropped.

**R-17.3** Rule identifiers are stable. A removed identifier is retired, never
reused.
