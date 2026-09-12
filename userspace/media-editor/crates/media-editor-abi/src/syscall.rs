// SPDX-License-Identifier: GPL-3.0-only
//! The proposed native Phipia system call surface.
//!
//! Phipia's existing Linux compatibility boundary passes the call number in
//! `RAX`, arguments in `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9`, and returns a
//! signed result in `RAX`, with refusals as negative errno values. This
//! proposal keeps that register convention exactly — it is the architectural
//! one, `src/arch/x86_64/linux_syscall.S` already implements the entry for it,
//! and reusing it means `PHIP-01` is a new *numbering and allowlist*, not new
//! assembly.
//!
//! It keeps nothing else. This is a separate surface with its own numbers, its
//! own allowlist, its own installed proof, and its own errno space, because
//! the measured Linux boundary earns its trust by never growing.

use core::arch::asm;

/// The version both sides check before anything else happens.
///
/// A mismatch refuses to start (R-3.2.10). Zero means unreleased: every number
/// below may still change, and no image built against it should be run on a
/// kernel that was built against a different one.
pub const ABI_VERSION: AbiVersion = AbiVersion(0);

/// The boundary's version.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct AbiVersion(pub u32);

/// The proposed call numbers.
///
/// Deliberately short. Each entry names the `PHIP-nn` capability that has to
/// exist in Phipia before it means anything, and the first three are the whole
/// of `PHIP-01`'s initial allowlist.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
#[repr(u64)]
pub enum Number {
    /// `PHIP-01`. Stop, with a status. Does not return.
    Exit = 0,
    /// `PHIP-01`. Write bytes to the kernel's console and serial transcript.
    ConsoleWrite = 1,
    /// `PHIP-05`. Nanoseconds from the kernel's one monotonic source.
    MonotonicNanoseconds = 2,
    /// `PHIP-03`. Map anonymous read-write, no-execute pages.
    MapAnonymous = 3,
    /// `PHIP-03`. Release pages a previous call mapped.
    UnmapAnonymous = 4,
    /// `PHIP-06`. Obtain a pixel surface of a fixed geometry.
    SurfaceAcquire = 5,
    /// `PHIP-06`. Ask the kernel to present a damage rectangle from it.
    SurfacePresent = 6,
    /// `PHIP-07`. Drain the bounded input event queue.
    InputDrain = 7,
}

/// A refusal from the kernel, as a negative result made positive.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Errno(pub i64);

/// Issue a call with no arguments.
///
/// # Safety
///
/// The caller must be running on a Phipia kernel that implements `PHIP-01` at
/// [`ABI_VERSION`], and `number` must be one this program is permitted to
/// make. On any other kernel the `syscall` instruction's effect is whatever
/// that kernel does with an unrecognised number, which is why nothing above
/// this crate calls it directly.
#[inline]
#[must_use]
pub unsafe fn call0(number: Number) -> i64 {
    let result: i64;
    // SAFETY: the caller guarantees the kernel implements this boundary. The
    // clobbers are the architectural ones for `syscall`: it destroys `rcx` and
    // `r11`, and the kernel may leave memory changed, so nothing is assumed
    // preserved across it.
    unsafe {
        asm!(
            "syscall",
            inlateout("rax") number as u64 => result,
            lateout("rcx") _,
            lateout("r11") _,
            options(nostack)
        );
    }
    result
}

/// Issue a call with one argument.
///
/// # Safety
///
/// As [`call0`], and `first` must satisfy whatever the named call requires of
/// it — a pointer argument must address memory this process owns, over its
/// complete range, for the whole call.
#[inline]
#[must_use]
pub unsafe fn call1(number: Number, first: u64) -> i64 {
    let result: i64;
    // SAFETY: as `call0`, with one argument in the register the boundary
    // names for it.
    unsafe {
        asm!(
            "syscall",
            inlateout("rax") number as u64 => result,
            in("rdi") first,
            lateout("rcx") _,
            lateout("r11") _,
            options(nostack)
        );
    }
    result
}

/// Issue a call with two arguments.
///
/// # Safety
///
/// As [`call1`], for both arguments.
#[inline]
#[must_use]
pub unsafe fn call2(number: Number, first: u64, second: u64) -> i64 {
    let result: i64;
    // SAFETY: as `call0`, with two arguments in the registers the boundary
    // names for them.
    unsafe {
        asm!(
            "syscall",
            inlateout("rax") number as u64 => result,
            in("rdi") first,
            in("rsi") second,
            lateout("rcx") _,
            lateout("r11") _,
            options(nostack)
        );
    }
    result
}

/// Stop this program.
///
/// # Safety
///
/// As [`call0`]. Does not return on a kernel that implements the boundary.
pub unsafe fn exit(status: i32) -> ! {
    loop {
        // SAFETY: the caller guarantees the boundary exists. The loop is not
        // reachable on a kernel that implements `Exit`; on one that does not,
        // spinning is the only correct thing left to do, because returning
        // would hand control to a caller that expects never to see it again.
        // The result is unreachable on a kernel that implements `Exit`, and
        // on one that does not there is nothing to do with it either.
        // The status crosses as its two's-complement bit pattern, which is
        // what the kernel's signed argument register holds.
        let status = u64::from(u32::from_ne_bytes(status.to_ne_bytes()));
        let _ = unsafe { call1(Number::Exit, status) };
    }
}
