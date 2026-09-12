// SPDX-License-Identifier: GPL-3.0-only
//! Saving, without ever being able to lose the last one.
//!
//! R-9.4 says a save is atomic and all-or-nothing, and that a save which is
//! interrupted leaves the previous file intact. This is the protocol that
//! makes that true rather than hoped for:
//!
//! 1. encode the project into bytes, in memory;
//! 2. write those bytes to the scratch slot;
//! 3. read the scratch slot back and compare it, byte for byte, with what was
//!    written;
//! 4. only then commit, which is the one step that changes the project slot.
//!
//! Steps one to three cannot touch the project slot at all, and step four is
//! the seam's single atomic operation. A failure at any point leaves the last
//! good project exactly where it was, and the test suite proves it by failing
//! at each step in turn.

use alloc::vec;
use alloc::vec::Vec;

use media_editor_abi::seam::{Slot, Storage};
use media_editor_core::Digest;
use media_editor_model::Project;

use crate::bytes::{Extent, Sink};
use crate::format;
use crate::status::{IoStatus, Result};

/// Write a project, and return the digest of the file that was committed.
///
/// # Errors
///
/// [`IoStatus::Seam`] for anything storage refuses,
/// [`IoStatus::WriteNotVerified`] if the bytes did not read back as
/// themselves, or an encoding refusal. In every case the project slot is
/// unchanged.
pub fn save(project: &Project, storage: &mut dyn Storage) -> Result<Digest> {
    let file = format::encode(project)?;
    storage.write(Slot::Scratch, &file)?;

    // Read it back before committing. A storage that accepted the write and
    // stored something else is exactly the failure this step exists to catch,
    // and it is cheap next to losing a day's work.
    let mut echoed = vec![0_u8; file.len()];
    let read = storage.read(Slot::Scratch, &mut echoed)?;
    if read != file.len() || echoed != file {
        return Err(IoStatus::WriteNotVerified);
    }

    storage.commit(Slot::Project)?;
    Ok(Digest::of(&file))
}

/// Read the committed project.
///
/// # Errors
///
/// [`IoStatus::Seam`] if there is nothing to read, or any decoding refusal.
pub fn load(storage: &dyn Storage) -> Result<Project> {
    let length = storage.len(Slot::Project)?;
    let mut file = Vec::new();
    file.try_reserve(length)
        .map_err(|_| IoStatus::OutOfMemory)?;
    file.resize(length, 0);
    let read = storage.read(Slot::Project, &mut file)?;
    if read != length {
        return Err(IoStatus::TruncatedPayload);
    }
    format::decode(&file)
}

/// The scratch slot, as something a recorder can extend.
///
/// The write-side counterpart of the read-side adapters a vault already has,
/// and the reason both exist: a format module should know about *bytes*, not
/// about slots, and a seam should know about slots and not about formats. This
/// is the sixteen lines in between.
///
/// It carries a length of its own rather than asking storage each time, and
/// that is a decision rather than a cache. [`Storage::len`] answers `Empty`
/// for a slot that has never been written, which is a perfectly good answer to
/// a different question; a recorder wants to know how much *it* has put there,
/// and the two agree only as long as nobody else is writing. Nobody else is —
/// but a number that would be wrong if they were is a number that should not
/// be read from somewhere else.
pub struct Scratch<'a> {
    storage: &'a mut dyn Storage,
    written: u64,
}

impl<'a> Scratch<'a> {
    /// Begin extending the scratch slot.
    ///
    /// **This does not empty it.** A caller that needs it empty says so by
    /// checking [`Sink::written`] — which is what [`crate::sprw::Winder`] does
    /// — because clearing it here would make "start a recording" a destructive
    /// act that looks like a constructor.
    pub fn new(storage: &'a mut dyn Storage) -> Self {
        let written = storage
            .len(Slot::Scratch)
            .map_or(0, |length| u64::try_from(length).unwrap_or(u64::MAX));
        Self { storage, written }
    }

    /// Empty the scratch slot, so that a recording starts from nothing.
    ///
    /// # Errors
    ///
    /// [`IoStatus::Seam`].
    pub fn clear(&mut self) -> Result<()> {
        self.storage.write(Slot::Scratch, &[])?;
        self.written = 0;
        Ok(())
    }
}

impl Sink for Scratch<'_> {
    fn written(&self) -> u64 {
        self.written
    }

    fn append(&mut self, bytes: &[u8]) -> Result<()> {
        self.storage.append(bytes)?;
        self.written = self
            .written
            .checked_add(u64::try_from(bytes.len()).map_err(|_| IoStatus::TooMany)?)
            .ok_or(IoStatus::TooMany)?;
        Ok(())
    }
}

/// The scratch slot, as something a reader can read.
///
/// So that what was just written can be read back and checked before it is
/// committed, which is step three of the protocol at the top of this module
/// and the only step a streaming save performs differently: it reads in
/// windows rather than in one piece, because the whole point is that one piece
/// would not fit.
pub struct Staged<'a> {
    storage: &'a dyn Storage,
    length: u64,
}

impl<'a> Staged<'a> {
    /// Read what is staged.
    ///
    /// # Errors
    ///
    /// [`IoStatus::Seam`] if the scratch slot holds nothing.
    pub fn new(storage: &'a dyn Storage) -> Result<Self> {
        let length = u64::try_from(storage.len(Slot::Scratch)?).map_err(|_| IoStatus::TooMany)?;
        Ok(Self { storage, length })
    }
}

impl Extent for Staged<'_> {
    fn length(&self) -> u64 {
        self.length
    }

    fn read_at(&self, offset: u64, into: &mut [u8]) -> Result<usize> {
        let at = usize::try_from(offset).unwrap_or(usize::MAX);
        Ok(self.storage.read_at(Slot::Scratch, at, into)?)
    }
}
