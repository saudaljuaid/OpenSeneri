// SPDX-License-Identifier: GPL-3.0-only
//! Storage in memory, with faults on demand.
//!
//! Every seam has two implementations: the Phipia one, and a deterministic one
//! the host suite drives. This is the second. It exists mostly so that R-9.4's
//! negative control is a test rather than a thought experiment: it can be told
//! to fail at each step of a save, and the project slot must survive all of
//! them.

use core::cell::Cell;

use alloc::vec::Vec;

use media_editor_abi::seam::{Result, SeamStatus, Slot, Storage};

/// Where a fault should happen.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Fault {
    /// Nothing goes wrong.
    #[default]
    None,
    /// The write to the scratch slot is refused.
    OnWrite,
    /// The write is accepted and stores something else, which the read-back
    /// step must catch.
    Corrupting,
    /// Reading the scratch slot back is refused.
    OnReadBack,
    /// The commit is refused, after everything else succeeded.
    OnCommit,
    /// The append at this position is accepted and stores something else.
    ///
    /// Distinct from [`Fault::Corrupting`], which damages *every* append
    /// including the last. A reel's last append is its digest, so a store that
    /// damaged everything would damage the digest too — and a file whose
    /// samples and trailer are both wrong is caught by comparing the trailer
    /// against what was computed, without anybody reading the samples. This
    /// damages one row and leaves the trailer alone, which is the only failure
    /// that a full rehash catches and a trailer comparison cannot.
    CorruptingAppend(usize),
    /// Reads of the scratch slot serve the vault's bytes instead.
    ///
    /// Not a disk fault: a *program* fault, and the one a verification step is
    /// most likely to have. A reader pointed at the wrong slot checks a
    /// perfectly sound file and says so, and if that file happens to be the
    /// same length as the one being staged then nothing about its structure or
    /// its own digest is wrong. Only "is this the reel I just wrote" catches
    /// it.
    ReadsTheWrongSlot,
    /// The append at this position is refused; the ones before it succeed.
    ///
    /// The fault a *streaming* write has and a whole-file write cannot: a
    /// drive that fills up on row four hundred, with three hundred and
    /// ninety-nine rows already on it. `OnWrite` refuses the only call there
    /// is; this refuses the four-hundredth of a thousand, which is where the
    /// interesting half of R-9.4 lives.
    OnAppend(usize),
}

/// Two extents and a swap, in memory.
#[derive(Clone, Debug)]
pub struct MemoryStorage {
    project: Option<Vec<u8>>,
    vault: Option<Vec<u8>>,
    scratch: Option<Vec<u8>>,
    capacity: usize,
    fault: Fault,
    /// How many whole-slot reads have been served.
    ///
    /// A [`Cell`] because the seam reads through a shared reference, which is
    /// right — reading a file does not change it — and a test double still has
    /// to be able to say what it was asked. There was a counter here before
    /// with the same name and no cell, so nothing could ever increment it and
    /// nothing ever read it: dead by R-15.6 and invisible because a number
    /// that stays at nought looks like a number.
    whole: Cell<usize>,
    /// How many ranged reads have been served.
    ranged: Cell<usize>,
    /// The largest single read served, of either kind.
    ///
    /// The number that decides whether a reader fits, which the other two do
    /// not: a path that performs a thousand small reads is a path that runs on
    /// a small machine, and one that performs a single large one is not.
    largest: Cell<usize>,
    commits: usize,
    /// How many appends have been served, refused ones included.
    appends: usize,
    /// The largest single append served.
    largest_append: usize,
}

impl MemoryStorage {
    /// Empty storage with a per-slot capacity.
    #[must_use]
    pub const fn new(capacity: usize) -> Self {
        Self {
            project: None,
            vault: None,
            scratch: None,
            capacity,
            fault: Fault::None,
            whole: Cell::new(0),
            ranged: Cell::new(0),
            largest: Cell::new(0),
            commits: 0,
            appends: 0,
            largest_append: 0,
        }
    }

    /// Arrange for the next save to fail in a particular way.
    pub fn set_fault(&mut self, fault: Fault) {
        self.fault = fault;
    }

    /// What the project slot holds, if anything.
    #[must_use]
    pub fn committed(&self) -> Option<&[u8]> {
        self.project.as_deref()
    }

    /// What the vault slot holds, if anything.
    #[must_use]
    pub fn stored(&self) -> Option<&[u8]> {
        self.vault.as_deref()
    }

    /// How many ranged reads have been served.
    ///
    /// Counted separately from whole-slot reads, because the point of a
    /// catalogue is that it never performs the second kind: a test that wants
    /// to prove a vault was read without ever being loaded asks this.
    #[must_use]
    pub fn ranged_reads(&self) -> usize {
        self.ranged.get()
    }

    /// The largest single read served since the last [`MemoryStorage::forget_reads`].
    ///
    /// What a test asks when it wants to know whether a reader **fits**. The
    /// counts above say how a reader behaved; this says what it needed at
    /// once, which is the number a seventy-six-kilobyte program cares about.
    #[must_use]
    pub fn largest_read(&self) -> usize {
        self.largest.get()
    }

    /// Forget every read counted so far.
    ///
    /// A test that wants to measure one path has to start from somewhere, and
    /// a save's own read-back is a whole-slot read that would otherwise be the
    /// largest thing in every measurement taken after it.
    pub fn forget_reads(&self) {
        self.whole.set(0);
        self.ranged.set(0);
        self.largest.set(0);
    }

    /// How many whole-slot reads have been served.
    #[must_use]
    pub fn whole_reads(&self) -> usize {
        self.whole.get()
    }

    /// How many commits have succeeded.
    #[must_use]
    pub const fn commits(&self) -> usize {
        self.commits
    }

    /// How many appends have been attempted.
    ///
    /// The write-side counterpart of [`MemoryStorage::ranged_reads`], and it
    /// answers the same kind of question: a recorder that appended once a row
    /// is a recorder that never held a frame, and the number is what says so.
    #[must_use]
    pub const fn appends(&self) -> usize {
        self.appends
    }

    /// The largest single append served.
    ///
    /// The write-side [`MemoryStorage::largest_read`], and it decides the same
    /// thing. A recorder that made a thousand appends of five thousand bytes
    /// fits on a machine mapped seventy-six kilobytes; one that made three
    /// appends of two megabytes does not, and the count alone would rank the
    /// second one better.
    #[must_use]
    pub const fn largest_append(&self) -> usize {
        self.largest_append
    }

    fn slot(&self, slot: Slot) -> Option<&Vec<u8>> {
        match slot {
            Slot::Project => self.project.as_ref(),
            Slot::Vault => self.vault.as_ref(),
            Slot::Scratch => self.scratch.as_ref(),
        }
    }
}

impl Storage for MemoryStorage {
    fn capacity(&self, _slot: Slot) -> usize {
        self.capacity
    }

    fn len(&self, slot: Slot) -> Result<usize> {
        self.slot(slot)
            .map_or(Err(SeamStatus::Empty), |bytes| Ok(bytes.len()))
    }

    fn read(&self, slot: Slot, into: &mut [u8]) -> Result<usize> {
        if self.fault == Fault::OnReadBack && slot == Slot::Scratch {
            return Err(SeamStatus::Refused);
        }
        let stored = self.slot(slot).ok_or(SeamStatus::Empty)?;
        if into.len() < stored.len() {
            return Err(SeamStatus::TooLarge);
        }
        into[..stored.len()].copy_from_slice(stored);
        self.whole.set(self.whole.get() + 1);
        self.largest.set(self.largest.get().max(stored.len()));
        Ok(stored.len())
    }

    fn write(&mut self, slot: Slot, bytes: &[u8]) -> Result<()> {
        if self.fault == Fault::OnWrite {
            return Err(SeamStatus::Refused);
        }
        if bytes.len() > self.capacity {
            return Err(SeamStatus::TooLarge);
        }
        let mut stored = Vec::new();
        stored
            .try_reserve(bytes.len())
            .map_err(|_| SeamStatus::Refused)?;
        stored.extend_from_slice(bytes);
        if self.fault == Fault::Corrupting {
            // One byte, in the middle, exactly as a bad sector would.
            if let Some(byte) = stored.get_mut(bytes.len() / 2) {
                *byte ^= 0x01;
            }
        }
        match slot {
            Slot::Project => self.project = Some(stored),
            Slot::Vault => self.vault = Some(stored),
            Slot::Scratch => self.scratch = Some(stored),
        }
        Ok(())
    }

    fn append(&mut self, bytes: &[u8]) -> Result<()> {
        let at = self.appends;
        self.appends += 1;
        if self.fault == Fault::OnWrite || self.fault == Fault::OnAppend(at) {
            return Err(SeamStatus::Refused);
        }
        // Started rather than refused when there is nothing there: a file that
        // does not exist and a file of no bytes are the same file to anything
        // that only ever extends one, and Phipia's own save protocol begins by
        // creating `STUTEMP.PHI`.
        let stored = self.scratch.get_or_insert_with(Vec::new);
        if stored.len() + bytes.len() > self.capacity {
            return Err(SeamStatus::TooLarge);
        }
        stored
            .try_reserve(bytes.len())
            .map_err(|_| SeamStatus::Refused)?;
        let was = stored.len();
        stored.extend_from_slice(bytes);
        self.largest_append = self.largest_append.max(bytes.len());
        if self.fault == Fault::Corrupting || self.fault == Fault::CorruptingAppend(at) {
            // The middle of *this* chunk rather than of the file, so that a
            // corruption lands wherever the stream happens to be -- which is
            // the whole difference between damaging a write and damaging a
            // save.
            if let Some(byte) = stored.get_mut(was + bytes.len() / 2) {
                *byte ^= 0x01;
            }
        }
        Ok(())
    }

    fn read_at(&self, slot: Slot, offset: usize, into: &mut [u8]) -> Result<usize> {
        if self.fault == Fault::OnReadBack && slot == Slot::Scratch {
            return Err(SeamStatus::Refused);
        }
        let asked = if self.fault == Fault::ReadsTheWrongSlot && slot == Slot::Scratch {
            Slot::Vault
        } else {
            slot
        };
        let stored = self.slot(asked).ok_or(SeamStatus::Empty)?;
        // Short at the end rather than refused, which is what Phipia's
        // `phipfs_read` does: "reads at EOF succeed with a short or zero byte
        // count". A seam that refused there would make every reader carry an
        // arm for a condition that is not an error.
        let from = stored.get(offset.min(stored.len())..).unwrap_or(&[]);
        let count = from.len().min(into.len());
        into[..count].copy_from_slice(&from[..count]);
        self.ranged.set(self.ranged.get() + 1);
        self.largest.set(self.largest.get().max(count));
        Ok(count)
    }

    fn commit(&mut self, into: Slot) -> Result<()> {
        if self.fault == Fault::OnCommit {
            return Err(SeamStatus::Refused);
        }
        // The scratch slot is refused in the match below rather than here.
        // There was a guard in both places and the one here could not be made
        // to fail: whatever it caught, the arm caught too. One refusal, one
        // place -- the same finding `Clip::with_ramp`'s timebase check
        // produced, and the same answer.
        let staged = self.scratch.take().ok_or(SeamStatus::Empty)?;
        match into {
            Slot::Project => self.project = Some(staged),
            Slot::Vault => self.vault = Some(staged),
            // It is where a save is assembled. An operation that made it its
            // own destination would be an operation with nothing to say about
            // what happened -- and it would consume the scratch, so the next
            // commit would find nothing.
            Slot::Scratch => return Err(SeamStatus::Refused),
        }
        self.commits += 1;
        Ok(())
    }
}
