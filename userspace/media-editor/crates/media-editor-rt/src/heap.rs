// SPDX-License-Identifier: GPL-3.0-only
//! The M1 heap.
//!
//! A bump allocator over one static arena, with last-in-first-out reclaim.
//! It is deliberately the simplest thing that satisfies R-5.2: it returns null
//! when it cannot satisfy a request, and every caller in Media Editor reaches the
//! heap through `try_reserve`, which turns null into a typed refusal instead of
//! an abort.
//!
//! It is not the allocator Media Editor will ship. That is a bounded, constant
//! time allocator over pages obtained from `PHIP-03`, and it arrives with the
//! capability. This one exists so that the model can run on the target before
//! Phipia can hand out a page, and its limits are stated rather than hidden:
//! reclaim only in reverse order, one arena, no growth.

use core::alloc::{GlobalAlloc, Layout};
use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicUsize, Ordering};

/// How much the static arena holds.
///
/// Sixty-four kibibytes: a Phipia program is given seventy-six kibibytes of
/// mapped address space in total today, so this is most of what there is. Real
/// editing needs `PHIP-03` and `PHIP-12`, and this constant is what will be
/// deleted when they arrive rather than quietly raised.
pub const HEAP_BYTES: usize = 64 * 1024;

/// The arena, its cursor, and how much has been handed out.
pub struct BumpHeap {
    bytes: UnsafeCell<[u8; HEAP_BYTES]>,
    next: AtomicUsize,
    high_water: AtomicUsize,
}

// SAFETY: every field is either an atomic or is reached only through the
// cursor, which is atomic. Phipia is single-core and Media Editor has no
// userspace threads, so the only concurrency this must survive is none; the
// atomics are here so that the type is sound rather than merely adequate.
unsafe impl Sync for BumpHeap {}

impl BumpHeap {
    /// An empty arena.
    #[must_use]
    #[allow(
        clippy::large_stack_arrays,
        reason = "this value initialises a static, so the arena lives in .bss"
    )]
    pub const fn new() -> Self {
        Self {
            bytes: UnsafeCell::new([0; HEAP_BYTES]),
            next: AtomicUsize::new(0),
            high_water: AtomicUsize::new(0),
        }
    }

    /// How many bytes are currently handed out.
    #[must_use]
    pub fn in_use(&self) -> usize {
        self.next.load(Ordering::Relaxed)
    }

    /// The most that was ever handed out at once.
    #[must_use]
    pub fn high_water(&self) -> usize {
        self.high_water.load(Ordering::Relaxed)
    }

    /// The base of the arena, as an address.
    fn base(&self) -> *mut u8 {
        self.bytes.get().cast::<u8>()
    }
}

impl Default for BumpHeap {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: `alloc` returns either null or a pointer to `layout.size()` bytes
// inside the arena that no live allocation covers, aligned as asked. `dealloc`
// only ever moves the cursor back over the block it was given, and only when
// that block is the most recent one, so a pointer is never handed out twice.
unsafe impl GlobalAlloc for BumpHeap {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let base = self.base();
        let mut start = self.next.load(Ordering::Relaxed);
        loop {
            // Align the cursor by aligning the address it corresponds to, not
            // the offset: the arena's own alignment is whatever the linker
            // gave it, and assuming it would be a bug on a smaller alignment.
            let address = base as usize;
            let Some(unaligned) = address.checked_add(start) else {
                return core::ptr::null_mut();
            };
            let padding = unaligned.wrapping_neg() & (layout.align() - 1);
            let Some(offset) = start.checked_add(padding) else {
                return core::ptr::null_mut();
            };
            let Some(end) = offset.checked_add(layout.size()) else {
                return core::ptr::null_mut();
            };
            if end > HEAP_BYTES {
                return core::ptr::null_mut();
            }
            match self
                .next
                .compare_exchange_weak(start, end, Ordering::Relaxed, Ordering::Relaxed)
            {
                Ok(_) => {
                    self.high_water.fetch_max(end, Ordering::Relaxed);
                    // SAFETY: `offset` is within the arena and `end` does not
                    // exceed it, so the whole block is inside the allocation
                    // this pointer was derived from.
                    return unsafe { base.add(offset) };
                }
                Err(observed) => start = observed,
            }
        }
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        let base = self.base() as usize;
        let start = pointer as usize;
        let Some(offset) = start.checked_sub(base) else {
            return;
        };
        let Some(end) = offset.checked_add(layout.size()) else {
            return;
        };
        // Only the most recent block can be returned. Anything else is simply
        // kept, which is a leak this allocator admits to rather than a
        // corruption it would cause by guessing.
        let _ = self
            .next
            .compare_exchange(end, offset, Ordering::Relaxed, Ordering::Relaxed);
    }
}
