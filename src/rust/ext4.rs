// SPDX-License-Identifier: GPL-3.0-only
//! Checked, journaled ext4 operations over Phipia's native block boundary.

extern crate alloc;

use alloc::boxed::Box;
use alloc::rc::Rc;
use alloc::vec::Vec;
use core::error::Error;
use core::fmt::{self, Display, Formatter};
use core::time::Duration;
use ext4plus::dir::Dir;
use ext4plus::error::Ext4Error;
use ext4plus::inode::{InodeCreationOptions, InodeFlags, InodeMode};
use ext4plus::path::Path;
use ext4plus::{
    DirEntryName, Ext4, Ext4Read, FileType, FilesystemSuperblockCheckpoint,
    FollowSymlinks, JOURNAL_BLOCK_BYTES, JournalCommitOperation,
    JournalExecutionError, JournalFlush, JournalInodeMap, JournalInodeMapError,
    JournalMutationStage, JournalPreparedTransaction, JournalRing, JournalStorage,
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
    filesystem: Ext4,
    journal: JournalRing,
    stage: Rc<JournalMutationStage>,
    pending_mutation: Option<PendingMutation>,
    context: usize,
    image_bytes: u64,
}

/// Return the allocator's current capacity for the admitted 4 KiB profile.
/// A committed mutation reloads the checked filesystem view, so callers never
/// confuse unused NVMe namespace bytes with allocatable ext4 blocks.
pub(crate) fn free_bytes(mounted: &Mounted) -> Result<u64, Status> {
    mounted
        .filesystem
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
    LinkFile,
    CreateDirectory,
    RemoveDirectory,
    Rename,
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
) -> Result<(Ext4, Rc<JournalMutationStage>), Status> {
    let stage = Rc::new(
        JournalMutationStage::new(Box::new(PhipiaReader { context }), image_bytes)
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
    let (filesystem, stage) =
        load_staged_view(mounted.context, mounted.image_bytes, needs_recovery)?;
    mounted.filesystem = filesystem;
    mounted.stage = stage;
    Ok(())
}

fn discard_uncommitted_stage(mounted: &mut Mounted, needs_recovery: bool) -> Result<(), Status> {
    mounted.stage.rollback();
    replace_staged_view(mounted, needs_recovery)
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
        || read_u32(&superblock, 0x20) == Some(0)
        || read_u32(&superblock, 0x28) == Some(0)
    {
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
    execute_storage_plan(context, &operations)?;
    Ok(report)
}

fn validate_xattrs(filesystem: &Ext4, path: &[u8]) -> Result<(), Status> {
    let xattrs = filesystem.list_xattrs(path).map_err(map_error)?;
    for name in xattrs {
        let _value = filesystem
            .get_xattr(path, name.as_slice())
            .map_err(map_error)?;
    }
    Ok(())
}

fn validate_namespace(filesystem: &Ext4) -> Result<(), Status> {
    let mut pending = Vec::new();
    pending.try_reserve_exact(1).map_err(|_| Status::Range)?;
    let mut root = Vec::new();
    root.try_reserve_exact(1).map_err(|_| Status::Range)?;
    root.push(b'/');
    validate_xattrs(filesystem, root.as_slice())?;
    pending.push(root);
    let mut visited = 0usize;
    while let Some(path) = pending.pop() {
        let mut directory = filesystem.read_dir(path.as_slice()).map_err(map_error)?;
        for result in &mut directory {
            let entry = result.map_err(map_error)?;
            let name = entry.file_name();
            if name == "." || name == ".." {
                continue;
            }
            visited = visited.checked_add(1).ok_or(Status::Range)?;
            if visited > MAX_VALIDATED_ENTRIES {
                return Err(Status::Range);
            }
            let entry_path = entry.path();
            let metadata = entry.metadata().map_err(map_error)?;
            validate_xattrs(filesystem, entry_path.as_ref())?;
            let kind = classify(metadata.file_type())?;
            if kind == 2 {
                if pending.len() >= MAX_PENDING_DIRECTORIES {
                    return Err(Status::Range);
                }
                pending.try_reserve(1).map_err(|_| Status::Range)?;
                let path_bytes: &[u8] = entry_path.as_ref();
                let mut owned_path = Vec::new();
                owned_path
                    .try_reserve_exact(path_bytes.len())
                    .map_err(|_| Status::Range)?;
                owned_path.extend_from_slice(path_bytes);
                pending.push(owned_path);
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
    }
    Ok(())
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
        if journal.filesystem_needs_recovery() {
            return Err(Status::Invalid);
        }
    }
    drop(journal);
    drop(filesystem);
    let stage = Rc::new(
        JournalMutationStage::new(Box::new(PhipiaReader { context }), image_bytes)
            .map_err(|_| Status::Invalid)?,
    );
    let filesystem = Ext4::load_with_writer(Box::new(stage.clone()), Some(Box::new(stage.clone())))
        .map_err(map_error)?;
    let journal = load_journal_inode_map(&filesystem).map_err(map_journal_error)?;
    if journal.filesystem_needs_recovery() || !stage.is_empty() {
        return Err(Status::Invalid);
    }
    let clean_ring = journal.into_clean_ring().map_err(|_| Status::Invalid)?;
    validate_namespace(&filesystem)?;
    let identity = Identity {
        label: *filesystem.label().as_bytes(),
        uuid: *filesystem.uuid().as_bytes(),
        recovered_transactions: recovery.transactions,
        replayed_blocks: recovery.replayed_blocks,
        consumed_slots: recovery.consumed_slots,
        recovery_performed,
        reserved: [0; 3],
    };
    Ok((
        Box::new(Mounted {
            filesystem,
            journal: clean_ring,
            stage,
            pending_mutation: None,
            context,
            image_bytes,
        }),
        identity,
    ))
}

fn arm_recovery_marker(mounted: &mut Mounted) -> Result<(), Status> {
    let durable = mounted
        .journal
        .filesystem_recovery_marker_is_durable()
        .map_err(|_| Status::Invalid)?;
    let view_needs_recovery = load_journal_inode_map(&mounted.filesystem)
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

/// Execute one controlled staged write through the native journal executor.
///
/// The same request retries a retained commit/checkpoint plan after storage I/O
/// refusal; different input is rejected while a request remains pending.
pub(crate) fn transaction_probe(
    mounted: &mut Mounted,
    path: &[u8],
    offset: u64,
    source: &[u8],
) -> Result<usize, Status> {
    if source.is_empty() || source.len() > MAX_PROBE_WRITE_BYTES {
        return Err(Status::Range);
    }
    let absolute = absolute_path(path)?;
    if mounted.pending_mutation.is_some() {
        return resume_pending_mutation(
            mounted,
            PendingMutationKind::Write,
            &absolute,
            offset,
            source,
        );
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

    let mut file = match mounted.filesystem.open(absolute.as_slice()) {
        Ok(file) => file,
        Err(error) => return Err(map_error(error)),
    };
    let written = match file.write_bytes_at(source, offset) {
        Ok(0) => {
            discard_uncommitted_stage(mounted, true)?;
            return Err(Status::Invalid);
        }
        Ok(written) => written,
        Err(error) => {
            discard_uncommitted_stage(mounted, true)?;
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
    )
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
    let absolute = absolute_path(path)?;
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
    let file = mounted
        .filesystem
        .open(absolute.as_slice())
        .map_err(map_error)?;
    let old_size = file.inode().size_in_bytes();
    drop(file);
    if size == old_size {
        return Ok(());
    }
    arm_recovery_marker(mounted)?;
    let mut file = mounted
        .filesystem
        .open(absolute.as_slice())
        .map_err(map_error)?;
    if let Err(error) = file.truncate(size) {
        discard_uncommitted_stage(mounted, true)?;
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
    let mutation = (|| {
        let parent_inode = mounted
            .filesystem
            .path_to_inode(parent, FollowSymlinks::All)?;
        let mut directory = Dir::open_inode(&mounted.filesystem, parent_inode)?;
        let mut inode = mounted.filesystem.create_inode(InodeCreationOptions {
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

/// Remove one regular-file link through the journaled mutation path.
pub(crate) fn unlink_file_probe(
    mounted: &mut Mounted,
    path: &[u8],
) -> Result<(), Status> {
    let absolute = absolute_path(path)?;
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
    let mutation = (|| {
        let path = Path::try_from(absolute.as_slice())
            .map_err(|_| Ext4Error::MalformedPath)?;
        let inode = mounted
            .filesystem
            .path_to_inode(path, FollowSymlinks::ExcludeFinalComponent)?;
        if !inode.file_type().is_regular_file() {
            return Err(Ext4Error::IsADirectory);
        }
        let parent_inode = mounted
            .filesystem
            .path_to_inode(parent, FollowSymlinks::All)?;
        let mut directory = Dir::open_inode(&mounted.filesystem, parent_inode)?;
        directory.unlink(name, inode).map(|_| ())
    })();
    if let Err(error) = mutation {
        discard_uncommitted_stage(mounted, true)?;
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

/// Create one regular-file hard link through the journaled mutation path.
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
    let mutation = (|| {
        let mut inode = mounted
            .filesystem
            .path_to_inode(source_path, FollowSymlinks::ExcludeFinalComponent)?;
        if !inode.file_type().is_regular_file() {
            return Err(Ext4Error::IsADirectory);
        }
        let parent_inode = mounted
            .filesystem
            .path_to_inode(parent, FollowSymlinks::All)?;
        let mut directory = Dir::open_inode(&mounted.filesystem, parent_inode)?;
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
    let mutation = (|| {
        let parent_inode = mounted
            .filesystem
            .path_to_inode(parent, FollowSymlinks::All)?;
        let mut parent_directory =
            Dir::open_inode(&mounted.filesystem, parent_inode)?;
        let inode = mounted.filesystem.create_inode(InodeCreationOptions {
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
            mounted.filesystem.clone(),
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
    let mutation = (|| {
        let path = Path::try_from(absolute.as_slice())
            .map_err(|_| Ext4Error::MalformedPath)?;
        let inode = mounted
            .filesystem
            .path_to_inode(path, FollowSymlinks::ExcludeFinalComponent)?;
        if !inode.file_type().is_dir() {
            return Err(Ext4Error::NotADirectory);
        }
        let parent_inode = mounted
            .filesystem
            .path_to_inode(parent, FollowSymlinks::All)?;
        let mut parent_directory =
            Dir::open_inode(&mounted.filesystem, parent_inode)?;
        parent_directory.remove_empty_directory(name, inode)
    })();
    let revoked_block = match mutation {
        Ok(block) => block,
        Err(error) => {
            discard_uncommitted_stage(mounted, true)?;
            return Err(map_error(error));
        }
    };
    if mounted.stage.is_empty() || mounted.stage.revoked_block_count() != 1 {
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
        Some(revoked_block),
    )?;
    if resumed == 0 {
        Ok(())
    } else {
        Err(Status::Invalid)
    }
}

/// Rename one regular file or directory within its parent through JBD2.
pub(crate) fn rename_probe(
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
            PendingMutationKind::Rename,
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
    let (source_parent, source_name) = parent_and_name(&source_absolute)?;
    let (destination_parent, destination_name) =
        parent_and_name(&destination_absolute)?;
    if source_parent != destination_parent {
        discard_uncommitted_stage(mounted, true)?;
        return Err(Status::Invalid);
    }
    let mutation = (|| {
        let inode = mounted
            .filesystem
            .path_to_inode(source_path, FollowSymlinks::ExcludeFinalComponent)?;
        if !inode.file_type().is_regular_file() && !inode.file_type().is_dir() {
            return Err(Ext4Error::IsASpecialFile);
        }
        let parent_inode = mounted
            .filesystem
            .path_to_inode(source_parent, FollowSymlinks::All)?;
        let mut parent_directory =
            Dir::open_inode(&mounted.filesystem, parent_inode)?;
        parent_directory.rename_entry(source_name, destination_name, inode)
    })();
    if let Err(error) = mutation {
        discard_uncommitted_stage(mounted, true)?;
        return Err(map_error(error));
    }
    if mounted.stage.is_empty() || mounted.stage.revoked_block_count() != 0 {
        discard_uncommitted_stage(mounted, true)?;
        return Err(Status::Invalid);
    }
    commit_namespace_mutation(mounted, PendingMutationKind::Rename, pending_key)
}

/// Retry and durably execute the final clean plan while C holds a write lease.
pub(crate) fn prepare_unmount(mounted: &mut Mounted) -> Result<(), Status> {
    if mounted.pending_mutation.is_some() {
        let _ = resume_pending_mutation_inner(mounted)?;
    }
    if !mounted.stage.is_empty() || mounted.stage.is_sealed() {
        return Err(Status::Invalid);
    }
    match mounted.journal.filesystem_is_clean() {
        Ok(true) => {
            let view_needs_recovery = load_journal_inode_map(&mounted.filesystem)
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

/// Force all retained journal state clean without releasing the live mount.
///
/// Every mutation commit is already synchronous through checkpoint. Sync adds
/// the retry-stable final marker clear; a later mutation must durably re-arm
/// recovery before it can start another journal transaction.
pub(crate) fn sync(mounted: &mut Mounted) -> Result<(), Status> {
    prepare_unmount(mounted)
}

/// Refuse to release a mount unless its retained journal state is idle and clean.
pub(crate) fn unmount(mounted: &Mounted) -> Result<(), Status> {
    if mounted.pending_mutation.is_some()
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
    let absolute = absolute_path(path)?;
    let checked = Path::try_from(absolute.as_slice()).map_err(|_| Status::Invalid)?;
    let inode = mounted
        .filesystem
        .path_to_inode(checked, FollowSymlinks::All)
        .map_err(map_error)?;
    inode_metadata(&inode)
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
        .filesystem
        .open(absolute.as_slice())
        .map_err(map_error)?;
    file.read_bytes_at(destination, offset).map_err(map_error)
}

/// Return the visible directory entry at `wanted_index`.
pub(crate) fn directory_entry(
    mounted: &Mounted,
    path: &[u8],
    wanted_index: u64,
) -> Result<Option<DirectoryEntry>, Status> {
    let absolute = absolute_path(path)?;
    let mut directory = mounted
        .filesystem
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
            let inode = ext4plus::inode::Inode::read(&mounted.filesystem, entry.inode)
                .map_err(map_error)?;
            let mut output = DirectoryEntry {
                metadata: inode_metadata(&inode)?,
                name_length: u16::try_from(bytes.len()).map_err(|_| Status::Range)?,
                ..DirectoryEntry::default()
            };
            if bytes.len() > output.name.len() {
                return Err(Status::Range);
            }
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
}

const _: i32 = Status::Volume as i32;
