// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_op_in_unsafe_fn)]
#![allow(
    clippy::doc_markdown,
    reason = "Media Editor and Phipia are product names, not identifiers"
)]
//! Media Editor's freestanding runtime.
//!
//! The second of the two crates permitted to contain `unsafe` (R-3.1.4). It
//! owns exactly four things: clearing `.bss`, the heap, the panic path, and
//! the Phipia implementations of the seams. Nothing else in Media Editor knows
//! that a machine exists.
//!
//! There is no hidden runtime here (R-1.8): no unwinder, no dynamic loader, no
//! constructor list, no lazy initialisation. [`start`] does what it says, in
//! the order it says, and calls the application.

pub mod heap;
pub mod phipia;

use media_editor_abi::seam::Console;

pub use heap::{BumpHeap, HEAP_BYTES};
pub use phipia::{PhipiaClock, PhipiaConsole};

/// The process heap.
///
/// Registered as the global allocator only when this is built for a
/// freestanding target; on the host the standard allocator is already there
/// and registering a second one is a link error. The allocator itself is the
/// same code either way.
#[cfg_attr(target_os = "none", global_allocator)]
pub static HEAP: BumpHeap = BumpHeap::new();

unsafe extern "C" {
    /// The first byte of `.bss`, from the linker script.
    static mut __bss_start: u8;
    /// One past the last byte of `.bss`, from the linker script.
    static mut __bss_end: u8;
}

/// Zero the `.bss` section without relying on loader page initialization.
///
/// # Safety
///
/// Must be called exactly once, before anything reads a static, and only on an
/// image linked with `targets/media-editor.ld`, which is what defines the two
/// symbols and guarantees the region between them is mapped read-write.
pub unsafe fn clear_bss() {
    // SAFETY: the caller guarantees the linker script placed these symbols at
    // the ends of one mapped read-write region, so the range between them is a
    // single object this program owns.
    unsafe {
        let start = &raw mut __bss_start;
        let end = &raw mut __bss_end;
        let length = usize::try_from(end.offset_from(start)).unwrap_or(0);
        core::ptr::write_bytes(start, 0, length);
    }
}

/// Bring the runtime up and run the application.
///
/// # Safety
///
/// Must be called exactly once, from `_start`, on a Phipia kernel that
/// implements `PHIP-01` at [`media_editor_abi::ABI_VERSION`]. Does not return.
pub unsafe fn start(application: fn(&mut dyn Console) -> i32) -> ! {
    // SAFETY: `_start` is the entry point, so this is the first code to run
    // and nothing has read a static yet.
    unsafe {
        clear_bss();
    }
    let mut console = PhipiaConsole::new();
    let status = application(&mut console);
    // SAFETY: the caller guarantees the boundary exists.
    unsafe { media_editor_abi::syscall::exit(status) }
}

/// Where a panic goes.
///
/// Nothing in Media Editor panics: every fallible path returns a typed refusal,
/// and the image is built with `panic = "abort"` so unwinding does not exist.
/// This is the backstop for a bounds check the compiler inserted that this
/// code did not anticipate — which is exactly the class of bug that should
/// stop the program rather than continue past it (R-7.2).
#[cfg(target_os = "none")]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    let mut console = PhipiaConsole::new();
    // A panic is already a failure of the error path, so a failure to report
    // it changes nothing that can still be done about it.
    let _ = console.write(b"media-editor: panicked at ");
    if let Some(location) = info.location() {
        let _ = console.write(location.file().as_bytes());
    } else {
        let _ = console.write(b"an unknown location");
    }
    let _ = console.write(b"\n");
    // SAFETY: reached only on the freestanding target, where the boundary is
    // the only way to stop.
    unsafe { media_editor_abi::syscall::exit(70) }
}
