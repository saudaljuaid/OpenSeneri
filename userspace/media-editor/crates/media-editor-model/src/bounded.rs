// SPDX-License-Identifier: GPL-3.0-only
//! Fallible growth.
//!
//! R-5.2 forbids the infallible allocating APIs: a failed allocation is a
//! typed refusal the caller handles, not an abort inside a container. These
//! two helpers are the only way anything in this crate grows a vector, and
//! each one also enforces the policy capacity its caller passes.

use alloc::vec::Vec;

use crate::status::{ModelStatus, Result};

/// Append a value, reserving first and refusing past a capacity.
pub(crate) fn push_bounded<T>(vector: &mut Vec<T>, value: T, capacity: usize) -> Result<()> {
    if vector.len() >= capacity {
        return Err(ModelStatus::CapacityExhausted);
    }
    vector
        .try_reserve(1)
        .map_err(|_| ModelStatus::OutOfMemory)?;
    vector.push(value);
    Ok(())
}

/// Insert a value at an index, reserving first and refusing past a capacity.
///
/// An index equal to the length appends; anything beyond it is refused rather
/// than clamped, because a caller that computed a wrong index wants to know.
pub(crate) fn insert_bounded<T>(
    vector: &mut Vec<T>,
    index: usize,
    value: T,
    capacity: usize,
) -> Result<()> {
    if index > vector.len() {
        return Err(ModelStatus::UnknownItem);
    }
    if vector.len() >= capacity {
        return Err(ModelStatus::CapacityExhausted);
    }
    vector
        .try_reserve(1)
        .map_err(|_| ModelStatus::OutOfMemory)?;
    vector.insert(index, value);
    Ok(())
}
