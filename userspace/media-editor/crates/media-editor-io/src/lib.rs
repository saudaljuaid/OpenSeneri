// SPDX-License-Identifier: GPL-3.0-only
#![cfg_attr(not(test), no_std)]
#![allow(
    clippy::doc_markdown,
    reason = "Media Editor and Phipia are product names, not identifiers"
)]
//! The project file, and the save that cannot lose the last one.
//!
//! Two things live here, and they are the two that decide whether a user ever
//! loses work: a format that refuses a damaged file instead of loading part of
//! it, and a save protocol whose failure modes all end with the previous file
//! still there.
//!
//! Both are written by hand. The format has no derive behind it and the save
//! has no library behind it, because both are custody of the user's work
//! (R-9.3, R-9.4) and neither is a place to be surprised by a default.

extern crate alloc;

pub mod bmp;
pub mod bytes;
pub mod conform;
pub mod cube;
pub mod edl;
pub mod format;
pub mod memory;
pub mod peaks;
pub mod phipia;
pub mod png;
pub mod save;
pub mod sprw;
pub mod status;
pub mod vault;
pub mod vtt;

pub use bytes::Extent;
pub use conform::{Conformed, LeftBehind};
pub use edl::{Channel, EditDecisionList, Event, Transition};
pub use format::{FORMAT_VERSION, HEADER_BYTES, MAGIC, MAX_PAYLOAD_BYTES, decode, encode};
pub use memory::{Fault, MemoryStorage};
pub use peaks::{decode as decode_summary, encode as encode_summary};
pub use save::{load, save};
pub use sprw::{Reel, Spool};
pub use status::{IoStatus, Result};
pub use vault::{Catalogue, Entry, Item, Material, Stacks, Vault};
