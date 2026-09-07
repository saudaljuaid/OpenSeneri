// SPDX-License-Identifier: GPL-3.0-only
//! Checked, journaled ext4 operations over Phipia's native block boundary.

extern crate alloc;

use alloc::boxed::Box;
use alloc::collections::{BTreeMap, BTreeSet};
use alloc::rc::Rc;
use alloc::vec::Vec;
use core::error::Error;
use core::fmt::{self, Display, Formatter};
use core::time::Duration;
use ext4plus::dir::Dir;
use ext4plus::error::Ext4Error;
use ext4plus::inode::{Inode, InodeCreationOptions, InodeFlags, InodeMode};
use ext4plus::path::{Path, PathBuf};
use ext4plus::{
    DirEntryName, Ext4, Ext4Read, FileType, FilesystemSuperblockCheckpoint, FilesystemSuperblockImage,
    FollowSymlinks, JOURNAL_BLOCK_BYTES, JournalCommitOperation,
    JournalBlockImage, JournalExecutionError, JournalFlush, JournalInodeMap, JournalInodeMapError,
    JournalMutationStage, JournalMutationStageError, JournalPreparedTransaction, JournalRing, JournalStorage,
    execute_commit_operations, load_journal_inode_map, recover_committed_ring,
};

const SUPERBLOCK_BYTES: usize = 1024;
const SUPERBLOCK_START: u64 = 1024;
const BLOCK_BYTES: u64 = 4096;
const COMPAT_FEATURES: u32 = 0x002c;
const INCOMPAT_FEATURES: u32 = 0x20c2;
const INCOMPAT_RECOVERY_FEATURE: u32 = 0x0004;
const READ_ONLY_FEATURES: u32 = 0x046b;
const MAX_VALIDATED_ENTRIES: usize = 8_192;
const MAX_PENDING_DIRECTORIES: usize = 512;
const MAX_PROBE_WRITE_BYTES: usize = 64 * JOURNAL_BLOCK_BYTES;
const TRANSACTION_WRITE_BYTES: usize = 32 * JOURNAL_BLOCK_BYTES;
const MAX_SPLIT_ORPHAN_BYTES: u64 = 64 * 1024 * 1024;

/// A pointer-free identity copied from a validated ext4 superblock.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(C)]
pub(crate) struct Identity {
    pub(crate) label: [u8; 16],
    pub(crate) uuid: [u8; 16],
    pub(crate) recovered_transactions: u32,
    pub(crate) replayed_blocks: u32,
    pub(crate) consumed_slots: u32,
    pub(crate) recovery_performed: u8,
    pub(crate) reserved: [u8; 3],
}

/// Pointer-free metadata returned to C.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
#[repr(C)]
pub(crate) struct Metadata {
    pub(crate) inode: u64,
    pub(crate) size: u64,
    pub(crate) uid: u32,
    pub(crate) gid: u32,
    pub(crate) mode: u16,
    pub(crate) links: u16,
    pub(crate) file_type: u8,
    pub(crate) reserved: [u8; 7],
}

/// One ext4 directory entry returned without borrowing Rust storage.
#[derive(Clone, Copy)]
#[repr(C)]
pub(crate) struct DirectoryEntry {
    pub(crate) metadata: Metadata,
    pub(crate) name_length: u16,
    pub(crate) name: [u8; 255],
    pub(crate) reserved: u8,
}

impl Default for DirectoryEntry {
    fn default() -> Self {
        Self {
            metadata: Metadata::default(),
            name_length: 0,
            name: [0; 255],
            reserved: 0,
        }
    }
}

/// A filesystem whose only upstream writer is an in-memory journal stage.
/// C installs a short NVMe lease per operation.
pub(crate) struct Mounted {
    filesystem: Option<Ext4>,
    journal: JournalRing,
    stage: Rc<JournalMutationStage>,
    pending_mutation: Option<PendingMutation>,
    pending_write: Option<PendingWrite>,
    pending_reclaim: Option<PendingReclaim>,
    context: usize,
    image_bytes: u64,
}

impl Mounted {
    fn filesystem(&self) -> Result<&Ext4, Status> {
        self.filesystem.as_ref().ok_or(Status::Io)
    }

    fn readable_filesystem(&self) -> Result<&Ext4, Status> {
        // A retained transaction may contain uncommitted namespace and allocator
        // updates. Only the reloaded checkpointed view is public.
        if self.pending_write.is_some() || self.pending_mutation.is_some() || self.pending_reclaim.is_some()
            || !self.stage.is_empty() || self.stage.is_sealed() {
            return Err(Status::Io);
        }
        self.filesystem()
    }
}

/// Return the allocator's current capacity for the admitted 4 KiB profile.
/// A committed mutation reloads the checked filesystem view, so callers never
/// confuse unused NVMe namespace bytes with allocatable ext4 blocks.
pub(crate) fn free_bytes(mounted: &Mounted) -> Result<u64, Status> {
    mounted
        .readable_filesystem()?
        .superblock()
        .free_blocks_count()
        .checked_mul(BLOCK_BYTES)
        .filter(|bytes| *bytes <= mounted.image_bytes)
        .ok_or(Status::Invalid)
}

enum PendingMutationPhase {
    Commit(JournalPreparedTransaction),
    Checkpoint(u64),
    Reload,
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum PendingMutationKind {
    Write,
    Truncate,
    CreateFile,
    UnlinkFile,
    FinalizeOrphan,
    LinkFile,
    CreateDirectory,
    RemoveDirectory,
    Rename,
    RenameReplace,
    Symlink,
    Chmod,
    SetXattr,
    RemoveXattr,
    SetTimes,
}

struct PendingMutation {
    kind: PendingMutationKind,
    path: Vec<u8>,
    source: Vec<u8>,
    offset: u64,
    written: usize,
    checkpointed_superblock: Option<FilesystemSuperblockCheckpoint>,
    phase: PendingMutationPhase,
}

struct PendingWrite {
    path: Vec<u8>,
    source: Vec<u8>,
    offset: u64,
    completed: usize,
    append: bool,
    chunk_bytes: usize,
}

struct PendingReclaim {
    kind: PendingMutationKind,
    path: Vec<u8>,
    argument: u64,
    inode: u64,
    retain_unlinked: bool,
    namespace_committed: bool,
}

enum WriteTransactionOutcome {
    Written(usize),
    StageFull,
}

#[derive(Debug)]
struct BlockReadError;

impl Display for BlockReadError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        formatter.write_str("Phipia block read failed")
    }
}

impl Error for BlockReadError {}

#[derive(Debug)]
struct BlockStorageError;

impl Display for BlockStorageError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> fmt::Result {
        formatter.write_str("Phipia block write or flush failed")
    }
}

impl Error for BlockStorageError {}

struct PhipiaReader {
    context: usize,
}

impl Ext4Read for PhipiaReader {
    fn read(
        &self,
        start_byte: u64,
        destination: &mut [u8],
    ) -> Result<(), Box<dyn Error + Send + Sync + 'static>> {
        if crate::abi::ext4_block_read(self.context, start_byte, destination) {
            Ok(())
        } else {
            Err(Box::new(BlockReadError))
        }
    }
}

struct PhipiaJournalStorage {
    context: usize,
}

/// Read-only post-replay view used to validate recovered metadata before any home
/// write. Clearing recovery here only disables upstream's second replay; this
/// image is never sent to the platform executor.
struct RecoveryValidationReader {
    context: usize,
    images: Vec<JournalBlockImage>,
    superblock: FilesystemSuperblockImage,
}

impl Ext4Read for RecoveryValidationReader {
    fn read(&self, start: u64, destination: &mut [u8])
        -> Result<(), Box<dyn Error + Send + Sync + 'static>> {
        PhipiaReader { context: self.context }.read(start, destination)?;
        let end = start.checked_add(destination.len() as u64)
            .ok_or_else(|| Box::new(BlockReadError) as Box<dyn Error + Send + Sync>)?;
        let mut overlay = |offset: u64, bytes: &[u8]| {
            let first = start.max(offset);
            let last = end.min(offset + bytes.len() as u64);
            if first < last {
                destination[(first - start) as usize..(last - start) as usize]
                    .copy_from_slice(&bytes[(first - offset) as usize..(last - offset) as usize]);
            }
        };
        for image in &self.images { overlay(image.block_index() * BLOCK_BYTES, image.bytes()); }
        overlay(SUPERBLOCK_START, self.superblock.bytes());
        Ok(())
    }
}

impl JournalStorage for PhipiaJournalStorage {
    type Error = BlockStorageError;

    fn write(&mut self, start_byte: u64, bytes: &[u8]) -> Result<(), Self::Error> {
        if crate::abi::ext4_block_write(self.context, start_byte, bytes) {
            Ok(())
        } else {
            Err(BlockStorageError)
        }
    }

    fn flush(&mut self, boundary: JournalFlush) -> Result<(), Self::Error> {
        let boundary = match boundary {
            JournalFlush::FilesystemState => 0,
            JournalFlush::OrderedData => 1,
            JournalFlush::JournalPayload => 2,
            JournalFlush::Commit => 3,
            JournalFlush::Checkpoint => 4,
            JournalFlush::JournalState => 5,
        };
        if crate::abi::ext4_block_flush(self.context, boundary) {
            Ok(())
        } else {
            Err(BlockStorageError)
        }
    }
}

fn execute_storage_plan(
    context: usize,
    operations: &[JournalCommitOperation],
) -> Result<(), Status> {
    execute_commit_operations(&mut PhipiaJournalStorage { context }, operations).map_err(|error| {
        match error {
            JournalExecutionError::AddressOverflow => Status::Range,
            JournalExecutionError::Storage(_) => Status::Io,
        }
    })
}

fn load_staged_view(
    context: usize,
    image_bytes: u64,
    needs_recovery: bool,
    block_limit: usize,
) -> Result<(Ext4, Rc<JournalMutationStage>), Status> {
    let stage = Rc::new(
        JournalMutationStage::with_block_limit(Box::new(PhipiaReader { context }), image_bytes, block_limit)
            .map_err(|_| Status::Invalid)?,
    );
    let filesystem = if needs_recovery {
        Ext4::load_with_recovery_writer(Box::new(stage.clone()), Some(Box::new(stage.clone())))
    } else {
        Ext4::load_with_writer(Box::new(stage.clone()), Some(Box::new(stage.clone())))
    }
    .map_err(map_error)?;
    let journal = load_journal_inode_map(&filesystem).map_err(map_journal_error)?;
    if journal.filesystem_needs_recovery() != needs_recovery
        || !stage.is_empty()
        || stage.is_sealed()
    {
        return Err(Status::Invalid);
    }
    validate_namespace(&filesystem)?;
    Ok((filesystem, stage))
}

fn replace_staged_view(mounted: &mut Mounted, needs_recovery: bool) -> Result<(), Status> {
    // Drop the old allocator view before any fallible read. If reload fails,
    // every public reader refuses the absent view and sync can retry the load.
    mounted.filesystem = None;
    let (filesystem, stage) =
        load_staged_view(mounted.context, mounted.image_bytes, needs_recovery, mounted.stage.block_limit())?;
    mounted.filesystem = Some(filesystem);
    mounted.stage = stage;
    Ok(())
}

fn discard_uncommitted_stage(mounted: &mut Mounted, needs_recovery: bool) -> Result<(), Status> {
    mounted.filesystem = None;
    mounted.stage.rollback();
    replace_staged_view(mounted, needs_recovery)
}

#[cfg(test)]
pub(crate) fn set_stage_block_limit(mounted: &mut Mounted, block_limit: usize) -> Result<(), Status> {
    mounted.readable_filesystem()?;
    let recovery = mounted.journal.filesystem_recovery_marker_is_durable().map_err(|_| Status::Invalid)?;
    let (filesystem, stage) = load_staged_view(mounted.context, mounted.image_bytes, recovery, block_limit)?;
    mounted.filesystem = Some(filesystem);
    mounted.stage = stage;
    Ok(())
}

fn ensure_staged_view(mounted: &mut Mounted) -> Result<(), Status> {
    if mounted.pending_reclaim.is_some() { return Err(Status::Busy); }
    if mounted.filesystem.is_none() {
        let needs_recovery = mounted.journal.filesystem_recovery_marker_is_durable()
            .map_err(|_| Status::Invalid)?;
        replace_staged_view(mounted, needs_recovery)?;
    }
    Ok(())
}

fn read_u16(bytes: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        bytes.get(offset..offset.checked_add(2)?)?.try_into().ok()?,
    ))
}

fn read_u32(bytes: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        bytes.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
    ))
}

fn validate_profile(context: usize, media_bytes: u64) -> Result<u64, Status> {
    let mut superblock = [0u8; SUPERBLOCK_BYTES];
    if !crate::abi::ext4_block_read(context, SUPERBLOCK_START, &mut superblock) {
        return Err(Status::Io);
    }
    let magic = read_u16(&superblock, 0x38).ok_or(Status::Invalid)?;
    let log_block_size = read_u32(&superblock, 0x18).ok_or(Status::Invalid)?;
    let inode_size = read_u16(&superblock, 0x58).ok_or(Status::Invalid)?;
    let descriptor_size = read_u16(&superblock, 0xfe).ok_or(Status::Invalid)?;
    let compat = read_u32(&superblock, 0x5c).ok_or(Status::Invalid)?;
    let incompat = read_u32(&superblock, 0x60).ok_or(Status::Invalid)?;
    let read_only = read_u32(&superblock, 0x64).ok_or(Status::Invalid)?;
    let blocks = u64::from(read_u32(&superblock, 0x04).ok_or(Status::Invalid)?)
        | (u64::from(read_u32(&superblock, 0x150).ok_or(Status::Invalid)?) << 32);
    let free_blocks = u64::from(read_u32(&superblock, 0x0c).ok_or(Status::Invalid)?)
        | (u64::from(read_u32(&superblock, 0x158).ok_or(Status::Invalid)?) << 32);
    let inodes = read_u32(&superblock, 0x00).ok_or(Status::Invalid)?;
    let free_inodes = read_u32(&superblock, 0x10).ok_or(Status::Invalid)?;
    let blocks_per_group = read_u32(&superblock, 0x20).ok_or(Status::Invalid)?;
    let inodes_per_group = read_u32(&superblock, 0x28).ok_or(Status::Invalid)?;
    let image_bytes = blocks.checked_mul(BLOCK_BYTES).ok_or(Status::Invalid)?;

    if magic != 0xef53
        || log_block_size != 2
        || inode_size != 256
        || descriptor_size != 64
        || read_u32(&superblock, 0x14) != Some(0)
        || compat != COMPAT_FEATURES
        || incompat & !INCOMPAT_RECOVERY_FEATURE != INCOMPAT_FEATURES
        || read_only != READ_ONLY_FEATURES
        || blocks == 0
        || image_bytes > media_bytes
        || free_blocks > blocks
        || inodes == 0
        || free_inodes > inodes
        || blocks_per_group == 0 || u64::from(blocks_per_group) > BLOCK_BYTES * 8
        || inodes_per_group == 0 || u64::from(inodes_per_group) > BLOCK_BYTES * 8
        || read_u32(&superblock, 0x54) != Some(11)
    {
        return Err(Status::Invalid);
    }
    // Match Linux ext4_check_geometry before any inode-table or journal load.
    if blocks.div_ceil(u64::from(blocks_per_group)).checked_mul(u64::from(inodes_per_group)) != Some(u64::from(inodes)) {
        return Err(Status::Invalid);
    }
    let orphan = read_u32(&superblock, 0xe8).ok_or(Status::Invalid)?;
    if orphan != 0 && (orphan < 11 || orphan > inodes || incompat & INCOMPAT_RECOVERY_FEATURE == 0) {
        return Err(Status::Invalid);
    }
    Ok(image_bytes)
}

fn absolute_path(path: &[u8]) -> Result<Vec<u8>, Status> {
    if path == b"." {
        return Ok(Vec::from(&b"/"[..]));
    }
    if path.is_empty() || path.len() >= 4096 {
        return Err(Status::Range);
    }
    let capacity = path.len().checked_add(1).ok_or(Status::Range)?;
    let mut absolute = Vec::new();
    absolute
        .try_reserve_exact(capacity)
        .map_err(|_| Status::Range)?;
    absolute.push(b'/');
    absolute.extend_from_slice(path);
    Path::try_from(absolute.as_slice()).map_err(|_| Status::Invalid)?;
    Ok(absolute)
}

fn parent_and_name(
    absolute: &[u8],
) -> Result<(Path<'_>, DirEntryName<'_>), Status> {
    if absolute.len() <= 1 || absolute.last() == Some(&b'/') {
        return Err(Status::Invalid);
    }
    let separator = absolute
        .iter()
        .rposition(|byte| *byte == b'/')
        .ok_or(Status::Invalid)?;
    let parent_end = separator.max(1);
    let parent = Path::try_from(&absolute[..parent_end])
        .map_err(|_| Status::Invalid)?;
    let name = DirEntryName::try_from(&absolute[separator + 1..])
        .map_err(|_| Status::Invalid)?;
    if name == b"." || name == b".." {
        return Err(Status::Invalid);
    }
    Ok((parent, name))
}

fn namespace_pair_key(first: &[u8], second: &[u8]) -> Result<Vec<u8>, Status> {
    let capacity = first
        .len()
        .checked_add(second.len())
        .and_then(|length| length.checked_add(1))
        .ok_or(Status::Range)?;
    let mut key = Vec::new();
    key.try_reserve_exact(capacity).map_err(|_| Status::Range)?;
    key.extend_from_slice(first);
    key.push(0);
    key.extend_from_slice(second);
    Ok(key)
}

fn classify(file_type: FileType) -> Result<u8, Status> {
    if file_type.is_regular_file() {
        Ok(1)
    } else if file_type.is_dir() {
        Ok(2)
    } else if file_type.is_symlink() {
        Ok(3)
    } else {
        Err(Status::Special)
    }
}

fn inode_metadata(inode: &ext4plus::inode::Inode) -> Result<Metadata, Status> {
    Ok(Metadata {
        inode: u64::from(inode.index.get()),
        size: inode.size_in_bytes(),
        uid: inode.uid(),
        gid: inode.gid(),
        mode: inode.mode().bits(),
        links: inode.links_count(),
        file_type: classify(inode.file_type())?,
        reserved: [0; 7],
    })
}

fn map_error(error: Ext4Error) -> Status {
    match error {
        Ext4Error::Io(_) => Status::Io,
        Ext4Error::NoSpace => Status::Full,
        Ext4Error::Readonly => Status::ReadOnly,
        Ext4Error::NotFound => Status::NotFound,
        Ext4Error::AlreadyExists => Status::Exists,
        Ext4Error::DirectoryNotEmpty => Status::NotEmpty,
        Ext4Error::NotADirectory => Status::NotDirectory,
        Ext4Error::IsADirectory => Status::IsDirectory,
        Ext4Error::PathTooLong | Ext4Error::FileTooLarge => Status::Range,
        Ext4Error::IsASpecialFile => Status::Special,
        _ => Status::Invalid,
    }
}

fn map_journal_error(error: JournalInodeMapError) -> Status {
    match error {
        JournalInodeMapError::Filesystem(error) => map_error(error),
        JournalInodeMapError::MissingJournal | JournalInodeMapError::Transaction(_) => {
            Status::Invalid
        }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct RecoveryReport {
    transactions: u32,
    replayed_blocks: u32,
    consumed_slots: u32,
}

fn recover_dirty_journal(
    context: usize,
    journal: &JournalInodeMap,
) -> Result<RecoveryReport, Status> {
    let physical_blocks = journal.physical_blocks();
    let slot_count = physical_blocks
        .len()
        .checked_sub(1)
        .ok_or(Status::Invalid)?;
    let ring_bytes = slot_count
        .checked_mul(JOURNAL_BLOCK_BYTES)
        .ok_or(Status::Range)?;
    let mut storage_bytes = Vec::new();
    storage_bytes
        .try_reserve_exact(ring_bytes)
        .map_err(|_| Status::Range)?;
    storage_bytes.resize(ring_bytes, 0);
    for (index, block) in physical_blocks[1..].iter().enumerate() {
        let start_byte = block.checked_mul(BLOCK_BYTES).ok_or(Status::Range)?;
        let start = index
            .checked_mul(JOURNAL_BLOCK_BYTES)
            .ok_or(Status::Range)?;
        let end = start
            .checked_add(JOURNAL_BLOCK_BYTES)
            .ok_or(Status::Range)?;
        if !crate::abi::ext4_block_read(
            context,
            start_byte,
            storage_bytes.get_mut(start..end).ok_or(Status::Range)?,
        ) {
            return Err(Status::Io);
        }
    }
    let mut references = Vec::new();
    references
        .try_reserve_exact(slot_count)
        .map_err(|_| Status::Range)?;
    references.extend(storage_bytes.chunks_exact(JOURNAL_BLOCK_BYTES));
    if references.len() != slot_count {
        return Err(Status::Invalid);
    }
    let recovery = recover_committed_ring(
        journal.superblock(),
        true,
        journal.maximum_block(),
        &references,
    )
    .map_err(|_| Status::Invalid)?;
    let report = RecoveryReport {
        transactions: u32::try_from(recovery.committed_transactions())
            .map_err(|_| Status::Range)?,
        replayed_blocks: u32::try_from(recovery.replay_images().len())
            .map_err(|_| Status::Range)?,
        consumed_slots: u32::try_from(recovery.consumed_slots()).map_err(|_| Status::Range)?,
    };
    let journal_superblock_block = *physical_blocks.first().ok_or(Status::Invalid)?;
    let operations = recovery
        .checkpoint_plan(journal_superblock_block, journal.filesystem_superblock())
        .map_err(|_| Status::Invalid)?;
    let checkpointed = operations.iter().find_map(|operation| match operation {
        JournalCommitOperation::WriteFilesystemSuperblock { image, .. } => Some(image),
        _ => None,
    }).ok_or(Status::Invalid)?;
    let projected = Ext4::load(Box::new(RecoveryValidationReader {
        context, images: recovery.replay_images().to_vec(),
        superblock: checkpointed.with_recovery_state(false),
    })).map_err(map_error)?;
    projected.orphan_inodes().map_err(map_error)?;
    validate_namespace(&projected)?;
    drop(projected);
    execute_storage_plan(context, &operations)?;
    Ok(report)
}

fn validate_inode_storage(filesystem: &Ext4, path: &[u8],
    blocks: &mut ext4plus::BlockAllocationSnapshot<'_>) -> Result<Inode, Status> {
    // Validate the entry itself: dangling/looping symlinks are valid namespace
    // objects and their targets must not replace their own xattr validation.
    let path = Path::try_from(path).map_err(|_| Status::Invalid)?;
    let inode = filesystem.path_to_inode(path, FollowSymlinks::ExcludeFinalComponent)
        .map_err(map_error)?;
    blocks.validate_inode_extents(&inode).map_err(map_error)?;
    let xattrs = inode.list_xattrs(filesystem).map_err(map_error)?;
    for name in xattrs {
        let _value = inode
            .get_xattr(filesystem, name.as_slice())
            .map_err(map_error)?;
    }
    Ok(inode)
}

fn untracked_directory_links(filesystem: &Ext4, inode: &Inode) -> bool {
    inode.file_type().is_dir() && inode.flags().contains(InodeFlags::DIRECTORY_HTREE)
        && filesystem.superblock().read_only_compatible_features().bits() & 0x20 != 0
        && inode.links_count() == 1
}

fn validate_namespace(filesystem: &Ext4) -> Result<(), Status> {
    // Count directory entries, including dot/dotdot, per distinct inode.
    // A zero-link orphan is deliberately absent from the live namespace.
    let mut references = BTreeMap::new();
    let mut blocks = filesystem.block_allocation_snapshot();
    blocks.reserve_fixed_metadata().map_err(map_error)?;
    blocks.validate_internal_journal().map_err(map_error)?;
    for index in filesystem.orphan_inodes().map_err(map_error)? {
        let inode = Inode::read(filesystem, index).map_err(map_error)?;
        blocks.validate_inode_extents(&inode).map_err(map_error)?;
        references.insert(index, (inode.links_count(), 0u32, false));
    }
    let mut allocations = filesystem.inode_allocation_snapshot();
    if !allocations.is_allocated(core::num::NonZeroU32::new(2).unwrap()).map_err(map_error)? {
        return Err(Status::Invalid);
    }
    let mut pending = Vec::new();
    pending.try_reserve_exact(1).map_err(|_| Status::Range)?;
    let mut root = Vec::new();
    root.try_reserve_exact(1).map_err(|_| Status::Range)?;
    root.push(b'/');
    let root_inode = validate_inode_storage(filesystem, root.as_slice(), &mut blocks)?;
    let root_index = root_inode.index;
    references.insert(root_index, (root_inode.links_count(), 0u32, untracked_directory_links(filesystem, &root_inode)));
    let mut directories = BTreeSet::new();
    directories.insert(root_index);
    pending.push((root, root_index, root_index));
    let mut visited = 0usize;
    while let Some((path, index, parent)) = pending.pop() {
        let mut directory = filesystem.read_dir(path.as_slice()).map_err(map_error)?;
        let mut names = BTreeSet::new();
        let (mut dot, mut dotdot) = (false, false);
        for result in &mut directory {
            let entry = result.map_err(map_error)?;
            // Validate before metadata() computes an inode-table location.
            // A checksummed directory can still point outside the inode table
            // or at a freed inode whose old body remains checksummed.
            if !allocations.is_allocated(entry.inode).map_err(map_error)? {
                return Err(Status::Invalid);
            }
            let name = entry.file_name();
            if name == "." || name == ".." {
                let (seen, expected) = if name == "." { (&mut dot, index) } else { (&mut dotdot, parent) };
                if *seen || entry.inode != expected || !entry.file_type().map_err(map_error)?.is_dir() {
                    return Err(Status::Invalid);
                }
                *seen = true;
                let (_, count, _) = references.get_mut(&entry.inode).ok_or(Status::Invalid)?;
                *count = count.checked_add(1).ok_or(Status::Range)?;
                continue;
            }
            visited = visited.checked_add(1).ok_or(Status::Range)?;
            if visited > MAX_VALIDATED_ENTRIES {
                return Err(Status::Range);
            }
            if !names.insert(name.as_ref().to_vec()) { return Err(Status::Invalid); }
            let entry_path = entry.path();
            let metadata = entry.metadata().map_err(map_error)?;
            // A zero-link orphan must never remain reachable by a directory.
            if metadata.links_count == 0 || metadata.file_type() != entry.file_type().map_err(map_error)? {
                return Err(Status::Invalid);
            }
            let inode = validate_inode_storage(filesystem, entry_path.as_ref(), &mut blocks)?;
            if inode.index != entry.inode { return Err(Status::Invalid); }
            let (links, seen, untracked) = references.entry(entry.inode).or_insert((metadata.links_count, 0,
                untracked_directory_links(filesystem, &inode)));
            if *links != metadata.links_count { return Err(Status::Invalid); }
            *seen = seen.checked_add(1).ok_or(Status::Range)?;
            if !*untracked && *seen > u32::from(*links) { return Err(Status::Invalid); }
            let kind = classify(metadata.file_type())?;
            if kind == 2 {
                // Directories have one namespace parent. Reject aliases and
                // cycles before following another path to the same inode.
                if !directories.insert(entry.inode) { return Err(Status::Invalid); }
                if pending.len() >= MAX_PENDING_DIRECTORIES {
                    return Err(Status::Range);
                }
                pending.try_reserve(1).map_err(|_| Status::Range)?;
                let path_bytes: &[u8] = entry_path.as_ref();
                if path_bytes.len() >= 4096 { return Err(Status::Range); }
                let mut owned_path = Vec::new();
                owned_path
                    .try_reserve_exact(path_bytes.len())
                    .map_err(|_| Status::Range)?;
                owned_path.extend_from_slice(path_bytes);
                pending.push((owned_path, entry.inode, index));
            } else if kind == 3 {
                let _target = filesystem
                    .read_link(entry_path.as_ref())
                    .map_err(map_error)?;
            } else if metadata.size_in_bytes != 0 {
                let mut file = filesystem.open(entry_path.as_ref()).map_err(map_error)?;
                let mut byte = [0u8; 1];
                let first = file.read_bytes_at(&mut byte, 0).map_err(map_error)?;
                let last = file
                    .read_bytes_at(&mut byte, metadata.size_in_bytes - 1)
                    .map_err(map_error)?;
                if first != 1 || last != 1 {
                    return Err(Status::Invalid);
                }
            }
        }
        if !dot || !dotdot { return Err(Status::Invalid); }
    }
    if references.values().any(|(links, seen, untracked)| !*untracked && *seen != u32::from(*links)) {
        return Err(Status::Invalid);
    }
    blocks.finish(&mut allocations).map_err(map_error)
}

/// Load and validate the exact Phipia ext4 profile and reachable namespace.
pub(crate) fn mount(context: usize, media_bytes: u64) -> Result<(Box<Mounted>, Identity), Status> {
    let mut image_bytes = validate_profile(context, media_bytes)?;
    let mut filesystem = Ext4::load(Box::new(PhipiaReader { context })).map_err(map_error)?;
    let mut journal = load_journal_inode_map(&filesystem).map_err(map_journal_error)?;
    let mut recovery = RecoveryReport::default();
    let mut recovery_performed = 0u8;
    if journal.filesystem_needs_recovery() {
        recovery = recover_dirty_journal(context, &journal)?;
        recovery_performed = 1;
        drop(journal);
        drop(filesystem);
        image_bytes = validate_profile(context, media_bytes)?;
        filesystem = Ext4::load(Box::new(PhipiaReader { context })).map_err(map_error)?;
        journal = load_journal_inode_map(&filesystem).map_err(map_journal_error)?;
        if journal.filesystem_needs_recovery() && journal.filesystem_superblock().last_orphan() == 0 {
            return Err(Status::Invalid);
        }
    }
    let needs_orphan_cleanup = journal.filesystem_needs_recovery();
    drop(journal);
    drop(filesystem);
    let (filesystem, stage) = load_staged_view(context, image_bytes, needs_orphan_cleanup,
        ext4plus::JOURNAL_TRANSACTION_MAX_METADATA_BLOCKS)?;
    let journal = load_journal_inode_map(&filesystem).map_err(map_journal_error)?;
    let orphans = filesystem.orphan_inodes().map_err(map_error)?;
    let ring = if needs_orphan_cleanup { journal.into_orphan_cleanup_ring() }
        else { journal.into_clean_ring() }.map_err(|_| Status::Invalid)?;
    let identity = Identity {
        label: *filesystem.label().as_bytes(),
        uuid: *filesystem.uuid().as_bytes(),
        recovered_transactions: recovery.transactions,
        replayed_blocks: recovery.replayed_blocks,
        consumed_slots: recovery.consumed_slots,
        recovery_performed,
        reserved: [0; 3],
    };
    let mut mounted = Box::new(Mounted {
            filesystem: Some(filesystem),
            journal: ring,
            stage,
            pending_mutation: None,
            pending_write: None,
            pending_reclaim: None,
            context,
            image_bytes,
        });
    for inode in orphans { finalize_orphan(&mut mounted, u64::from(inode.get()))?; }
    if needs_orphan_cleanup { prepare_unmount(&mut mounted)?; }
    Ok((mounted, identity))
}

fn arm_recovery_marker(mounted: &mut Mounted) -> Result<(), Status> {
    let seconds = crate::abi::ext4_current_time(mounted.context);
    if seconds > 0x3_7fff_ffff { return Err(Status::Io); }
    arm_recovery_marker_inner(mounted)?;
    mounted.filesystem()?.set_mutation_time(Some(seconds)).map_err(map_error)
}

fn arm_recovery_marker_inner(mounted: &mut Mounted) -> Result<(), Status> {
    ensure_staged_view(mounted)?;
    let durable = mounted
        .journal
        .filesystem_recovery_marker_is_durable()
        .map_err(|_| Status::Invalid)?;
    let view_needs_recovery = load_journal_inode_map(mounted.filesystem()?)
        .map_err(map_journal_error)?
        .filesystem_needs_recovery();
    if durable {
        if !view_needs_recovery {
            replace_staged_view(mounted, true)?;
        }
        return Ok(());
    }
    if view_needs_recovery || !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        return Err(Status::Invalid);
    }
    let operations = mounted
        .journal
        .prepare_recovery_marker_plan()
        .map_err(|_| Status::Invalid)?;
    execute_storage_plan(mounted.context, &operations)?;
    mounted
        .journal
        .mark_recovery_marker_durable()
        .map_err(|_| Status::Invalid)?;
    replace_staged_view(mounted, true)
}

fn resume_pending_mutation(
    mounted: &mut Mounted,
    kind: PendingMutationKind,
    path: &[u8],
    offset: u64,
    source: &[u8],
) -> Result<usize, Status> {
    if mounted.pending_reclaim.is_some() { return Err(Status::Busy); }
    let pending = mounted.pending_mutation.as_ref().ok_or(Status::Invalid)?;
    if pending.kind != kind
        || pending.path != path
        || pending.offset != offset
        || pending.source != source
    {
        return Err(Status::Invalid);
    }
    resume_pending_mutation_inner(mounted)
}

fn resume_pending_mutation_inner(mounted: &mut Mounted) -> Result<usize, Status> {
    loop {
        if matches!(
            mounted
                .pending_mutation
                .as_ref()
                .map(|pending| &pending.phase),
            Some(PendingMutationPhase::Commit(_))
        ) {
            let (ticket, operations) = {
                let prepared = match &mounted
                    .pending_mutation
                    .as_ref()
                    .ok_or(Status::Invalid)?
                    .phase
                {
                    PendingMutationPhase::Commit(prepared) => prepared,
                    PendingMutationPhase::Checkpoint(_) | PendingMutationPhase::Reload => {
                        return Err(Status::Invalid);
                    }
                };
                let ticket = prepared.ticket();
                let operations = match mounted.journal.prepare_commit_plan(prepared) {
                    Ok(operations) => operations,
                    Err(_) => {
                        mounted
                            .journal
                            .abort_precommit(ticket)
                            .map_err(|_| Status::Invalid)?;
                        mounted.pending_mutation = None;
                        discard_uncommitted_stage(mounted, true)?;
                        return Err(Status::Invalid);
                    }
                };
                (ticket, operations)
            };
            execute_storage_plan(mounted.context, &operations)?;
            mounted
                .journal
                .mark_commit_durable(ticket)
                .map_err(|_| Status::Invalid)?;
            mounted
                .pending_mutation
                .as_mut()
                .ok_or(Status::Invalid)?
                .phase = PendingMutationPhase::Checkpoint(ticket);
            continue;
        }
        if let Some(PendingMutationPhase::Checkpoint(ticket)) = mounted
            .pending_mutation
            .as_ref()
            .map(|pending| &pending.phase)
        {
            let ticket = *ticket;
            let operations = mounted
                .journal
                .prepare_checkpoint_plan(ticket)
                .map_err(|_| Status::Invalid)?;
            execute_storage_plan(mounted.context, &operations)?;
            let checkpointed_superblock = mounted
                .pending_mutation
                .as_ref()
                .ok_or(Status::Invalid)?
                .checkpointed_superblock
                .clone();
            if let Some(superblock) = checkpointed_superblock.as_ref() {
                mounted
                    .journal
                    .checkpoint_durable_with_filesystem_superblock(superblock)
                    .map_err(|_| Status::Invalid)?;
            } else {
                mounted
                    .journal
                    .checkpoint_durable(ticket)
                    .map_err(|_| Status::Invalid)?;
            }
            mounted
                .pending_mutation
                .as_mut()
                .ok_or(Status::Invalid)?
                .phase = PendingMutationPhase::Reload;
            continue;
        }
        if matches!(
            mounted
                .pending_mutation
                .as_ref()
                .map(|pending| &pending.phase),
            Some(PendingMutationPhase::Reload)
        ) {
            let written = mounted
                .pending_mutation
                .as_ref()
                .ok_or(Status::Invalid)?
                .written;
            replace_staged_view(mounted, true)?;
            mounted.pending_mutation = None;
            return Ok(written);
        }
        return Err(Status::Invalid);
    }
}

fn commit_staged_mutation(
    mounted: &mut Mounted,
    kind: PendingMutationKind,
    path: Vec<u8>,
    source: Vec<u8>,
    offset: u64,
    written: usize,
    ordered_data: &[u64],
    expected_revoke: Option<u64>,
) -> Result<usize, Status> {
    let transaction = match mounted.journal.begin_transaction() {
        Ok(transaction) => transaction,
        Err(_) => {
            discard_uncommitted_stage(mounted, true)?;
            return Err(Status::Invalid);
        }
    };
    let transaction = match mounted.stage.build_transaction(&transaction, ordered_data) {
        Ok(transaction) => transaction,
        Err(_) => {
            discard_uncommitted_stage(mounted, true)?;
            return Err(Status::Invalid);
        }
    };
    let allocation_check = (|| -> Result<(), Status> {
        let mut blocks = mounted.filesystem()?.block_allocation_snapshot();
        for block in ordered_data {
            if !blocks.range_is_allocated(*block, 1).map_err(map_error)? {
                return Err(Status::Invalid);
            }
        }
        Ok(())
    })();
    if let Err(error) = allocation_check {
        discard_uncommitted_stage(mounted, true)?;
        return Err(error);
    }
    for block in transaction.revoked_blocks() {
        match mounted.filesystem()?.is_fixed_metadata_block(*block) {
            Ok(false) => {}
            result => {
                let error = result.err().map(map_error).unwrap_or(Status::Invalid);
                discard_uncommitted_stage(mounted, true)?;
                return Err(error);
            }
        }
    }
    if let Some(block) = expected_revoke {
        if !transaction.revokes_block(block) {
            discard_uncommitted_stage(mounted, true)?;
            return Err(Status::Invalid);
        }
    }
    let prepared = match mounted.journal.prepare(&transaction) {
        Ok(prepared) => prepared,
        Err(_) => {
            discard_uncommitted_stage(mounted, true)?;
            return Err(Status::Invalid);
        }
    };
    let checkpointed_superblock = match mounted
        .stage
        .staged_images()
        .into_iter()
        .find(|image| image.block_index() == 0)
        .map(|image| {
            mounted
                .journal
                .admit_checkpointed_filesystem_superblock(&prepared, &image)
        })
        .transpose()
    {
        Ok(superblock) => superblock,
        Err(_) => {
            mounted
                .journal
                .abort_precommit(prepared.ticket())
                .map_err(|_| Status::Invalid)?;
            discard_uncommitted_stage(mounted, true)?;
            return Err(Status::Invalid);
        }
    };
    mounted.pending_mutation = Some(PendingMutation {
        kind,
        path,
        source,
        offset,
        written,
        checkpointed_superblock,
        phase: PendingMutationPhase::Commit(prepared),
    });
    resume_pending_mutation_inner(mounted)
}

/// Execute a bounded write request as checkpointed journal transactions.
///
/// The same request retries a retained commit/checkpoint plan after storage I/O
/// refusal; different input is rejected while a request remains pending.
/// Completed chunks can survive a crash independently. A later precommit
/// refusal returns their byte count as a short write, never whole-write atomicity.
pub(crate) fn transaction_probe(
    mounted: &mut Mounted,
    path: &[u8],
    offset: u64,
    source: &[u8],
) -> Result<usize, Status> {
    write_target(mounted, absolute_path(path)?, offset, source, false)
}

fn inode_key(inode: u64) -> Result<Vec<u8>, Status> {
    inode_index(inode)?;
    let mut key = Vec::from([0]);
    key.extend_from_slice(&inode.to_le_bytes());
    Ok(key)
}

fn inode_index(inode: u64) -> Result<core::num::NonZeroU32, Status> {
    u32::try_from(inode).ok().and_then(core::num::NonZeroU32::new).ok_or(Status::Invalid)
}

fn allocated_inode(filesystem: &Ext4, number: u64) -> Result<Inode, Status> {
    let index = inode_index(number)?;
    if !filesystem.inode_is_allocated(index).map_err(map_error)? { return Err(Status::NotFound); }
    Inode::read(filesystem, index).map_err(map_error)
}

fn open_io_target(filesystem: &Ext4, key: &[u8]) -> Result<ext4plus::file::File, Status> {
    if key.first() != Some(&0) { return filesystem.open(key).map_err(map_error); }
    let number = u64::from_le_bytes(key.get(1..).ok_or(Status::Invalid)?.try_into().map_err(|_| Status::Invalid)?);
    let inode = allocated_inode(filesystem, number)?;
    if !inode.file_type().is_regular_file() { return Err(Status::Special); }
    ext4plus::file::File::open_inode(filesystem, inode).map_err(map_error)
}

pub(crate) fn write_inode(mounted: &mut Mounted, inode: u64, offset: u64, source: &[u8]) -> Result<usize, Status> {
    write_target(mounted, inode_key(inode)?, offset, source, false)
}

fn write_target(mounted: &mut Mounted, absolute: Vec<u8>, offset: u64, source: &[u8], append: bool) -> Result<usize, Status> {
    if source.is_empty() || source.len() > MAX_PROBE_WRITE_BYTES {
        return Err(Status::Range);
    }
    offset.checked_add(source.len() as u64).ok_or(Status::Range)?;
    if let Some(pending) = &mounted.pending_write {
        if pending.path != absolute || pending.source != source || pending.offset != offset || pending.append != append {
            return Err(Status::Invalid);
        }
    } else {
        if mounted.pending_mutation.is_some() {
            return Err(Status::Invalid);
        }
        let file = open_io_target(mounted.readable_filesystem()?, &absolute)?;
        if file.inode().flags().contains(InodeFlags::APPEND_ONLY) && !append {
            return Err(Status::ReadOnly);
        }
        let mut copy = Vec::new();
        copy.try_reserve_exact(source.len()).map_err(|_| Status::Range)?;
        copy.extend_from_slice(source);
        mounted.pending_write = Some(PendingWrite {
            path: absolute, source: copy, offset, completed: 0, append,
            chunk_bytes: TRANSACTION_WRITE_BYTES,
        });
    }
    resume_write_request(mounted)
}

fn resume_write_request(mounted: &mut Mounted) -> Result<usize, Status> {
    let mut request = mounted.pending_write.take().ok_or(Status::Invalid)?;
    while request.completed < request.source.len() {
        let offset = request.offset + request.completed as u64;
        // Start with at most 32 data blocks. Fragmented allocation can need
        // more metadata than the remaining stage budget, so a precommit-only
        // capacity refusal halves the touched-block count after full rollback.
        let capacity = request.chunk_bytes - (offset % BLOCK_BYTES) as usize;
        let length = capacity.min(request.source.len() - request.completed);
        let end = request.completed + length;
        let outcome = write_transaction(mounted, &request.path, offset,
            &request.source[request.completed..end]);
        let outcome = match outcome {
            Ok(WriteTransactionOutcome::StageFull) => {
                let touched = ((offset % BLOCK_BYTES) as usize + length).div_ceil(JOURNAL_BLOCK_BYTES);
                if touched > 1 {
                    // Persist this exact choice through commit/checkpoint I/O
                    // failures; retry must not recompute a larger chunk.
                    request.chunk_bytes = (touched / 2) * JOURNAL_BLOCK_BYTES;
                    continue;
                }
                Err(Status::Io)
            }
            Ok(WriteTransactionOutcome::Written(written)) => Ok(written),
            Err(error) => Err(error),
        };
        match outcome {
            Ok(written) if written != 0 && written <= length => request.completed += written,
            Ok(_) => return Err(Status::Invalid),
            Err(error) => {
                if mounted.pending_mutation.is_some() {
                    // An unknown durable prefix owns this exact chunk. Keep the
                    // whole request and its already checkpointed byte count so
                    // retry/sync never allocates or replays earlier chunks twice.
                    mounted.pending_write = Some(request);
                    return Err(error);
                }
                // Setup/ENOSPC rolled back this chunk before storage started.
                // A completed prefix is a normal short write; it stays durable.
                return if request.completed != 0 { Ok(request.completed) } else { Err(error) };
            }
        }
    }
    Ok(request.completed)
}

/// Choose EOF under the caller's exclusive volume lease. A refused append
/// retains its original start through the same bounded write request retry.
pub(crate) fn append_probe(
    mounted: &mut Mounted, path: &[u8], source: &[u8], maximum_size: u64,
) -> Result<(u64, usize), Status> {
    append_target(mounted, absolute_path(path)?, source, maximum_size)
}

pub(crate) fn append_inode(mounted: &mut Mounted, inode: u64, source: &[u8], maximum_size: u64) -> Result<(u64, usize), Status> {
    append_target(mounted, inode_key(inode)?, source, maximum_size)
}

fn append_target(mounted: &mut Mounted, key: Vec<u8>, source: &[u8], maximum_size: u64) -> Result<(u64, usize), Status> {
    let offset = if let Some(pending) = &mounted.pending_write {
        if pending.path != key || pending.source != source || !pending.append {
            return Err(Status::Invalid);
        }
        pending.offset
    } else {
        open_io_target(mounted.readable_filesystem()?, &key)?.inode().size_in_bytes()
    };
    if offset.checked_add(source.len() as u64).is_none_or(|end| end > maximum_size) {
        return Err(Status::Range);
    }
    write_target(mounted, key, offset, source, true).map(|written| (offset, written))
}

fn write_transaction(
    mounted: &mut Mounted,
    path: &[u8],
    offset: u64,
    source: &[u8],
) -> Result<WriteTransactionOutcome, Status> {
    if source.is_empty() || source.len() > MAX_PROBE_WRITE_BYTES {
        return Err(Status::Range);
    }
    let absolute = Vec::from(path);
    if mounted.pending_mutation.is_some() {
        return resume_pending_mutation(
            mounted,
            PendingMutationKind::Write,
            &absolute,
            offset,
            source,
        ).map(WriteTransactionOutcome::Written);
    }
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        let recovery = mounted
            .journal
            .filesystem_recovery_marker_is_durable()
            .map_err(|_| Status::Invalid)?;
        discard_uncommitted_stage(mounted, recovery)?;
    }
    let mut source_copy = Vec::new();
    source_copy
        .try_reserve_exact(source.len())
        .map_err(|_| Status::Range)?;
    source_copy.extend_from_slice(source);
    arm_recovery_marker(mounted)?;

    let mut file = open_io_target(mounted.filesystem()?, &absolute)?;
    let written = match file.write_bytes_at(source, offset) {
        Ok(0) => {
            discard_uncommitted_stage(mounted, true)?;
            return Err(Status::Invalid);
        }
        Ok(written) => written,
        Err(error) => {
            let stage_full = matches!(&error, Ext4Error::Io(cause)
                if cause.downcast_ref::<JournalMutationStageError>() == Some(&JournalMutationStageError::TooManyBlocks));
            drop(file);
            discard_uncommitted_stage(mounted, true)?;
            if stage_full { return Ok(WriteTransactionOutcome::StageFull); }
            return Err(map_error(error));
        }
    };
    let written_u64 = match u64::try_from(written) {
        Ok(written) => written,
        Err(_) => {
            discard_uncommitted_stage(mounted, true)?;
            return Err(Status::Range);
        }
    };
    let end = match offset.checked_add(written_u64) {
        Some(end) => end,
        None => {
            discard_uncommitted_stage(mounted, true)?;
            return Err(Status::Range);
        }
    };
    let mut ordered_data = Vec::new();
    let first_block = offset / BLOCK_BYTES;
    let block_count = match end
        .div_ceil(BLOCK_BYTES)
        .checked_sub(first_block)
        .and_then(|count| usize::try_from(count).ok())
    {
        Some(count) => count,
        None => {
            discard_uncommitted_stage(mounted, true)?;
            return Err(Status::Range);
        }
    };
    if ordered_data.try_reserve_exact(block_count).is_err() {
        discard_uncommitted_stage(mounted, true)?;
        return Err(Status::Range);
    }
    let mut position = offset;
    while position < end {
        let block = match file.filesystem_block_at_offset(position) {
            Ok(Some(block)) => block,
            Ok(None) => {
                discard_uncommitted_stage(mounted, true)?;
                return Err(Status::Invalid);
            }
            Err(error) => {
                discard_uncommitted_stage(mounted, true)?;
                return Err(map_error(error));
            }
        };
        // A checksummed but hostile extent can point at allocator metadata.
        // Never let file-data classification turn that image into a home
        // write before journal commit. The journal planner separately rejects
        // overlap with the journal's physical storage.
        match mounted.filesystem()?.is_fixed_metadata_block(block) {
            Ok(false) => {}
            result => {
                let error = result.err().map(map_error).unwrap_or(Status::Invalid);
                drop(file);
                discard_uncommitted_stage(mounted, true)?;
                return Err(error);
            }
        }
        if ordered_data.contains(&block) {
            discard_uncommitted_stage(mounted, true)?;
            return Err(Status::Invalid);
        }
        ordered_data.push(block);
        let next = match position
            .checked_div(BLOCK_BYTES)
            .and_then(|block| block.checked_add(1))
            .and_then(|block| block.checked_mul(BLOCK_BYTES))
        {
            Some(next) => next,
            None => {
                discard_uncommitted_stage(mounted, true)?;
                return Err(Status::Range);
            }
        };
        position = next.min(end);
    }
    drop(file);
    commit_staged_mutation(
        mounted,
        PendingMutationKind::Write,
        absolute,
        source_copy,
        offset,
        written,
        &ordered_data,
        None,
    ).map(WriteTransactionOutcome::Written)
}

/// Execute one controlled truncate through the native journal executor.
///
/// Every freed block reported by ext4plus becomes a JBD2 revocation committed
/// on platform storage.
pub(crate) fn truncate_probe(
    mounted: &mut Mounted,
    path: &[u8],
    size: u64,
) -> Result<(), Status> {
    truncate_target(mounted, absolute_path(path)?, size)
}

pub(crate) fn truncate_inode(mounted: &mut Mounted, inode: u64, size: u64) -> Result<(), Status> {
    truncate_target(mounted, inode_key(inode)?, size)
}

fn truncate_target(mounted: &mut Mounted, absolute: Vec<u8>, size: u64) -> Result<(), Status> {
    if let Some(pending) = &mounted.pending_reclaim {
        if pending.kind != PendingMutationKind::Truncate || pending.path != absolute
            || pending.argument != size { return Err(Status::Invalid); }
        return resume_reclaim_request(mounted);
    }
    if mounted.pending_mutation.is_some() {
        let resumed = resume_pending_mutation(
            mounted,
            PendingMutationKind::Truncate,
            &absolute,
            size,
            &[],
        )?;
        return if resumed == 0 {
            Ok(())
        } else {
            Err(Status::Invalid)
        };
    }
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        let recovery = mounted
            .journal
            .filesystem_recovery_marker_is_durable()
            .map_err(|_| Status::Invalid)?;
        discard_uncommitted_stage(mounted, recovery)?;
    }
    ensure_staged_view(mounted)?;
    let file = open_io_target(mounted.filesystem()?, &absolute)?;
    if file.inode().flags().intersects(InodeFlags::IMMUTABLE | InodeFlags::APPEND_ONLY) {
        return Err(Status::ReadOnly);
    }
    let old_size = file.inode().size_in_bytes();
    drop(file);
    if size == old_size {
        return Ok(());
    }
    arm_recovery_marker(mounted)?;
    let mut file = open_io_target(mounted.filesystem()?, &absolute)?;
    if size < old_size && size % BLOCK_BYTES != 0 {
        // Tail zeroing is journaled as metadata, so the ordered-data and
        // freed-block classifiers never see it. Check its physical owner
        // class before upstream can stage bytes over fixed allocator metadata.
        let validation = (|| {
            if let Some(block) = file.filesystem_block_at_offset(size).map_err(map_error)? {
                if mounted.filesystem()?.is_fixed_metadata_block(block).map_err(map_error)? {
                    return Err(Status::Invalid);
                }
                if !mounted.filesystem()?.block_allocation_snapshot()
                    .range_is_allocated(block, 1).map_err(map_error)? {
                    return Err(Status::Invalid);
                }
            }
            Ok(())
        })();
        if let Err(error) = validation {
            drop(file);
            discard_uncommitted_stage(mounted, true)?;
            return Err(error);
        }
    }
    if let Err(error) = file.truncate(size) {
        let capacity = mutation_capacity_error(&error);
        let inode = file.inode().index;
        let linked = file.inode().links_count() != 0;
        drop(file);
        discard_uncommitted_stage(mounted, true)?;
        if capacity && size < old_size && old_size <= MAX_SPLIT_ORPHAN_BYTES {
            arm_recovery_marker(mounted)?;
            if let Err(error) = mounted.filesystem()?.begin_truncate_orphan(inode, size) {
                discard_uncommitted_stage(mounted, true)?;
                return Err(map_error(error));
            }
            mounted.pending_reclaim = Some(PendingReclaim {
                kind: PendingMutationKind::Truncate, path: absolute, argument: size,
                inode: u64::from(inode.get()), retain_unlinked: !linked, namespace_committed: false,
            });
            return resume_reclaim_request(mounted);
        }
        return Err(map_error(error));
    }
    if file.inode().size_in_bytes() != size {
        discard_uncommitted_stage(mounted, true)?;
        return Err(Status::Invalid);
    }
    drop(file);

    let resumed = commit_staged_mutation(
        mounted,
        PendingMutationKind::Truncate,
        absolute,
        Vec::new(),
        size,
        0,
        &[],
        None,
    )?;
    if resumed == 0 {
        Ok(())
    } else {
        Err(Status::Invalid)
    }
}

fn resume_namespace_mutation(
    mounted: &mut Mounted,
    kind: PendingMutationKind,
    path: &[u8],
) -> Result<(), Status> {
    let resumed = resume_pending_mutation(mounted, kind, path, 0, &[])?;
    if resumed == 0 {
        Ok(())
    } else {
        Err(Status::Invalid)
    }
}

fn commit_namespace_mutation(
    mounted: &mut Mounted,
    kind: PendingMutationKind,
    path: Vec<u8>,
) -> Result<(), Status> {
    let resumed = commit_staged_mutation(
        mounted,
        kind,
        path,
        Vec::new(),
        0,
        0,
        &[],
        None,
    )?;
    if resumed == 0 {
        Ok(())
    } else {
        Err(Status::Invalid)
    }
}

/// Apply one inode mutation with an exact-input retry key and staged rollback.
fn mutate_inode<F>(mounted: &mut Mounted, path: &[u8], kind: PendingMutationKind,
    input: Vec<u8>, mutation: F) -> Result<(), Status>
where F: FnOnce(&Ext4, &mut Inode) -> Result<(), Ext4Error> {
    let absolute = absolute_path(path)?;
    if mounted.pending_mutation.is_some() {
        return resume_pending_mutation(mounted, kind, &absolute, 0, &input).map(|_| ());
    }
    if mounted.pending_write.is_some() { return Err(Status::Invalid); }
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        let recovery = mounted.journal.filesystem_recovery_marker_is_durable()
            .map_err(|_| Status::Invalid)?;
        discard_uncommitted_stage(mounted, recovery)?;
    }
    arm_recovery_marker(mounted)?;
    let filesystem = mounted.filesystem()?;
    let result = (|| {
        let path = Path::try_from(absolute.as_slice()).map_err(|_| Ext4Error::MalformedPath)?;
        let mut inode = filesystem.path_to_inode(path, FollowSymlinks::All)?;
        if inode.flags().intersects(InodeFlags::IMMUTABLE | InodeFlags::APPEND_ONLY) { return Err(Ext4Error::Readonly); }
        mutation(filesystem, &mut inode)
    })();
    if let Err(error) = result {
        discard_uncommitted_stage(mounted, true)?;
        return Err(map_error(error));
    }
    if mounted.stage.is_empty() {
        discard_uncommitted_stage(mounted, true)?;
        return Err(Status::Invalid);
    }
    commit_staged_mutation(mounted, kind, absolute, input, 0, 0, &[], None).map(|_| ())
}

pub(crate) fn chmod(mounted: &mut Mounted, path: &[u8], mode: u16) -> Result<(), Status> {
    if mode & !0o7777 != 0 { return Err(Status::Invalid); }
    mutate_inode(mounted, path, PendingMutationKind::Chmod, Vec::from(mode.to_le_bytes()),
        |filesystem, inode| {
            // Updating mode alone would leave an access ACL inconsistent.
            if inode.get_xattr(filesystem, b"system.posix_acl_access")?.is_some() {
                return Err(Ext4Error::Readonly);
            }
            inode.set_mode(InodeMode::from_bits_retain((inode.mode().bits() & !0o7777) | mode))?;
            inode.write(filesystem)
        })
}

pub(crate) fn set_times(mounted: &mut Mounted, path: &[u8], atime_seconds: u64,
    atime_nanos: u32, mtime_seconds: u64, mtime_nanos: u32) -> Result<(), Status> {
    if atime_seconds > 0x3_7fff_ffff || mtime_seconds > 0x3_7fff_ffff ||
        atime_nanos >= 1_000_000_000 || mtime_nanos >= 1_000_000_000 { return Err(Status::Range); }
    let mut input = Vec::from(atime_seconds.to_le_bytes());
    input.extend_from_slice(&atime_nanos.to_le_bytes());
    input.extend_from_slice(&mtime_seconds.to_le_bytes());
    input.extend_from_slice(&mtime_nanos.to_le_bytes());
    mutate_inode(mounted, path, PendingMutationKind::SetTimes, input, |filesystem, inode| {
        // Phipia admits the modern inode extra fields; do not silently truncate
        // epochs or nanoseconds on an older short-extra inode.
        if inode.metadata().crtime.is_none() { return Err(Ext4Error::Readonly); }
        inode.set_atime(Duration::new(atime_seconds, atime_nanos));
        inode.set_mtime(Duration::new(mtime_seconds, mtime_nanos));
        inode.write(filesystem)
    })
}

fn validate_user_xattr(name: &[u8]) -> Result<(), Status> {
    if !name.starts_with(b"user.") || name.len() <= 5 || name.len() > 255 || name.contains(&0) {
        return Err(Status::Invalid);
    }
    Ok(())
}

pub(crate) fn set_xattr(mounted: &mut Mounted, path: &[u8], name: &[u8],
    value: Option<&[u8]>) -> Result<(), Status> {
    validate_user_xattr(name)?;
    if value.is_some_and(|bytes| bytes.len() > 4096) { return Err(Status::Range); }
    let mut input = Vec::from(name);
    input.push(0);
    if let Some(value) = value { input.extend_from_slice(value); }
    let kind = if value.is_some() { PendingMutationKind::SetXattr } else { PendingMutationKind::RemoveXattr };
    mutate_inode(mounted, path, kind, input, |filesystem, inode| {
        if let Some(value) = value { inode.set_xattr(filesystem, name, value) }
        else { inode.remove_xattr(filesystem, name) }
    })
}

pub(crate) fn get_xattr(mounted: &Mounted, path: &[u8], name: &[u8],
    output: &mut [u8]) -> Result<usize, Status> {
    validate_user_xattr(name)?;
    let filesystem = mounted.readable_filesystem()?;
    let absolute = absolute_path(path)?;
    let path = Path::try_from(absolute.as_slice()).map_err(|_| Status::Invalid)?;
    let inode = filesystem.path_to_inode(path, FollowSymlinks::All).map_err(map_error)?;
    let value = inode.get_xattr(filesystem, name).map_err(map_error)?.ok_or(Status::NotFound)?;
    if output.is_empty() { return Ok(value.len()); }
    if output.len() < value.len() { return Err(Status::Range); }
    output[..value.len()].copy_from_slice(&value);
    Ok(value.len())
}

/// Create one empty regular file through the journaled mutation path.
pub(crate) fn create_file_probe(
    mounted: &mut Mounted,
    path: &[u8],
    mode: u16,
) -> Result<(), Status> {
    if mode & !0o777 != 0 {
        return Err(Status::Invalid);
    }
    let absolute = absolute_path(path)?;
    let mode_bytes = mode.to_le_bytes();
    if mounted.pending_mutation.is_some() {
        let resumed = resume_pending_mutation(
            mounted,
            PendingMutationKind::CreateFile,
            &absolute,
            0,
            &mode_bytes,
        )?;
        return if resumed == 0 {
            Ok(())
        } else {
            Err(Status::Invalid)
        };
    }
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        let recovery = mounted
            .journal
            .filesystem_recovery_marker_is_durable()
            .map_err(|_| Status::Invalid)?;
        discard_uncommitted_stage(mounted, recovery)?;
    }
    arm_recovery_marker(mounted)?;
    let (parent, name) = parent_and_name(&absolute)?;
    let filesystem = mounted.filesystem()?;
    let mutation = (|| {
        let parent_inode = filesystem
            .path_to_inode(parent, FollowSymlinks::All)?;
        let mut directory = Dir::open_inode(filesystem, parent_inode)?;
        let mut inode = filesystem.create_inode(InodeCreationOptions {
            file_type: FileType::Regular,
            mode: InodeMode::S_IFREG | InodeMode::from_bits_retain(mode),
            uid: 0,
            gid: 0,
            time: Duration::from_secs(0),
            flags: InodeFlags::empty(),
        })?;
        directory.link(name, &mut inode)
    })();
    if let Err(error) = mutation {
        discard_uncommitted_stage(mounted, true)?;
        return Err(map_error(error));
    }
    if mounted.stage.is_empty() || mounted.stage.revoked_block_count() != 0 {
        discard_uncommitted_stage(mounted, true)?;
        return Err(Status::Invalid);
    }
    let resumed = commit_staged_mutation(
        mounted,
        PendingMutationKind::CreateFile,
        absolute,
        Vec::from(mode_bytes),
        0,
        0,
        &[],
        None,
    )?;
    if resumed == 0 {
        Ok(())
    } else {
        Err(Status::Invalid)
    }
}

/// Create a symlink with its literal target journaled alongside its inode.
pub(crate) fn symlink_probe(
    mounted: &mut Mounted,
    path: &[u8],
    target: &[u8],
) -> Result<(), Status> {
    if target.is_empty() || target.len() >= 4096 {
        return Err(Status::Range);
    }
    let target_path = PathBuf::try_from(target).map_err(|_| Status::Invalid)?;
    let absolute = absolute_path(path)?;
    let key = namespace_pair_key(&absolute, target)?;
    if mounted.pending_mutation.is_some() {
        return resume_namespace_mutation(mounted, PendingMutationKind::Symlink, &key);
    }
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        let recovery = mounted.journal.filesystem_recovery_marker_is_durable()
            .map_err(|_| Status::Invalid)?;
        discard_uncommitted_stage(mounted, recovery)?;
    }
    arm_recovery_marker(mounted)?;
    let (parent, name) = parent_and_name(&absolute)?;
    let filesystem = mounted.filesystem()?;
    let mutation = (|| {
        let parent_inode = filesystem.path_to_inode(parent, FollowSymlinks::All)?;
        let mut directory = Dir::open_inode(filesystem, parent_inode)?;
        filesystem.symlink(&mut directory, name, target_path, 0, 0,
            Duration::from_secs(0)).map(|_| ())
    })();
    if let Err(error) = mutation {
        discard_uncommitted_stage(mounted, true)?;
        return Err(map_error(error));
    }
    if mounted.stage.is_empty() || mounted.stage.revoked_block_count() != 0 {
        discard_uncommitted_stage(mounted, true)?;
        return Err(Status::Invalid);
    }
    // Long-target data is metadata for this operation: journal the complete
    // target with its namespace publication instead of using ordered data.
    commit_namespace_mutation(mounted, PendingMutationKind::Symlink, key)
}

/// Copy a literal target without following the final component or adding NUL.
pub(crate) fn readlink(
    mounted: &Mounted,
    path: &[u8],
    output: &mut [u8],
) -> Result<usize, Status> {
    let absolute = absolute_path(path)?;
    let target = mounted.readable_filesystem()?.read_link(absolute.as_slice())
        .map_err(map_error)?;
    let bytes = target.as_ref();
    let count = output.len().min(bytes.len());
    output[..count].copy_from_slice(&bytes[..count]);
    Ok(count)
}

/// Remove an entry of either kind, resuming the original journal operation.
pub(crate) fn remove_entry_guarded(mounted: &mut Mounted, path: &[u8], open_inodes: &[u64]) -> Result<(), Status> {
    if mounted.pending_mutation.as_ref().is_some_and(|pending| pending.kind == PendingMutationKind::RemoveDirectory) {
        return remove_directory_guarded(mounted, path, open_inodes);
    }
    match unlink_file_guarded(mounted, path, open_inodes) {
        Err(Status::IsDirectory) => remove_directory_guarded(mounted, path, open_inodes),
        result => result,
    }
}

/// Remove one regular-file or symbolic link through the journaled mutation path.
pub(crate) fn unlink_file_probe(
    mounted: &mut Mounted,
    path: &[u8],
) -> Result<(), Status> {
    unlink_file_guarded(mounted, path, &[])
}

/// Retain an open regular inode on the journaled orphan chain after its final
/// namespace link is removed. C supplies every live inode under the lease.
pub(crate) fn unlink_file_guarded(
    mounted: &mut Mounted,
    path: &[u8],
    open_inodes: &[u64],
) -> Result<(), Status> {
    let absolute = absolute_path(path)?;
    if let Some(pending) = &mounted.pending_reclaim {
        if pending.kind != PendingMutationKind::UnlinkFile || pending.path != absolute { return Err(Status::Invalid); }
        return resume_reclaim_request(mounted);
    }
    if mounted.pending_mutation.is_some() {
        return resume_namespace_mutation(mounted, PendingMutationKind::UnlinkFile, &absolute);
    }
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        let recovery = mounted
            .journal
            .filesystem_recovery_marker_is_durable()
            .map_err(|_| Status::Invalid)?;
        discard_uncommitted_stage(mounted, recovery)?;
    }
    arm_recovery_marker(mounted)?;
    let (parent, name) = parent_and_name(&absolute)?;
    let filesystem = mounted.filesystem()?;
    let path = Path::try_from(absolute.as_slice()).map_err(|_| Status::Invalid)?;
    let mut reclaim = None;
    let mutation = (|| {
        let inode = filesystem
            .path_to_inode(path, FollowSymlinks::ExcludeFinalComponent)?;
        if !inode.file_type().is_regular_file() && !inode.file_type().is_symlink() {
            return Err(Ext4Error::IsADirectory);
        }
        let retain = inode.file_type().is_regular_file()
            && open_inodes.contains(&u64::from(inode.index.get()));
        if !retain && inode.file_type().is_regular_file() && inode.links_count() == 1
            && inode.size_in_bytes() <= MAX_SPLIT_ORPHAN_BYTES {
            reclaim = Some(u64::from(inode.index.get()));
        }
        let parent_inode = filesystem
            .path_to_inode(parent, FollowSymlinks::All)?;
        let mut directory = Dir::open_inode(filesystem, parent_inode)?;
        if retain { directory.unlink_open(name, inode) } else { directory.unlink(name, inode) }
            .map(|_| ())
    })();
    if let Err(error) = mutation {
        let capacity = mutation_capacity_error(&error);
        discard_uncommitted_stage(mounted, true)?;
        if let Some(inode) = reclaim.filter(|_| capacity) {
            // Publish the final unlink and durable orphan link atomically,
            // then reclaim its allocation in bounded transactions. Retain the
            // original pathname/phase across failures so retry never looks up
            // an already removed name or unlinks a newly created replacement.
            arm_recovery_marker(mounted)?;
            let filesystem = mounted.filesystem()?;
            filesystem.validate_extent_reclaim(inode_index(inode)?,
                (MAX_SPLIT_ORPHAN_BYTES / BLOCK_BYTES) as u32).map_err(map_error)?;
            let node = allocated_inode(filesystem, inode)?;
            let (parent, name) = parent_and_name(&absolute)?;
            let mutation = (|| {
                let parent_inode = filesystem.path_to_inode(parent, FollowSymlinks::All)?;
                Dir::open_inode(filesystem, parent_inode)?.unlink_open(name, node)
            })();
            if let Err(error) = mutation {
                discard_uncommitted_stage(mounted, true)?;
                return Err(map_error(error));
            }
            mounted.pending_reclaim = Some(PendingReclaim {
                kind: PendingMutationKind::UnlinkFile,
                path: absolute, argument: 0, inode, retain_unlinked: false, namespace_committed: false,
            });
            return resume_reclaim_request(mounted);
        }
        return Err(map_error(error));
    }
    /*
     * Dropping the final link legitimately frees every data/extent block in
     * the inode. JournalMutationStage records those blocks as JBD2 revokes;
     * build_transaction() below serializes the complete bounded set before
     * any allocator metadata can reach its home location. A non-final unlink
     * has no revokes, so both cases are valid here.
     */
    if mounted.stage.is_empty() {
        discard_uncommitted_stage(mounted, true)?;
        return Err(Status::Invalid);
    }
    commit_namespace_mutation(mounted, PendingMutationKind::UnlinkFile, absolute)
}

fn resume_reclaim_request(mounted: &mut Mounted) -> Result<(), Status> {
    let mut request = mounted.pending_reclaim.take().ok_or(Status::Invalid)?;
    let result = (|| {
        if !request.namespace_committed {
            if mounted.pending_mutation.is_some() {
                resume_pending_mutation(mounted, request.kind, &request.path, request.argument, &[])?;
            } else {
                commit_staged_mutation(mounted, request.kind, request.path.clone(), Vec::new(),
                    request.argument, 0, &[], None)?;
            }
            request.namespace_committed = true;
        }
        if request.retain_unlinked {
            let key = inode_key(request.inode)?;
            if mounted.pending_mutation.is_some() {
                resume_namespace_mutation(mounted, PendingMutationKind::FinalizeOrphan, &key)?;
            }
            while !complete_truncate_orphan_transaction(mounted, request.inode, &key, true)? {}
            Ok(())
        } else { finalize_orphan(mounted, request.inode) }
    })();
    if result.is_err() && (request.namespace_committed || mounted.pending_mutation.is_some()) {
        mounted.pending_reclaim = Some(request);
    }
    result
}

/// Finalize one orphan through the same retryable coordinator used by unlink.
pub(crate) fn finalize_orphan(mounted: &mut Mounted, inode: u64) -> Result<(), Status> {
    let key = inode_key(inode)?;
    if mounted.pending_write.is_some() { return Err(Status::Busy); }
    if mounted.pending_mutation.is_some() {
        resume_namespace_mutation(mounted, PendingMutationKind::FinalizeOrphan, &key)?;
    }
    let index = inode_index(inode)?;
    loop {
        ensure_staged_view(mounted)?;
        if !mounted.readable_filesystem()?.orphan_inodes().map_err(map_error)?.contains(&index) {
            return Ok(());
        }
        if allocated_inode(mounted.filesystem()?, inode)?.links_count() != 0 {
            if complete_truncate_orphan_transaction(mounted, inode, &key, false)? { return Ok(()); }
            continue;
        }
        arm_recovery_marker(mounted)?;
        match mounted.filesystem()?.release_orphan(index) {
            Ok(()) => return commit_namespace_mutation(mounted, PendingMutationKind::FinalizeOrphan, key),
            Err(error) => {
                let capacity = mutation_capacity_error(&error);
                discard_uncommitted_stage(mounted, true)?;
                if !capacity { return Err(map_error(error)); }
            }
        }
        // The inode is unreachable and has no live handle. Keep it on the
        // durable orphan chain while releasing a bounded suffix. A crash or
        // failed checkpoint can resume either that exact plan or the next
        // suffix; the inode number is freed only by the final transaction.
        trim_orphan_for_release(mounted, inode, &key)?;
    }
}

fn mutation_capacity_error(error: &Ext4Error) -> bool {
    matches!(error, Ext4Error::Io(cause) if matches!(cause.downcast_ref::<JournalMutationStageError>(),
        Some(JournalMutationStageError::TooManyBlocks | JournalMutationStageError::TooManyRevocations)))
}

fn complete_truncate_orphan_transaction(mounted: &mut Mounted, inode: u64, key: &[u8],
    retain_unlinked: bool) -> Result<bool, Status> {
    let index = inode_index(inode)?;
    let mut blocks = ext4plus::JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS as u32;
    loop {
        arm_recovery_marker(mounted)?;
        let complete = match mounted.filesystem()?.complete_truncate_orphan(index, blocks, retain_unlinked) {
            Ok(complete) => complete,
            Err(error) => {
                let capacity = mutation_capacity_error(&error);
                discard_uncommitted_stage(mounted, true)?;
                if capacity && blocks > 1 { blocks /= 2; continue; }
                return Err(map_error(error));
            }
        };
        if mounted.stage.is_empty() {
            return if complete && retain_unlinked { Ok(true) } else { Err(Status::Invalid) };
        }
        commit_namespace_mutation(mounted, PendingMutationKind::FinalizeOrphan, Vec::from(key))?;
        return Ok(complete);
    }
}

fn trim_orphan_for_release(mounted: &mut Mounted, inode: u64, key: &[u8]) -> Result<(), Status> {
    let index = inode_index(inode)?;
    let mut blocks = ext4plus::JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS as u32;
    loop {
        arm_recovery_marker(mounted)?;
        if let Err(error) = mounted.filesystem()?.trim_orphan_suffix(index, blocks,
            (MAX_SPLIT_ORPHAN_BYTES / BLOCK_BYTES) as u32) {
            let capacity = mutation_capacity_error(&error);
            discard_uncommitted_stage(mounted, true)?;
            if capacity && blocks > 1 { blocks /= 2; continue; }
            return Err(map_error(error));
        }
        return commit_namespace_mutation(mounted, PendingMutationKind::FinalizeOrphan, Vec::from(key));
    }
}

/// Hard-link a regular file or the final symbolic link without following it.
pub(crate) fn link_file_probe(
    mounted: &mut Mounted,
    source: &[u8],
    destination: &[u8],
) -> Result<(), Status> {
    let source_absolute = absolute_path(source)?;
    let destination_absolute = absolute_path(destination)?;
    let pending_key = namespace_pair_key(&source_absolute, &destination_absolute)?;
    if mounted.pending_mutation.is_some() {
        return resume_namespace_mutation(
            mounted,
            PendingMutationKind::LinkFile,
            &pending_key,
        );
    }
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        let recovery = mounted
            .journal
            .filesystem_recovery_marker_is_durable()
            .map_err(|_| Status::Invalid)?;
        discard_uncommitted_stage(mounted, recovery)?;
    }
    arm_recovery_marker(mounted)?;
    let source_path = Path::try_from(source_absolute.as_slice())
        .map_err(|_| Status::Invalid)?;
    let (parent, name) = parent_and_name(&destination_absolute)?;
    let filesystem = mounted.filesystem()?;
    let mutation = (|| {
        let mut inode = filesystem
            .path_to_inode(source_path, FollowSymlinks::ExcludeFinalComponent)?;
        if !inode.file_type().is_regular_file() && !inode.file_type().is_symlink() {
            return Err(Ext4Error::IsADirectory);
        }
        let parent_inode = filesystem
            .path_to_inode(parent, FollowSymlinks::All)?;
        let mut directory = Dir::open_inode(filesystem, parent_inode)?;
        directory.link(name, &mut inode)
    })();
    if let Err(error) = mutation {
        discard_uncommitted_stage(mounted, true)?;
        return Err(map_error(error));
    }
    if mounted.stage.is_empty() || mounted.stage.revoked_block_count() != 0 {
        discard_uncommitted_stage(mounted, true)?;
        return Err(Status::Invalid);
    }
    commit_namespace_mutation(mounted, PendingMutationKind::LinkFile, pending_key)
}

/// Create one empty directory through the journaled mutation path.
pub(crate) fn create_directory_probe(
    mounted: &mut Mounted,
    path: &[u8],
) -> Result<(), Status> {
    let absolute = absolute_path(path)?;
    if mounted.pending_mutation.is_some() {
        return resume_namespace_mutation(
            mounted,
            PendingMutationKind::CreateDirectory,
            &absolute,
        );
    }
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        let recovery = mounted
            .journal
            .filesystem_recovery_marker_is_durable()
            .map_err(|_| Status::Invalid)?;
        discard_uncommitted_stage(mounted, recovery)?;
    }
    arm_recovery_marker(mounted)?;
    let (parent, name) = parent_and_name(&absolute)?;
    let filesystem = mounted.filesystem()?;
    let mutation = (|| {
        let parent_inode = filesystem
            .path_to_inode(parent, FollowSymlinks::All)?;
        let mut parent_directory =
            Dir::open_inode(filesystem, parent_inode)?;
        let inode = filesystem.create_inode(InodeCreationOptions {
            file_type: FileType::Directory,
            mode: InodeMode::S_IFDIR
                | InodeMode::S_IRUSR
                | InodeMode::S_IWUSR
                | InodeMode::S_IXUSR
                | InodeMode::S_IRGRP
                | InodeMode::S_IXGRP
                | InodeMode::S_IROTH
                | InodeMode::S_IXOTH,
            uid: 0,
            gid: 0,
            time: Duration::from_secs(0),
            flags: InodeFlags::empty(),
        })?;
        let mut directory = Dir::init(
            filesystem.clone(),
            inode,
            parent_directory.inode(),
        )?;
        parent_directory.link(name, directory.inode_mut())
    })();
    if let Err(error) = mutation {
        discard_uncommitted_stage(mounted, true)?;
        return Err(map_error(error));
    }
    if mounted.stage.is_empty() || mounted.stage.revoked_block_count() != 0 {
        discard_uncommitted_stage(mounted, true)?;
        return Err(Status::Invalid);
    }
    commit_namespace_mutation(
        mounted,
        PendingMutationKind::CreateDirectory,
        absolute,
    )
}

/// Remove one empty directory through the journaled mutation path.
pub(crate) fn remove_directory_probe(
    mounted: &mut Mounted,
    path: &[u8],
) -> Result<(), Status> {
    remove_directory_guarded(mounted, path, &[])
}

pub(crate) fn remove_directory_guarded(mounted: &mut Mounted, path: &[u8], open_inodes: &[u64]) -> Result<(), Status> {
    let absolute = absolute_path(path)?;
    if mounted.pending_mutation.is_some() {
        return resume_namespace_mutation(
            mounted,
            PendingMutationKind::RemoveDirectory,
            &absolute,
        );
    }
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        let recovery = mounted
            .journal
            .filesystem_recovery_marker_is_durable()
            .map_err(|_| Status::Invalid)?;
        discard_uncommitted_stage(mounted, recovery)?;
    }
    arm_recovery_marker(mounted)?;
    let (parent, name) = parent_and_name(&absolute)?;
    let filesystem = mounted.filesystem()?;
    let mutation = (|| {
        let path = Path::try_from(absolute.as_slice())
            .map_err(|_| Ext4Error::MalformedPath)?;
        let inode = filesystem
            .path_to_inode(path, FollowSymlinks::ExcludeFinalComponent)?;
        if !inode.file_type().is_dir() {
            return Err(Ext4Error::NotADirectory);
        }
        let parent_inode = filesystem
            .path_to_inode(parent, FollowSymlinks::All)?;
        let mut parent_directory =
            Dir::open_inode(filesystem, parent_inode)?;
        if open_inodes.contains(&u64::from(inode.index.get())) {
            parent_directory.remove_open_directory(name, inode)?;
            Ok(None)
        } else {
            parent_directory.remove_empty_directory(name, inode).map(Some)
        }
    })();
    let revoked_block = match mutation {
        Ok(block) => block,
        Err(error) => {
            discard_uncommitted_stage(mounted, true)?;
            return Err(map_error(error));
        }
    };
    // Removing the child can also shrink its parent; an empty grown child may
    // itself own several blocks. Keep every stage revoke, requiring at least
    // the child's validated first block in the committed transaction below.
    if mounted.stage.is_empty() || (revoked_block.is_some() && mounted.stage.revoked_block_count() == 0) {
        discard_uncommitted_stage(mounted, true)?;
        return Err(Status::Invalid);
    }
    let resumed = commit_staged_mutation(
        mounted,
        PendingMutationKind::RemoveDirectory,
        absolute,
        Vec::new(),
        0,
        0,
        &[],
        revoked_block,
    )?;
    if resumed == 0 {
        Ok(())
    } else {
        Err(Status::Invalid)
    }
}

/// Rename without replacement through JBD2, including indexed directories
/// across parents with their dotdot entry and parent link counts in one stage.
pub(crate) fn rename_probe(
    mounted: &mut Mounted,
    source: &[u8],
    destination: &[u8],
) -> Result<(), Status> {
    rename_transaction(mounted, source, destination, false, &[])
}

pub(crate) fn rename_replace_probe(
    mounted: &mut Mounted,
    source: &[u8],
    destination: &[u8],
) -> Result<(), Status> {
    rename_replace_guarded(mounted, source, destination, &[])
}

/// Replace while preserving live regular inodes and open directory snapshots.
pub(crate) fn rename_replace_guarded(mounted: &mut Mounted, source: &[u8],
    destination: &[u8], open_inodes: &[u64]) -> Result<(), Status> {
    rename_transaction(mounted, source, destination, true, open_inodes)
}

fn remove_rename_destination(
    directory: &mut Dir,
    name: DirEntryName<'_>,
    source: &ext4plus::inode::Inode,
    open_inodes: &[u64],
    defer_target: Option<u64>,
    reclaim: &mut Option<u64>,
) -> Result<bool, Ext4Error> {
    let target = match directory.get_entry(name) {
        Ok(target) => target,
        Err(Ext4Error::NotFound) => return Ok(true),
        Err(error) => return Err(error),
    };
    if target.index == source.index {
        return Ok(false); // POSIX same-inode replacement is a no-op.
    }
    if target.flags().intersects(InodeFlags::IMMUTABLE | InodeFlags::APPEND_ONLY) {
        return Err(Ext4Error::Readonly);
    }
    if target.file_type().is_dir() {
        if !source.file_type().is_dir() { return Err(Ext4Error::IsADirectory); }
        if open_inodes.contains(&u64::from(target.index.get())) {
            directory.remove_open_directory(name, target)?;
        } else {
            directory.remove_empty_directory(name, target)?;
        }
    } else {
        if source.file_type().is_dir() { return Err(Ext4Error::NotADirectory); }
        if !target.file_type().is_regular_file() && !target.file_type().is_symlink() {
            return Err(Ext4Error::IsASpecialFile);
        }
        let number = u64::from(target.index.get());
        let retain = target.file_type().is_regular_file() && open_inodes.contains(&number);
        if target.file_type().is_regular_file() && !retain && target.links_count() == 1
            && target.size_in_bytes() <= MAX_SPLIT_ORPHAN_BYTES {
            *reclaim = Some(number);
        }
        if retain || defer_target == Some(number) {
            directory.unlink_open(name, target)?;
        } else {
            directory.unlink(name, target)?;
        }
    }
    Ok(true)
}

fn rename_transaction(
    mounted: &mut Mounted,
    source: &[u8],
    destination: &[u8],
    replace: bool,
    open_inodes: &[u64],
) -> Result<(), Status> {
    let kind = if replace { PendingMutationKind::RenameReplace } else { PendingMutationKind::Rename };
    let source_absolute = absolute_path(source)?;
    let destination_absolute = absolute_path(destination)?;
    let pending_key = namespace_pair_key(&source_absolute, &destination_absolute)?;
    if let Some(pending) = &mounted.pending_reclaim {
        if pending.kind != kind || pending.path != pending_key { return Err(Status::Invalid); }
        return resume_reclaim_request(mounted);
    }
    if mounted.pending_mutation.is_some() {
        return resume_namespace_mutation(
            mounted,
            kind,
            &pending_key,
        );
    }
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        let recovery = mounted
            .journal
            .filesystem_recovery_marker_is_durable()
            .map_err(|_| Status::Invalid)?;
        discard_uncommitted_stage(mounted, recovery)?;
    }
    let mut defer_target = None;
    loop {
        arm_recovery_marker(mounted)?;
        let source_path = Path::try_from(source_absolute.as_slice())
            .map_err(|_| Status::Invalid)?;
        let (source_parent, source_name) = parent_and_name(&source_absolute)?;
        let (destination_parent, destination_name) =
            parent_and_name(&destination_absolute)?;
        let filesystem = mounted.filesystem()?;
        let mut inode = filesystem
            .path_to_inode(source_path, FollowSymlinks::ExcludeFinalComponent)
            .map_err(map_error)?;
        if inode.flags().intersects(InodeFlags::IMMUTABLE | InodeFlags::APPEND_ONLY) {
            return Err(Status::ReadOnly);
        }
        if !inode.file_type().is_regular_file() && !inode.file_type().is_dir()
            && !inode.file_type().is_symlink() {
            return Err(Status::Special);
        }
        let source_parent_inode = filesystem
            .path_to_inode(source_parent, FollowSymlinks::All).map_err(map_error)?;
        let destination_parent_inode = filesystem
            .path_to_inode(destination_parent, FollowSymlinks::All).map_err(map_error)?;
        if source_parent_inode.flags().intersects(InodeFlags::IMMUTABLE | InodeFlags::APPEND_ONLY)
            || destination_parent_inode.flags().contains(InodeFlags::IMMUTABLE) {
            return Err(Status::ReadOnly);
        }
        // Different path strings can resolve to the same directory through a
        // symlink. Use one directory object in that case so inode size updates
        // cannot be lost between two independently cached parent inodes.
        let same_parent = source_parent_inode.index == destination_parent_inode.index;
        let mut reclaim = None;
        let mutation = (|| {
            let mut source_directory = Dir::open_inode(filesystem, source_parent_inode)?;
            if same_parent {
                if replace && !remove_rename_destination(&mut source_directory, destination_name, &inode,
                    open_inodes, defer_target, &mut reclaim)? {
                    return Ok(());
                }
                return source_directory.rename_entry(source_name, destination_name, inode);
            }
            let mut destination_directory = Dir::open_inode(filesystem, destination_parent_inode)?;
            if replace && !remove_rename_destination(&mut destination_directory, destination_name, &inode,
                open_inodes, defer_target, &mut reclaim)? {
                return Ok(());
            }
            if inode.file_type().is_dir() {
                return source_directory.move_directory(source_name,
                    &mut destination_directory, destination_name, inode);
            }
            // Both changes remain in one stage. Link-before-unlink keeps the inode
            // allocated; its temporary extra link never reaches home metadata.
            destination_directory.link(destination_name, &mut inode)?;
            source_directory.unlink(source_name, inode).map(|_| ())
        })();
        if let Err(error) = mutation {
            let capacity = mutation_capacity_error(&error);
            discard_uncommitted_stage(mounted, true)?;
            if let Some(inode) = reclaim.filter(|_| capacity && defer_target.is_none()) {
                mounted.filesystem()?.validate_extent_reclaim(inode_index(inode)?,
                    (MAX_SPLIT_ORPHAN_BYTES / BLOCK_BYTES) as u32).map_err(map_error)?;
                defer_target = Some(inode);
                continue;
            }
            return Err(map_error(error));
        }
        if mounted.stage.is_empty() {
            discard_uncommitted_stage(mounted, true)?;
            return if replace { Ok(()) } else { Err(Status::Invalid) };
        }
        if let Some(inode) = defer_target {
            // The new name and the old target's orphan link become durable in
            // one transaction. Cleanup retries retain both pathname arguments.
            mounted.pending_reclaim = Some(PendingReclaim {
                kind, path: pending_key, argument: 0, inode, retain_unlinked: false, namespace_committed: false,
            });
            return resume_reclaim_request(mounted);
        }
        return commit_namespace_mutation(mounted, kind, pending_key);
    }
}

/// Retry and durably execute the final clean plan while C holds a write lease.
pub(crate) fn prepare_unmount(mounted: &mut Mounted) -> Result<(), Status> {
    sync_with_open_inodes(mounted, &[])?;
    unmount(mounted)
}

/// Drain durable mutations and reclaim only orphans with no live C handle.
/// Live orphans keep the recovery marker set even after a successful sync.
pub(crate) fn sync_with_open_inodes(mounted: &mut Mounted, open_inodes: &[u64]) -> Result<(), Status> {
    if mounted.pending_reclaim.is_some() {
        resume_reclaim_request(mounted)?;
    } else if mounted.pending_write.is_some() {
        let _ = resume_write_request(mounted)?;
    } else if mounted.pending_mutation.is_some() {
        let _ = resume_pending_mutation_inner(mounted)?;
    }
    ensure_staged_view(mounted)?;
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        return Err(Status::Invalid);
    }
    let orphans = mounted.filesystem()?.orphan_inodes().map_err(map_error)?;
    for inode in orphans {
        if !open_inodes.contains(&u64::from(inode.get()))
            || allocated_inode(mounted.filesystem()?, u64::from(inode.get()))?.links_count() != 0 {
            finalize_orphan(mounted, u64::from(inode.get()))?;
        }
    }
    if mounted.filesystem()?.superblock().last_orphan() != 0 { return Ok(()); }
    match mounted.journal.filesystem_is_clean() {
        Ok(true) => {
            let view_needs_recovery = load_journal_inode_map(mounted.filesystem()?)
                .map_err(map_journal_error)?
                .filesystem_needs_recovery();
            if view_needs_recovery {
                replace_staged_view(mounted, false)?;
            }
            return unmount(mounted);
        }
        Ok(false) => {}
        Err(_) => return Err(Status::Invalid),
    }
    if !mounted.journal.filesystem_recovery_marker_is_durable()
        .map_err(|_| Status::Invalid)?
    {
        // A failed activation write/flush owns a retryable marker plan even
        // though no upstream mutation has started. Finish that exact plan
        // before attempting the final clear; otherwise sync cannot release it.
        arm_recovery_marker(mounted)?;
    }
    let operations = mounted
        .journal
        .prepare_filesystem_clean_plan()
        .map_err(|_| Status::Invalid)?;
    execute_storage_plan(mounted.context, &operations)?;
    mounted
        .journal
        .mark_filesystem_clean_durable()
        .map_err(|_| Status::Invalid)?;
    replace_staged_view(mounted, false)?;
    unmount(mounted)
}

/// Synchronize a caller with no live inode handles without releasing the mount.
///
/// Every mutation commit is already synchronous through checkpoint. Sync adds
/// the retry-stable final marker clear; a later mutation must durably re-arm
/// recovery before it can start another journal transaction.
pub(crate) fn sync(mounted: &mut Mounted) -> Result<(), Status> {
    sync_with_open_inodes(mounted, &[])
}

/// Refuse to release a mount unless its retained journal state is idle and clean.
pub(crate) fn unmount(mounted: &Mounted) -> Result<(), Status> {
    if mounted.filesystem.is_none()
        || mounted.pending_write.is_some()
        || mounted.pending_reclaim.is_some()
        || mounted.pending_mutation.is_some()
        || !mounted.stage.is_empty()
        || mounted.stage.is_sealed()
    {
        return Err(Status::Invalid);
    }
    match mounted.journal.filesystem_is_clean() {
        Ok(true) => Ok(()),
        Ok(false) | Err(_) => Err(Status::Invalid),
    }
}

/// Resolve a path and return inode-stable metadata.
pub(crate) fn stat(mounted: &Mounted, path: &[u8]) -> Result<Metadata, Status> {
    metadata_at(mounted, path, FollowSymlinks::All)
}

pub(crate) fn lstat(mounted: &Mounted, path: &[u8]) -> Result<Metadata, Status> {
    metadata_at(mounted, path, FollowSymlinks::ExcludeFinalComponent)
}

fn metadata_at(mounted: &Mounted, path: &[u8], follow: FollowSymlinks) -> Result<Metadata, Status> {
    let absolute = absolute_path(path)?;
    let checked = Path::try_from(absolute.as_slice()).map_err(|_| Status::Invalid)?;
    let inode = mounted
        .readable_filesystem()?
        .path_to_inode(checked, follow)
        .map_err(map_error)?;
    inode_metadata(&inode)
}

/// Read bytes at a 64-bit offset without changing any shared cursor.
pub(crate) fn stat_inode(mounted: &Mounted, number: u64) -> Result<Metadata, Status> {
    let inode = allocated_inode(mounted.readable_filesystem()?, number)?;
    inode_metadata(&inode)
}

pub(crate) fn pread_inode(mounted: &Mounted, number: u64, offset: u64, destination: &mut [u8]) -> Result<usize, Status> {
    let mut file = open_io_target(mounted.readable_filesystem()?, &inode_key(number)?)?;
    file.read_bytes_at(destination, offset).map_err(map_error)
}

/// Read bytes at a 64-bit offset without changing any shared cursor.
pub(crate) fn pread(
    mounted: &Mounted,
    path: &[u8],
    offset: u64,
    destination: &mut [u8],
) -> Result<usize, Status> {
    let absolute = absolute_path(path)?;
    let mut file = mounted
        .readable_filesystem()?
        .open(absolute.as_slice())
        .map_err(map_error)?;
    file.read_bytes_at(destination, offset).map_err(map_error)
}

/// Owned point-in-time directory entries, released with their VFS handle.
pub(crate) struct DirectorySnapshot {
    pub(crate) metadata: Metadata,
    entries: Vec<DirectoryEntry>,
}

impl DirectorySnapshot {
    pub(crate) fn entry(&self, index: u64) -> Option<DirectoryEntry> {
        usize::try_from(index).ok().and_then(|index| self.entries.get(index)).copied()
    }
}

/// Capture a bounded, owned directory view under one read lease. Later
/// namespace mutations cannot reorder, duplicate or skip this iterator's names.
pub(crate) fn directory_snapshot(mounted: &Mounted, path: &[u8]) -> Result<Box<DirectorySnapshot>, Status> {
    let metadata = stat(mounted, path)?;
    let filesystem = mounted.readable_filesystem()?;
    let absolute = absolute_path(path)?;
    let mut entries = Vec::new();
    for result in filesystem.read_dir(absolute.as_slice()).map_err(map_error)? {
        let entry = result.map_err(map_error)?;
        let name = entry.file_name();
        if name == "." || name == ".." { continue; }
        if entries.len() == MAX_VALIDATED_ENTRIES { return Err(Status::Range); }
        let bytes = name.as_ref();
        let inode = Inode::read(filesystem, entry.inode).map_err(map_error)?;
        let mut output = DirectoryEntry {
            metadata: inode_metadata(&inode)?,
            name_length: u16::try_from(bytes.len()).map_err(|_| Status::Range)?,
            ..DirectoryEntry::default()
        };
        output.name[..bytes.len()].copy_from_slice(bytes);
        entries.try_reserve(1).map_err(|_| Status::Range)?;
        entries.push(output);
    }
    Ok(Box::new(DirectorySnapshot { metadata, entries }))
}

/// Return the visible directory entry at `wanted_index`.
pub(crate) fn directory_entry(
    mounted: &Mounted,
    path: &[u8],
    wanted_index: u64,
) -> Result<Option<DirectoryEntry>, Status> {
    let absolute = absolute_path(path)?;
    let mut directory = mounted
        .readable_filesystem()?
        .read_dir(absolute.as_slice())
        .map_err(map_error)?;
    let mut visible = 0u64;
    for result in &mut directory {
        let entry = result.map_err(map_error)?;
        let name = entry.file_name();
        if name == "." || name == ".." {
            continue;
        }
        if visible == wanted_index {
            let bytes = name.as_ref();
            let inode = ext4plus::inode::Inode::read(mounted.readable_filesystem()?, entry.inode)
                .map_err(map_error)?;
            let mut output = DirectoryEntry {
                metadata: inode_metadata(&inode)?,
                name_length: u16::try_from(bytes.len()).map_err(|_| Status::Range)?,
                ..DirectoryEntry::default()
            };
            output.name[..bytes.len()].copy_from_slice(bytes);
            return Ok(Some(output));
        }
        visible = visible.checked_add(1).ok_or(Status::Range)?;
    }
    Ok(None)
}

/// Stable status codes returned across the C boundary.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(i32)]
pub(crate) enum Status {
    /// The operation succeeded.
    Ok = 0,
    /// A required pointer was null.
    NullArgument = 1,
    /// The native volume lease could not be acquired.
    Volume = 2,
    /// The native block reader failed.
    Io = 3,
    /// The media or path is invalid for the supported profile.
    Invalid = 4,
    /// No inode exists at the path.
    NotFound = 5,
    /// A directory operation targeted a non-directory.
    NotDirectory = 6,
    /// A file operation targeted a directory.
    IsDirectory = 7,
    /// A checked bound was exceeded.
    Range = 8,
    /// Device nodes, FIFOs, and sockets are outside the profile.
    Special = 9,
    /// A namespace entry already exists at the requested path.
    Exists = 10,
    /// A directory removal targeted a directory with live children.
    NotEmpty = 11,
    /// Filesystem blocks or inodes are exhausted.
    Full = 12,
    /// The filesystem or inode refuses mutation.
    ReadOnly = 13,
    /// An open inode requires unsupported orphan handling before deletion.
    Busy = 14,
}

const _: i32 = Status::Volume as i32;
