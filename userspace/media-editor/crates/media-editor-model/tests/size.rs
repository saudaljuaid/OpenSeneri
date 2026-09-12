// SPDX-License-Identifier: GPL-3.0-only
//! What the model's central types cost, kept by something that runs.
//!
//! `Item` carries a `clippy::large_enum_variant` expectation and the argument
//! for it rests on a number: a clip is 520 bytes. `Edit` carried one too until
//! a lift gave it a *second* variant holding an item — at which point the lint
//! stopped firing, the `expect` said so, and the exemption went. The ceiling
//! below stayed, because the ceiling is the half that was doing the work.
//!
//! A clip is 552 bytes and an edit is 576. A number that lives only in a doc comment is a number that
//! goes stale, and these would go stale silently — nothing else in the suite
//! would notice a field that doubled the cost of every item on every track,
//! or every entry in the history.
//!
//! So these are bounds rather than equalities. `repr(Rust)` layout is
//! deliberately unspecified and a compiler is free to pack these differently
//! tomorrow; what this crate is entitled to require is a *ceiling*, and a
//! ceiling is what a platform with seventy-six kibibytes of address space
//! actually cares about.

use media_editor_model::journal::MAX_HISTORY;
use media_editor_model::{Clip, Edit, Item, Mask, Motion, Transform};

/// What one item on a track may cost.
///
/// Set a little above the 520 bytes it is, so that ordinary layout differences
/// between compilers do not fail the build, and far enough below a doubling
/// that a new field large enough to matter does.
///
/// The history of this number is the history of what a clip carries: 288
/// before retiming, 320 after an exact speed, 344 after an opacity curve,
/// 416 once a clip could animate its mask as well as its framing — an
/// `Option<Motion>` was 72 bytes, three lanes of curve, and there are two of
/// them — 440 once a grade could come on over a shot, which is one more
/// `Option<Curve>` at 24, 488 once a motion gained a fourth lane for the turn,
/// which is 24 more in each of the two motions, 520 once a transform carried
/// the point it acts about — two rationals, 32 bytes — and 528 once a speed
/// could be a ramp.
///
/// That last step is eight bytes for a whole third case, and the reason is
/// worth knowing: **a freeze used to be free.** `Playback` was `At(Rational)`
/// and `Frozen`, and a rational cannot have a denominator of nought, so the
/// second variant lived in that niche and the enum was exactly one rational at
/// 16 bytes. A ramp is a `Curve`, which is a `Vec` at 24, so the enum is now
/// 24 — and the eight bytes appear here, and in `Edit`, and in the image.
///
/// Then 552, once a clip could carry notes of its own: a `Vec` at 24 bytes,
/// whatever is in it. That is the same shape the ramp took and it is the last
/// field this ceiling had room for, which is why the ceiling moves with it.
///
/// The ceiling moved from 512 to 576 when a transform gained its anchor, and
/// what ate the room is named above rather than left to be worked out: a
/// fourth lane, twice. At 512 the headroom had fallen to 24 bytes — exactly
/// one more `Option<Curve>` — which makes a ceiling a tripwire for the next
/// field rather than a bound on the shape, and a tripwire fails for the wrong
/// reason.
///
/// It moved again, 576 to 640, when a clip learned to carry notes. The
/// headroom had fallen to 24 bytes for the second time, which is the same
/// tripwire as before; what ate it this time was a ramp at 8 and a note list
/// at 24. Sixty-four bytes of new room is two more fields of that size, which
/// is a bound on the shape rather than a dare.
///
/// Raising a ceiling is allowed; raising it without saying which change ate
/// the room is not. And raising it without measuring the *image* would be
/// worse: `Clip` going 320 → 344 took two pages **off** the program, because
/// past 320 the optimiser stops copying a clip inline into each of
/// `Edit::apply`'s arms; going 344 → 416 put those two pages back. The
/// relationship is not monotone and cannot be reasoned about, only measured,
/// which is why every milestone that moves this number records what the image
/// did in [the platform contract](../../../docs/PLATFORM_CONTRACT.md).
const MAX_ITEM_BYTES: usize = 640;

/// What one edit in the history may cost.
///
/// The largest variants are `InsertItem` and `DropItem`, which each carry a
/// whole item, so this tracks [`MAX_ITEM_BYTES`] and sits the same distance
/// above what it is. It moves when that one does and for the same reason.
const MAX_EDIT_BYTES: usize = 672;

#[test]
fn an_item_costs_no_more_than_the_argument_for_its_shape_assumes() {
    let item = core::mem::size_of::<Item>();
    assert!(
        item <= MAX_ITEM_BYTES,
        "an item is {item} bytes, past the {MAX_ITEM_BYTES} the enum's \
         documented exemption argues from -- either shrink it or rewrite the \
         argument, but do not leave the two disagreeing"
    );
    assert_eq!(
        core::mem::size_of::<Clip>(),
        item,
        "the clip variant is the whole of an item's cost, which is the premise \
         the exemption rests on"
    );
}

#[test]
fn the_decorations_are_what_a_clip_is_made_of() {
    // Named individually because the exemption's argument is that no *one* of
    // these can be made smaller without giving up exact arithmetic -- four
    // rationals are four rationals -- and a future reader deserves to see
    // which one grew rather than only that something did.
    let decorations = core::mem::size_of::<Option<Transform>>()
        + core::mem::size_of::<Option<Motion>>() * 2
        + core::mem::size_of::<Option<Mask>>();
    assert!(
        decorations < core::mem::size_of::<Clip>(),
        "the decorations are {decorations} bytes of a clip's {}, and a clip \
         also holds its media, its source position and its length",
        core::mem::size_of::<Clip>()
    );
    assert!(
        decorations * 2 > core::mem::size_of::<Clip>(),
        "and they are the majority of it, which is why boxing them is the \
         remedy clippy suggests and R-5.2 refuses"
    );
}

#[test]
fn an_edit_costs_no_more_than_the_argument_for_its_shape_assumes() {
    let edit = core::mem::size_of::<Edit>();
    assert!(
        edit <= MAX_EDIT_BYTES,
        "an edit is {edit} bytes, past the {MAX_EDIT_BYTES} this crate's \
         history budget argues from"
    );
    assert!(
        edit >= core::mem::size_of::<Item>(),
        "and it is at least an item, because two of its variants carry one -- \
         which is why this number follows the one above"
    );
}

#[test]
fn the_history_is_bounded_by_the_platform_before_it_is_bounded_by_its_constant() {
    // MAX_HISTORY is a policy ceiling, and on a machine with room it is the
    // one that bites. On Phipia it is not, and the arithmetic here is the
    // reason: a program gets nineteen mapped pages, and one entry is a pair.
    const MAPPED_BYTES: usize = 19 * 4096;
    let pair = core::mem::size_of::<Edit>() * 2;
    let affordable = MAPPED_BYTES / pair;
    assert!(
        affordable < MAX_HISTORY,
        "the constant would be reachable in {MAPPED_BYTES} bytes, which would \
         make this test's premise wrong rather than merely its number stale"
    );
    // Not an assertion about the exact figure -- the project itself, the
    // media library and the render caches are in those pages too, so the true
    // depth is well under this. What is asserted is the order of the two
    // limits: the allocator's typed refusal arrives first, and it is
    // `ModelStatus::OutOfMemory` rather than `CapacityExhausted`. A reader of
    // `MAX_HISTORY` should not take four thousand as a promise.
    assert!(
        affordable < 200,
        "a hundred and sixteen pairs of edits is the whole of the address \
         space, and the real figure is {affordable}"
    );
}
