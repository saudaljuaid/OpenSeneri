// SPDX-License-Identifier: GPL-3.0-only
//! History.
//!
//! The journal holds pairs: what was done, and what undoes it. Undo applies
//! the second half of the last pair; redo applies the first half of the next.
//!
//! Every undo checks itself. Applying an inverse returns *its* inverse, which
//! must be the edit that was originally applied — if it is not, the model and
//! the history have diverged, and the journal says so by name instead of
//! carrying on with a history that no longer describes the project.

use alloc::vec::Vec;

use crate::bounded::push_bounded;
use crate::edit::Edit;
use crate::sequence::Sequence;
use crate::status::{ModelStatus, Result};

/// How many edits one journal remembers. A policy bound: past this the oldest
/// edits are dropped, which is a decision the editor should be told about
/// rather than a silent one, so reaching it is refused instead.
///
/// It is not the bound that bites on Phipia, and reading it as a promise of
/// four thousand undos would be reading it wrong. An entry is a pair of
/// edits, an edit is a little over three hundred bytes, and a program is
/// given nineteen mapped pages for everything it holds — so the address space
/// runs out somewhere around a hundred pairs, with the project and its caches
/// still to fit. What arrives there is [`ModelStatus::OutOfMemory`] from the
/// fallible reservation rather than [`ModelStatus::CapacityExhausted`] from
/// this constant, which is the difference between "you have edited enough"
/// and "there is no room", and the two deserve different words.
///
/// `tests/size.rs` verifies this bound against the platform memory limit.
pub const MAX_HISTORY: usize = 4096;

/// One applied edit and the edit that undoes it.
#[derive(Clone, Debug, PartialEq, Eq)]
struct Entry {
    forward: Edit,
    inverse: Edit,
}

/// An undo and redo history over one sequence.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct EditJournal {
    entries: Vec<Entry>,
    cursor: usize,
}

impl EditJournal {
    /// An empty history.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            entries: Vec::new(),
            cursor: 0,
        }
    }

    /// How many edits can be undone.
    #[must_use]
    pub const fn undo_depth(&self) -> usize {
        self.cursor
    }

    /// How many edits can be redone.
    #[must_use]
    pub fn redo_depth(&self) -> usize {
        self.entries.len() - self.cursor
    }

    /// Apply an edit and record it.
    ///
    /// Anything that had been undone is discarded, because a new edit made
    /// from an undone state is a new branch of history and this journal keeps
    /// one.
    ///
    /// # Errors
    ///
    /// Whatever the edit refuses, or [`ModelStatus::CapacityExhausted`]. On a
    /// refusal neither the sequence nor the history changes.
    pub fn apply(&mut self, sequence: &mut Sequence, edit: Edit) -> Result<()> {
        if self.cursor >= MAX_HISTORY {
            return Err(ModelStatus::CapacityExhausted);
        }
        self.entries.truncate(self.cursor);
        let inverse = edit.apply(sequence)?;
        push_bounded(
            &mut self.entries,
            Entry {
                forward: edit,
                inverse,
            },
            MAX_HISTORY,
        )?;
        self.cursor += 1;
        Ok(())
    }

    /// Undo the most recent edit.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NothingToDo`] if there is nothing to undo, or
    /// [`ModelStatus::HistoryInconsistent`] if the model no longer matches the
    /// history.
    pub fn undo(&mut self, sequence: &mut Sequence) -> Result<Edit> {
        let index = self.cursor.checked_sub(1).ok_or(ModelStatus::NothingToDo)?;
        let entry = self
            .entries
            .get(index)
            .ok_or(ModelStatus::NothingToDo)?
            .clone();
        let recovered = entry.inverse.apply(sequence)?;
        if recovered != entry.forward {
            return Err(ModelStatus::HistoryInconsistent);
        }
        self.cursor = index;
        Ok(entry.inverse)
    }

    /// Redo the edit that was most recently undone.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NothingToDo`] if there is nothing to redo, or
    /// [`ModelStatus::HistoryInconsistent`].
    pub fn redo(&mut self, sequence: &mut Sequence) -> Result<Edit> {
        let entry = self
            .entries
            .get(self.cursor)
            .ok_or(ModelStatus::NothingToDo)?
            .clone();
        let recovered = entry.forward.apply(sequence)?;
        if recovered != entry.inverse {
            return Err(ModelStatus::HistoryInconsistent);
        }
        self.cursor += 1;
        Ok(entry.forward)
    }

    /// Undo everything, returning how many edits were undone.
    ///
    /// # Errors
    ///
    /// As [`EditJournal::undo`].
    pub fn undo_all(&mut self, sequence: &mut Sequence) -> Result<usize> {
        let mut undone = 0;
        while self.cursor > 0 {
            self.undo(sequence)?;
            undone += 1;
        }
        Ok(undone)
    }

    /// Redo everything, returning how many edits were redone.
    ///
    /// # Errors
    ///
    /// As [`EditJournal::redo`].
    pub fn redo_all(&mut self, sequence: &mut Sequence) -> Result<usize> {
        let mut redone = 0;
        while self.cursor < self.entries.len() {
            self.redo(sequence)?;
            redone += 1;
        }
        Ok(redone)
    }
}
