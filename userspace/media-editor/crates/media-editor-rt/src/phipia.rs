// SPDX-License-Identifier: GPL-3.0-only
//! The Phipia side of the seams.
//!
//! Each type here is the thin translation between a seam trait and a call
//! number. There is no buffering, no retry, and no policy: a seam that decides
//! anything has stopped being a boundary.

use media_editor_abi::seam::{Console, Result, SeamStatus, Time};
use media_editor_abi::syscall::{Number, call0, call2};

/// The most bytes one console call accepts.
///
/// The kernel copies from user memory over a validated range, so an unbounded
/// write would be an unbounded copy inside the kernel (R-1.1). Callers that
/// need more make more calls.
pub const CONSOLE_WRITE_MAX: usize = 4096;

/// The kernel's console and serial transcript.
#[derive(Clone, Copy, Debug, Default)]
pub struct PhipiaConsole;

impl PhipiaConsole {
    /// A handle to the console.
    #[must_use]
    pub const fn new() -> Self {
        Self
    }
}

impl Console for PhipiaConsole {
    fn write(&mut self, bytes: &[u8]) -> Result<()> {
        if bytes.is_empty() {
            return Ok(());
        }
        if bytes.len() > CONSOLE_WRITE_MAX {
            return Err(SeamStatus::TooLarge);
        }
        // SAFETY: the slice is live for the whole call, its address and length
        // describe exactly the bytes the kernel is being asked to read, and
        // `ConsoleWrite` reads and never writes through them.
        let written = unsafe {
            call2(
                Number::ConsoleWrite,
                bytes.as_ptr() as u64,
                bytes.len() as u64,
            )
        };
        if written < 0 {
            return Err(SeamStatus::Refused);
        }
        // A partial write is a refusal, not a state to resume from: the
        // boundary promises all or nothing (R-1.4).
        if usize::try_from(written).unwrap_or(0) == bytes.len() {
            Ok(())
        } else {
            Err(SeamStatus::Refused)
        }
    }
}

/// The kernel's one monotonic clock.
#[derive(Clone, Copy, Debug, Default)]
pub struct PhipiaClock;

impl PhipiaClock {
    /// A handle to the clock.
    #[must_use]
    pub const fn new() -> Self {
        Self
    }
}

impl Time for PhipiaClock {
    fn monotonic_nanoseconds(&self) -> Result<u64> {
        // SAFETY: the call takes no arguments and returns a scalar.
        let nanoseconds = unsafe { call0(Number::MonotonicNanoseconds) };
        u64::try_from(nanoseconds).map_err(|_| SeamStatus::Unavailable)
    }
}
