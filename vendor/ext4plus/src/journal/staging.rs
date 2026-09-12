// Copyright 2026 Phipia contributors
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! Bounded copy-on-write storage for one synchronous ext4 mutation attempt.

use super::transaction::{
    FILESYSTEM_SUPERBLOCK_START_BYTE, JOURNAL_BLOCK_BYTES,
    JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS, JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS,
    JournalBlockImage, JournalTransaction, JournalTransactionError,
};
use crate::error::BoxedError;
use crate::sync::RwLock;
use crate::{Ext4Read, Ext4Write};
use alloc::boxed::Box;
use alloc::collections::{BTreeMap, BTreeSet};
use alloc::vec;
use alloc::vec::Vec;
use core::error::Error;
use core::fmt::{self, Display, Formatter};

/// A refusal while creating or using a bounded mutation stage.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JournalMutationStageError {
    /// The declared filesystem byte geometry is zero or not 4 KiB aligned.
    Geometry,
    /// A read or write lies outside the declared filesystem byte geometry.
    Range,
    /// One mutation attempt exceeded the stage's transaction safety bound.
    TooManyBlocks,
    /// One mutation attempt freed more blocks than one revoke record permits.
    TooManyRevocations,
    /// The stage was sealed into an immutable transaction snapshot.
    Sealed,
}

impl Display for JournalMutationStageError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        let message = match self {
            Self::Geometry => "mutation stage geometry is invalid",
            Self::Range => "mutation stage byte range is invalid",
            Self::TooManyBlocks => "mutation stage block bound exceeded",
            Self::TooManyRevocations => "mutation stage revocation bound exceeded",
            Self::Sealed => "mutation stage is sealed",
        };
        formatter.write_str(message)
    }
}

impl Error for JournalMutationStageError {}

/// A refusal while classifying one immutable stage snapshot for JBD2.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JournalMutationPlanError {
    /// No upstream block image was captured for the requested transaction.
    EmptyStage,
    /// A block named as ordered file data is not present in the stage.
    OrderedDataNotStaged,
    /// The ordered file-data classification repeats one staged block.
    DuplicateOrderedData,
    /// A current staged image is also named as an older-image revocation.
    StagedBlockRevoked,
    /// A transaction snapshot has already sealed this stage.
    StageSealed,
    /// The resulting bounded JBD2 transaction is invalid.
    Transaction(JournalTransactionError),
}

impl Display for JournalMutationPlanError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        match self {
            Self::EmptyStage => formatter.write_str("mutation stage is empty"),
            Self::OrderedDataNotStaged => {
                formatter.write_str("ordered data block is not staged")
            }
            Self::DuplicateOrderedData => {
                formatter.write_str("ordered data classification repeats a block")
            }
            Self::StagedBlockRevoked => {
                formatter.write_str("staged mutation block is also revoked")
            }
            Self::StageSealed => formatter.write_str("mutation stage is already sealed"),
            Self::Transaction(error) => Display::fmt(error, formatter),
        }
    }
}

impl Error for JournalMutationPlanError {}

impl From<JournalTransactionError> for JournalMutationPlanError {
    fn from(error: JournalTransactionError) -> Self {
        Self::Transaction(error)
    }
}

#[cfg(feature = "sync")]
struct JournalMutationState {
    sealed: bool,
    blocks: BTreeMap<u64, Vec<u8>>,
    revoked_blocks: BTreeSet<u64>,
}

/// A bounded overlay that never writes through to its backing reader.
///
/// The first write to a block reads its complete 4 KiB home image, then every
/// partial write coalesces into that owned copy. Reads observe staged copies.
/// Successful transaction classification atomically seals the overlay against
/// further writes. The caller must discard both this stage and the `Ext4`
/// instance that used it after rollback because ext4plus also updates in-memory
/// allocation counters.
#[cfg(feature = "sync")]
pub struct JournalMutationStage {
    reader: Box<dyn Ext4Read>,
    filesystem_bytes: u64,
    state: RwLock<JournalMutationState>,
}

#[cfg(feature = "sync")]
impl JournalMutationStage {
    /// Create an empty stage over an immutable admitted-filesystem reader.
    pub fn new(
        reader: Box<dyn Ext4Read>,
        filesystem_bytes: u64,
    ) -> Result<Self, JournalMutationStageError> {
        if filesystem_bytes == 0 || filesystem_bytes % JOURNAL_BLOCK_BYTES as u64 != 0 {
            return Err(JournalMutationStageError::Geometry);
        }
        Ok(Self {
            reader,
            filesystem_bytes,
            state: RwLock::new(JournalMutationState {
                sealed: false,
                blocks: BTreeMap::new(),
                revoked_blocks: BTreeSet::new(),
            }),
        })
    }

    fn range_end(&self, start_byte: u64, length: usize) -> Result<u64, JournalMutationStageError> {
        let length = u64::try_from(length).map_err(|_| JournalMutationStageError::Range)?;
        let end = start_byte
            .checked_add(length)
            .ok_or(JournalMutationStageError::Range)?;
        if start_byte > self.filesystem_bytes || end > self.filesystem_bytes {
            return Err(JournalMutationStageError::Range);
        }
        Ok(end)
    }

    /// Return the number of complete block images currently staged.
    #[must_use]
    pub fn staged_block_count(&self) -> usize {
        self.state.read().blocks.len()
    }

    /// Return the number of distinct filesystem blocks revoked by this stage.
    #[must_use]
    pub fn revoked_block_count(&self) -> usize {
        self.state.read().revoked_blocks.len()
    }

    /// Return whether this stage owns no pending images or revocations.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        let state = self.state.read();
        state.blocks.is_empty() && state.revoked_blocks.is_empty()
    }

    /// Return whether classification has frozen this stage for execution.
    #[must_use]
    pub fn is_sealed(&self) -> bool {
        self.state.read().sealed
    }

    /// Snapshot every complete staged block image in ascending block order.
    #[must_use]
    pub fn staged_images(&self) -> Vec<JournalBlockImage> {
        self.state
            .read()
            .blocks
            .iter()
            .map(|(block_index, bytes)| {
                JournalBlockImage::from_staged(*block_index, bytes.clone())
            })
            .collect()
    }

    /// Classify one atomic stage snapshot into ordered data and metadata.
    ///
    /// Every named ordered-data block must occur exactly once in this stage.
    /// Every remaining staged image is journaled as metadata. The supplied
    /// transaction is cloned before classification, so a refusal cannot leave
    /// the caller's transaction partially populated. Callers must prevent new
    /// upstream writes until the returned transaction is durably resolved.
    pub fn build_transaction(
        &self,
        transaction: &JournalTransaction,
        ordered_data_blocks: &[u64],
    ) -> Result<JournalTransaction, JournalMutationPlanError> {
        let mut state = self.state.write();
        if state.sealed {
            return Err(JournalMutationPlanError::StageSealed);
        }
        if state.blocks.is_empty() {
            return Err(JournalMutationPlanError::EmptyStage);
        }
        for (index, block) in ordered_data_blocks.iter().enumerate() {
            if ordered_data_blocks[..index].contains(block) {
                return Err(JournalMutationPlanError::DuplicateOrderedData);
            }
            if !state.blocks.contains_key(block) {
                return Err(JournalMutationPlanError::OrderedDataNotStaged);
            }
        }
        let mut output = transaction.clone();
        for block in state.revoked_blocks.iter() {
            if !output.revokes_block(*block) {
                output.stage_revocation(*block)?;
            }
        }
        for (block, bytes) in state.blocks.iter() {
            if output.revokes_block(*block) {
                return Err(JournalMutationPlanError::StagedBlockRevoked);
            }
            if ordered_data_blocks.contains(block) {
                output.stage_ordered_data(*block, bytes)?;
            } else {
                output.stage_metadata(*block, bytes)?;
            }
        }
        state.sealed = true;
        Ok(output)
    }

    /// Discard every staged byte.
    ///
    /// The associated ext4plus filesystem object must be discarded too.
    pub fn rollback(&self) {
        let mut state = self.state.write();
        state.blocks.clear();
        state.revoked_blocks.clear();
        state.sealed = false;
    }
}

#[cfg(feature = "sync")]
impl Ext4Read for JournalMutationStage {
    fn read(&self, start_byte: u64, destination: &mut [u8]) -> Result<(), BoxedError> {
        self.range_end(start_byte, destination.len())
            .map_err(|error| Box::new(error) as BoxedError)?;
        let mut position = start_byte;
        let mut remaining = destination;
        while !remaining.is_empty() {
            let block_index = position / JOURNAL_BLOCK_BYTES as u64;
            let within = usize::try_from(position % JOURNAL_BLOCK_BYTES as u64)
                .map_err(|_| Box::new(JournalMutationStageError::Range) as BoxedError)?;
            let length = remaining.len().min(JOURNAL_BLOCK_BYTES - within);
            if let Some(image) = self.state.read().blocks.get(&block_index) {
                remaining[..length].copy_from_slice(&image[within..within + length]);
            } else {
                self.reader.read(position, &mut remaining[..length])?;
            }
            position += length as u64;
            remaining = &mut remaining[length..];
        }
        Ok(())
    }
}

#[cfg(feature = "sync")]
impl Ext4Write for JournalMutationStage {
    fn write(&self, start_byte: u64, source: &[u8]) -> Result<(), BoxedError> {
        self.range_end(start_byte, source.len())
            .map_err(|error| Box::new(error) as BoxedError)?;
        if !source.is_empty() && start_byte < FILESYSTEM_SUPERBLOCK_START_BYTE {
            return Err(Box::new(JournalMutationStageError::Range));
        }
        let mut position = start_byte;
        let mut remaining = source;
        let mut state = self.state.write();
        if state.sealed {
            return Err(Box::new(JournalMutationStageError::Sealed));
        }
        while !remaining.is_empty() {
            let block_index = position / JOURNAL_BLOCK_BYTES as u64;
            let block_start = block_index
                .checked_mul(JOURNAL_BLOCK_BYTES as u64)
                .ok_or_else(|| Box::new(JournalMutationStageError::Range) as BoxedError)?;
            let within = usize::try_from(position - block_start)
                .map_err(|_| Box::new(JournalMutationStageError::Range) as BoxedError)?;
            let length = remaining.len().min(JOURNAL_BLOCK_BYTES - within);
            if !state.blocks.contains_key(&block_index) {
                if state.blocks.len() >= JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS {
                    return Err(Box::new(JournalMutationStageError::TooManyBlocks));
                }
                let mut image = vec![0; JOURNAL_BLOCK_BYTES];
                self.reader.read(block_start, &mut image)?;
                state.blocks.insert(block_index, image);
            }
            let image = state
                .blocks
                .get_mut(&block_index)
                .ok_or_else(|| Box::new(JournalMutationStageError::Range) as BoxedError)?;
            image[within..within + length].copy_from_slice(&remaining[..length]);
            position += length as u64;
            remaining = &remaining[length..];
        }
        Ok(())
    }

    fn revoke_blocks(
        &self,
        start_block: u64,
        block_count: u32,
    ) -> Result<(), BoxedError> {
        let end = start_block
            .checked_add(u64::from(block_count))
            .ok_or_else(|| Box::new(JournalMutationStageError::Range) as BoxedError)?;
        let filesystem_blocks = self.filesystem_bytes / JOURNAL_BLOCK_BYTES as u64;
        if block_count == 0 || start_block == 0 || end > filesystem_blocks {
            return Err(Box::new(JournalMutationStageError::Range));
        }
        let mut state = self.state.write();
        if state.sealed {
            return Err(Box::new(JournalMutationStageError::Sealed));
        }
        let additions = (start_block..end)
            .filter(|block| !state.revoked_blocks.contains(block))
            .count();
        if state
            .revoked_blocks
            .len()
            .checked_add(additions)
            .is_none_or(|count| count > JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS)
        {
            return Err(Box::new(JournalMutationStageError::TooManyRevocations));
        }
        for block in start_block..end {
            state.blocks.remove(&block);
            state.revoked_blocks.insert(block);
        }
        Ok(())
    }
}
