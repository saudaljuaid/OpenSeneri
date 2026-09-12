// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![allow(
    clippy::doc_markdown,
    reason = "Media Editor and Phipia are product names, not identifiers"
)]
//! Exact time, identity, and typed errors for Media Editor.
//!
//! This crate contains the dependency-free primitives used by higher layers:
//!
//! - [`Rational`], exact ratios of integers;
//! - [`Fixed`], fixed-point arithmetic with an integer `pow`, `log` and
//!   `sqrt`, so a power law is the same on every machine and needs no libm;
//! - [`Timebase`], a rate in ticks per second;
//! - [`Instant`] and [`Duration`], positions and lengths that carry their
//!   timebase and refuse to combine across one;
//! - [`TimeRange`], the half-open interval every range in Media Editor is;
//! - [`Timecode`], the label a frame wears, including drop-frame counting;
//! - [`Id`], a generational identifier that goes stale rather than dangling;
//! - [`Digest`], content identity by SHA-256;
//! - [`CoreStatus`], typed operation failures.
//!
//! The crate uses no floating point, allocation, or `unsafe`, and public
//! fallible operations return [`CoreStatus`].
//!
//! The crate is `no_std` when it is built for Phipia and links `std` only when
//! the host test harness needs it, which changes no behaviour (R-2.2).

pub mod digest;
pub mod fixed;
pub mod id;
pub mod range;
pub mod rational;
pub mod status;
pub mod time;
pub mod timebase;
pub mod timecode;

pub use digest::{Digest, Sha256};
pub use fixed::{FRACTION_BITS, Fixed, WIDE_BITS};
pub use id::Id;
pub use range::TimeRange;
pub use rational::Rational;
pub use status::{CoreStatus, Result};
pub use time::{Duration, Instant};
pub use timebase::Timebase;
pub use timecode::Timecode;
