// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_op_in_unsafe_fn)]
#![allow(
    clippy::doc_markdown,
    reason = "Media Editor and Phipia are product names, not identifiers"
)]
//! Where Media Editor meets Phipia.
//!
//! This is one of the two crates permitted to contain `unsafe` (R-3.1.4), and
//! it is deliberately the smallest thing that can be a boundary: the raw
//! syscall sequence, the proposed native call numbers, and the five seams
//! everything above it talks through.
//!
//! # Status
//!
//! Phipia 2.1.0 has **no generally stable native application ABI**. It does
//! have an experimental versioned native boundary — its own entry, its own
//! request and response records, authenticated process generations, checked
//! user ranges — and what that boundary carries is networking, time and
//! entropy rather than an application surface. Beside it is a measured Linux
//! compatibility boundary admitting a few profiled BusyBox programs, and
//! widening *that* to fit an application would destroy the property that makes
//! it trustworthy.
//!
//! So the numbers in [`syscall`] are still a *proposal*: they are `PHIP-01` in
//! `docs/PLATFORM_CONTRACT.md`, written down here in the shape Media Editor needs
//! so that the kernel work has something exact to implement against. The
//! encouraging part is that Phipia has since built a boundary of very nearly
//! this shape for another purpose, so the pattern is proven rather than
//! hypothetical.
//!
//! Until those numbers exist, an image built from this crate is a conforming
//! ELF that the kernel will refuse to run — which is the honest state of
//! affairs, and is why [`seam`] exists: everything above this crate is written
//! against traits, and the host test suite supplies its own implementations of
//! them.

pub mod seam;
pub mod syscall;

pub use seam::{Console, Slot, Storage, Time};
pub use syscall::{ABI_VERSION, AbiVersion};
