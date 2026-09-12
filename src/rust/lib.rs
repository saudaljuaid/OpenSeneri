// SPDX-License-Identifier: GPL-3.0-only
//! The parts of Phipia written in Rust.
//!
//! Phipia is a C kernel. Rust is used here for one specific job: parsing input
//! this kernel did not produce. A bounds check that the compiler inserts and
//! cannot be talked out of is worth more on a byte stream from outside than
//! anywhere else, because that is where a missing one becomes an attacker's
//! primitive rather than a bug.
//!
//! It is deliberately *not* used for the layers that talk to hardware. Page
//! tables, port I/O and the context switch are unsafe operations by nature:
//! writing them in Rust would wrap every line in `unsafe` and buy nothing but
//! a second language in the boot path. `docs/RUST.md` argues that split.
//!
//! The whole crate compiles with no operating system. The ext4 parser uses
//! Phipia's bounded kernel heap through the allocator boundary in [`abi`]; the
//! older parsers remain allocation-free. Unsafe code is confined to that ABI
//! boundary, which turns validated C pointers into Rust slices, writes results
//! through validated C pointers, and calls the kernel allocator and block I/O.

#![no_std]
#![deny(warnings)]
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(missing_docs)]

pub mod abi;
pub mod elf64;
pub(crate) mod elf64_dynamic;
pub(crate) mod ext4;
pub mod fat16;
pub(crate) mod fat32;
pub(crate) mod linux_fat16;
pub(crate) mod linux_elf64;
pub mod font;
pub mod logo;
pub(crate) mod native_image;
pub mod nvbios;
pub(crate) mod sha256;
pub mod ui_font;
pub mod wallpaper;

/// Where a Rust panic goes.
///
/// Phipia-authored fallible paths return a status, while compiler-inserted
/// bounds checks in the reviewed ext4 dependency may still trap. The crate is
/// built with `panic=abort`, so those traps cannot unwind. This handler turns
/// an unanticipated metadata-parser defect into a kernel stop rather than
/// silent corruption.
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    abi::panic()
}
