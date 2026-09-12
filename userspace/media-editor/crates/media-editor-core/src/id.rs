// SPDX-License-Identifier: GPL-3.0-only
//! Generational identifiers.
//!
//! An identifier names a slot *and* the occupancy of that slot. When a clip is
//! deleted its slot's generation moves on, so every identifier that named it
//! stops resolving — by [`crate::CoreStatus::StaleIdentifier`], immediately, rather
//! than by quietly resolving to whatever was put there next.
//!
//! The type parameter is a marker only. A `Id<Clip>` cannot be passed where a
//! `Id<Sequence>` is expected, so a whole class of mix-up is a compile error.

use core::hash::{Hash, Hasher};
use core::marker::PhantomData;
use core::num::NonZeroU32;

/// A stable reference to a slot in a generational store.
pub struct Id<T> {
    index: u32,
    generation: NonZeroU32,
    marker: PhantomData<fn() -> T>,
}

impl<T> Id<T> {
    /// Build an identifier for a slot and generation.
    #[must_use]
    pub const fn new(index: u32, generation: NonZeroU32) -> Self {
        Self {
            index,
            generation,
            marker: PhantomData,
        }
    }

    /// The slot this names.
    #[must_use]
    pub const fn index(self) -> u32 {
        self.index
    }

    /// The occupancy this names.
    #[must_use]
    pub const fn generation(self) -> NonZeroU32 {
        self.generation
    }
}

// The derives would demand `T: Clone`, `T: PartialEq`, and so on, which is
// wrong: an identifier is two integers regardless of what it points at.

impl<T> Clone for Id<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for Id<T> {}

impl<T> PartialEq for Id<T> {
    fn eq(&self, other: &Self) -> bool {
        self.index == other.index && self.generation == other.generation
    }
}

impl<T> Eq for Id<T> {}

impl<T> Hash for Id<T> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.index.hash(state);
        self.generation.hash(state);
    }
}

impl<T> core::fmt::Debug for Id<T> {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(formatter, "#{}.{}", self.index, self.generation)
    }
}
