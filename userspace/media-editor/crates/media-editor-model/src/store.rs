// SPDX-License-Identifier: GPL-3.0-only
//! A generational store.
//!
//! Removing an entry moves its slot's generation on, so every identifier that
//! named it stops resolving. A stale identifier is a named refusal, never a
//! silent reference to whatever occupies the slot now.

use alloc::vec::Vec;
use core::num::NonZeroU32;

use media_editor_core::{CoreStatus, Id};

use crate::bounded::push_bounded;
use crate::status::{ModelStatus, Result};

/// One slot, occupied or not, with the generation of its current occupancy.
#[derive(Clone, Debug, PartialEq, Eq)]
struct Slot<T> {
    generation: NonZeroU32,
    value: Option<T>,
}

/// A collection addressed by generational identifiers.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Store<T> {
    slots: Vec<Slot<T>>,
    capacity: usize,
    live: usize,
}

/// The first generation any slot carries.
const FIRST_GENERATION: NonZeroU32 = match NonZeroU32::new(1) {
    Some(one) => one,
    None => panic!("one is not zero"),
};

impl<T> Store<T> {
    /// An empty store that will refuse to grow past `capacity`.
    #[must_use]
    pub const fn new(capacity: usize) -> Self {
        Self {
            slots: Vec::new(),
            capacity,
            live: 0,
        }
    }

    /// How many entries are present.
    #[must_use]
    pub const fn len(&self) -> usize {
        self.live
    }

    /// Whether the store holds nothing.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.live == 0
    }

    /// Add an entry and return the identifier that names it.
    ///
    /// A freed slot is reused, at a new generation. Only when none is free
    /// does the store grow, and it refuses to grow past its capacity.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::CapacityExhausted`] or [`ModelStatus::OutOfMemory`].
    pub fn insert(&mut self, value: T) -> Result<Id<T>> {
        for (index, slot) in self.slots.iter_mut().enumerate() {
            if slot.value.is_none() {
                slot.value = Some(value);
                self.live += 1;
                let index = u32::try_from(index).map_err(|_| ModelStatus::CapacityExhausted)?;
                return Ok(Id::new(index, slot.generation));
            }
        }
        let index = u32::try_from(self.slots.len()).map_err(|_| ModelStatus::CapacityExhausted)?;
        push_bounded(
            &mut self.slots,
            Slot {
                generation: FIRST_GENERATION,
                value: Some(value),
            },
            self.capacity,
        )?;
        self.live += 1;
        Ok(Id::new(index, FIRST_GENERATION))
    }

    /// Borrow an entry.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping [`CoreStatus::StaleIdentifier`] if the
    /// slot has moved on, or [`CoreStatus::UnknownIdentifier`] if it is empty.
    pub fn get(&self, id: Id<T>) -> Result<&T> {
        let slot = self
            .slots
            .get(usize::try_from(id.index()).map_err(|_| CoreStatus::UnknownIdentifier)?)
            .ok_or(CoreStatus::UnknownIdentifier)?;
        if slot.generation != id.generation() {
            return Err(CoreStatus::StaleIdentifier.into());
        }
        slot.value
            .as_ref()
            .ok_or(CoreStatus::UnknownIdentifier.into())
    }

    /// Borrow an entry for modification.
    ///
    /// # Errors
    ///
    /// As [`Store::get`].
    pub fn get_mut(&mut self, id: Id<T>) -> Result<&mut T> {
        let slot = self
            .slots
            .get_mut(usize::try_from(id.index()).map_err(|_| CoreStatus::UnknownIdentifier)?)
            .ok_or(CoreStatus::UnknownIdentifier)?;
        if slot.generation != id.generation() {
            return Err(CoreStatus::StaleIdentifier.into());
        }
        slot.value
            .as_mut()
            .ok_or(CoreStatus::UnknownIdentifier.into())
    }

    /// Remove an entry, returning it, and retire every identifier that named
    /// it.
    ///
    /// # Errors
    ///
    /// As [`Store::get`].
    pub fn remove(&mut self, id: Id<T>) -> Result<T> {
        let index = usize::try_from(id.index()).map_err(|_| CoreStatus::UnknownIdentifier)?;
        let slot = self
            .slots
            .get_mut(index)
            .ok_or(CoreStatus::UnknownIdentifier)?;
        if slot.generation != id.generation() {
            return Err(CoreStatus::StaleIdentifier.into());
        }
        let value = slot.value.take().ok_or(CoreStatus::UnknownIdentifier)?;
        // Saturating rather than wrapping: a slot that somehow reached the top
        // of the generation space stops being reused instead of handing out an
        // identifier that a retired one would match.
        slot.generation = slot.generation.saturating_add(1);
        self.live -= 1;
        Ok(value)
    }

    /// Every live entry with its identifier, in slot order.
    pub fn iter(&self) -> impl Iterator<Item = (Id<T>, &T)> {
        self.slots.iter().enumerate().filter_map(|(index, slot)| {
            let value = slot.value.as_ref()?;
            let index = u32::try_from(index).ok()?;
            Some((Id::new(index, slot.generation), value))
        })
    }
}
