// Copyright 2026 Phipia contributors
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

//! Bounded JBD2 transaction images and an ordered-data commit plan.
//!
//! This module deliberately stops below ext4 namespace mutation. It provides
//! checked descriptor/data/revoke/commit serialization, explicit durability
//! barriers, replay validation for one transaction or a bounded live ring,
//! discovery of the internal journal inode, and allocation within a
//! clean-journal ring. A mapped ring adds the checksummed live-superblock and
//! clean/tail-superblock writes required around each commit. A caller must
//! execute the returned operations in order and acknowledge durability only
//! after the corresponding flush.

use super::block_header::{JournalBlockHeader, JournalBlockType};
use super::commit_block::validate_commit_block_checksum;
use super::descriptor_block::{DescriptorBlockTagIter, validate_descriptor_block_checksum};
use super::revocation_block::{read_revocation_block_table, validate_revocation_block_checksum};
use super::superblock::JournalSuperblock;
use crate::Ext4;
use crate::checksum::Checksum;
use crate::error::Ext4Error;
use crate::inode::Inode;
#[cfg(not(feature = "sync"))]
use crate::iters::AsyncIterator;
use crate::iters::file_blocks::FileBlocks;
use crate::superblock::Superblock;
use crate::util::{read_u32be, read_u32le, write_u32le};
use crate::uuid::Uuid;
use alloc::collections::{BTreeMap, VecDeque};
use alloc::vec;
use alloc::vec::Vec;
use core::error::Error;
use core::fmt::{self, Display, Formatter};

/// The only journal block size admitted by Phipia's ext4 profile.
pub const JOURNAL_BLOCK_BYTES: usize = 4096;

/// A deliberately small upper bound for one transaction's metadata images.
pub const JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS: usize = 64;

/// The maximum number of block revocations represented by one transaction.
pub const JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS: usize = 64;

/// The maximum clean-journal data slots admitted by the bounded ring planner.
pub const JOURNAL_RING_MAX_SLOTS: usize = 8192;

/// The byte size of the JBD2 superblock admitted by the public ring boundary.
pub const JOURNAL_SUPERBLOCK_BYTES: usize = 1024;

/// The exact byte size of the ext4 primary superblock.
pub const FILESYSTEM_SUPERBLOCK_BYTES: usize = 1024;

/// The byte offset of the ext4 primary superblock on a 4 KiB filesystem.
pub const FILESYSTEM_SUPERBLOCK_START_BYTE: u64 = 1024;

const FILESYSTEM_SUPERBLOCK_OFFSET_IN_BLOCK: usize = 1024;
const JOURNAL_SUPERBLOCK_BLOCK_SIZE_OFFSET: usize = 0x0c;
const JOURNAL_SUPERBLOCK_MAX_LENGTH_OFFSET: usize = 0x10;
const JOURNAL_SUPERBLOCK_FIRST_BLOCK_OFFSET: usize = 0x14;
const JOURNAL_SUPERBLOCK_SEQUENCE_OFFSET: usize = 0x18;
const JOURNAL_SUPERBLOCK_START_BLOCK_OFFSET: usize = 0x1c;
const JOURNAL_SUPERBLOCK_FEATURE_INCOMPAT_OFFSET: usize = 0x28;
const JOURNAL_SUPERBLOCK_UUID_OFFSET: usize = 0x30;
const JOURNAL_SUPERBLOCK_USER_COUNT_OFFSET: usize = 0x40;
const JOURNAL_SUPERBLOCK_CHECKSUM_TYPE_OFFSET: usize = 0x50;
const JOURNAL_SUPERBLOCK_CHECKSUM_OFFSET: usize = 0xfc;
const JOURNAL_FEATURE_BLOCK_REVOCATIONS: u32 = 0x1;
const JOURNAL_FEATURE_64_BIT: u32 = 0x2;
const JOURNAL_FEATURE_CHECKSUM_V3: u32 = 0x10;
const JOURNAL_REQUIRED_FEATURES: u32 = JOURNAL_FEATURE_64_BIT | JOURNAL_FEATURE_CHECKSUM_V3;
const JOURNAL_ALLOWED_FEATURES: u32 = JOURNAL_REQUIRED_FEATURES | JOURNAL_FEATURE_BLOCK_REVOCATIONS;
const JOURNAL_CHECKSUM_TYPE_CRC32C: u8 = 4;
const FILESYSTEM_INCOMPAT_FEATURES_OFFSET: usize = 0x60;
const FILESYSTEM_READ_ONLY_FEATURES_OFFSET: usize = 0x64;
const FILESYSTEM_CHECKSUM_OFFSET: usize = 0x3fc;
const FILESYSTEM_RECOVERY_FEATURE: u32 = 0x4;
const FILESYSTEM_METADATA_CHECKSUM_FEATURE: u32 = 0x400;

/// A checked block image owned by a transaction or replay result.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct JournalBlockImage {
    block_index: u64,
    bytes: Vec<u8>,
}

impl JournalBlockImage {
    #[cfg(feature = "sync")]
    pub(super) fn from_staged(block_index: u64, bytes: Vec<u8>) -> Self {
        debug_assert_eq!(bytes.len(), JOURNAL_BLOCK_BYTES);
        Self { block_index, bytes }
    }

    /// Return the absolute filesystem block targeted by this image.
    #[must_use]
    pub fn block_index(&self) -> u64 {
        self.block_index
    }

    /// Return the complete 4 KiB block image.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
}

/// The durability boundary represented by a commit-plan flush operation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JournalFlush {
    /// Ext4's checksummed incompat-recovery marker is durable.
    FilesystemState,
    /// Ordered file data must be durable before journal metadata is committed.
    OrderedData,
    /// Descriptor and journaled metadata must be durable before the commit.
    JournalPayload,
    /// The checksummed commit record must be durable before home metadata.
    Commit,
    /// Checkpointed home metadata must be durable before reclaiming the log.
    Checkpoint,
    /// The clean or advanced journal tail must be durable before slot reuse.
    JournalState,
}

/// The kind of a block written into the journal.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JournalRecordKind {
    /// A JBD2 descriptor containing checksummed home-block tags.
    Descriptor,
    /// A metadata image named by the corresponding descriptor tag.
    Metadata,
    /// A checksummed list of home blocks whose older images must not replay.
    Revocation,
    /// The checksummed record that makes the transaction replayable.
    Commit,
}

/// One operation in a strictly ordered JBD2 commit plan.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum JournalCommitOperation {
    /// Write the complete checksummed ext4 primary superblock.
    WriteFilesystemSuperblock {
        /// Absolute byte offset of the primary ext4 superblock.
        start_byte: u64,
        /// Complete 1,024-byte ext4 superblock image.
        image: FilesystemSuperblockImage,
    },
    /// Write file data to its final block before committing related metadata.
    WriteOrderedData(JournalBlockImage),
    /// Persist every write preceding this operation.
    Flush(JournalFlush),
    /// Write one descriptor, metadata, or commit image into the journal.
    WriteJournal {
        /// Absolute filesystem block belonging to the journal inode.
        journal_block: u64,
        /// Semantic type of this journal record.
        kind: JournalRecordKind,
        /// Complete 4 KiB record image.
        bytes: Vec<u8>,
    },
    /// Persist the checksummed 1,024-byte state at journal logical block zero.
    WriteJournalSuperblock {
        /// Absolute filesystem block containing journal logical block zero.
        journal_block: u64,
        /// Complete checksummed JBD2 superblock image.
        image: JournalSuperblockImage,
    },
    /// Checkpoint committed metadata to its final home block.
    WriteHomeMetadata(JournalBlockImage),
}

/// Synchronous storage used to execute an already validated journal plan.
///
/// Implementations must not reorder writes across [`JournalStorage::flush`].
/// A successful flush must make every earlier write durable according to the
/// backing device's persistence contract.
pub trait JournalStorage {
    /// The platform-specific write or flush failure.
    type Error;

    /// Write every byte at the supplied absolute filesystem offset.
    fn write(&mut self, start_byte: u64, bytes: &[u8]) -> Result<(), Self::Error>;

    /// Make all preceding writes durable at the named protocol boundary.
    fn flush(&mut self, boundary: JournalFlush) -> Result<(), Self::Error>;
}

/// A failure while executing an ordered journal plan.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum JournalExecutionError<E> {
    /// A filesystem-block address could not be represented as a byte offset.
    AddressOverflow,
    /// The platform storage implementation refused a write or flush.
    Storage(E),
}

impl<E: Display> Display for JournalExecutionError<E> {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        match self {
            Self::AddressOverflow => formatter.write_str("journal byte address overflowed"),
            Self::Storage(error) => write!(formatter, "journal storage failed: {error}"),
        }
    }
}

impl<E: Error + 'static> Error for JournalExecutionError<E> {}

/// Execute a validated journal plan exactly in the order it was constructed.
///
/// Journal and home-block addresses are converted from 4 KiB filesystem
/// blocks with checked arithmetic. Flush operations are passed through without
/// coalescing so the platform cannot erase a protocol durability boundary.
pub fn execute_commit_operations<S: JournalStorage>(
    storage: &mut S,
    operations: &[JournalCommitOperation],
) -> Result<(), JournalExecutionError<S::Error>> {
    for operation in operations {
        match operation {
            JournalCommitOperation::WriteFilesystemSuperblock { start_byte, image } => storage
                .write(*start_byte, image.bytes())
                .map_err(JournalExecutionError::Storage)?,
            JournalCommitOperation::WriteOrderedData(image)
            | JournalCommitOperation::WriteHomeMetadata(image) => {
                let start_byte = image
                    .block_index()
                    .checked_mul(JOURNAL_BLOCK_BYTES as u64)
                    .ok_or(JournalExecutionError::AddressOverflow)?;
                storage
                    .write(start_byte, image.bytes())
                    .map_err(JournalExecutionError::Storage)?;
            }
            JournalCommitOperation::Flush(boundary) => storage
                .flush(*boundary)
                .map_err(JournalExecutionError::Storage)?,
            JournalCommitOperation::WriteJournal {
                journal_block,
                bytes,
                ..
            } => {
                let start_byte = journal_block
                    .checked_mul(JOURNAL_BLOCK_BYTES as u64)
                    .ok_or(JournalExecutionError::AddressOverflow)?;
                storage
                    .write(start_byte, bytes)
                    .map_err(JournalExecutionError::Storage)?;
            }
            JournalCommitOperation::WriteJournalSuperblock {
                journal_block,
                image,
            } => {
                let start_byte = journal_block
                    .checked_mul(JOURNAL_BLOCK_BYTES as u64)
                    .ok_or(JournalExecutionError::AddressOverflow)?;
                storage
                    .write(start_byte, image.bytes())
                    .map_err(JournalExecutionError::Storage)?;
            }
        }
    }
    Ok(())
}

/// A refusal produced while constructing or validating a transaction.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum JournalTransactionError {
    /// Sequence `u32::MAX` cannot be followed by another checked transaction.
    SequenceOverflow,
    /// The transaction has no metadata image to journal.
    EmptyTransaction,
    /// A transaction exceeded the fixed metadata or ordered-data bound.
    TooManyBlocks,
    /// A supplied image was not exactly one Phipia filesystem block.
    IncorrectBlockLength,
    /// A home or journal block lies beyond the admitted filesystem geometry.
    BlockOutOfRange,
    /// Two images or journal slots name the same physical block.
    DuplicateBlock,
    /// The journal slot list does not exactly fit descriptor, data, and commit.
    JournalSlotCount,
    /// A journal slot overlaps a staged home block.
    JournalSlotOverlap,
    /// A metadata image needs JBD2 magic escaping, which is not implemented.
    EscapedBlockUnsupported,
    /// A descriptor is missing, malformed, or fails its checksum.
    CorruptDescriptor,
    /// A journaled metadata image fails its descriptor-tag checksum.
    CorruptData,
    /// A complete, checksummed commit record is not present.
    MissingCommit,
    /// The commit header, sequence, or checksum is invalid.
    CorruptCommit,
    /// A revocation header, table, sequence, or checksum is invalid.
    CorruptRevocation,
    /// A journal superblock image is not exactly 1,024 bytes.
    CorruptSuperblock,
    /// The JBD2 magic is absent from the journal superblock header.
    CorruptSuperblockMagic,
    /// The journal superblock type is not the admitted JBD2 v2 type.
    UnsupportedSuperblockType(u32),
    /// Required JBD2 incompatible feature bits are absent.
    MissingSuperblockFeatures(u32),
    /// JBD2 incompatible feature bits outside the admitted profile are present.
    UnsupportedSuperblockFeatures(u32),
    /// The journal superblock checksum algorithm is not CRC32C.
    UnsupportedSuperblockChecksumType(u8),
    /// The journal superblock's stored CRC32C does not match its contents.
    CorruptSuperblockChecksum {
        /// CRC32C stored in the on-disk journal superblock.
        stored: u32,
        /// CRC32C calculated over the admitted superblock image.
        calculated: u32,
    },
    /// A clean-only operation was requested for a live journal.
    JournalNotClean,
    /// The ext4 recovery bit and the JBD2 live-start field do not prove one state.
    RecoveryStateMismatch,
    /// A transaction requested revokes without the journal feature bit.
    RevocationsUnsupported,
    /// The clean-journal ring geometry is too small, too large, or malformed.
    RingGeometry,
    /// The ring has insufficient uncheckpointed space for a transaction.
    RingFull,
    /// A transaction was prepared with a stale sequence or different geometry.
    RingTransactionMismatch,
    /// A reservation ticket is unknown or no longer active.
    ReservationUnknown,
    /// A reservation transition was requested from the wrong durability state.
    ReservationState,
    /// Commit, abort, or checkpoint order would reclaim live journal records.
    ReservationOrder,
    /// The monotonic reservation ticket would overflow.
    ReservationOverflow,
    /// A durable plan was requested from a ring without a mapped superblock.
    JournalStateUnavailable,
    /// A real ext4 ring has no validated filesystem-superblock state.
    FilesystemStateUnavailable,
    /// The first journal commit was requested before the recovery marker flush.
    RecoveryMarkerNotDurable,
}

impl Display for JournalTransactionError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        let message = match self {
            Self::SequenceOverflow => "journal sequence would overflow",
            Self::EmptyTransaction => "journal transaction has no metadata",
            Self::TooManyBlocks => "journal transaction block bound exceeded",
            Self::IncorrectBlockLength => "journal image is not 4096 bytes",
            Self::BlockOutOfRange => "journal block lies outside the filesystem",
            Self::DuplicateBlock => "journal transaction repeats a block",
            Self::JournalSlotCount => "journal slot count does not fit the transaction",
            Self::JournalSlotOverlap => "journal and home blocks overlap",
            Self::EscapedBlockUnsupported => "journal magic escaping is unsupported",
            Self::CorruptDescriptor => "journal descriptor is corrupt",
            Self::CorruptData => "journal metadata image is corrupt",
            Self::MissingCommit => "durable journal commit is missing",
            Self::CorruptCommit => "journal commit is corrupt",
            Self::CorruptRevocation => "journal revocation record is corrupt",
            Self::CorruptSuperblock => "journal superblock is not exactly 1024 bytes",
            Self::CorruptSuperblockMagic => "journal superblock magic is invalid",
            Self::UnsupportedSuperblockType(_) => "journal superblock type is unsupported",
            Self::MissingSuperblockFeatures(_) => {
                "journal superblock is missing required incompatible features"
            }
            Self::UnsupportedSuperblockFeatures(_) => {
                "journal superblock advertises unsupported incompatible features"
            }
            Self::UnsupportedSuperblockChecksumType(_) => {
                "journal superblock checksum type is unsupported"
            }
            Self::CorruptSuperblockChecksum { .. } => "journal superblock checksum is invalid",
            Self::JournalNotClean => "journal contains a live transaction",
            Self::RecoveryStateMismatch => {
                "ext4 and JBD2 recovery state do not prove a supported journal state"
            }
            Self::RevocationsUnsupported => "journal does not advertise block revocations",
            Self::RingGeometry => "journal ring geometry is invalid",
            Self::RingFull => "journal ring has insufficient free slots",
            Self::RingTransactionMismatch => "journal transaction does not match the ring",
            Self::ReservationUnknown => "journal reservation is not active",
            Self::ReservationState => "journal reservation state transition is invalid",
            Self::ReservationOrder => "journal reservation order is invalid",
            Self::ReservationOverflow => "journal reservation ticket would overflow",
            Self::JournalStateUnavailable => "journal ring has no mapped checksummed superblock",
            Self::FilesystemStateUnavailable => {
                "journal ring has no validated ext4 superblock state"
            }
            Self::RecoveryMarkerNotDurable => {
                "ext4 recovery marker is not durably recorded"
            }
        };
        formatter.write_str(message)
    }
}

/// A checksum-validated JBD2 superblock for Phipia's bounded 4 KiB profile.
///
/// The image preserves every admitted byte when its sequence/start state is
/// changed. Phipia admits logical journal data beginning at block one; callers
/// must supply the complete physical journal-inode map, including logical block
/// zero, before a clean ring can be constructed.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct JournalSuperblockImage {
    bytes: Vec<u8>,
    maximum_length: u32,
    sequence: u32,
    start_block: u32,
    uuid: Uuid,
    block_revocations: bool,
}

/// A complete checksummed ext4 primary-superblock state image.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FilesystemSuperblockImage {
    bytes: [u8; FILESYSTEM_SUPERBLOCK_BYTES],
    needs_recovery: bool,
}

impl FilesystemSuperblockImage {
    fn from_superblock(superblock: &Superblock, needs_recovery: bool) -> Self {
        Self {
            bytes: superblock.recovery_state_image(needs_recovery),
            needs_recovery,
        }
    }

    /// Return the complete 1,024-byte primary-superblock image.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    /// Return whether this image carries ext4's incompat-recovery bit.
    #[must_use]
    pub fn needs_recovery(&self) -> bool {
        self.needs_recovery
    }

    /// Produce the requested recovery state while preserving every other byte.
    #[must_use]
    pub fn with_recovery_state(&self, needs_recovery: bool) -> Self {
        let mut bytes = self.bytes;
        let mut incompat = read_u32le(&bytes, FILESYSTEM_INCOMPAT_FEATURES_OFFSET);
        if needs_recovery {
            incompat |= FILESYSTEM_RECOVERY_FEATURE;
        } else {
            incompat &= !FILESYSTEM_RECOVERY_FEATURE;
        }
        write_u32le(
            &mut bytes,
            FILESYSTEM_INCOMPAT_FEATURES_OFFSET,
            incompat,
        );
        if read_u32le(&bytes, FILESYSTEM_READ_ONLY_FEATURES_OFFSET)
            & FILESYSTEM_METADATA_CHECKSUM_FEATURE
            != 0
        {
            let mut checksum = Checksum::new();
            checksum.update(&bytes[..FILESYSTEM_CHECKSUM_OFFSET]);
            write_u32le(
                &mut bytes,
                FILESYSTEM_CHECKSUM_OFFSET,
                checksum.finalize(),
            );
        }
        Self {
            bytes,
            needs_recovery,
        }
    }

    fn from_checkpointed_home_block(
        current: &Self,
        image: &JournalBlockImage,
    ) -> Result<Self, JournalTransactionError> {
        if image.block_index() != 0 || image.bytes().len() != JOURNAL_BLOCK_BYTES {
            return Err(JournalTransactionError::IncorrectBlockLength);
        }
        if !current.needs_recovery() {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }
        let bytes: [u8; FILESYSTEM_SUPERBLOCK_BYTES] = image.bytes()
            [FILESYSTEM_SUPERBLOCK_OFFSET_IN_BLOCK
                ..FILESYSTEM_SUPERBLOCK_OFFSET_IN_BLOCK + FILESYSTEM_SUPERBLOCK_BYTES]
            .try_into()
            .map_err(|_| JournalTransactionError::IncorrectBlockLength)?;
        let needs_recovery = read_u32le(&bytes, FILESYSTEM_INCOMPAT_FEATURES_OFFSET)
            & FILESYSTEM_RECOVERY_FEATURE
            != 0;
        let candidate = Self {
            bytes,
            needs_recovery,
        };
        current.validate_checkpointed_successor(&candidate)?;
        Ok(candidate)
    }

    fn validate_checkpointed_successor(
        &self,
        candidate: &Self,
    ) -> Result<(), JournalTransactionError> {
        if !self.needs_recovery() || !candidate.needs_recovery() {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }

        // ext4plus mutations currently change only the allocation counters.
        // Require every other byte to match the admitted, recovery-marked
        // image so a staged metadata block cannot silently replace filesystem
        // identity, geometry, features, or policy fields.
        for (index, (before, after)) in self
            .bytes
            .iter()
            .zip(candidate.bytes.iter())
            .enumerate()
        {
            let mutable_counter = (0x0c..0x14).contains(&index) || (0x158..0x15c).contains(&index);
            let checksum = (FILESYSTEM_CHECKSUM_OFFSET..FILESYSTEM_SUPERBLOCK_BYTES)
                .contains(&index);
            if !mutable_counter && !checksum && before != after {
                return Err(JournalTransactionError::RecoveryStateMismatch);
            }
        }
        if read_u32le(&candidate.bytes, FILESYSTEM_READ_ONLY_FEATURES_OFFSET)
            & FILESYSTEM_METADATA_CHECKSUM_FEATURE
            != 0
        {
            let mut checksum = Checksum::new();
            checksum.update(&candidate.bytes[..FILESYSTEM_CHECKSUM_OFFSET]);
            if read_u32le(&candidate.bytes, FILESYSTEM_CHECKSUM_OFFSET) != checksum.finalize() {
                return Err(JournalTransactionError::RecoveryStateMismatch);
            }
        }
        let blocks = u64::from(read_u32le(&candidate.bytes, 0x04))
            | (u64::from(read_u32le(&candidate.bytes, 0x150)) << 32);
        let free_blocks = u64::from(read_u32le(&candidate.bytes, 0x0c))
            | (u64::from(read_u32le(&candidate.bytes, 0x158)) << 32);
        let inodes = read_u32le(&candidate.bytes, 0x00);
        let free_inodes = read_u32le(&candidate.bytes, 0x10);
        if free_blocks > blocks || free_inodes > inodes {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }
        Ok(())
    }
}

/// A failure while discovering the journal inode from an admitted filesystem.
#[derive(Debug)]
pub enum JournalInodeMapError {
    /// The ext4 profile has no internal journal inode.
    MissingJournal,
    /// Ext4 metadata or block I/O failed validation.
    Filesystem(Ext4Error),
    /// The JBD2 superblock or physical map failed the bounded ring profile.
    Transaction(JournalTransactionError),
}

impl Display for JournalInodeMapError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        match self {
            Self::MissingJournal => formatter.write_str("filesystem has no internal journal"),
            Self::Filesystem(error) => write!(formatter, "journal inode discovery failed: {error}"),
            Self::Transaction(error) => write!(formatter, "journal ring admission failed: {error}"),
        }
    }
}

impl Error for JournalInodeMapError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::MissingJournal => None,
            Self::Filesystem(error) => Some(error),
            Self::Transaction(error) => Some(error),
        }
    }
}

impl From<Ext4Error> for JournalInodeMapError {
    fn from(error: Ext4Error) -> Self {
        Self::Filesystem(error)
    }
}

impl From<JournalTransactionError> for JournalInodeMapError {
    fn from(error: JournalTransactionError) -> Self {
        Self::Transaction(error)
    }
}

/// A complete, bounded physical map of one internal journal inode.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct JournalInodeMap {
    superblock: JournalSuperblockImage,
    filesystem_superblock: FilesystemSuperblockImage,
    maximum_block: u64,
    physical_blocks: Vec<u64>,
    filesystem_needs_recovery: bool,
}

/// A bounded replay result for every committed transaction in one journal ring.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct JournalRecovery {
    committed_transactions: usize,
    consumed_slots: usize,
    replay_images: Vec<JournalBlockImage>,
    clean_superblock: JournalSuperblockImage,
    maximum_block: u64,
}

impl JournalRecovery {
    /// Return the number of consecutive committed transactions admitted.
    #[must_use]
    pub fn committed_transactions(&self) -> usize {
        self.committed_transactions
    }

    /// Return the number of logical journal data slots consumed by replay.
    #[must_use]
    pub fn consumed_slots(&self) -> usize {
        self.consumed_slots
    }

    /// Return the final metadata images to checkpoint to their home blocks.
    #[must_use]
    pub fn replay_images(&self) -> &[JournalBlockImage] {
        &self.replay_images
    }

    /// Return the checksummed clean state to persist after checkpointing.
    #[must_use]
    pub fn clean_superblock(&self) -> &JournalSuperblockImage {
        &self.clean_superblock
    }

    /// Build the ordered mount-recovery checkpoint and cleanup plan.
    ///
    /// Home metadata is flushed before the clean journal state is written.
    /// The journal state is then flushed before ext4's recovery marker is
    /// cleared and independently flushed. When replay updates the primary
    /// ext4 superblock, the clean marker is derived from that validated replay
    /// image rather than the stale mount-time image. This mirrors the
    /// separation Linux keeps between recovery replay, journal cleanup, and
    /// marking ext4 clean.
    pub fn checkpoint_plan(
        &self,
        journal_superblock_block: u64,
        filesystem_superblock: &FilesystemSuperblockImage,
    ) -> Result<Vec<JournalCommitOperation>, JournalTransactionError> {
        if !filesystem_superblock.needs_recovery() {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }
        if journal_superblock_block == 0 || journal_superblock_block > self.maximum_block {
            return Err(JournalTransactionError::BlockOutOfRange);
        }
        let capacity = self
            .replay_images
            .len()
            .checked_add(5)
            .ok_or(JournalTransactionError::TooManyBlocks)?;
        let mut operations = Vec::new();
        operations
            .try_reserve_exact(capacity)
            .map_err(|_| JournalTransactionError::TooManyBlocks)?;
        operations.extend(
            self.replay_images
                .iter()
                .cloned()
                .map(JournalCommitOperation::WriteHomeMetadata),
        );
        operations.push(JournalCommitOperation::Flush(JournalFlush::Checkpoint));
        operations.push(JournalCommitOperation::WriteJournalSuperblock {
            journal_block: journal_superblock_block,
            image: self.clean_superblock.clone(),
        });
        operations.push(JournalCommitOperation::Flush(JournalFlush::JournalState));
        let checkpointed_filesystem_superblock = self
            .replay_images
            .iter()
            .find(|image| image.block_index() == 0)
            .map(|image| {
                FilesystemSuperblockImage::from_checkpointed_home_block(
                    filesystem_superblock,
                    image,
                )
            })
            .transpose()?
            .unwrap_or_else(|| filesystem_superblock.clone());
        operations.push(JournalCommitOperation::WriteFilesystemSuperblock {
            start_byte: FILESYSTEM_SUPERBLOCK_START_BYTE,
            image: checkpointed_filesystem_superblock.with_recovery_state(false),
        });
        operations.push(JournalCommitOperation::Flush(JournalFlush::FilesystemState));
        Ok(operations)
    }
}

impl JournalInodeMap {
    /// Return the validated JBD2 superblock stored at logical block zero.
    #[must_use]
    pub fn superblock(&self) -> &JournalSuperblockImage {
        &self.superblock
    }

    /// Return every physical journal-inode block in logical order.
    #[must_use]
    pub fn physical_blocks(&self) -> &[u64] {
        &self.physical_blocks
    }

    /// Return the validated ext4 primary-superblock state seen at admission.
    #[must_use]
    pub fn filesystem_superblock(&self) -> &FilesystemSuperblockImage {
        &self.filesystem_superblock
    }

    /// Return the authoritative ext4 incompat-recovery feature state.
    #[must_use]
    pub fn filesystem_needs_recovery(&self) -> bool {
        self.filesystem_needs_recovery
    }

    /// Return the greatest valid absolute filesystem block index.
    #[must_use]
    pub fn maximum_block(&self) -> u64 {
        self.maximum_block
    }

    /// Admit the discovered map only when ext4 and JBD2 both prove it clean.
    pub fn into_clean_ring(self) -> Result<JournalRing, JournalTransactionError> {
        if self.filesystem_needs_recovery {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }
        let mut ring = self
            .superblock
            .map_clean_ring(self.maximum_block, &self.physical_blocks)?;
        ring.filesystem_superblock = Some(self.filesystem_superblock);
        ring.filesystem_recovery_state = Some(FilesystemRecoveryState::Clean);
        Ok(ring)
    }
}

/// Discover and validate the internal journal inode and its physical block map.
#[maybe_async::maybe_async]
pub async fn load_journal_inode_map(fs: &Ext4) -> Result<JournalInodeMap, JournalInodeMapError> {
    let journal_inode_index =
        fs.0.superblock
            .journal_inode()
            .ok_or(JournalInodeMapError::MissingJournal)?;
    let journal_inode = Inode::read(fs, journal_inode_index).await?;
    let logical_blocks = journal_inode.file_size_in_blocks(fs)?;
    let logical_blocks =
        usize::try_from(logical_blocks).map_err(|_| JournalTransactionError::RingGeometry)?;
    if logical_blocks < 4 || logical_blocks > JOURNAL_RING_MAX_SLOTS + 1 {
        return Err(JournalTransactionError::RingGeometry.into());
    }
    let mut physical_blocks = Vec::new();
    physical_blocks
        .try_reserve_exact(logical_blocks)
        .map_err(|_| JournalTransactionError::TooManyBlocks)?;
    let mut iterator = FileBlocks::new(fs.clone(), &journal_inode)?;
    while let Some(block) = iterator.next().await {
        if physical_blocks.len() >= logical_blocks {
            return Err(JournalTransactionError::RingGeometry.into());
        }
        physical_blocks.push(block?);
    }
    if physical_blocks.len() != logical_blocks {
        return Err(JournalTransactionError::RingGeometry.into());
    }
    let maximum_block =
        fs.0.superblock
            .blocks_count()
            .checked_sub(1)
            .ok_or(JournalTransactionError::RingGeometry)?;
    validate_physical_journal_blocks(maximum_block, logical_blocks, &physical_blocks)?;
    let mut superblock_bytes = vec![0; JOURNAL_SUPERBLOCK_BYTES];
    fs.read_from_block_raw(physical_blocks[0], 0, &mut superblock_bytes)
        .await?;
    let superblock = JournalSuperblockImage::from_bytes(&superblock_bytes)?;
    if usize::try_from(superblock.maximum_length())
        .map_err(|_| JournalTransactionError::RingGeometry)?
        != logical_blocks
    {
        return Err(JournalTransactionError::RingGeometry.into());
    }
    Ok(JournalInodeMap {
        superblock,
        filesystem_superblock: FilesystemSuperblockImage::from_superblock(
            &fs.0.superblock,
            fs.0.superblock.needs_recovery(),
        ),
        maximum_block,
        physical_blocks,
        filesystem_needs_recovery: fs.0.superblock.needs_recovery(),
    })
}

impl JournalSuperblockImage {
    /// Build a canonical clean JBD2 superblock for deterministic image tooling.
    pub fn new_clean(
        next_sequence: u32,
        journal_uuid: [u8; 16],
        maximum_length: u32,
    ) -> Result<Self, JournalTransactionError> {
        if next_sequence == u32::MAX {
            return Err(JournalTransactionError::SequenceOverflow);
        }
        let data_slots = maximum_length
            .checked_sub(1)
            .ok_or(JournalTransactionError::RingGeometry)?;
        if data_slots < 3
            || usize::try_from(data_slots).map_err(|_| JournalTransactionError::RingGeometry)?
                > JOURNAL_RING_MAX_SLOTS
        {
            return Err(JournalTransactionError::RingGeometry);
        }
        let mut bytes = vec![0; JOURNAL_SUPERBLOCK_BYTES];
        write_u32be(&mut bytes, 0, JournalBlockHeader::MAGIC);
        write_u32be(&mut bytes, 4, JournalBlockType::SUPERBLOCK_V2.0);
        write_u32be(
            &mut bytes,
            JOURNAL_SUPERBLOCK_BLOCK_SIZE_OFFSET,
            u32::try_from(JOURNAL_BLOCK_BYTES).expect("4096 fits u32"),
        );
        write_u32be(
            &mut bytes,
            JOURNAL_SUPERBLOCK_MAX_LENGTH_OFFSET,
            maximum_length,
        );
        write_u32be(&mut bytes, JOURNAL_SUPERBLOCK_FIRST_BLOCK_OFFSET, 1);
        write_u32be(
            &mut bytes,
            JOURNAL_SUPERBLOCK_SEQUENCE_OFFSET,
            next_sequence,
        );
        write_u32be(
            &mut bytes,
            JOURNAL_SUPERBLOCK_FEATURE_INCOMPAT_OFFSET,
            JOURNAL_FEATURE_BLOCK_REVOCATIONS
                | JOURNAL_FEATURE_64_BIT
                | JOURNAL_FEATURE_CHECKSUM_V3,
        );
        bytes[JOURNAL_SUPERBLOCK_UUID_OFFSET..JOURNAL_SUPERBLOCK_UUID_OFFSET + 16]
            .copy_from_slice(&journal_uuid);
        write_u32be(&mut bytes, JOURNAL_SUPERBLOCK_USER_COUNT_OFFSET, 1);
        bytes[JOURNAL_SUPERBLOCK_CHECKSUM_TYPE_OFFSET] = JOURNAL_CHECKSUM_TYPE_CRC32C;
        update_journal_superblock_checksum(&mut bytes);
        Self::from_bytes(&bytes)
    }

    /// Parse and validate one complete JBD2 superblock image.
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, JournalTransactionError> {
        if bytes.len() != JOURNAL_SUPERBLOCK_BYTES {
            return Err(JournalTransactionError::CorruptSuperblock);
        }
        let header = JournalBlockHeader::read_bytes(bytes)
            .ok_or(JournalTransactionError::CorruptSuperblockMagic)?;
        if header.block_type != JournalBlockType::SUPERBLOCK_V2 {
            return Err(JournalTransactionError::UnsupportedSuperblockType(
                header.block_type.0,
            ));
        }
        let incompat = read_u32be(bytes, JOURNAL_SUPERBLOCK_FEATURE_INCOMPAT_OFFSET);
        let missing = JOURNAL_REQUIRED_FEATURES & !incompat;
        if missing != 0 {
            return Err(JournalTransactionError::MissingSuperblockFeatures(missing));
        }
        let unsupported = incompat & !JOURNAL_ALLOWED_FEATURES;
        if unsupported != 0 {
            return Err(JournalTransactionError::UnsupportedSuperblockFeatures(
                unsupported,
            ));
        }
        let checksum_type = bytes[JOURNAL_SUPERBLOCK_CHECKSUM_TYPE_OFFSET];
        if checksum_type != JOURNAL_CHECKSUM_TYPE_CRC32C {
            return Err(JournalTransactionError::UnsupportedSuperblockChecksumType(
                checksum_type,
            ));
        }
        let stored_checksum = read_u32be(bytes, JOURNAL_SUPERBLOCK_CHECKSUM_OFFSET);
        let calculated_checksum = journal_superblock_checksum(bytes);
        if stored_checksum != calculated_checksum {
            return Err(JournalTransactionError::CorruptSuperblockChecksum {
                stored: stored_checksum,
                calculated: calculated_checksum,
            });
        }
        let superblock = JournalSuperblock::read_bytes(bytes)
            .map_err(|_| JournalTransactionError::CorruptSuperblock)?;
        let maximum_length = read_u32be(bytes, JOURNAL_SUPERBLOCK_MAX_LENGTH_OFFSET);
        let first_block = read_u32be(bytes, JOURNAL_SUPERBLOCK_FIRST_BLOCK_OFFSET);
        let sequence = read_u32be(bytes, JOURNAL_SUPERBLOCK_SEQUENCE_OFFSET);
        let start_block = read_u32be(bytes, JOURNAL_SUPERBLOCK_START_BLOCK_OFFSET);
        let data_slots = maximum_length
            .checked_sub(first_block)
            .ok_or(JournalTransactionError::RingGeometry)?;
        if superblock.block_size != u32::try_from(JOURNAL_BLOCK_BYTES).expect("4096 fits u32")
            || first_block != 1
            || data_slots < 3
            || usize::try_from(data_slots).map_err(|_| JournalTransactionError::RingGeometry)?
                > JOURNAL_RING_MAX_SLOTS
            || (start_block != 0 && (start_block < first_block || start_block >= maximum_length))
        {
            return Err(JournalTransactionError::RingGeometry);
        }
        if sequence == u32::MAX {
            return Err(JournalTransactionError::SequenceOverflow);
        }
        Ok(Self {
            bytes: Vec::from(bytes),
            maximum_length,
            sequence,
            start_block,
            uuid: Uuid::new(*superblock.uuid.as_bytes()),
            block_revocations: incompat & JOURNAL_FEATURE_BLOCK_REVOCATIONS != 0,
        })
    }

    /// Return the exact 1,024-byte checksummed image.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    /// Return the journal inode length in logical blocks, including block zero.
    #[must_use]
    pub fn maximum_length(&self) -> u32 {
        self.maximum_length
    }

    /// Return the oldest sequence to replay or the next sequence when clean.
    #[must_use]
    pub fn sequence(&self) -> u32 {
        self.sequence
    }

    /// Return the oldest live logical journal block, or zero when clean.
    #[must_use]
    pub fn start_block(&self) -> u32 {
        self.start_block
    }

    /// Return whether the journal advertises block-revocation records.
    #[must_use]
    pub fn block_revocations(&self) -> bool {
        self.block_revocations
    }

    /// Produce a checksummed state image while preserving all other fields.
    pub fn with_state(
        &self,
        sequence: u32,
        start_block: u32,
    ) -> Result<Self, JournalTransactionError> {
        if sequence == u32::MAX {
            return Err(JournalTransactionError::SequenceOverflow);
        }
        if start_block != 0 && (start_block < 1 || start_block >= self.maximum_length) {
            return Err(JournalTransactionError::RingGeometry);
        }
        let mut bytes = self.bytes.clone();
        write_u32be(&mut bytes, JOURNAL_SUPERBLOCK_SEQUENCE_OFFSET, sequence);
        write_u32be(
            &mut bytes,
            JOURNAL_SUPERBLOCK_START_BLOCK_OFFSET,
            start_block,
        );
        update_journal_superblock_checksum(&mut bytes);
        Self::from_bytes(&bytes)
    }

    /// Map a complete, distinct journal inode into an empty ring.
    pub fn map_clean_ring(
        &self,
        maximum_block: u64,
        physical_journal_blocks: &[u64],
    ) -> Result<JournalRing, JournalTransactionError> {
        if self.start_block != 0 {
            return Err(JournalTransactionError::JournalNotClean);
        }
        let maximum_length = usize::try_from(self.maximum_length)
            .map_err(|_| JournalTransactionError::RingGeometry)?;
        validate_physical_journal_blocks(maximum_block, maximum_length, physical_journal_blocks)?;
        let mut ring = JournalRing::new_clean(
            self.sequence,
            *self.uuid.as_bytes(),
            maximum_block,
            self.block_revocations,
            &physical_journal_blocks[1..],
        )?;
        ring.superblock = Some(self.clone());
        ring.superblock_block = Some(physical_journal_blocks[0]);
        Ok(ring)
    }
}

fn validate_physical_journal_blocks(
    maximum_block: u64,
    expected_length: usize,
    physical_journal_blocks: &[u64],
) -> Result<(), JournalTransactionError> {
    if physical_journal_blocks.len() != expected_length
        || physical_journal_blocks
            .iter()
            .any(|block| *block == 0 || *block > maximum_block)
    {
        return Err(JournalTransactionError::RingGeometry);
    }
    let mut sorted = Vec::from(physical_journal_blocks);
    sorted.sort_unstable();
    if sorted.windows(2).any(|pair| pair[0] == pair[1]) {
        return Err(JournalTransactionError::RingGeometry);
    }
    Ok(())
}

impl Error for JournalTransactionError {}

/// A bounded, single-descriptor JBD2 transaction.
#[derive(Clone, Debug)]
pub struct JournalTransaction {
    sequence: u32,
    uuid: Uuid,
    maximum_block: u64,
    ordered_data: Vec<JournalBlockImage>,
    metadata: Vec<JournalBlockImage>,
    revoked_blocks: Vec<u64>,
}

impl JournalTransaction {
    /// Start a transaction for one admitted filesystem and journal sequence.
    ///
    /// `maximum_block` is the highest valid absolute filesystem block index,
    /// inclusive.
    pub fn new(
        sequence: u32,
        journal_uuid: [u8; 16],
        maximum_block: u64,
    ) -> Result<Self, JournalTransactionError> {
        if sequence == u32::MAX {
            return Err(JournalTransactionError::SequenceOverflow);
        }
        Ok(Self {
            sequence,
            uuid: Uuid::new(journal_uuid),
            maximum_block,
            ordered_data: Vec::new(),
            metadata: Vec::new(),
            revoked_blocks: Vec::new(),
        })
    }

    /// Stage a block revocation for an older journaled image.
    pub fn stage_revocation(&mut self, block_index: u64) -> Result<(), JournalTransactionError> {
        if self.revoked_blocks.len() >= JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS {
            return Err(JournalTransactionError::TooManyBlocks);
        }
        if block_index > self.maximum_block {
            return Err(JournalTransactionError::BlockOutOfRange);
        }
        if self.revoked_blocks.contains(&block_index) {
            return Err(JournalTransactionError::DuplicateBlock);
        }
        self.revoked_blocks.push(block_index);
        Ok(())
    }

    /// Return whether this transaction revokes an older image of `block_index`.
    #[must_use]
    pub fn revokes_block(&self, block_index: u64) -> bool {
        self.revoked_blocks.contains(&block_index)
    }

    /// Return the exact descriptor/data/revoke/commit slot requirement.
    pub fn required_journal_slots(&self) -> Result<usize, JournalTransactionError> {
        if self.metadata.is_empty() {
            return Err(JournalTransactionError::EmptyTransaction);
        }
        self.metadata
            .len()
            .checked_add(2)
            .and_then(|count| count.checked_add(usize::from(!self.revoked_blocks.is_empty())))
            .ok_or(JournalTransactionError::TooManyBlocks)
    }

    /// Stage a complete file-data block that must precede the journal commit.
    pub fn stage_ordered_data(
        &mut self,
        block_index: u64,
        bytes: &[u8],
    ) -> Result<(), JournalTransactionError> {
        if self.ordered_data.len() >= JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS {
            return Err(JournalTransactionError::TooManyBlocks);
        }
        self.stage(block_index, bytes, false)
    }

    /// Stage a complete metadata block for descriptor/data journaling.
    pub fn stage_metadata(
        &mut self,
        block_index: u64,
        bytes: &[u8],
    ) -> Result<(), JournalTransactionError> {
        if self.metadata.len() >= JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS {
            return Err(JournalTransactionError::TooManyBlocks);
        }
        self.stage(block_index, bytes, true)
    }

    fn stage(
        &mut self,
        block_index: u64,
        bytes: &[u8],
        metadata: bool,
    ) -> Result<(), JournalTransactionError> {
        if bytes.len() != JOURNAL_BLOCK_BYTES {
            return Err(JournalTransactionError::IncorrectBlockLength);
        }
        if block_index > self.maximum_block {
            return Err(JournalTransactionError::BlockOutOfRange);
        }
        if self
            .ordered_data
            .iter()
            .chain(self.metadata.iter())
            .any(|image| image.block_index == block_index)
        {
            return Err(JournalTransactionError::DuplicateBlock);
        }
        if metadata && bytes[..4] == JournalBlockHeader::MAGIC.to_be_bytes() {
            return Err(JournalTransactionError::EscapedBlockUnsupported);
        }
        let image = JournalBlockImage {
            block_index,
            bytes: Vec::from(bytes),
        };
        if metadata {
            self.metadata.push(image);
        } else {
            self.ordered_data.push(image);
        }
        Ok(())
    }

    /// Build the only legal ordered-data execution sequence for this image.
    ///
    /// `journal_blocks` must contain exactly one descriptor slot, one slot for
    /// each metadata image, an optional revoke slot, and one final commit slot.
    /// The returned operations intentionally place the commit flush before
    /// every home-metadata write.
    pub fn commit_plan(
        &self,
        journal_blocks: &[u64],
    ) -> Result<Vec<JournalCommitOperation>, JournalTransactionError> {
        let required_slots = self.required_journal_slots()?;
        if journal_blocks.len() != required_slots {
            return Err(JournalTransactionError::JournalSlotCount);
        }
        for (index, block) in journal_blocks.iter().enumerate() {
            if *block == 0 || *block > self.maximum_block {
                return Err(JournalTransactionError::BlockOutOfRange);
            }
            if journal_blocks[..index].contains(block) {
                return Err(JournalTransactionError::DuplicateBlock);
            }
            if self
                .ordered_data
                .iter()
                .chain(self.metadata.iter())
                .any(|image| image.block_index == *block)
                || self.revoked_blocks.contains(block)
            {
                return Err(JournalTransactionError::JournalSlotOverlap);
            }
        }

        let mut operations = Vec::new();
        operations
            .try_reserve_exact(
                self.ordered_data.len()
                    + self.metadata.len() * 2
                    + usize::from(!self.revoked_blocks.is_empty())
                    + 7,
            )
            .map_err(|_| JournalTransactionError::TooManyBlocks)?;
        operations.extend(
            self.ordered_data
                .iter()
                .cloned()
                .map(JournalCommitOperation::WriteOrderedData),
        );
        if !self.ordered_data.is_empty() {
            operations.push(JournalCommitOperation::Flush(JournalFlush::OrderedData));
        }
        operations.push(JournalCommitOperation::WriteJournal {
            journal_block: journal_blocks[0],
            kind: JournalRecordKind::Descriptor,
            bytes: self.descriptor_block(),
        });
        for (index, image) in self.metadata.iter().enumerate() {
            operations.push(JournalCommitOperation::WriteJournal {
                journal_block: journal_blocks[index + 1],
                kind: JournalRecordKind::Metadata,
                bytes: image.bytes.clone(),
            });
        }
        let mut commit_slot = self.metadata.len() + 1;
        if !self.revoked_blocks.is_empty() {
            operations.push(JournalCommitOperation::WriteJournal {
                journal_block: journal_blocks[commit_slot],
                kind: JournalRecordKind::Revocation,
                bytes: self.revocation_block(),
            });
            commit_slot += 1;
        }
        operations.push(JournalCommitOperation::Flush(JournalFlush::JournalPayload));
        operations.push(JournalCommitOperation::WriteJournal {
            journal_block: journal_blocks[commit_slot],
            kind: JournalRecordKind::Commit,
            bytes: self.commit_block(),
        });
        operations.push(JournalCommitOperation::Flush(JournalFlush::Commit));
        operations.extend(
            self.metadata
                .iter()
                .cloned()
                .map(JournalCommitOperation::WriteHomeMetadata),
        );
        operations.push(JournalCommitOperation::Flush(JournalFlush::Checkpoint));
        Ok(operations)
    }

    fn descriptor_block(&self) -> Vec<u8> {
        const UUID_OMITTED: u32 = 0x2;
        const LAST_TAG: u32 = 0x8;
        const TAG_BYTES: usize = 16;

        let mut block = vec![0; JOURNAL_BLOCK_BYTES];
        write_u32be(&mut block, 0, JournalBlockHeader::MAGIC);
        write_u32be(&mut block, 4, JournalBlockType::DESCRIPTOR.0);
        write_u32be(&mut block, 8, self.sequence);
        let mut offset = JournalBlockHeader::SIZE;
        for (index, image) in self.metadata.iter().enumerate() {
            let low = u32::try_from(image.block_index & 0xffff_ffff)
                .expect("masked block index fits u32");
            let high =
                u32::try_from(image.block_index >> 32).expect("shifted block index fits u32");
            let flags = if index == 0 { 0 } else { UUID_OMITTED }
                | if index + 1 == self.metadata.len() {
                    LAST_TAG
                } else {
                    0
                };
            let mut checksum = Checksum::new();
            checksum.update(self.uuid.as_bytes());
            checksum.update_u32_be(self.sequence);
            checksum.update(&image.bytes);
            write_u32be(&mut block, offset, low);
            write_u32be(&mut block, offset + 4, flags);
            write_u32be(&mut block, offset + 8, high);
            write_u32be(&mut block, offset + 12, checksum.finalize());
            offset += TAG_BYTES;
            if index == 0 {
                block[offset..offset + 16].copy_from_slice(self.uuid.as_bytes());
                offset += 16;
            }
        }
        let checksum_offset = JOURNAL_BLOCK_BYTES - 4;
        let mut checksum = Checksum::new();
        checksum.update(self.uuid.as_bytes());
        checksum.update(&block[..checksum_offset]);
        checksum.update_u32_be(0);
        write_u32be(&mut block, checksum_offset, checksum.finalize());
        block
    }

    fn commit_block(&self) -> Vec<u8> {
        const CHECKSUM_OFFSET: usize = 16;

        let mut block = vec![0; JOURNAL_BLOCK_BYTES];
        write_u32be(&mut block, 0, JournalBlockHeader::MAGIC);
        write_u32be(&mut block, 4, JournalBlockType::COMMIT.0);
        write_u32be(&mut block, 8, self.sequence);
        let mut checksum = Checksum::new();
        checksum.update(self.uuid.as_bytes());
        checksum.update(&block[..CHECKSUM_OFFSET]);
        checksum.update_u32_be(0);
        checksum.update(&block[CHECKSUM_OFFSET + 4..]);
        write_u32be(&mut block, CHECKSUM_OFFSET, checksum.finalize());
        block
    }

    fn revocation_block(&self) -> Vec<u8> {
        const TABLE_BYTES_OFFSET: usize = JournalBlockHeader::SIZE;
        const TABLE_OFFSET: usize = TABLE_BYTES_OFFSET + 4;

        let mut block = vec![0; JOURNAL_BLOCK_BYTES];
        write_u32be(&mut block, 0, JournalBlockHeader::MAGIC);
        write_u32be(&mut block, 4, JournalBlockType::REVOCATION.0);
        write_u32be(&mut block, 8, self.sequence);
        let table_bytes = self
            .revoked_blocks
            .len()
            .checked_mul(8)
            .and_then(|bytes| u32::try_from(bytes).ok())
            .expect("bounded revocation table fits u32");
        write_u32be(&mut block, TABLE_BYTES_OFFSET, table_bytes);
        for (index, revoked) in self.revoked_blocks.iter().enumerate() {
            let offset = TABLE_OFFSET + index * 8;
            block[offset..offset + 8].copy_from_slice(&revoked.to_be_bytes());
        }
        let checksum_offset = JOURNAL_BLOCK_BYTES - 4;
        let mut checksum = Checksum::new();
        checksum.update(self.uuid.as_bytes());
        checksum.update(&block[..checksum_offset]);
        checksum.update_u32_be(0);
        write_u32be(&mut block, checksum_offset, checksum.finalize());
        block
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ReservationState {
    Prepared,
    CommitStarted,
    CommitDurable,
    TailStatePrepared,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum FilesystemRecoveryState {
    Clean,
    PlanPrepared,
    Durable,
    CleanPlanPrepared,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ActiveReservation {
    ticket: u64,
    sequence: u32,
    start: usize,
    slot_count: usize,
    updates_filesystem_superblock: bool,
    state: ReservationState,
}

/// One ring reservation and its complete ordered commit operation list.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct JournalPreparedTransaction {
    ticket: u64,
    sequence: u32,
    journal_blocks: Vec<u64>,
    operations: Vec<JournalCommitOperation>,
}

/// A validated block-zero image bound to one prepared ring reservation.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FilesystemSuperblockCheckpoint {
    ticket: u64,
    image: FilesystemSuperblockImage,
}

impl JournalPreparedTransaction {
    /// Return the monotonic ticket used for durability transitions.
    #[must_use]
    pub fn ticket(&self) -> u64 {
        self.ticket
    }

    /// Return the JBD2 transaction sequence encoded into every record.
    #[must_use]
    pub fn sequence(&self) -> u32 {
        self.sequence
    }

    /// Return the physical journal blocks reserved in logical ring order.
    #[must_use]
    pub fn journal_blocks(&self) -> &[u64] {
        &self.journal_blocks
    }

    /// Return the ordered-data payload and home-checkpoint operation body.
    ///
    /// A mapped ring must wrap this body with
    /// [`JournalRing::prepare_commit_plan`] so the live superblock state is
    /// ordered before the commit record.
    #[must_use]
    pub fn operations(&self) -> &[JournalCommitOperation] {
        &self.operations
    }
}

/// Bounded allocation and reclamation for an admitted clean JBD2 ring.
///
/// The caller supplies the physical blocks of the journal inode after its
/// superblock in logical ring order. A ring admitted through
/// [`JournalSuperblockImage::map_clean_ring`] can construct the live and
/// clean/tail superblock writes around each transaction. It prevents reuse
/// until the caller acknowledges the final journal-state flush.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct JournalRing {
    uuid: Uuid,
    maximum_block: u64,
    block_revocations: bool,
    superblock: Option<JournalSuperblockImage>,
    superblock_block: Option<u64>,
    filesystem_superblock: Option<FilesystemSuperblockImage>,
    filesystem_recovery_state: Option<FilesystemRecoveryState>,
    slots: Vec<u64>,
    head: usize,
    tail: usize,
    used: usize,
    next_sequence: u32,
    next_ticket: u64,
    active: VecDeque<ActiveReservation>,
}

impl JournalRing {
    /// Admit a clean journal ring with no live or replayable transactions.
    pub fn new_clean(
        next_sequence: u32,
        journal_uuid: [u8; 16],
        maximum_block: u64,
        block_revocations: bool,
        journal_slots: &[u64],
    ) -> Result<Self, JournalTransactionError> {
        if next_sequence == u32::MAX {
            return Err(JournalTransactionError::SequenceOverflow);
        }
        if journal_slots.len() < 3
            || journal_slots.len() > JOURNAL_RING_MAX_SLOTS
            || journal_slots
                .iter()
                .any(|slot| *slot == 0 || *slot > maximum_block)
        {
            return Err(JournalTransactionError::RingGeometry);
        }
        let mut sorted = Vec::from(journal_slots);
        sorted.sort_unstable();
        if sorted.windows(2).any(|pair| pair[0] == pair[1]) {
            return Err(JournalTransactionError::RingGeometry);
        }
        Ok(Self {
            uuid: Uuid::new(journal_uuid),
            maximum_block,
            block_revocations,
            superblock: None,
            superblock_block: None,
            filesystem_superblock: None,
            filesystem_recovery_state: None,
            slots: Vec::from(journal_slots),
            head: 0,
            tail: 0,
            used: 0,
            next_sequence,
            next_ticket: 1,
            active: VecDeque::new(),
        })
    }

    /// Start a transaction using the ring's next checked sequence.
    pub fn begin_transaction(&self) -> Result<JournalTransaction, JournalTransactionError> {
        JournalTransaction::new(
            self.next_sequence,
            *self.uuid.as_bytes(),
            self.maximum_block,
        )
    }

    /// Return whether the admitted filesystem and journal are idle and clean.
    ///
    /// A clean filesystem has no active reservation or occupied ring slot, a
    /// zero JBD2 start block, matching head/tail and sequence state, and a
    /// cleared ext4 recovery marker. Transitional marker states and a durable
    /// recovery marker report `false`; inconsistent in-memory marker state is
    /// rejected.
    pub fn filesystem_is_clean(&self) -> Result<bool, JournalTransactionError> {
        let state = self
            .filesystem_recovery_state
            .ok_or(JournalTransactionError::FilesystemStateUnavailable)?;
        let superblock = self
            .superblock
            .as_ref()
            .ok_or(JournalTransactionError::JournalStateUnavailable)?;
        let filesystem = self
            .filesystem_superblock
            .as_ref()
            .ok_or(JournalTransactionError::FilesystemStateUnavailable)?;
        let marker_matches_state = match state {
            FilesystemRecoveryState::Clean | FilesystemRecoveryState::PlanPrepared => {
                !filesystem.needs_recovery()
            }
            FilesystemRecoveryState::Durable
            | FilesystemRecoveryState::CleanPlanPrepared => filesystem.needs_recovery(),
        };
        if !marker_matches_state {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }
        Ok(state == FilesystemRecoveryState::Clean
            && self.active.is_empty()
            && self.used == 0
            && self.head == self.tail
            && superblock.start_block() == 0
            && superblock.sequence() == self.next_sequence)
    }

    /// Return whether ext4's recovery marker is acknowledged as durable.
    ///
    /// A prepared marker write is not durable yet. A pending final-clean plan
    /// continues to report durable because its clearing flush has not been
    /// acknowledged. Any disagreement between the retained image and planner
    /// state is rejected.
    pub fn filesystem_recovery_marker_is_durable(
        &self,
    ) -> Result<bool, JournalTransactionError> {
        let state = self
            .filesystem_recovery_state
            .ok_or(JournalTransactionError::FilesystemStateUnavailable)?;
        let filesystem = self
            .filesystem_superblock
            .as_ref()
            .ok_or(JournalTransactionError::FilesystemStateUnavailable)?;
        let expected = matches!(
            state,
            FilesystemRecoveryState::Durable | FilesystemRecoveryState::CleanPlanPrepared
        );
        if filesystem.needs_recovery() != expected {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }
        Ok(expected)
    }

    /// Build the write and flush that make ext4's recovery marker durable.
    ///
    /// A ring discovered from a real clean ext4 image must execute this plan
    /// once before its first mapped commit plan. Synthetic journal-only rings
    /// have no filesystem-superblock state and do not use this transition.
    /// Re-preparing a pending plan returns the same operations so a failed
    /// marker write or flush can be retried without advancing in-memory state.
    pub fn prepare_recovery_marker_plan(
        &mut self,
    ) -> Result<Vec<JournalCommitOperation>, JournalTransactionError> {
        let state = self
            .filesystem_recovery_state
            .ok_or(JournalTransactionError::FilesystemStateUnavailable)?;
        if state != FilesystemRecoveryState::Clean
            && state != FilesystemRecoveryState::PlanPrepared
        {
            return Err(JournalTransactionError::ReservationState);
        }
        let marker = self
            .filesystem_superblock
            .as_ref()
            .ok_or(JournalTransactionError::FilesystemStateUnavailable)?;
        if marker.needs_recovery {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }
        let marker = marker.with_recovery_state(true);
        self.filesystem_recovery_state = Some(FilesystemRecoveryState::PlanPrepared);
        Ok(vec![
            JournalCommitOperation::WriteFilesystemSuperblock {
                start_byte: FILESYSTEM_SUPERBLOCK_START_BYTE,
                image: marker,
            },
            JournalCommitOperation::Flush(JournalFlush::FilesystemState),
        ])
    }

    /// Acknowledge the completed recovery-marker flush.
    pub fn mark_recovery_marker_durable(&mut self) -> Result<(), JournalTransactionError> {
        if self.filesystem_recovery_state != Some(FilesystemRecoveryState::PlanPrepared) {
            return Err(JournalTransactionError::ReservationState);
        }
        let marker = self
            .filesystem_superblock
            .as_ref()
            .ok_or(JournalTransactionError::FilesystemStateUnavailable)?;
        self.filesystem_superblock = Some(marker.with_recovery_state(true));
        self.filesystem_recovery_state = Some(FilesystemRecoveryState::Durable);
        Ok(())
    }

    /// Validate a staged primary-superblock home image before committing it.
    ///
    /// The admitted image must preserve the mounted filesystem identity and
    /// geometry, retain the durable incompat-recovery marker, and carry a
    /// correct metadata checksum. The returned value is safe to retain until
    /// the corresponding checkpoint becomes durable.
    pub fn admit_checkpointed_filesystem_superblock(
        &self,
        prepared: &JournalPreparedTransaction,
        image: &JournalBlockImage,
    ) -> Result<FilesystemSuperblockCheckpoint, JournalTransactionError> {
        if self.filesystem_recovery_state != Some(FilesystemRecoveryState::Durable) {
            return Err(JournalTransactionError::RecoveryMarkerNotDurable);
        }
        let reservation = self
            .active
            .iter()
            .find(|reservation| reservation.ticket == prepared.ticket())
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if reservation.state != ReservationState::Prepared
            || !reservation.updates_filesystem_superblock
            || !prepared.operations().iter().any(|operation| {
                matches!(operation, JournalCommitOperation::WriteHomeMetadata(home) if home == image)
            })
        {
            return Err(JournalTransactionError::RingTransactionMismatch);
        }
        let current = self
            .filesystem_superblock
            .as_ref()
            .ok_or(JournalTransactionError::FilesystemStateUnavailable)?;
        Ok(FilesystemSuperblockCheckpoint {
            ticket: prepared.ticket(),
            image: FilesystemSuperblockImage::from_checkpointed_home_block(current, image)?,
        })
    }

    /// Build the final write and flush that mark an idle filesystem clean.
    ///
    /// The recovery marker may clear only after every committed transaction
    /// has been checkpointed, its clean JBD2 superblock has been flushed, and
    /// the corresponding reservation has been reclaimed. While this plan is
    /// pending, the ring refuses new reservations so a transaction cannot be
    /// prepared behind a filesystem-superblock image that claims no recovery
    /// is required. Re-preparing a pending plan returns the same operations so
    /// a failed write or flush can be retried without advancing in-memory state.
    pub fn prepare_filesystem_clean_plan(
        &mut self,
    ) -> Result<Vec<JournalCommitOperation>, JournalTransactionError> {
        if self.filesystem_recovery_state != Some(FilesystemRecoveryState::Durable)
            && self.filesystem_recovery_state != Some(FilesystemRecoveryState::CleanPlanPrepared)
        {
            return Err(JournalTransactionError::ReservationState);
        }
        if !self.active.is_empty() || self.used != 0 {
            return Err(JournalTransactionError::JournalNotClean);
        }
        let superblock = self
            .superblock
            .as_ref()
            .ok_or(JournalTransactionError::JournalStateUnavailable)?;
        if superblock.start_block() != 0 {
            return Err(JournalTransactionError::JournalNotClean);
        }
        let marker = self
            .filesystem_superblock
            .as_ref()
            .ok_or(JournalTransactionError::FilesystemStateUnavailable)?;
        if !marker.needs_recovery() {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }
        let clean = marker.with_recovery_state(false);
        self.filesystem_recovery_state = Some(FilesystemRecoveryState::CleanPlanPrepared);
        Ok(vec![
            JournalCommitOperation::WriteFilesystemSuperblock {
                start_byte: FILESYSTEM_SUPERBLOCK_START_BYTE,
                image: clean,
            },
            JournalCommitOperation::Flush(JournalFlush::FilesystemState),
        ])
    }

    /// Acknowledge the completed final filesystem-state flush.
    pub fn mark_filesystem_clean_durable(&mut self) -> Result<(), JournalTransactionError> {
        if self.filesystem_recovery_state != Some(FilesystemRecoveryState::CleanPlanPrepared)
            || !self.active.is_empty()
            || self.used != 0
        {
            return Err(JournalTransactionError::ReservationState);
        }
        let superblock = self
            .superblock
            .as_ref()
            .ok_or(JournalTransactionError::JournalStateUnavailable)?;
        if superblock.start_block() != 0 {
            return Err(JournalTransactionError::JournalNotClean);
        }
        let marker = self
            .filesystem_superblock
            .as_ref()
            .ok_or(JournalTransactionError::FilesystemStateUnavailable)?;
        if !marker.needs_recovery() {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }
        self.filesystem_superblock = Some(marker.with_recovery_state(false));
        self.filesystem_recovery_state = Some(FilesystemRecoveryState::Clean);
        Ok(())
    }

    /// Reserve ring slots and build a complete commit plan without issuing I/O.
    pub fn prepare(
        &mut self,
        transaction: &JournalTransaction,
    ) -> Result<JournalPreparedTransaction, JournalTransactionError> {
        if self.filesystem_recovery_state == Some(FilesystemRecoveryState::CleanPlanPrepared)
            || self
                .active
                .iter()
                .any(|reservation| reservation.state == ReservationState::TailStatePrepared)
        {
            return Err(JournalTransactionError::ReservationState);
        }
        if transaction.sequence != self.next_sequence
            || transaction.uuid != self.uuid
            || transaction.maximum_block != self.maximum_block
        {
            return Err(JournalTransactionError::RingTransactionMismatch);
        }
        if !self.block_revocations && !transaction.revoked_blocks.is_empty() {
            return Err(JournalTransactionError::RevocationsUnsupported);
        }
        if self.filesystem_superblock.is_some()
            && transaction
                .ordered_data
                .iter()
                .any(|image| image.block_index() == 0)
        {
            return Err(JournalTransactionError::RingTransactionMismatch);
        }
        let updates_filesystem_superblock = self.filesystem_superblock.is_some()
            && transaction
                .metadata
                .iter()
                .any(|image| image.block_index() == 0);
        if updates_filesystem_superblock
            && self
                .active
                .iter()
                .any(|reservation| reservation.updates_filesystem_superblock)
        {
            return Err(JournalTransactionError::ReservationState);
        }
        if self.superblock_block.is_some_and(|superblock_block| {
            transaction
                .ordered_data
                .iter()
                .chain(transaction.metadata.iter())
                .any(|image| image.block_index == superblock_block)
                || transaction.revoked_blocks.contains(&superblock_block)
        }) {
            return Err(JournalTransactionError::JournalSlotOverlap);
        }
        let required = transaction.required_journal_slots()?;
        let free = self
            .slots
            .len()
            .checked_sub(self.used)
            .ok_or(JournalTransactionError::ReservationState)?;
        if required > free {
            return Err(JournalTransactionError::RingFull);
        }
        let next_ticket = self
            .next_ticket
            .checked_add(1)
            .ok_or(JournalTransactionError::ReservationOverflow)?;
        let next_sequence = self
            .next_sequence
            .checked_add(1)
            .ok_or(JournalTransactionError::SequenceOverflow)?;
        if next_sequence == u32::MAX {
            return Err(JournalTransactionError::SequenceOverflow);
        }
        let mut journal_blocks = Vec::new();
        journal_blocks
            .try_reserve_exact(required)
            .map_err(|_| JournalTransactionError::TooManyBlocks)?;
        for offset in 0..required {
            let index = self
                .head
                .checked_add(offset)
                .ok_or(JournalTransactionError::RingGeometry)?
                % self.slots.len();
            journal_blocks.push(self.slots[index]);
        }
        let operations = transaction.commit_plan(&journal_blocks)?;
        self.active
            .try_reserve(1)
            .map_err(|_| JournalTransactionError::TooManyBlocks)?;
        let start = self.head;
        self.head = self
            .head
            .checked_add(required)
            .ok_or(JournalTransactionError::RingGeometry)?
            % self.slots.len();
        self.used = self
            .used
            .checked_add(required)
            .ok_or(JournalTransactionError::RingGeometry)?;
        let ticket = self.next_ticket;
        self.next_ticket = next_ticket;
        self.next_sequence = next_sequence;
        self.active.push_back(ActiveReservation {
            ticket,
            sequence: transaction.sequence,
            start,
            slot_count: required,
            updates_filesystem_superblock,
            state: ReservationState::Prepared,
        });
        Ok(JournalPreparedTransaction {
            ticket,
            sequence: transaction.sequence,
            journal_blocks,
            operations,
        })
    }

    /// Add the current checksummed live-tail state to a prepared commit plan.
    ///
    /// The returned superblock write precedes ordered data and journal payload.
    /// A later payload/commit flush therefore makes the nonzero `s_start`
    /// durable no later than the commit record. Commits must start in sequence;
    /// an older durable commit cannot begin its tail update while this plan is
    /// being issued. The filesystem owner must make ext4's incompat-recovery
    /// marker durable before executing the first such plan after a clean mount.
    /// Re-preparing a started commit returns the exact same plan so any failed
    /// write or flush can be retried before durability is acknowledged.
    pub fn prepare_commit_plan(
        &mut self,
        prepared: &JournalPreparedTransaction,
    ) -> Result<Vec<JournalCommitOperation>, JournalTransactionError> {
        if self.filesystem_recovery_state.is_some()
            && self.filesystem_recovery_state != Some(FilesystemRecoveryState::Durable)
        {
            return Err(JournalTransactionError::RecoveryMarkerNotDurable);
        }
        let index = self
            .active
            .iter()
            .position(|reservation| reservation.ticket == prepared.ticket)
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if self
            .active
            .iter()
            .take(index)
            .any(|reservation| reservation.state != ReservationState::CommitDurable)
        {
            return Err(JournalTransactionError::ReservationOrder);
        }
        let reservation = self
            .active
            .get(index)
            .copied()
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if reservation.state != ReservationState::Prepared
            && reservation.state != ReservationState::CommitStarted
        {
            return Err(JournalTransactionError::ReservationState);
        }
        if reservation.sequence != prepared.sequence
            || reservation.slot_count != prepared.journal_blocks.len()
            || prepared
                .journal_blocks
                .iter()
                .enumerate()
                .any(|(offset, block)| {
                    let slot = (reservation.start + offset) % self.slots.len();
                    self.slots[slot] != *block
                })
        {
            return Err(JournalTransactionError::RingTransactionMismatch);
        }
        let superblock = self
            .superblock
            .as_ref()
            .ok_or(JournalTransactionError::JournalStateUnavailable)?;
        let superblock_block = self
            .superblock_block
            .ok_or(JournalTransactionError::JournalStateUnavailable)?;
        let tail = self
            .active
            .front()
            .copied()
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        let tail_block = u32::try_from(
            tail.start
                .checked_add(1)
                .ok_or(JournalTransactionError::RingGeometry)?,
        )
        .map_err(|_| JournalTransactionError::RingGeometry)?;
        let live_superblock = superblock.with_state(tail.sequence, tail_block)?;
        let mut operations = Vec::new();
        operations
            .try_reserve_exact(
                prepared
                    .operations
                    .len()
                    .checked_add(1)
                    .ok_or(JournalTransactionError::TooManyBlocks)?,
            )
            .map_err(|_| JournalTransactionError::TooManyBlocks)?;
        operations.push(JournalCommitOperation::WriteJournalSuperblock {
            journal_block: superblock_block,
            image: live_superblock,
        });
        operations.extend(prepared.operations.iter().cloned());
        self.active
            .get_mut(index)
            .ok_or(JournalTransactionError::ReservationUnknown)?
            .state = ReservationState::CommitStarted;
        Ok(operations)
    }

    /// Build the final FUA-equivalent journal-tail update after checkpointing.
    ///
    /// The caller invokes this only after the prepared commit plan's
    /// [`JournalFlush::Checkpoint`] has completed. If another transaction is
    /// reserved, it must already have a durable commit before the old tail can
    /// advance to it. Otherwise the returned image marks the journal clean.
    /// Re-preparing a pending tail update returns the same write and flush so
    /// the checkpoint boundary can be retried before slot reclamation.
    pub fn prepare_checkpoint_plan(
        &mut self,
        ticket: u64,
    ) -> Result<Vec<JournalCommitOperation>, JournalTransactionError> {
        let reservation = self
            .active
            .front()
            .copied()
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if reservation.ticket != ticket {
            return Err(JournalTransactionError::ReservationOrder);
        }
        if reservation.state != ReservationState::CommitDurable
            && reservation.state != ReservationState::TailStatePrepared
        {
            return Err(JournalTransactionError::ReservationState);
        }
        if self
            .active
            .get(1)
            .is_some_and(|next| next.state != ReservationState::CommitDurable)
        {
            return Err(JournalTransactionError::ReservationOrder);
        }
        let superblock = self
            .superblock
            .as_ref()
            .ok_or(JournalTransactionError::JournalStateUnavailable)?;
        let superblock_block = self
            .superblock_block
            .ok_or(JournalTransactionError::JournalStateUnavailable)?;
        let state = if let Some(next) = self.active.get(1) {
            let start_block = u32::try_from(
                next.start
                    .checked_add(1)
                    .ok_or(JournalTransactionError::RingGeometry)?,
            )
            .map_err(|_| JournalTransactionError::RingGeometry)?;
            superblock.with_state(next.sequence, start_block)?
        } else {
            superblock.with_state(self.next_sequence, 0)?
        };
        self.active
            .front_mut()
            .ok_or(JournalTransactionError::ReservationUnknown)?
            .state = ReservationState::TailStatePrepared;
        Ok(vec![
            JournalCommitOperation::WriteJournalSuperblock {
                journal_block: superblock_block,
                image: state,
            },
            JournalCommitOperation::Flush(JournalFlush::JournalState),
        ])
    }

    /// Mark a commit flush durable, preserving transaction sequence order.
    pub fn mark_commit_durable(&mut self, ticket: u64) -> Result<(), JournalTransactionError> {
        let index = self
            .active
            .iter()
            .position(|reservation| reservation.ticket == ticket)
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if self
            .active
            .iter()
            .take(index)
            .any(|reservation| reservation.state == ReservationState::Prepared)
        {
            return Err(JournalTransactionError::ReservationOrder);
        }
        let reservation = self
            .active
            .get_mut(index)
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if reservation.state != ReservationState::CommitStarted {
            return Err(JournalTransactionError::ReservationState);
        }
        reservation.state = ReservationState::CommitDurable;
        Ok(())
    }

    /// Abort only the newest reservation before its commit becomes durable.
    pub fn abort_precommit(&mut self, ticket: u64) -> Result<(), JournalTransactionError> {
        let reservation = self
            .active
            .back()
            .copied()
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if reservation.ticket != ticket {
            return Err(JournalTransactionError::ReservationOrder);
        }
        if reservation.state != ReservationState::Prepared {
            return Err(JournalTransactionError::ReservationState);
        }
        self.head = reservation.start;
        self.used = self
            .used
            .checked_sub(reservation.slot_count)
            .ok_or(JournalTransactionError::ReservationState)?;
        self.next_sequence = self
            .next_sequence
            .checked_sub(1)
            .ok_or(JournalTransactionError::ReservationState)?;
        let _ = self.active.pop_back();
        Ok(())
    }

    /// Reclaim the oldest reservation after its journal-tail state is durable.
    fn checkpoint_durable_inner(
        &mut self,
        ticket: u64,
        adopts_filesystem_superblock: bool,
    ) -> Result<(), JournalTransactionError> {
        let reservation = self
            .active
            .front()
            .copied()
            .ok_or(JournalTransactionError::ReservationUnknown)?;
        if reservation.ticket != ticket {
            return Err(JournalTransactionError::ReservationOrder);
        }
        if reservation.state != ReservationState::TailStatePrepared {
            return Err(JournalTransactionError::ReservationState);
        }
        if reservation.updates_filesystem_superblock != adopts_filesystem_superblock {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }
        let superblock = self
            .superblock
            .as_ref()
            .ok_or(JournalTransactionError::JournalStateUnavailable)?;
        let durable_state = if let Some(next) = self.active.get(1) {
            let start_block = u32::try_from(
                next.start
                    .checked_add(1)
                    .ok_or(JournalTransactionError::RingGeometry)?,
            )
            .map_err(|_| JournalTransactionError::RingGeometry)?;
            superblock.with_state(next.sequence, start_block)?
        } else {
            superblock.with_state(self.next_sequence, 0)?
        };
        self.tail = self
            .tail
            .checked_add(reservation.slot_count)
            .ok_or(JournalTransactionError::RingGeometry)?
            % self.slots.len();
        self.used = self
            .used
            .checked_sub(reservation.slot_count)
            .ok_or(JournalTransactionError::ReservationState)?;
        self.superblock = Some(durable_state);
        let _ = self.active.pop_front();
        Ok(())
    }

    /// Reclaim the oldest reservation after its journal-tail state is durable.
    pub fn checkpoint_durable(&mut self, ticket: u64) -> Result<(), JournalTransactionError> {
        self.checkpoint_durable_inner(ticket, false)
    }

    /// Reclaim a durable checkpoint and retain its updated ext4 superblock.
    ///
    /// Validation happens before any ring state changes. This prevents the
    /// eventual clean-marker write from restoring allocation counters from the
    /// mount-time snapshot after block-zero metadata has been checkpointed.
    pub fn checkpoint_durable_with_filesystem_superblock(
        &mut self,
        checkpoint: &FilesystemSuperblockCheckpoint,
    ) -> Result<(), JournalTransactionError> {
        let current = self
            .filesystem_superblock
            .as_ref()
            .ok_or(JournalTransactionError::FilesystemStateUnavailable)?;
        current.validate_checkpointed_successor(&checkpoint.image)?;
        let admitted = checkpoint.image.clone();
        self.checkpoint_durable_inner(checkpoint.ticket, true)?;
        self.filesystem_superblock = Some(admitted);
        Ok(())
    }

    /// Return the number of journal data slots currently reserved.
    #[must_use]
    pub fn used_slots(&self) -> usize {
        self.used
    }

    /// Return the next transaction sequence that will be assigned.
    #[must_use]
    pub fn next_sequence(&self) -> u32 {
        self.next_sequence
    }
}

/// Replay every consecutive committed transaction in one bounded journal ring.
///
/// `journal_blocks` contains logical journal blocks one through
/// `superblock.maximum_length() - 1`, in order. The scan begins at the
/// superblock's live start, wraps at most once, and stops at a stale sequence or
/// a transaction that has no commit record. A transaction with a present but
/// corrupt commit, descriptor, data image, or revocation record is refused.
/// Later revocations suppress earlier pending home-block images. The caller
/// must pass the ext4 incompat-recovery feature state; a disagreement with the
/// JBD2 start field is refused when ext4 claims to be clean. A durable ext4
/// recovery marker with a zero JBD2 start is the valid crash state between the
/// marker flush and the first live journal-superblock write; it replays no
/// blocks but still requires the ordered cleanup plan before the marker clears.
pub fn recover_committed_ring(
    superblock: &JournalSuperblockImage,
    filesystem_needs_recovery: bool,
    maximum_block: u64,
    journal_blocks: &[&[u8]],
) -> Result<JournalRecovery, JournalTransactionError> {
    let expected_slots = usize::try_from(
        superblock
            .maximum_length
            .checked_sub(1)
            .ok_or(JournalTransactionError::RingGeometry)?,
    )
    .map_err(|_| JournalTransactionError::RingGeometry)?;
    if journal_blocks.len() != expected_slots
        || journal_blocks
            .iter()
            .any(|block| block.len() != JOURNAL_BLOCK_BYTES)
    {
        return Err(JournalTransactionError::RingGeometry);
    }
    if !filesystem_needs_recovery {
        if superblock.start_block != 0 {
            return Err(JournalTransactionError::RecoveryStateMismatch);
        }
        return Ok(JournalRecovery {
            committed_transactions: 0,
            consumed_slots: 0,
            replay_images: Vec::new(),
            clean_superblock: superblock.clone(),
            maximum_block,
        });
    }
    if superblock.start_block == 0 {
        return Ok(JournalRecovery {
            committed_transactions: 0,
            consumed_slots: 0,
            replay_images: Vec::new(),
            clean_superblock: superblock.clone(),
            maximum_block,
        });
    }

    let mut cursor = usize::try_from(superblock.start_block - 1)
        .map_err(|_| JournalTransactionError::RingGeometry)?;
    let mut consumed_slots = 0usize;
    let mut committed_transactions = 0usize;
    let mut sequence = superblock.sequence;
    let mut replay = BTreeMap::new();

    while consumed_slots < journal_blocks.len() {
        let descriptor = journal_blocks[cursor];
        let Some(descriptor_header) = JournalBlockHeader::read_bytes(descriptor) else {
            break;
        };
        if descriptor_header.block_type != JournalBlockType::DESCRIPTOR
            || descriptor_header.sequence != sequence
        {
            break;
        }
        let replay_superblock = JournalSuperblock {
            block_size: u32::try_from(JOURNAL_BLOCK_BYTES).expect("4096 fits u32"),
            sequence,
            start_block: 0,
            uuid: superblock.uuid,
        };
        validate_descriptor_block_checksum(&replay_superblock, descriptor)
            .map_err(|_| JournalTransactionError::CorruptDescriptor)?;
        let mut tag_count = 0usize;
        for tag in DescriptorBlockTagIter::new(&descriptor[JournalBlockHeader::SIZE..]) {
            let _ = tag.map_err(|_| JournalTransactionError::CorruptDescriptor)?;
            tag_count = tag_count
                .checked_add(1)
                .ok_or(JournalTransactionError::TooManyBlocks)?;
        }
        if tag_count == 0 || tag_count > JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS {
            return Err(JournalTransactionError::CorruptDescriptor);
        }

        let record_offset = tag_count
            .checked_add(1)
            .ok_or(JournalTransactionError::TooManyBlocks)?;
        let minimum_slots = record_offset
            .checked_add(1)
            .ok_or(JournalTransactionError::TooManyBlocks)?;
        if minimum_slots > journal_blocks.len() - consumed_slots {
            break;
        }
        let record_index = (cursor + record_offset) % journal_blocks.len();
        let record_header = JournalBlockHeader::read_bytes(journal_blocks[record_index]);
        let has_revocation = record_header.is_some_and(|header| {
            header.block_type == JournalBlockType::REVOCATION && header.sequence == sequence
        });
        if has_revocation && !superblock.block_revocations {
            return Err(JournalTransactionError::RevocationsUnsupported);
        }
        let commit_offset = record_offset + usize::from(has_revocation);
        let transaction_slots = commit_offset
            .checked_add(1)
            .ok_or(JournalTransactionError::TooManyBlocks)?;
        if transaction_slots > journal_blocks.len() - consumed_slots {
            break;
        }
        let commit_index = (cursor + commit_offset) % journal_blocks.len();
        let commit_header = JournalBlockHeader::read_bytes(journal_blocks[commit_index]);
        let commit_present = commit_header.is_some_and(|header| {
            header.block_type == JournalBlockType::COMMIT && header.sequence == sequence
        });
        if !commit_present {
            if !has_revocation {
                let possible_commit_index = (commit_index + 1) % journal_blocks.len();
                let possible_commit =
                    JournalBlockHeader::read_bytes(journal_blocks[possible_commit_index]);
                if possible_commit.is_some_and(|header| {
                    header.block_type == JournalBlockType::COMMIT && header.sequence == sequence
                }) {
                    return Err(JournalTransactionError::CorruptRevocation);
                }
            }
            break;
        }

        let mut transaction = Vec::new();
        transaction
            .try_reserve_exact(transaction_slots)
            .map_err(|_| JournalTransactionError::TooManyBlocks)?;
        for offset in 0..transaction_slots {
            transaction.push(journal_blocks[(cursor + offset) % journal_blocks.len()]);
        }
        let images = replay_committed_transaction(
            *superblock.uuid.as_bytes(),
            sequence,
            maximum_block,
            &transaction,
        )?;
        if has_revocation {
            let mut revoked_blocks = Vec::new();
            read_revocation_block_table(transaction[record_offset], &mut revoked_blocks)
                .map_err(|_| JournalTransactionError::CorruptRevocation)?;
            for revoked in revoked_blocks {
                let _ = replay.remove(&revoked);
            }
        }
        for image in images {
            replay.insert(image.block_index, image);
        }

        consumed_slots = consumed_slots
            .checked_add(transaction_slots)
            .ok_or(JournalTransactionError::RingGeometry)?;
        committed_transactions = committed_transactions
            .checked_add(1)
            .ok_or(JournalTransactionError::TooManyBlocks)?;
        cursor = (cursor + transaction_slots) % journal_blocks.len();
        sequence = sequence
            .checked_add(1)
            .ok_or(JournalTransactionError::SequenceOverflow)?;
        if sequence == u32::MAX {
            return Err(JournalTransactionError::SequenceOverflow);
        }
    }

    Ok(JournalRecovery {
        committed_transactions,
        consumed_slots,
        replay_images: replay.into_values().collect(),
        clean_superblock: superblock.with_state(sequence, 0)?,
        maximum_block,
    })
}

/// Validate and replay one complete descriptor/data/commit transaction.
///
/// No home-block images are returned unless the final commit header, sequence,
/// and checksum are valid. Callers can therefore discard every power-cut
/// prefix before the durable commit without exposing partial metadata.
/// `maximum_block` is the highest valid replay target, inclusive.
pub fn replay_committed_transaction(
    journal_uuid: [u8; 16],
    sequence: u32,
    maximum_block: u64,
    journal_blocks: &[&[u8]],
) -> Result<Vec<JournalBlockImage>, JournalTransactionError> {
    if journal_blocks.len() < 2 {
        return Err(JournalTransactionError::MissingCommit);
    }
    if journal_blocks
        .iter()
        .any(|block| block.len() != JOURNAL_BLOCK_BYTES)
    {
        return Err(JournalTransactionError::IncorrectBlockLength);
    }
    let superblock = JournalSuperblock {
        block_size: u32::try_from(JOURNAL_BLOCK_BYTES).expect("4096 fits u32"),
        sequence,
        start_block: 0,
        uuid: Uuid::new(journal_uuid),
    };
    let descriptor = journal_blocks[0];
    let descriptor_header = JournalBlockHeader::read_bytes(descriptor)
        .ok_or(JournalTransactionError::CorruptDescriptor)?;
    if descriptor_header.block_type != JournalBlockType::DESCRIPTOR
        || descriptor_header.sequence != sequence
        || validate_descriptor_block_checksum(&superblock, descriptor).is_err()
    {
        return Err(JournalTransactionError::CorruptDescriptor);
    }
    let mut tags = Vec::new();
    for tag in DescriptorBlockTagIter::new(&descriptor[JournalBlockHeader::SIZE..]) {
        tags.push(tag.map_err(|_| JournalTransactionError::CorruptDescriptor)?);
    }
    let without_revocation = tags
        .len()
        .checked_add(2)
        .ok_or(JournalTransactionError::TooManyBlocks)?;
    let with_revocation = without_revocation
        .checked_add(1)
        .ok_or(JournalTransactionError::TooManyBlocks)?;
    if tags.is_empty()
        || (journal_blocks.len() != without_revocation && journal_blocks.len() != with_revocation)
    {
        return Err(JournalTransactionError::JournalSlotCount);
    }
    if tags.len() > JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS {
        return Err(JournalTransactionError::TooManyBlocks);
    }

    let mut replay = Vec::new();
    replay
        .try_reserve_exact(tags.len())
        .map_err(|_| JournalTransactionError::TooManyBlocks)?;
    for (index, tag) in tags.iter().enumerate() {
        if tag.block_index > maximum_block {
            return Err(JournalTransactionError::BlockOutOfRange);
        }
        if replay
            .iter()
            .any(|image: &JournalBlockImage| image.block_index == tag.block_index)
        {
            return Err(JournalTransactionError::DuplicateBlock);
        }
        let data = journal_blocks[index + 1];
        let mut checksum = Checksum::new();
        checksum.update(superblock.uuid.as_bytes());
        checksum.update_u32_be(sequence);
        checksum.update(data);
        if checksum.finalize() != tag.checksum {
            return Err(JournalTransactionError::CorruptData);
        }
        replay.push(JournalBlockImage {
            block_index: tag.block_index,
            bytes: Vec::from(data),
        });
    }

    if journal_blocks.len() == with_revocation {
        let revocation = journal_blocks[tags.len() + 1];
        let header = JournalBlockHeader::read_bytes(revocation)
            .ok_or(JournalTransactionError::CorruptRevocation)?;
        if header.block_type != JournalBlockType::REVOCATION
            || header.sequence != sequence
            || validate_revocation_block_checksum(&superblock, revocation).is_err()
        {
            return Err(JournalTransactionError::CorruptRevocation);
        }
        let mut revoked_blocks = Vec::new();
        read_revocation_block_table(revocation, &mut revoked_blocks)
            .map_err(|_| JournalTransactionError::CorruptRevocation)?;
        if revoked_blocks.len() > JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS {
            return Err(JournalTransactionError::TooManyBlocks);
        }
        for (index, revoked) in revoked_blocks.iter().enumerate() {
            if *revoked > maximum_block || revoked_blocks[..index].contains(revoked) {
                return Err(JournalTransactionError::CorruptRevocation);
            }
        }
        replay.retain(|image| !revoked_blocks.contains(&image.block_index));
    }

    let commit = journal_blocks[journal_blocks.len() - 1];
    let commit_header =
        JournalBlockHeader::read_bytes(commit).ok_or(JournalTransactionError::MissingCommit)?;
    if commit_header.block_type != JournalBlockType::COMMIT
        || commit_header.sequence != sequence
        || validate_commit_block_checksum(&superblock, commit).is_err()
    {
        return Err(JournalTransactionError::CorruptCommit);
    }
    Ok(replay)
}

fn write_u32be(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_be_bytes());
}

fn update_journal_superblock_checksum(bytes: &mut [u8]) {
    write_u32be(bytes, JOURNAL_SUPERBLOCK_CHECKSUM_OFFSET, 0);
    let checksum = journal_superblock_checksum(bytes);
    write_u32be(bytes, JOURNAL_SUPERBLOCK_CHECKSUM_OFFSET, checksum);
}

fn journal_superblock_checksum(bytes: &[u8]) -> u32 {
    let mut checksum = Checksum::new();
    checksum.update(&bytes[..JOURNAL_SUPERBLOCK_CHECKSUM_OFFSET]);
    checksum.update_u32_be(0);
    checksum.update(&bytes[JOURNAL_SUPERBLOCK_CHECKSUM_OFFSET + 4..]);
    checksum.finalize()
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::collections::BTreeMap;

    const UUID: [u8; 16] = [0x5a; 16];
    const MAXIMUM_BLOCK: u64 = 4095;
    const JOURNAL_SLOTS: [u64; 4] = [3000, 3001, 3002, 3003];

    fn filled(value: u8) -> Vec<u8> {
        vec![value; JOURNAL_BLOCK_BYTES]
    }

    fn transaction() -> JournalTransaction {
        let mut transaction = JournalTransaction::new(17, UUID, MAXIMUM_BLOCK).unwrap();
        transaction.stage_ordered_data(100, &filled(0x11)).unwrap();
        transaction.stage_metadata(200, &filled(0x22)).unwrap();
        transaction.stage_metadata(201, &filled(0x33)).unwrap();
        transaction
    }

    fn mapped_ring(next_sequence: u32, slots: &[u64]) -> JournalRing {
        let superblock = JournalSuperblockImage::new_clean(
            next_sequence,
            UUID,
            u32::try_from(slots.len() + 1).unwrap(),
        )
        .unwrap();
        let mut physical = Vec::with_capacity(slots.len() + 1);
        physical.push(MAXIMUM_BLOCK);
        physical.extend_from_slice(slots);
        superblock.map_clean_ring(MAXIMUM_BLOCK, &physical).unwrap()
    }

    fn mapped_filesystem_ring(next_sequence: u32, slots: &[u64]) -> JournalRing {
        let mut ring = mapped_ring(next_sequence, slots);
        ring.filesystem_superblock = Some(FilesystemSuperblockImage {
            bytes: [0; FILESYSTEM_SUPERBLOCK_BYTES],
            needs_recovery: false,
        });
        ring.filesystem_recovery_state = Some(FilesystemRecoveryState::Clean);
        ring
    }

    fn finish_transaction(ring: &mut JournalRing, prepared: &JournalPreparedTransaction) {
        let _commit = ring.prepare_commit_plan(prepared).unwrap();
        ring.mark_commit_durable(prepared.ticket()).unwrap();
        let _checkpoint = ring.prepare_checkpoint_plan(prepared.ticket()).unwrap();
        ring.checkpoint_durable(prepared.ticket()).unwrap();
    }

    #[test]
    fn clean_filesystem_state_waits_for_durable_empty_journal() {
        let mut ring = mapped_filesystem_ring(17, &JOURNAL_SLOTS);
        assert_eq!(ring.filesystem_is_clean(), Ok(true));
        let mut mismatched_head = ring.clone();
        mismatched_head.head = 1;
        assert_eq!(mismatched_head.filesystem_is_clean(), Ok(false));
        let mut mismatched_sequence = ring.clone();
        mismatched_sequence.next_sequence = 18;
        assert_eq!(mismatched_sequence.filesystem_is_clean(), Ok(false));
        assert_eq!(
            ring.prepare_filesystem_clean_plan(),
            Err(JournalTransactionError::ReservationState)
        );
        let marker = ring.prepare_recovery_marker_plan().unwrap();
        assert_eq!(marker.len(), 2);
        assert_eq!(ring.filesystem_is_clean(), Ok(false));
        ring.mark_recovery_marker_durable().unwrap();
        assert_eq!(ring.filesystem_is_clean(), Ok(false));

        let mut transaction = ring.begin_transaction().unwrap();
        transaction.stage_metadata(200, &filled(0x22)).unwrap();
        let prepared = ring.prepare(&transaction).unwrap();
        assert_eq!(
            ring.prepare_filesystem_clean_plan(),
            Err(JournalTransactionError::JournalNotClean)
        );
        finish_transaction(&mut ring, &prepared);

        let clean = ring.prepare_filesystem_clean_plan().unwrap();
        assert_eq!(ring.filesystem_is_clean(), Ok(false));
        assert_eq!(ring.prepare_filesystem_clean_plan().unwrap(), clean);
        assert!(matches!(
            &clean[0],
            JournalCommitOperation::WriteFilesystemSuperblock { start_byte, image }
                if *start_byte == FILESYSTEM_SUPERBLOCK_START_BYTE
                    && !image.needs_recovery()
        ));
        assert_eq!(
            clean[1],
            JournalCommitOperation::Flush(JournalFlush::FilesystemState)
        );
        let mut blocked = ring.begin_transaction().unwrap();
        blocked.stage_metadata(201, &filled(0x33)).unwrap();
        assert_eq!(
            ring.prepare(&blocked),
            Err(JournalTransactionError::ReservationState)
        );
        ring.mark_filesystem_clean_durable().unwrap();
        assert_eq!(ring.filesystem_is_clean(), Ok(true));
        assert_eq!(
            ring.mark_filesystem_clean_durable(),
            Err(JournalTransactionError::ReservationState)
        );

        let prepared = ring.prepare(&blocked).unwrap();
        assert_eq!(ring.filesystem_is_clean(), Ok(false));
        assert_eq!(
            ring.prepare_commit_plan(&prepared),
            Err(JournalTransactionError::RecoveryMarkerNotDurable)
        );
        let marker = ring.prepare_recovery_marker_plan().unwrap();
        assert!(matches!(
            &marker[0],
            JournalCommitOperation::WriteFilesystemSuperblock { image, .. }
                if image.needs_recovery()
        ));
        ring.mark_recovery_marker_durable().unwrap();
        assert!(ring.prepare_commit_plan(&prepared).is_ok());
    }

    fn journal_images(operations: &[JournalCommitOperation]) -> Vec<Vec<u8>> {
        operations
            .iter()
            .filter_map(|operation| match operation {
                JournalCommitOperation::WriteJournal { bytes, .. } => Some(bytes.clone()),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn transaction_round_trips_through_reader_checks() {
        let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
        let journal = journal_images(&operations);
        let references: Vec<&[u8]> = journal.iter().map(Vec::as_slice).collect();
        let replay = replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &references).unwrap();

        assert_eq!(replay.len(), 2);
        assert_eq!(replay[0].block_index(), 200);
        assert_eq!(replay[0].bytes(), filled(0x22));
        assert_eq!(replay[1].block_index(), 201);
        assert_eq!(replay[1].bytes(), filled(0x33));
    }

    #[test]
    fn descriptor_has_linux_compatible_first_uuid_layout() {
        const SAME_UUID: u32 = 0x2;
        const LAST_TAG: u32 = 0x8;

        let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
        let descriptor = journal_images(&operations).remove(0);
        let first_tag = JournalBlockHeader::SIZE;
        assert_eq!(
            u32::from_be_bytes(descriptor[first_tag + 4..first_tag + 8].try_into().unwrap()),
            0
        );
        assert_eq!(&descriptor[first_tag + 16..first_tag + 32], &UUID);

        let second_tag = first_tag + 32;
        assert_eq!(
            u32::from_be_bytes(
                descriptor[second_tag + 4..second_tag + 8]
                    .try_into()
                    .unwrap()
            ),
            SAME_UUID | LAST_TAG
        );
    }

    #[test]
    fn commit_flush_precedes_every_home_metadata_write() {
        let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
        let ordered_data_flush = operations
            .iter()
            .position(|operation| {
                *operation == JournalCommitOperation::Flush(JournalFlush::OrderedData)
            })
            .unwrap();
        let commit_record = operations
            .iter()
            .position(|operation| {
                matches!(
                    operation,
                    JournalCommitOperation::WriteJournal {
                        kind: JournalRecordKind::Commit,
                        ..
                    }
                )
            })
            .unwrap();
        let commit_flush = operations
            .iter()
            .position(|operation| *operation == JournalCommitOperation::Flush(JournalFlush::Commit))
            .unwrap();
        let first_home = operations
            .iter()
            .position(|operation| matches!(operation, JournalCommitOperation::WriteHomeMetadata(_)))
            .unwrap();

        assert!(ordered_data_flush < commit_record);
        assert!(commit_flush < first_home);
        assert!(operations[..commit_flush].iter().all(|operation| {
            !matches!(operation, JournalCommitOperation::WriteHomeMetadata(_))
        }));
    }

    #[test]
    fn every_power_cut_before_commit_flush_discards_metadata() {
        let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
        let commit_flush = operations
            .iter()
            .position(|operation| *operation == JournalCommitOperation::Flush(JournalFlush::Commit))
            .unwrap();

        for cut in 0..=operations.len() {
            let mut pending_journal = BTreeMap::new();
            let mut durable_journal = BTreeMap::new();
            let mut pending_ordered_data = BTreeMap::new();
            let mut durable_ordered_data = BTreeMap::new();
            let mut pending_home = BTreeMap::new();
            let mut durable_home = BTreeMap::new();
            for operation in &operations[..cut] {
                match operation {
                    JournalCommitOperation::WriteJournal {
                        journal_block,
                        bytes,
                        ..
                    } => {
                        pending_journal.insert(*journal_block, bytes.clone());
                    }
                    JournalCommitOperation::WriteHomeMetadata(image) => {
                        pending_home.insert(image.block_index, image.bytes.clone());
                    }
                    JournalCommitOperation::WriteOrderedData(image) => {
                        pending_ordered_data.insert(image.block_index, image.bytes.clone());
                    }
                    JournalCommitOperation::Flush(_) => {
                        durable_journal.extend(pending_journal.clone());
                        durable_ordered_data.extend(pending_ordered_data.clone());
                        durable_home.extend(pending_home.clone());
                    }
                    JournalCommitOperation::WriteFilesystemSuperblock { .. } => {}
                    JournalCommitOperation::WriteJournalSuperblock { .. } => {}
                }
            }

            if cut <= commit_flush {
                assert!(durable_home.is_empty());
                let durable: Option<Vec<&[u8]>> = JOURNAL_SLOTS
                    .iter()
                    .map(|slot| durable_journal.get(slot).map(Vec::as_slice))
                    .collect();
                assert!(durable.is_none());
            } else {
                assert_eq!(durable_ordered_data.get(&100), Some(&filled(0x11)));
                let durable: Vec<&[u8]> = JOURNAL_SLOTS
                    .iter()
                    .map(|slot| durable_journal.get(slot).unwrap().as_slice())
                    .collect();
                let replay =
                    replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &durable).unwrap();
                assert_eq!(replay.len(), 2);
            }
        }
    }

    #[test]
    fn corruption_and_truncation_are_not_replayable() {
        let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
        let journal = journal_images(&operations);

        for (index, expected) in [
            (0usize, JournalTransactionError::CorruptDescriptor),
            (1usize, JournalTransactionError::CorruptData),
            (3usize, JournalTransactionError::CorruptCommit),
        ] {
            let mut corrupt = journal.clone();
            corrupt[index][128] ^= 0x80;
            let references: Vec<&[u8]> = corrupt.iter().map(Vec::as_slice).collect();
            assert_eq!(
                replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &references),
                Err(expected)
            );
        }
        let references: Vec<&[u8]> = journal[..3].iter().map(Vec::as_slice).collect();
        assert!(replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &references).is_err());

        let references: Vec<&[u8]> = journal.iter().map(Vec::as_slice).collect();
        assert_eq!(
            replay_committed_transaction(UUID, 17, 199, &references),
            Err(JournalTransactionError::BlockOutOfRange)
        );
    }

    #[test]
    fn revocations_are_checksummed_and_remove_stale_images() {
        let mut transaction = transaction();
        transaction.stage_revocation(200).unwrap();
        let slots = [3000, 3001, 3002, 3003, 3004];
        let operations = transaction.commit_plan(&slots).unwrap();
        assert!(operations.iter().any(|operation| {
            matches!(
                operation,
                JournalCommitOperation::WriteJournal {
                    kind: JournalRecordKind::Revocation,
                    ..
                }
            )
        }));
        let journal = journal_images(&operations);
        let references: Vec<&[u8]> = journal.iter().map(Vec::as_slice).collect();
        let replay = replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &references).unwrap();
        assert_eq!(replay.len(), 1);
        assert_eq!(replay[0].block_index(), 201);

        let mut corrupt = journal;
        corrupt[3][128] ^= 0x80;
        let references: Vec<&[u8]> = corrupt.iter().map(Vec::as_slice).collect();
        assert_eq!(
            replay_committed_transaction(UUID, 17, MAXIMUM_BLOCK, &references),
            Err(JournalTransactionError::CorruptRevocation)
        );
    }

    #[test]
    fn clean_ring_wraps_and_reclaims_only_after_checkpoint() {
        let slots = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007];
        let mut ring = mapped_ring(40, &slots);

        let mut first = ring.begin_transaction().unwrap();
        first.stage_metadata(100, &filled(1)).unwrap();
        let first = ring.prepare(&first).unwrap();
        assert_eq!(first.journal_blocks(), &slots[..3]);
        finish_transaction(&mut ring, &first);

        let mut second = ring.begin_transaction().unwrap();
        second.stage_metadata(101, &filled(2)).unwrap();
        second.stage_metadata(102, &filled(3)).unwrap();
        let second = ring.prepare(&second).unwrap();
        assert_eq!(second.journal_blocks(), &slots[3..7]);
        finish_transaction(&mut ring, &second);

        let mut wrapped = ring.begin_transaction().unwrap();
        wrapped.stage_metadata(103, &filled(4)).unwrap();
        let wrapped = ring.prepare(&wrapped).unwrap();
        assert_eq!(wrapped.journal_blocks(), &[3007, 3000, 3001]);
        assert_eq!(wrapped.sequence(), 42);
        assert_eq!(ring.next_sequence(), 43);
        assert_eq!(ring.used_slots(), 3);
    }

    #[test]
    fn ring_refuses_reuse_and_out_of_order_transitions() {
        let slots = [3000, 3001, 3002, 3003, 3004, 3005];
        let mut ring = mapped_ring(50, &slots);
        let mut first = ring.begin_transaction().unwrap();
        first.stage_metadata(100, &filled(1)).unwrap();
        let first = ring.prepare(&first).unwrap();
        let mut second = ring.begin_transaction().unwrap();
        second.stage_metadata(101, &filled(2)).unwrap();
        let second = ring.prepare(&second).unwrap();
        let mut full = ring.begin_transaction().unwrap();
        full.stage_metadata(102, &filled(3)).unwrap();
        assert_eq!(ring.prepare(&full), Err(JournalTransactionError::RingFull));
        assert_eq!(
            ring.mark_commit_durable(second.ticket()),
            Err(JournalTransactionError::ReservationOrder)
        );
        assert_eq!(
            ring.checkpoint_durable(first.ticket()),
            Err(JournalTransactionError::ReservationState)
        );
        assert_eq!(
            ring.abort_precommit(first.ticket()),
            Err(JournalTransactionError::ReservationOrder)
        );
        let _first_commit = ring.prepare_commit_plan(&first).unwrap();
        ring.mark_commit_durable(first.ticket()).unwrap();
        let _second_commit = ring.prepare_commit_plan(&second).unwrap();
        ring.mark_commit_durable(second.ticket()).unwrap();
        assert_eq!(
            ring.checkpoint_durable(second.ticket()),
            Err(JournalTransactionError::ReservationOrder)
        );
        let _first_checkpoint = ring.prepare_checkpoint_plan(first.ticket()).unwrap();
        ring.checkpoint_durable(first.ticket()).unwrap();
        let _second_checkpoint = ring.prepare_checkpoint_plan(second.ticket()).unwrap();
        ring.checkpoint_durable(second.ticket()).unwrap();
        assert_eq!(ring.used_slots(), 0);

        let mut aborted = ring.begin_transaction().unwrap();
        aborted.stage_metadata(103, &filled(4)).unwrap();
        let aborted = ring.prepare(&aborted).unwrap();
        let aborted_sequence = aborted.sequence();
        ring.abort_precommit(aborted.ticket()).unwrap();
        assert_eq!(ring.used_slots(), 0);
        assert_eq!(ring.next_sequence(), aborted_sequence);
        assert_eq!(
            ring.mark_commit_durable(aborted.ticket()),
            Err(JournalTransactionError::ReservationUnknown)
        );
    }

    #[test]
    fn hostile_bounds_duplicates_and_escape_are_refused() {
        assert_eq!(
            JournalTransaction::new(u32::MAX, UUID, MAXIMUM_BLOCK).unwrap_err(),
            JournalTransactionError::SequenceOverflow
        );
        assert_eq!(
            JournalTransaction::new(1, UUID, MAXIMUM_BLOCK)
                .unwrap()
                .commit_plan(&[100, 101]),
            Err(JournalTransactionError::EmptyTransaction)
        );

        let mut transaction = JournalTransaction::new(9, UUID, MAXIMUM_BLOCK).unwrap();
        assert_eq!(
            transaction.stage_metadata(1, &[0; 8]),
            Err(JournalTransactionError::IncorrectBlockLength)
        );
        assert_eq!(
            transaction.stage_metadata(MAXIMUM_BLOCK + 1, &filled(1)),
            Err(JournalTransactionError::BlockOutOfRange)
        );
        transaction.stage_metadata(2, &filled(2)).unwrap();
        assert_eq!(
            transaction.stage_ordered_data(2, &filled(3)),
            Err(JournalTransactionError::DuplicateBlock)
        );
        let mut escaped = filled(0);
        escaped[..4].copy_from_slice(&JournalBlockHeader::MAGIC.to_be_bytes());
        assert_eq!(
            transaction.stage_metadata(3, &escaped),
            Err(JournalTransactionError::EscapedBlockUnsupported)
        );
        assert_eq!(
            transaction.commit_plan(&[100, 101]),
            Err(JournalTransactionError::JournalSlotCount)
        );
        assert_eq!(
            transaction.commit_plan(&[100, 100, 102]),
            Err(JournalTransactionError::DuplicateBlock)
        );
        assert_eq!(
            transaction.commit_plan(&[2, 101, 102]),
            Err(JournalTransactionError::JournalSlotOverlap)
        );
        assert_eq!(
            transaction.commit_plan(&[100, 101, MAXIMUM_BLOCK + 1]),
            Err(JournalTransactionError::BlockOutOfRange)
        );
        transaction.stage_revocation(4).unwrap();
        assert_eq!(
            transaction.stage_revocation(4),
            Err(JournalTransactionError::DuplicateBlock)
        );
        assert_eq!(
            transaction.stage_revocation(MAXIMUM_BLOCK + 1),
            Err(JournalTransactionError::BlockOutOfRange)
        );
        let mut no_revocations =
            JournalRing::new_clean(9, UUID, MAXIMUM_BLOCK, false, &[100, 101, 102, 103]).unwrap();
        assert_eq!(
            no_revocations.prepare(&transaction),
            Err(JournalTransactionError::RevocationsUnsupported)
        );

        assert_eq!(
            JournalRing::new_clean(1, UUID, MAXIMUM_BLOCK, true, &[1, 2]),
            Err(JournalTransactionError::RingGeometry)
        );
        assert_eq!(
            JournalRing::new_clean(1, UUID, MAXIMUM_BLOCK, true, &[1, 2, 2]),
            Err(JournalTransactionError::RingGeometry)
        );
        assert_eq!(
            JournalRing::new_clean(u32::MAX, UUID, MAXIMUM_BLOCK, true, &[3000, 3001, 3002]),
            Err(JournalTransactionError::SequenceOverflow)
        );

        let mut bounded = JournalTransaction::new(10, UUID, MAXIMUM_BLOCK).unwrap();
        for block in 0..JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS {
            bounded
                .stage_metadata(u64::try_from(block).unwrap(), &filled(4))
                .unwrap();
        }
        assert_eq!(
            bounded.stage_metadata(
                u64::try_from(JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS).unwrap(),
                &filled(5)
            ),
            Err(JournalTransactionError::TooManyBlocks)
        );
    }
}
