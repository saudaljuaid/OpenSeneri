// SPDX-License-Identifier: GPL-3.0-only
#![no_std]
#![no_main]
//! The Media Editor program image.
//!
//! Everything this file does is name the entry point the linker script points
//! at and hand control to the runtime. There is no initialisation here that
//! `media_editor_rt::start` does not do in the open, because a program that does
//! work before its entry point is a program with a hidden runtime (R-1.8).

/// Where Phipia begins.
///
/// Phipia enters a program at the address in the ELF header with a guarded
/// stack and a private address space, and expects never to see it return —
/// the program leaves through the boundary's `Exit` call or not at all.
#[unsafe(no_mangle)]
pub extern "C" fn _start() -> ! {
    // SAFETY: this is the entry point, so it runs exactly once, before
    // anything else, which is `start`'s whole contract.
    unsafe { media_editor_rt::start(media_editor_app::run) }
}
