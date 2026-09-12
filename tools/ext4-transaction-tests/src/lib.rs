// SPDX-License-Identifier: GPL-3.0-only
#![cfg(test)]

use ext4plus::error::Ext4Error;
use ext4plus::{
    Ext4, Ext4Read, Ext4Write, FILESYSTEM_SUPERBLOCK_START_BYTE, JOURNAL_BLOCK_BYTES,
    JournalCommitOperation, JournalExecutionError, JournalFlush, JournalMutationStage,
    JournalMutationPlanError, JournalMutationStageError, JournalPreparedTransaction,
    JournalRecordKind, JournalRing, JournalStorage, JournalSuperblockImage, JournalTransaction,
    JournalTransactionError, execute_commit_operations, load_journal_inode_map,
    recover_committed_ring, replay_committed_transaction,
};
use std::collections::BTreeMap;
use std::rc::Rc;

const UUID: [u8; 16] = [0x5a; 16];
const MAXIMUM_BLOCK: u64 = 4095;
const JOURNAL_SLOTS: [u64; 4] = [3000, 3001, 3002, 3003];

fn filled(value: u8) -> Vec<u8> {
    vec![value; JOURNAL_BLOCK_BYTES]
}

fn ext4_crc32c(bytes: &[u8]) -> u32 {
    ext4_crc32c_with_seed(bytes, u32::MAX)
}

fn ext4_crc32c_with_seed(bytes: &[u8], seed: u32) -> u32 {
    const REFLECTED_CRC32C_POLYNOMIAL: u32 = 0x82f6_3b78;

    let mut checksum = seed;
    for byte in bytes {
        checksum ^= u32::from(*byte);
        for _ in 0..8 {
            let mask = 0u32.wrapping_sub(checksum & 1);
            checksum = (checksum >> 1) ^ (REFLECTED_CRC32C_POLYNOMIAL & mask);
        }
    }
    checksum
}

fn transaction() -> JournalTransaction {
    let mut transaction = JournalTransaction::new(17, UUID, MAXIMUM_BLOCK).unwrap();
    transaction.stage_ordered_data(100, &filled(0x11)).unwrap();
    transaction.stage_metadata(200, &filled(0x22)).unwrap();
    transaction.stage_metadata(201, &filled(0x33)).unwrap();
    transaction
}

fn mapped_ring(next_sequence: u32, slots: &[u64]) -> JournalRing {
    let superblock =
        JournalSuperblockImage::new_clean(next_sequence, UUID, (slots.len() + 1) as u32).unwrap();
    let mut physical = Vec::with_capacity(slots.len() + 1);
    physical.push(MAXIMUM_BLOCK);
    physical.extend_from_slice(slots);
    superblock.map_clean_ring(MAXIMUM_BLOCK, &physical).unwrap()
}

fn finish_transaction(ring: &mut JournalRing, prepared: &JournalPreparedTransaction) {
    let _commit = ring.prepare_commit_plan(prepared).unwrap();
    ring.mark_commit_durable(prepared.ticket()).unwrap();
    let _checkpoint = ring.prepare_checkpoint_plan(prepared.ticket()).unwrap();
    ring.checkpoint_durable(prepared.ticket()).unwrap();
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

#[derive(Debug, Eq, PartialEq)]
enum StorageEvent {
    Write(u64, usize),
    Flush(JournalFlush),
}

#[derive(Default)]
struct RecordingStorage {
    events: Vec<StorageEvent>,
}

impl JournalStorage for RecordingStorage {
    type Error = core::convert::Infallible;

    fn write(&mut self, start_byte: u64, bytes: &[u8]) -> Result<(), Self::Error> {
        self.events.push(StorageEvent::Write(start_byte, bytes.len()));
        Ok(())
    }

    fn flush(&mut self, boundary: JournalFlush) -> Result<(), Self::Error> {
        self.events.push(StorageEvent::Flush(boundary));
        Ok(())
    }
}

struct VectorStorage {
    bytes: Vec<u8>,
    flushes: Vec<JournalFlush>,
}

impl JournalStorage for VectorStorage {
    type Error = core::convert::Infallible;

    fn write(&mut self, start_byte: u64, bytes: &[u8]) -> Result<(), Self::Error> {
        let start = usize::try_from(start_byte).unwrap();
        let end = start.checked_add(bytes.len()).unwrap();
        self.bytes[start..end].copy_from_slice(bytes);
        Ok(())
    }

    fn flush(&mut self, boundary: JournalFlush) -> Result<(), Self::Error> {
        self.flushes.push(boundary);
        Ok(())
    }
}

#[test]
fn public_executor_maps_every_block_write_and_preserves_flushes() {
    let slots = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007];
    let mut ring = mapped_ring(17, &slots);
    let prepared = ring.prepare(&transaction()).unwrap();
    let operations = ring.prepare_commit_plan(&prepared).unwrap();
    assert_eq!(ring.prepare_commit_plan(&prepared).unwrap(), operations);
    let mut storage = RecordingStorage::default();
    execute_commit_operations(&mut storage, &operations).unwrap();

    assert_eq!(
        storage.events,
        vec![
            StorageEvent::Write(MAXIMUM_BLOCK * JOURNAL_BLOCK_BYTES as u64, 1024),
            StorageEvent::Write(100 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Flush(JournalFlush::OrderedData),
            StorageEvent::Write(3000 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Write(3001 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Write(3002 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Flush(JournalFlush::JournalPayload),
            StorageEvent::Write(3003 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Flush(JournalFlush::Commit),
            StorageEvent::Write(200 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Write(201 * JOURNAL_BLOCK_BYTES as u64, JOURNAL_BLOCK_BYTES),
            StorageEvent::Flush(JournalFlush::Checkpoint),
        ]
    );

    let overflow = [JournalCommitOperation::WriteJournal {
        journal_block: u64::MAX,
        kind: JournalRecordKind::Commit,
        bytes: filled(0),
    }];
    let mut storage = RecordingStorage::default();
    assert_eq!(
        execute_commit_operations(&mut storage, &overflow),
        Err(JournalExecutionError::AddressOverflow)
    );
    assert!(storage.events.is_empty());
}

#[test]
fn mutation_stage_coalesces_partial_blocks_without_writing_through() {
    let backing = Rc::new(
        (0..JOURNAL_BLOCK_BYTES * 66)
            .map(|index| (index % 251) as u8)
            .collect::<Vec<_>>(),
    );
    let stage = JournalMutationStage::new(
        Box::new(backing.clone()),
        u64::try_from(backing.len()).unwrap(),
    )
    .unwrap();
    assert_eq!(
        Ext4Write::write(&stage, 1023, &[0xff]).unwrap_err().to_string(),
        JournalMutationStageError::Range.to_string()
    );
    let original = backing[8186..8202].to_vec();
    Ext4Write::write(&stage, 8190, &[0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6]).unwrap();
    assert_eq!(stage.staged_block_count(), 2);
    let images = stage.staged_images();
    assert_eq!(images.len(), 2);
    assert_eq!(images[0].block_index(), 1);
    assert_eq!(images[1].block_index(), 2);
    assert_eq!(&images[0].bytes()[4094..], &[0xa1, 0xa2]);
    assert_eq!(&images[1].bytes()[..4], &[0xa3, 0xa4, 0xa5, 0xa6]);

    let mut overlaid = [0u8; 16];
    Ext4Read::read(&stage, 8186, &mut overlaid).unwrap();
    assert_eq!(&overlaid[..4], &original[..4]);
    assert_eq!(&overlaid[4..10], &[0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6]);
    assert_eq!(&overlaid[10..], &original[10..]);
    assert_eq!(&backing[8186..8202], original.as_slice());

    Ext4Write::revoke_blocks(&stage, 2, 1).unwrap();
    assert_eq!(stage.staged_block_count(), 1);
    assert_eq!(stage.revoked_block_count(), 1);
    assert!(!stage.is_empty());

    stage.rollback();
    assert_eq!(stage.staged_block_count(), 0);
    assert_eq!(stage.revoked_block_count(), 0);
    assert!(stage.is_empty());
    Ext4Read::read(&stage, 8186, &mut overlaid).unwrap();
    assert_eq!(overlaid.as_slice(), original.as_slice());

    for block in 1..=64u64 {
        Ext4Write::write(&stage, block * JOURNAL_BLOCK_BYTES as u64, &[0xff]).unwrap();
    }
    let error = Ext4Write::write(&stage, 65 * JOURNAL_BLOCK_BYTES as u64, &[0xff])
        .unwrap_err();
    assert_eq!(error.to_string(), JournalMutationStageError::TooManyBlocks.to_string());
    assert_eq!(stage.staged_block_count(), 64);
    assert_eq!(backing[65 * JOURNAL_BLOCK_BYTES], (65 * JOURNAL_BLOCK_BYTES % 251) as u8);
}

#[test]
fn mutation_stage_derives_bounded_revocations_from_freed_blocks() {
    let backing = Rc::new(vec![0u8; JOURNAL_BLOCK_BYTES * 70]);
    let stage = JournalMutationStage::new(
        Box::new(backing),
        u64::try_from(JOURNAL_BLOCK_BYTES * 70).unwrap(),
    )
    .unwrap();
    Ext4Write::write(&stage, JOURNAL_BLOCK_BYTES as u64, &[0x11]).unwrap();
    Ext4Write::write(&stage, JOURNAL_BLOCK_BYTES as u64 * 2, &[0x22]).unwrap();
    Ext4Write::revoke_blocks(&stage, 2, 1).unwrap();
    assert_eq!(stage.staged_block_count(), 1);
    assert_eq!(stage.revoked_block_count(), 1);

    let transaction = stage
        .build_transaction(&JournalTransaction::new(7, UUID, MAXIMUM_BLOCK).unwrap(), &[])
        .unwrap();
    assert!(transaction.revokes_block(2));
    let plan = transaction
        .commit_plan(&[3000, 3001, 3002, 3003])
        .unwrap();
    assert!(plan.iter().any(|operation| matches!(
        operation,
        JournalCommitOperation::WriteHomeMetadata(image) if image.block_index() == 1
    )));
    assert!(!plan.iter().any(|operation| matches!(
        operation,
        JournalCommitOperation::WriteHomeMetadata(image) if image.block_index() == 2
    )));

    stage.rollback();
    Ext4Write::revoke_blocks(&stage, 1, 64).unwrap();
    assert_eq!(stage.revoked_block_count(), 64);
    assert_eq!(
        Ext4Write::revoke_blocks(&stage, 65, 1)
            .unwrap_err()
            .to_string(),
        JournalMutationStageError::TooManyRevocations.to_string()
    );
    assert_eq!(stage.revoked_block_count(), 64);
    assert_eq!(
        Ext4Write::revoke_blocks(&stage, 0, 1)
            .unwrap_err()
            .to_string(),
        JournalMutationStageError::Range.to_string()
    );
}

#[test]
fn mutation_stage_classifies_one_atomic_journal_transaction() {
    let backing = Rc::new(vec![0u8; JOURNAL_BLOCK_BYTES * 8]);
    let stage = JournalMutationStage::new(
        Box::new(backing),
        u64::try_from(JOURNAL_BLOCK_BYTES * 8).unwrap(),
    )
    .unwrap();
    let base = JournalTransaction::new(7, UUID, MAXIMUM_BLOCK).unwrap();
    assert_eq!(
        stage.build_transaction(&base, &[]).unwrap_err(),
        JournalMutationPlanError::EmptyStage
    );
    Ext4Write::write(&stage, JOURNAL_BLOCK_BYTES as u64, &[0x11]).unwrap();
    Ext4Write::write(&stage, JOURNAL_BLOCK_BYTES as u64 * 2, &[0x22]).unwrap();
    assert_eq!(
        stage.build_transaction(&base, &[3]).unwrap_err(),
        JournalMutationPlanError::OrderedDataNotStaged
    );
    assert_eq!(
        stage.build_transaction(&base, &[2, 2]).unwrap_err(),
        JournalMutationPlanError::DuplicateOrderedData
    );
    let mut revoked = base.clone();
    revoked.stage_revocation(1).unwrap();
    assert_eq!(
        stage.build_transaction(&revoked, &[2]).unwrap_err(),
        JournalMutationPlanError::StagedBlockRevoked
    );

    let classified = stage.build_transaction(&base, &[2]).unwrap();
    assert!(stage.is_sealed());
    assert_eq!(
        stage.build_transaction(&base, &[2]).unwrap_err(),
        JournalMutationPlanError::StageSealed
    );
    assert_eq!(
        Ext4Write::write(&stage, JOURNAL_BLOCK_BYTES as u64 * 3, &[0x33])
            .unwrap_err()
            .to_string(),
        JournalMutationStageError::Sealed.to_string()
    );
    assert_eq!(
        base.required_journal_slots(),
        Err(JournalTransactionError::EmptyTransaction)
    );
    let plan = classified.commit_plan(&[3000, 3001, 3002]).unwrap();
    assert!(matches!(
        &plan[0],
        JournalCommitOperation::WriteOrderedData(image) if image.block_index() == 2
    ));
    assert_eq!(plan[1], JournalCommitOperation::Flush(JournalFlush::OrderedData));
    assert!(matches!(
        &plan[7],
        JournalCommitOperation::WriteHomeMetadata(image) if image.block_index() == 1
    ));
    assert_eq!(plan[8], JournalCommitOperation::Flush(JournalFlush::Checkpoint));
}

fn install_journal_writes(
    logical_slots: &[u64],
    storage: &mut [Vec<u8>],
    prepared: &JournalPreparedTransaction,
) {
    for operation in prepared.operations() {
        if let JournalCommitOperation::WriteJournal {
            journal_block,
            bytes,
            ..
        } = operation
        {
            let index = logical_slots
                .iter()
                .position(|slot| slot == journal_block)
                .unwrap();
            storage[index] = bytes.clone();
        }
    }
}

#[test]
fn public_transaction_round_trips_and_preserves_order() {
    let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
    let ordered_flush = operations
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

    assert!(ordered_flush < commit_record);
    assert!(commit_record < commit_flush);
    assert!(commit_flush < first_home);
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
fn every_precommit_power_cut_has_no_durable_home_or_replay() {
    let operations = transaction().commit_plan(&JOURNAL_SLOTS).unwrap();
    let commit_flush = operations
        .iter()
        .position(|operation| *operation == JournalCommitOperation::Flush(JournalFlush::Commit))
        .unwrap();

    for cut in 0..=commit_flush {
        let mut pending_journal = BTreeMap::new();
        let mut durable_journal = BTreeMap::new();
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
                    pending_home.insert(image.block_index(), image.bytes().to_vec());
                }
                JournalCommitOperation::Flush(_) => {
                    durable_journal.extend(pending_journal.clone());
                    durable_home.extend(pending_home.clone());
                }
                JournalCommitOperation::WriteOrderedData(_) => {}
                JournalCommitOperation::WriteFilesystemSuperblock { .. } => {}
                JournalCommitOperation::WriteJournalSuperblock { .. } => {}
            }
        }
        assert!(durable_home.is_empty());
        let complete: Option<Vec<&[u8]>> = JOURNAL_SLOTS
            .iter()
            .map(|slot| durable_journal.get(slot).map(Vec::as_slice))
            .collect();
        assert!(complete.is_none());
    }
}

#[test]
fn mapped_ring_power_cuts_preserve_a_recoverable_or_clean_state() {
    let slots = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007];
    let initial_superblock =
        JournalSuperblockImage::new_clean(60, UUID, (slots.len() + 1) as u32).unwrap();
    let mut physical = Vec::from([MAXIMUM_BLOCK]);
    physical.extend_from_slice(&slots);
    let mut ring = initial_superblock
        .map_clean_ring(MAXIMUM_BLOCK, &physical)
        .unwrap();
    let mut transaction = ring.begin_transaction().unwrap();
    transaction.stage_metadata(200, &filled(0x77)).unwrap();
    let prepared = ring.prepare(&transaction).unwrap();
    let mut operations = ring.prepare_commit_plan(&prepared).unwrap();

    let live = match &operations[0] {
        JournalCommitOperation::WriteJournalSuperblock {
            journal_block,
            image,
        } => {
            assert_eq!(*journal_block, MAXIMUM_BLOCK);
            image.clone()
        }
        _ => panic!("mapped commit must begin with a live superblock"),
    };
    assert_eq!(live.sequence(), 60);
    assert_eq!(live.start_block(), 1);
    ring.mark_commit_durable(prepared.ticket()).unwrap();
    let checkpoint = ring.prepare_checkpoint_plan(prepared.ticket()).unwrap();
    let clean = match &checkpoint[0] {
        JournalCommitOperation::WriteJournalSuperblock { image, .. } => image.clone(),
        _ => panic!("checkpoint must finish with a journal-state update"),
    };
    assert_eq!(clean.sequence(), 61);
    assert_eq!(clean.start_block(), 0);
    operations.extend(checkpoint);

    for cut in 0..=operations.len() {
        let mut durable_superblock = initial_superblock.clone();
        let mut pending_superblock = None;
        let mut durable_journal = vec![filled(0); slots.len()];
        let mut pending_journal = BTreeMap::new();
        let mut durable_home = BTreeMap::from([(200, filled(0))]);
        let mut pending_home = BTreeMap::new();

        for operation in &operations[..cut] {
            match operation {
                JournalCommitOperation::WriteJournalSuperblock { image, .. } => {
                    pending_superblock = Some(image.clone());
                }
                JournalCommitOperation::WriteJournal {
                    journal_block,
                    bytes,
                    ..
                } => {
                    pending_journal.insert(*journal_block, bytes.clone());
                }
                JournalCommitOperation::WriteHomeMetadata(image) => {
                    pending_home.insert(image.block_index(), image.bytes().to_vec());
                }
                JournalCommitOperation::Flush(_) => {
                    if let Some(superblock) = pending_superblock.take() {
                        durable_superblock = superblock;
                    }
                    for (block, bytes) in &pending_journal {
                        let index = slots.iter().position(|slot| slot == block).unwrap();
                        durable_journal[index] = bytes.clone();
                    }
                    durable_home.extend(pending_home.clone());
                }
                JournalCommitOperation::WriteOrderedData(_) => {}
                JournalCommitOperation::WriteFilesystemSuperblock { .. } => {}
            }
        }

        if durable_superblock.start_block() == 0 {
            if durable_superblock.sequence() == 60 {
                assert_eq!(durable_home.get(&200), Some(&filled(0)));
            } else {
                assert_eq!(durable_superblock.sequence(), 61);
                assert_eq!(durable_home.get(&200), Some(&filled(0x77)));
            }
            continue;
        }

        let references: Vec<&[u8]> = durable_journal.iter().map(Vec::as_slice).collect();
        let recovery =
            recover_committed_ring(&durable_superblock, true, MAXIMUM_BLOCK, &references).unwrap();
        if recovery.committed_transactions() == 0 {
            assert_eq!(durable_home.get(&200), Some(&filled(0)));
        } else {
            assert_eq!(recovery.committed_transactions(), 1);
            assert_eq!(recovery.replay_images().len(), 1);
            assert_eq!(recovery.replay_images()[0].block_index(), 200);
            assert_eq!(recovery.replay_images()[0].bytes(), filled(0x77));
        }
    }

    ring.checkpoint_durable(prepared.ticket()).unwrap();
    assert_eq!(ring.used_slots(), 0);
    assert_eq!(ring.next_sequence(), 61);
}

#[test]
fn corrupted_descriptor_data_and_commit_are_refused() {
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
}

#[test]
fn revocation_records_are_checksummed_and_suppress_stale_images() {
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
fn clean_ring_wraps_and_refuses_early_reclamation() {
    let slots = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007];
    let mut ring = mapped_ring(40, &slots);
    let mut first = ring.begin_transaction().unwrap();
    first.stage_metadata(100, &filled(1)).unwrap();
    let first = ring.prepare(&first).unwrap();
    assert_eq!(first.journal_blocks(), &slots[..3]);
    assert_eq!(
        ring.checkpoint_durable(first.ticket()),
        Err(JournalTransactionError::ReservationState)
    );
    let _commit = ring.prepare_commit_plan(&first).unwrap();
    ring.mark_commit_durable(first.ticket()).unwrap();
    assert_eq!(
        ring.checkpoint_durable(first.ticket()),
        Err(JournalTransactionError::ReservationState)
    );
    let _checkpoint = ring.prepare_checkpoint_plan(first.ticket()).unwrap();
    ring.checkpoint_durable(first.ticket()).unwrap();

    let mut second = ring.begin_transaction().unwrap();
    second.stage_metadata(101, &filled(2)).unwrap();
    second.stage_metadata(102, &filled(3)).unwrap();
    let second = ring.prepare(&second).unwrap();
    finish_transaction(&mut ring, &second);

    let mut wrapped = ring.begin_transaction().unwrap();
    wrapped.stage_metadata(103, &filled(4)).unwrap();
    let wrapped = ring.prepare(&wrapped).unwrap();
    assert_eq!(wrapped.journal_blocks(), &[3007, 3000, 3001]);
    assert_eq!(ring.used_slots(), 3);
    let aborted_sequence = wrapped.sequence();
    ring.abort_precommit(wrapped.ticket()).unwrap();
    assert_eq!(ring.used_slots(), 0);
    assert_eq!(ring.next_sequence(), aborted_sequence);
}

#[test]
fn checkpoint_tail_advances_only_to_the_next_durable_transaction() {
    let slots = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007];
    let mut ring = mapped_ring(70, &slots);
    let mut first = ring.begin_transaction().unwrap();
    first.stage_metadata(100, &filled(1)).unwrap();
    let first = ring.prepare(&first).unwrap();
    let _first_commit = ring.prepare_commit_plan(&first).unwrap();
    ring.mark_commit_durable(first.ticket()).unwrap();

    let mut second = ring.begin_transaction().unwrap();
    second.stage_metadata(101, &filled(2)).unwrap();
    let second = ring.prepare(&second).unwrap();
    assert_eq!(
        ring.prepare_checkpoint_plan(first.ticket()),
        Err(JournalTransactionError::ReservationOrder)
    );
    let _second_commit = ring.prepare_commit_plan(&second).unwrap();
    ring.mark_commit_durable(second.ticket()).unwrap();

    let first_checkpoint = ring.prepare_checkpoint_plan(first.ticket()).unwrap();
    let advanced = match &first_checkpoint[0] {
        JournalCommitOperation::WriteJournalSuperblock { image, .. } => image,
        _ => panic!("checkpoint must advance the journal tail"),
    };
    assert_eq!(advanced.sequence(), 71);
    assert_eq!(advanced.start_block(), 4);
    ring.checkpoint_durable(first.ticket()).unwrap();

    let second_checkpoint = ring.prepare_checkpoint_plan(second.ticket()).unwrap();
    let clean = match &second_checkpoint[0] {
        JournalCommitOperation::WriteJournalSuperblock { image, .. } => image,
        _ => panic!("final checkpoint must clean the journal"),
    };
    assert_eq!(clean.sequence(), 72);
    assert_eq!(clean.start_block(), 0);
    ring.checkpoint_durable(second.ticket()).unwrap();
}

#[test]
fn superblock_images_admit_only_a_complete_clean_inode_map() {
    let superblock = JournalSuperblockImage::new_clean(70, UUID, 9).unwrap();
    assert_eq!(superblock.maximum_length(), 9);
    assert_eq!(superblock.sequence(), 70);
    assert_eq!(superblock.start_block(), 0);
    assert!(superblock.block_revocations());
    assert_eq!(
        JournalSuperblockImage::from_bytes(superblock.bytes()).unwrap(),
        superblock
    );

    let physical = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007, 3008];
    let mut ring = superblock.map_clean_ring(MAXIMUM_BLOCK, &physical).unwrap();
    let mut transaction = ring.begin_transaction().unwrap();
    transaction.stage_metadata(100, &filled(1)).unwrap();
    let prepared = ring.prepare(&transaction).unwrap();
    assert_eq!(prepared.journal_blocks(), &[3001, 3002, 3003]);
    ring.abort_precommit(prepared.ticket()).unwrap();

    let mut overlaps_superblock = ring.begin_transaction().unwrap();
    overlaps_superblock
        .stage_metadata(physical[0], &filled(2))
        .unwrap();
    assert_eq!(
        ring.prepare(&overlaps_superblock),
        Err(JournalTransactionError::JournalSlotOverlap)
    );

    let dirty = superblock.with_state(70, 1).unwrap();
    assert_eq!(dirty.start_block(), 1);
    assert_eq!(
        dirty.map_clean_ring(MAXIMUM_BLOCK, &physical),
        Err(JournalTransactionError::JournalNotClean)
    );
    let clean = dirty.with_state(71, 0).unwrap();
    assert_eq!(clean.sequence(), 71);
    assert_eq!(clean.start_block(), 0);

    let mut malformed = Vec::from(clean.bytes());
    malformed[0] = 0;
    assert_eq!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::CorruptSuperblockMagic)
    );
    malformed = Vec::from(clean.bytes());
    malformed[7] = 3;
    assert_eq!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::UnsupportedSuperblockType(3))
    );
    malformed = Vec::from(clean.bytes());
    malformed[0x2b] &= !0x10;
    assert_eq!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::MissingSuperblockFeatures(0x10))
    );
    malformed = Vec::from(clean.bytes());
    malformed[0x2b] |= 0x20;
    assert_eq!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::UnsupportedSuperblockFeatures(0x20))
    );
    malformed = Vec::from(clean.bytes());
    malformed[0x50] = 1;
    assert_eq!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::UnsupportedSuperblockChecksumType(
            1
        ))
    );
    malformed = Vec::from(clean.bytes());
    malformed[128] ^= 0x80;
    assert!(matches!(
        JournalSuperblockImage::from_bytes(&malformed),
        Err(JournalTransactionError::CorruptSuperblockChecksum { .. })
    ));
    assert_eq!(
        clean.with_state(71, 9),
        Err(JournalTransactionError::RingGeometry)
    );
    assert_eq!(
        clean.map_clean_ring(MAXIMUM_BLOCK, &physical[..8]),
        Err(JournalTransactionError::RingGeometry)
    );
    let mut hole = physical;
    hole[4] = 0;
    assert_eq!(
        clean.map_clean_ring(MAXIMUM_BLOCK, &hole),
        Err(JournalTransactionError::RingGeometry)
    );
}

#[test]
fn deterministic_ext4_fixture_discovers_its_real_journal_inode_map() {
    let Ok(path) = std::env::var("PHIPIA_EXT4_RUST_FIXTURE") else {
        eprintln!("PHIPIA_EXT4_RUST_FIXTURE is unset; journal-inode integration is CI-only");
        return;
    };
    let Ok(bytes) = std::fs::read(&path) else {
        eprintln!("journal-inode fixture was not produced because e2fsprogs is unavailable");
        return;
    };
    let filesystem = Ext4::load(Box::new(bytes.clone())).unwrap();
    let journal = load_journal_inode_map(&filesystem).unwrap();
    assert_eq!(journal.superblock().start_block(), 0);
    assert!(!journal.filesystem_needs_recovery());
    let clean_filesystem_superblock = journal.filesystem_superblock().clone();
    assert!(!clean_filesystem_superblock.needs_recovery());
    let recovery_marker = clean_filesystem_superblock.with_recovery_state(true);
    assert!(recovery_marker.needs_recovery());
    assert_eq!(
        u32::from_le_bytes(recovery_marker.bytes()[0x60..0x64].try_into().unwrap()) & 0x4,
        0x4
    );
    assert_eq!(
        u32::from_le_bytes(recovery_marker.bytes()[0x3fc..0x400].try_into().unwrap()),
        ext4_crc32c(&recovery_marker.bytes()[..0x3fc])
    );
    for index in 0..recovery_marker.bytes().len() {
        if !(0x60..0x64).contains(&index) && !(0x3fc..0x400).contains(&index) {
            assert_eq!(
                recovery_marker.bytes()[index],
                clean_filesystem_superblock.bytes()[index]
            );
        }
    }
    assert_eq!(
        recovery_marker.with_recovery_state(false),
        clean_filesystem_superblock
    );
    assert_eq!(
        usize::try_from(journal.superblock().maximum_length()).unwrap(),
        journal.physical_blocks().len()
    );
    assert!(journal.physical_blocks().len() >= 4);
    assert!(journal.physical_blocks().iter().all(|block| *block != 0));

    let empty_ring = vec![filled(0); journal.physical_blocks().len() - 1];
    let empty_references: Vec<&[u8]> = empty_ring.iter().map(Vec::as_slice).collect();
    let marker_only = recover_committed_ring(
        journal.superblock(),
        true,
        journal.maximum_block(),
        &empty_references,
    )
    .unwrap();
    assert_eq!(marker_only.committed_transactions(), 0);
    assert_eq!(marker_only.consumed_slots(), 0);
    assert!(marker_only.replay_images().is_empty());
    assert_eq!(
        marker_only.checkpoint_plan(0, &recovery_marker),
        Err(JournalTransactionError::BlockOutOfRange)
    );
    assert_eq!(
        marker_only.checkpoint_plan(journal.physical_blocks()[0], &clean_filesystem_superblock),
        Err(JournalTransactionError::RecoveryStateMismatch)
    );
    let cleanup = marker_only
        .checkpoint_plan(journal.physical_blocks()[0], &recovery_marker)
        .unwrap();
    assert_eq!(cleanup.len(), 5);
    assert_eq!(cleanup[0], JournalCommitOperation::Flush(JournalFlush::Checkpoint));
    assert!(matches!(
        &cleanup[1],
        JournalCommitOperation::WriteJournalSuperblock { journal_block, image }
            if *journal_block == journal.physical_blocks()[0]
                && image == journal.superblock()
    ));
    assert_eq!(cleanup[2], JournalCommitOperation::Flush(JournalFlush::JournalState));
    assert!(matches!(
        &cleanup[3],
        JournalCommitOperation::WriteFilesystemSuperblock { start_byte, image }
            if *start_byte == FILESYSTEM_SUPERBLOCK_START_BYTE
                && image == &clean_filesystem_superblock
    ));
    assert_eq!(cleanup[4], JournalCommitOperation::Flush(JournalFlush::FilesystemState));

    let physical = Vec::from(journal.physical_blocks());
    let mut ring = journal.into_clean_ring().unwrap();
    assert_eq!(ring.filesystem_is_clean(), Ok(true));
    assert_eq!(ring.filesystem_recovery_marker_is_durable(), Ok(false));
    let marker_plan = ring.prepare_recovery_marker_plan().unwrap();
    assert_eq!(ring.prepare_recovery_marker_plan().unwrap(), marker_plan);
    assert_eq!(ring.filesystem_recovery_marker_is_durable(), Ok(false));
    assert_eq!(ring.filesystem_is_clean(), Ok(false));
    assert!(matches!(
        &marker_plan[0],
        JournalCommitOperation::WriteFilesystemSuperblock { start_byte, image }
            if *start_byte == FILESYSTEM_SUPERBLOCK_START_BYTE
                && image == &recovery_marker
    ));
    assert_eq!(
        marker_plan[1],
        JournalCommitOperation::Flush(JournalFlush::FilesystemState)
    );
    let home_block = (1..MAXIMUM_BLOCK)
        .find(|block| !physical.contains(block))
        .unwrap();
    let mut transaction = ring.begin_transaction().unwrap();
    transaction
        .stage_metadata(home_block, &filled(0x44))
        .unwrap();
    let prepared = ring.prepare(&transaction).unwrap();
    assert_eq!(prepared.journal_blocks(), &physical[1..4]);
    assert_eq!(
        ring.prepare_commit_plan(&prepared),
        Err(JournalTransactionError::RecoveryMarkerNotDurable)
    );
    ring.mark_recovery_marker_durable().unwrap();
    assert_eq!(ring.filesystem_recovery_marker_is_durable(), Ok(true));
    assert_eq!(ring.filesystem_is_clean(), Ok(false));
    assert!(ring.prepare_commit_plan(&prepared).is_ok());
    assert_eq!(
        ring.prepare_filesystem_clean_plan(),
        Err(JournalTransactionError::JournalNotClean)
    );
    ring.mark_commit_durable(prepared.ticket()).unwrap();
    let checkpoint = ring.prepare_checkpoint_plan(prepared.ticket()).unwrap();
    assert!(matches!(
        &checkpoint[0],
        JournalCommitOperation::WriteJournalSuperblock { image, .. }
            if image.start_block() == 0
    ));
    assert_eq!(
        checkpoint[1],
        JournalCommitOperation::Flush(JournalFlush::JournalState)
    );
    ring.checkpoint_durable(prepared.ticket()).unwrap();
    assert_eq!(ring.filesystem_is_clean(), Ok(false));
    let clean_plan = ring.prepare_filesystem_clean_plan().unwrap();
    assert_eq!(ring.filesystem_recovery_marker_is_durable(), Ok(true));
    assert_eq!(ring.prepare_filesystem_clean_plan().unwrap(), clean_plan);
    assert!(matches!(
        &clean_plan[0],
        JournalCommitOperation::WriteFilesystemSuperblock { start_byte, image }
            if *start_byte == FILESYSTEM_SUPERBLOCK_START_BYTE
                && image == &clean_filesystem_superblock
    ));
    assert_eq!(
        clean_plan[1],
        JournalCommitOperation::Flush(JournalFlush::FilesystemState)
    );
    ring.mark_filesystem_clean_durable().unwrap();
    assert_eq!(ring.filesystem_recovery_marker_is_durable(), Ok(false));
    assert_eq!(ring.filesystem_is_clean(), Ok(true));

    let mut next = ring.begin_transaction().unwrap();
    next.stage_metadata(home_block, &filled(0x55)).unwrap();
    let next = ring.prepare(&next).unwrap();
    assert_eq!(ring.filesystem_is_clean(), Ok(false));
    assert_eq!(
        ring.prepare_commit_plan(&next),
        Err(JournalTransactionError::RecoveryMarkerNotDurable)
    );
    let marker_plan = ring.prepare_recovery_marker_plan().unwrap();
    assert!(matches!(
        &marker_plan[0],
        JournalCommitOperation::WriteFilesystemSuperblock { image, .. }
            if image == &recovery_marker
    ));
    ring.mark_recovery_marker_durable().unwrap();
    assert!(ring.prepare_commit_plan(&next).is_ok());

    let mut dirty_bytes = bytes;
    let superblock_start = usize::try_from(FILESYSTEM_SUPERBLOCK_START_BYTE).unwrap();
    let superblock_end = superblock_start
        .checked_add(recovery_marker.bytes().len())
        .unwrap();
    dirty_bytes[superblock_start..superblock_end].copy_from_slice(recovery_marker.bytes());
    let dirty_filesystem = Ext4::load(Box::new(dirty_bytes.clone())).unwrap();
    let dirty_journal = load_journal_inode_map(&dirty_filesystem).unwrap();
    assert!(dirty_journal.filesystem_needs_recovery());
    let mut journal_bytes = Vec::new();
    for block in &dirty_journal.physical_blocks()[1..] {
        let start = usize::try_from(*block).unwrap() * JOURNAL_BLOCK_BYTES;
        journal_bytes.push(dirty_bytes[start..start + JOURNAL_BLOCK_BYTES].to_vec());
    }
    let journal_references: Vec<&[u8]> = journal_bytes.iter().map(Vec::as_slice).collect();
    let recovery = recover_committed_ring(
        dirty_journal.superblock(),
        true,
        dirty_journal.maximum_block(),
        &journal_references,
    )
    .unwrap();
    let recovery_plan = recovery
        .checkpoint_plan(
            dirty_journal.physical_blocks()[0],
            dirty_journal.filesystem_superblock(),
        )
        .unwrap();
    let mut storage = VectorStorage {
        bytes: dirty_bytes,
        flushes: Vec::new(),
    };
    execute_commit_operations(&mut storage, &recovery_plan).unwrap();
    assert_eq!(
        storage.flushes,
        vec![
            JournalFlush::Checkpoint,
            JournalFlush::JournalState,
            JournalFlush::FilesystemState,
        ]
    );
    let recovered_filesystem = Ext4::load(Box::new(storage.bytes)).unwrap();
    let recovered_journal = load_journal_inode_map(&recovered_filesystem).unwrap();
    assert!(!recovered_journal.filesystem_needs_recovery());
    assert_eq!(recovered_journal.superblock().start_block(), 0);
    recovered_journal.into_clean_ring().unwrap();

    let backing = Rc::new(std::fs::read(&path).unwrap());
    let stage = Rc::new(
        JournalMutationStage::new(
            Box::new(backing.clone()),
            u64::try_from(backing.len()).unwrap(),
        )
        .unwrap(),
    );
    let staged_filesystem = Ext4::load_with_writer(
        Box::new(stage.clone()),
        Some(Box::new(stage.clone())),
    )
    .unwrap();
    let mut file = staged_filesystem.open(b"/system/README.TXT").unwrap();
    let mut original = [0u8; 1];
    assert_eq!(file.read_bytes_at(&mut original, 0).unwrap(), 1);
    let data_block = file.filesystem_block_at_offset(0).unwrap().unwrap();
    assert_eq!(file.write_bytes_at(b"X", 0).unwrap(), 1);
    let mut overlaid = [0u8; 1];
    assert_eq!(file.read_bytes_at(&mut overlaid, 0).unwrap(), 1);
    assert_eq!(overlaid, *b"X");
    assert_ne!(original, overlaid);
    assert!(stage.staged_block_count() >= 1);
    assert!(
        stage
            .staged_images()
            .iter()
            .any(|image| image.block_index() == data_block)
    );
    assert_eq!(std::fs::read(&path).unwrap(), backing.as_slice());
    drop(file);
    drop(staged_filesystem);
    stage.rollback();
    let restored_filesystem = Ext4::load(Box::new(stage.clone())).unwrap();
    let mut restored = restored_filesystem
        .open(b"/system/README.TXT")
        .unwrap();
    assert_eq!(restored.read_bytes_at(&mut overlaid, 0).unwrap(), 1);
    assert_eq!(overlaid, original);
    drop(restored);
    drop(restored_filesystem);

    let commit_bytes = std::fs::read(&path).unwrap();
    let commit_filesystem = Ext4::load(Box::new(commit_bytes.clone())).unwrap();
    let commit_journal = load_journal_inode_map(&commit_filesystem).unwrap();
    let commit_physical = Vec::from(commit_journal.physical_blocks());
    let mut commit_ring = commit_journal.into_clean_ring().unwrap();
    let marker_plan = commit_ring.prepare_recovery_marker_plan().unwrap();
    let mut commit_storage = VectorStorage {
        bytes: commit_bytes,
        flushes: Vec::new(),
    };
    execute_commit_operations(&mut commit_storage, &marker_plan).unwrap();
    commit_ring.mark_recovery_marker_durable().unwrap();

    let marker_backing = Rc::new(commit_storage.bytes.clone());
    let refused_stage = Rc::new(
        JournalMutationStage::new(
            Box::new(marker_backing.clone()),
            u64::try_from(marker_backing.len()).unwrap(),
        )
        .unwrap(),
    );
    let refused_filesystem = Ext4::load_with_writer(
        Box::new(refused_stage.clone()),
        Some(Box::new(refused_stage.clone())),
    )
    .unwrap();
    let mut refused_file = refused_filesystem.open(b"/system/README.TXT").unwrap();
    let original_size = refused_file.inode().size_in_bytes();
    let append_offset = original_size
        .div_ceil(JOURNAL_BLOCK_BYTES as u64)
        .checked_mul(JOURNAL_BLOCK_BYTES as u64)
        .unwrap();
    assert!(matches!(
        refused_file.write_bytes_at(b"X", 0),
        Err(Ext4Error::Readonly)
    ));
    assert_eq!(refused_stage.staged_block_count(), 0);
    drop(refused_file);
    drop(refused_filesystem);

    let rollback_stage = Rc::new(
        JournalMutationStage::new(
            Box::new(marker_backing.clone()),
            u64::try_from(marker_backing.len()).unwrap(),
        )
        .unwrap(),
    );
    let rollback_filesystem = Ext4::load_with_recovery_writer(
        Box::new(rollback_stage.clone()),
        Some(Box::new(rollback_stage.clone())),
    )
    .unwrap();
    let mut rollback_file = rollback_filesystem
        .open(b"/system/README.TXT")
        .unwrap();
    assert_eq!(
        rollback_file
            .write_bytes_at(b"R", append_offset)
            .unwrap(),
        1
    );
    assert_eq!(rollback_file.inode().size_in_bytes(), append_offset + 1);
    assert!(
        rollback_stage
            .staged_images()
            .iter()
            .any(|image| image.block_index() == 0)
    );
    let rollback_transaction = commit_ring.begin_transaction().unwrap();
    assert_eq!(
        rollback_stage
            .build_transaction(&rollback_transaction, &[u64::MAX])
            .unwrap_err(),
        JournalMutationPlanError::OrderedDataNotStaged
    );
    assert!(!rollback_stage.is_sealed());
    drop(rollback_file);
    drop(rollback_filesystem);
    rollback_stage.rollback();
    assert_eq!(rollback_stage.staged_block_count(), 0);
    let restored_filesystem = Ext4::load_with_recovery_writer(
        Box::new(rollback_stage.clone()),
        Some(Box::new(rollback_stage.clone())),
    )
    .unwrap();
    let restored_journal = load_journal_inode_map(&restored_filesystem).unwrap();
    assert!(restored_journal.filesystem_needs_recovery());
    assert_eq!(restored_journal.filesystem_superblock(), &recovery_marker);
    let mut restored_file = restored_filesystem
        .open(b"/system/README.TXT")
        .unwrap();
    let mut rolled_back_byte = [0u8; 1];
    assert_eq!(restored_file.inode().size_in_bytes(), original_size);
    assert_eq!(
        restored_file
            .read_bytes_at(&mut rolled_back_byte, append_offset)
            .unwrap(),
        0
    );
    drop(restored_file);
    drop(restored_filesystem);

    let commit_stage = Rc::new(
        JournalMutationStage::new(
            Box::new(marker_backing.clone()),
            u64::try_from(marker_backing.len()).unwrap(),
        )
        .unwrap(),
    );
    let mutating_filesystem = Ext4::load_with_recovery_writer(
        Box::new(commit_stage.clone()),
        Some(Box::new(commit_stage.clone())),
    )
    .unwrap();
    assert!(
        load_journal_inode_map(&mutating_filesystem)
            .unwrap()
            .filesystem_needs_recovery()
    );
    let mut mutating_file = mutating_filesystem.open(b"/system/README.TXT").unwrap();
    assert_eq!(mutating_file.inode().size_in_bytes(), original_size);
    assert_eq!(mutating_file.write_bytes_at(b"X", append_offset).unwrap(), 1);
    let appended_data_block = mutating_file
        .filesystem_block_at_offset(append_offset)
        .unwrap()
        .unwrap();
    let staged_superblock = commit_stage
        .staged_images()
        .into_iter()
        .find(|image| image.block_index() == 0)
        .expect("allocation must stage the 4 KiB block containing the primary superblock");
    let superblock = &staged_superblock.bytes()[1024..2048];
    assert_eq!(
        u32::from_le_bytes(superblock[0x60..0x64].try_into().unwrap()) & 0x4,
        0x4
    );
    assert_eq!(
        u32::from_le_bytes(superblock[0x3fc..0x400].try_into().unwrap()),
        ext4_crc32c(&superblock[..0x3fc])
    );

    let transaction = commit_stage
        .build_transaction(
            &commit_ring.begin_transaction().unwrap(),
            &[appended_data_block],
        )
        .unwrap();
    let prepared = commit_ring.prepare(&transaction).unwrap();
    let checkpointed_superblock = commit_ring
        .admit_checkpointed_filesystem_superblock(&prepared, &staged_superblock)
        .unwrap();
    let commit_plan = commit_ring.prepare_commit_plan(&prepared).unwrap();
    assert_eq!(
        commit_ring.admit_checkpointed_filesystem_superblock(&prepared, &staged_superblock),
        Err(JournalTransactionError::RingTransactionMismatch)
    );
    execute_commit_operations(&mut commit_storage, &commit_plan).unwrap();

    // Model a reset after the commit record is durable but before any home
    // metadata is checkpointed. Recovery must replay the allocation counters
    // from journaled block zero and derive the final clean marker from that
    // image, not from the stale superblock admitted at mount.
    let mut crash_storage = VectorStorage {
        bytes: commit_storage.bytes.clone(),
        flushes: Vec::new(),
    };
    let crashed_filesystem = Ext4::load(Box::new(crash_storage.bytes.clone())).unwrap();
    let crashed_journal = load_journal_inode_map(&crashed_filesystem).unwrap();
    assert!(crashed_journal.filesystem_needs_recovery());
    assert_eq!(crashed_journal.physical_blocks(), commit_physical);
    let crashed_journal_bytes: Vec<Vec<u8>> = crashed_journal.physical_blocks()[1..]
        .iter()
        .map(|block| {
            let start = usize::try_from(*block).unwrap() * JOURNAL_BLOCK_BYTES;
            crash_storage.bytes[start..start + JOURNAL_BLOCK_BYTES].to_vec()
        })
        .collect();
    let crashed_journal_references: Vec<&[u8]> = crashed_journal_bytes
        .iter()
        .map(Vec::as_slice)
        .collect();
    let crashed_recovery = recover_committed_ring(
        crashed_journal.superblock(),
        true,
        crashed_journal.maximum_block(),
        &crashed_journal_references,
    )
    .unwrap();
    assert_eq!(crashed_recovery.committed_transactions(), 1);
    assert!(
        crashed_recovery
            .replay_images()
            .iter()
            .any(|image| image.block_index() == 0)
    );
    let crashed_recovery_plan = crashed_recovery
        .checkpoint_plan(
            crashed_journal.physical_blocks()[0],
            crashed_journal.filesystem_superblock(),
        )
        .unwrap();
    let recovered_clean_superblock = crashed_recovery_plan
        .iter()
        .find_map(|operation| match operation {
            JournalCommitOperation::WriteFilesystemSuperblock { image, .. } => Some(image),
            _ => None,
        })
        .unwrap();
    assert!(!recovered_clean_superblock.needs_recovery());
    let staged_checkpointed_superblock =
        &staged_superblock.bytes()[1024..1024 + recovered_clean_superblock.bytes().len()];
    for (index, (recovered, staged)) in recovered_clean_superblock
        .bytes()
        .iter()
        .zip(staged_checkpointed_superblock.iter())
        .enumerate()
    {
        if !(0x60..0x64).contains(&index) && !(0x3fc..0x400).contains(&index) {
            assert_eq!(recovered, staged);
        }
    }
    assert_eq!(
        u32::from_le_bytes(
            recovered_clean_superblock.bytes()[0x3fc..0x400]
                .try_into()
                .unwrap(),
        ),
        ext4_crc32c(&recovered_clean_superblock.bytes()[..0x3fc])
    );
    execute_commit_operations(&mut crash_storage, &crashed_recovery_plan).unwrap();
    assert_eq!(
        crash_storage.flushes,
        vec![
            JournalFlush::Checkpoint,
            JournalFlush::JournalState,
            JournalFlush::FilesystemState,
        ]
    );
    let recovered_filesystem = Ext4::load(Box::new(crash_storage.bytes.clone())).unwrap();
    let recovered_journal = load_journal_inode_map(&recovered_filesystem).unwrap();
    assert!(!recovered_journal.filesystem_needs_recovery());
    recovered_journal.into_clean_ring().unwrap();
    let mut recovered_file = recovered_filesystem.open(b"/system/README.TXT").unwrap();
    let mut recovered_append = [0u8; 1];
    assert_eq!(
        recovered_file
            .read_bytes_at(&mut recovered_append, append_offset)
            .unwrap(),
        1
    );
    assert_eq!(recovered_append, *b"X");

    let recovered_path = std::path::Path::new(&path).with_extension("recovered.img");
    std::fs::write(&recovered_path, &crash_storage.bytes).unwrap();
    let recovered_fsck = std::process::Command::new("e2fsck")
        .args(["-fn"])
        .arg(&recovered_path)
        .output()
        .unwrap();
    let _ = std::fs::remove_file(&recovered_path);
    assert!(
        recovered_fsck.status.success(),
        "e2fsck rejected recovered image:\n{}\n{}",
        String::from_utf8_lossy(&recovered_fsck.stdout),
        String::from_utf8_lossy(&recovered_fsck.stderr)
    );

    commit_ring.mark_commit_durable(prepared.ticket()).unwrap();
    let checkpoint_plan = commit_ring
        .prepare_checkpoint_plan(prepared.ticket())
        .unwrap();
    assert_eq!(
        commit_ring
            .prepare_checkpoint_plan(prepared.ticket())
            .unwrap(),
        checkpoint_plan
    );
    execute_commit_operations(&mut commit_storage, &checkpoint_plan).unwrap();
    assert_eq!(
        commit_ring.checkpoint_durable(prepared.ticket()),
        Err(JournalTransactionError::RecoveryStateMismatch)
    );
    commit_ring
        .checkpoint_durable_with_filesystem_superblock(&checkpointed_superblock)
        .unwrap();
    let clean_plan = commit_ring.prepare_filesystem_clean_plan().unwrap();
    execute_commit_operations(&mut commit_storage, &clean_plan).unwrap();
    commit_ring.mark_filesystem_clean_durable().unwrap();
    assert_eq!(commit_ring.filesystem_is_clean(), Ok(true));
    assert_eq!(
        commit_storage.flushes,
        vec![
            JournalFlush::FilesystemState,
            JournalFlush::OrderedData,
            JournalFlush::JournalPayload,
            JournalFlush::Commit,
            JournalFlush::Checkpoint,
            JournalFlush::JournalState,
            JournalFlush::FilesystemState,
        ]
    );

    let final_filesystem = Ext4::load(Box::new(commit_storage.bytes.clone())).unwrap();
    let final_journal = load_journal_inode_map(&final_filesystem).unwrap();
    assert!(!final_journal.filesystem_needs_recovery());
    assert_eq!(final_journal.superblock().start_block(), 0);
    final_journal.into_clean_ring().unwrap();
    let mut final_file = final_filesystem.open(b"/system/README.TXT").unwrap();
    let mut appended = [0u8; 1];
    assert_eq!(
        final_file
            .read_bytes_at(&mut appended, append_offset)
            .unwrap(),
        1
    );
    assert_eq!(appended, *b"X");
    assert_eq!(final_file.inode().size_in_bytes(), append_offset + 1);

    let final_superblock = &commit_storage.bytes[1024..2048];
    let initial_free_blocks = u64::from(u32::from_le_bytes(
        marker_backing[1024 + 0x0c..1024 + 0x10]
            .try_into()
            .unwrap(),
    )) | (u64::from(u32::from_le_bytes(
        marker_backing[1024 + 0x158..1024 + 0x15c]
            .try_into()
            .unwrap(),
    )) << 32);
    let final_free_blocks = u64::from(u32::from_le_bytes(
        final_superblock[0x0c..0x10].try_into().unwrap(),
    )) | (u64::from(u32::from_le_bytes(
        final_superblock[0x158..0x15c].try_into().unwrap(),
    )) << 32);
    assert_eq!(final_free_blocks + 1, initial_free_blocks);

    let descriptor = &commit_storage.bytes[JOURNAL_BLOCK_BYTES..JOURNAL_BLOCK_BYTES + 64];
    let bitmap_block = u64::from(u32::from_le_bytes(descriptor[0..4].try_into().unwrap()))
        | (u64::from(u32::from_le_bytes(descriptor[0x20..0x24].try_into().unwrap())) << 32);
    let bitmap_start = usize::try_from(bitmap_block).unwrap() * JOURNAL_BLOCK_BYTES;
    let blocks_per_group =
        usize::try_from(u32::from_le_bytes(final_superblock[0x20..0x24].try_into().unwrap()))
            .unwrap();
    let seed = u32::from_le_bytes(final_superblock[0x270..0x274].try_into().unwrap());
    let calculated_bitmap_checksum = ext4_crc32c_with_seed(
        &commit_storage.bytes[bitmap_start..bitmap_start + blocks_per_group / 8],
        seed,
    );
    let stored_bitmap_checksum =
        u32::from(u16::from_le_bytes(descriptor[0x18..0x1a].try_into().unwrap()))
            | (u32::from(u16::from_le_bytes(
                descriptor[0x38..0x3a].try_into().unwrap(),
            )) << 16);
    assert_eq!(stored_bitmap_checksum, calculated_bitmap_checksum);

    let committed_path = std::path::Path::new(&path).with_extension("committed.img");
    std::fs::write(&committed_path, &commit_storage.bytes).unwrap();
    let fsck = std::process::Command::new("e2fsck")
        .args(["-fn"])
        .arg(&committed_path)
        .output()
        .unwrap();
    let _ = std::fs::remove_file(&committed_path);
    assert!(
        fsck.status.success(),
        "e2fsck rejected committed image:\n{}\n{}",
        String::from_utf8_lossy(&fsck.stdout),
        String::from_utf8_lossy(&fsck.stderr)
    );

    drop(final_file);
    drop(final_filesystem);
    let truncate_filesystem = Ext4::load(Box::new(commit_storage.bytes.clone())).unwrap();
    let truncate_journal = load_journal_inode_map(&truncate_filesystem).unwrap();
    let mut truncate_ring = truncate_journal.into_clean_ring().unwrap();
    let truncate_marker_plan = truncate_ring.prepare_recovery_marker_plan().unwrap();
    let mut truncate_storage = VectorStorage {
        bytes: commit_storage.bytes.clone(),
        flushes: Vec::new(),
    };
    execute_commit_operations(&mut truncate_storage, &truncate_marker_plan).unwrap();
    truncate_ring.mark_recovery_marker_durable().unwrap();
    let truncate_backing = Rc::new(truncate_storage.bytes.clone());
    let truncate_stage = Rc::new(
        JournalMutationStage::new(
            Box::new(truncate_backing.clone()),
            u64::try_from(truncate_backing.len()).unwrap(),
        )
        .unwrap(),
    );
    let truncate_mutation = Ext4::load_with_recovery_writer(
        Box::new(truncate_stage.clone()),
        Some(Box::new(truncate_stage.clone())),
    )
    .unwrap();
    let mut truncate_file = truncate_mutation.open(b"/system/README.TXT").unwrap();
    truncate_file.truncate(original_size).unwrap();
    assert_eq!(truncate_file.inode().size_in_bytes(), original_size);
    assert_eq!(truncate_stage.revoked_block_count(), 1);
    drop(truncate_file);
    drop(truncate_mutation);
    let truncate_superblock = truncate_stage
        .staged_images()
        .into_iter()
        .find(|image| image.block_index() == 0)
        .expect("freeing the appended block must stage allocation counters");
    let truncate_transaction = truncate_stage
        .build_transaction(&truncate_ring.begin_transaction().unwrap(), &[])
        .unwrap();
    assert!(truncate_transaction.revokes_block(appended_data_block));
    let truncate_prepared = truncate_ring.prepare(&truncate_transaction).unwrap();
    let truncate_checkpointed_superblock = truncate_ring
        .admit_checkpointed_filesystem_superblock(
            &truncate_prepared,
            &truncate_superblock,
        )
        .unwrap();
    let truncate_commit_plan = truncate_ring
        .prepare_commit_plan(&truncate_prepared)
        .unwrap();
    execute_commit_operations(&mut truncate_storage, &truncate_commit_plan).unwrap();
    truncate_ring
        .mark_commit_durable(truncate_prepared.ticket())
        .unwrap();
    let truncate_checkpoint_plan = truncate_ring
        .prepare_checkpoint_plan(truncate_prepared.ticket())
        .unwrap();
    execute_commit_operations(&mut truncate_storage, &truncate_checkpoint_plan).unwrap();
    truncate_ring
        .checkpoint_durable_with_filesystem_superblock(
            &truncate_checkpointed_superblock,
        )
        .unwrap();
    let truncate_clean_plan = truncate_ring.prepare_filesystem_clean_plan().unwrap();
    execute_commit_operations(&mut truncate_storage, &truncate_clean_plan).unwrap();
    truncate_ring.mark_filesystem_clean_durable().unwrap();

    let truncated_filesystem = Ext4::load(Box::new(truncate_storage.bytes.clone())).unwrap();
    let truncated_journal = load_journal_inode_map(&truncated_filesystem).unwrap();
    assert!(!truncated_journal.filesystem_needs_recovery());
    truncated_journal.into_clean_ring().unwrap();
    let mut truncated_file = truncated_filesystem
        .open(b"/system/README.TXT")
        .unwrap();
    let mut removed_append = [0u8; 1];
    assert_eq!(truncated_file.inode().size_in_bytes(), original_size);
    assert_eq!(
        truncated_file
            .read_bytes_at(&mut removed_append, append_offset)
            .unwrap(),
        0
    );
    let truncated_superblock = &truncate_storage.bytes[1024..2048];
    let truncated_free_blocks = u64::from(u32::from_le_bytes(
        truncated_superblock[0x0c..0x10].try_into().unwrap(),
    )) | (u64::from(u32::from_le_bytes(
        truncated_superblock[0x158..0x15c].try_into().unwrap(),
    )) << 32);
    assert_eq!(truncated_free_blocks, initial_free_blocks);

    let truncated_path = std::path::Path::new(&path).with_extension("truncated.img");
    std::fs::write(&truncated_path, &truncate_storage.bytes).unwrap();
    let truncated_fsck = std::process::Command::new("e2fsck")
        .args(["-fn"])
        .arg(&truncated_path)
        .output()
        .unwrap();
    let _ = std::fs::remove_file(&truncated_path);
    assert!(
        truncated_fsck.status.success(),
        "e2fsck rejected truncated image:\n{}\n{}",
        String::from_utf8_lossy(&truncated_fsck.stdout),
        String::from_utf8_lossy(&truncated_fsck.stderr)
    );
}

#[test]
fn dirty_ring_recovery_wraps_replays_revokes_and_discards_an_uncommitted_tail() {
    let physical = [3000, 3001, 3002, 3003, 3004, 3005, 3006, 3007, 3008];
    let logical_slots = &physical[1..];
    let superblock = JournalSuperblockImage::new_clean(80, UUID, 9).unwrap();
    let mut ring = superblock.map_clean_ring(MAXIMUM_BLOCK, &physical).unwrap();

    let mut advance_three = ring.begin_transaction().unwrap();
    advance_three.stage_metadata(10, &filled(1)).unwrap();
    let advance_three = ring.prepare(&advance_three).unwrap();
    finish_transaction(&mut ring, &advance_three);

    let mut advance_four = ring.begin_transaction().unwrap();
    advance_four.stage_metadata(11, &filled(2)).unwrap();
    advance_four.stage_metadata(12, &filled(3)).unwrap();
    let advance_four = ring.prepare(&advance_four).unwrap();
    finish_transaction(&mut ring, &advance_four);

    let mut first_live = ring.begin_transaction().unwrap();
    first_live.stage_metadata(100, &filled(0x44)).unwrap();
    let first_live = ring.prepare(&first_live).unwrap();
    assert_eq!(first_live.journal_blocks(), &[3008, 3001, 3002]);
    let _first_commit = ring.prepare_commit_plan(&first_live).unwrap();
    ring.mark_commit_durable(first_live.ticket()).unwrap();

    let mut second_live = ring.begin_transaction().unwrap();
    second_live.stage_metadata(101, &filled(0x55)).unwrap();
    second_live.stage_revocation(100).unwrap();
    let second_live = ring.prepare(&second_live).unwrap();
    assert_eq!(second_live.journal_blocks(), &[3003, 3004, 3005, 3006]);
    let _second_commit = ring.prepare_commit_plan(&second_live).unwrap();
    ring.mark_commit_durable(second_live.ticket()).unwrap();

    let mut storage = vec![filled(0); logical_slots.len()];
    install_journal_writes(logical_slots, &mut storage, &first_live);
    install_journal_writes(logical_slots, &mut storage, &second_live);
    let dirty = superblock.with_state(82, 8).unwrap();
    let references: Vec<&[u8]> = storage.iter().map(Vec::as_slice).collect();
    let recovered = recover_committed_ring(&dirty, true, MAXIMUM_BLOCK, &references).unwrap();
    assert_eq!(recovered.committed_transactions(), 2);
    assert_eq!(recovered.consumed_slots(), 7);
    assert_eq!(recovered.clean_superblock().sequence(), 84);
    assert_eq!(recovered.clean_superblock().start_block(), 0);
    assert_eq!(recovered.replay_images().len(), 1);
    assert_eq!(recovered.replay_images()[0].block_index(), 101);
    assert_eq!(recovered.replay_images()[0].bytes(), filled(0x55));

    let mut uncommitted = JournalTransaction::new(84, UUID, MAXIMUM_BLOCK).unwrap();
    uncommitted.stage_metadata(102, &filled(0x66)).unwrap();
    let uncommitted = uncommitted
        .commit_plan(&[logical_slots[6], logical_slots[7], logical_slots[0]])
        .unwrap();
    let descriptor = uncommitted
        .iter()
        .find_map(|operation| match operation {
            JournalCommitOperation::WriteJournal {
                kind: JournalRecordKind::Descriptor,
                bytes,
                ..
            } => Some(bytes.clone()),
            _ => None,
        })
        .unwrap();
    storage[6] = descriptor;
    let references: Vec<&[u8]> = storage.iter().map(Vec::as_slice).collect();
    let recovered = recover_committed_ring(&dirty, true, MAXIMUM_BLOCK, &references).unwrap();
    assert_eq!(recovered.committed_transactions(), 2);
    assert_eq!(recovered.consumed_slots(), 7);

    storage[5][128] ^= 0x80;
    let references: Vec<&[u8]> = storage.iter().map(Vec::as_slice).collect();
    assert_eq!(
        recover_committed_ring(&dirty, true, MAXIMUM_BLOCK, &references),
        Err(JournalTransactionError::CorruptCommit)
    );

    let clean = superblock.with_state(84, 0).unwrap();
    let marker_only =
        recover_committed_ring(&clean, true, MAXIMUM_BLOCK, &references).unwrap();
    assert_eq!(marker_only.committed_transactions(), 0);
    assert_eq!(marker_only.consumed_slots(), 0);
    assert!(marker_only.replay_images().is_empty());
    assert_eq!(marker_only.clean_superblock(), &clean);
    assert_eq!(
        recover_committed_ring(&dirty, false, MAXIMUM_BLOCK, &references),
        Err(JournalTransactionError::RecoveryStateMismatch)
    );
}

#[test]
fn hostile_geometry_duplicates_escape_and_slot_overlap_are_refused() {
    let mut transaction = JournalTransaction::new(9, UUID, MAXIMUM_BLOCK).unwrap();
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
    escaped[..4].copy_from_slice(&0xc03b_3998_u32.to_be_bytes());
    assert_eq!(
        transaction.stage_metadata(3, &escaped),
        Err(JournalTransactionError::EscapedBlockUnsupported)
    );
    assert_eq!(
        transaction.commit_plan(&[2, 101, 102]),
        Err(JournalTransactionError::JournalSlotOverlap)
    );
    transaction.stage_revocation(4).unwrap();
    let mut ring =
        JournalRing::new_clean(9, UUID, MAXIMUM_BLOCK, false, &[100, 101, 102, 103]).unwrap();
    assert_eq!(
        ring.prepare(&transaction),
        Err(JournalTransactionError::RevocationsUnsupported)
    );
    assert_eq!(
        JournalRing::new_clean(9, UUID, MAXIMUM_BLOCK, true, &[0, 101, 102]),
        Err(JournalTransactionError::RingGeometry)
    );

    let mut unmapped =
        JournalRing::new_clean(20, UUID, MAXIMUM_BLOCK, true, &[100, 101, 102]).unwrap();
    let mut transaction = unmapped.begin_transaction().unwrap();
    transaction.stage_metadata(2, &filled(2)).unwrap();
    let prepared = unmapped.prepare(&transaction).unwrap();
    assert_eq!(
        unmapped.prepare_commit_plan(&prepared),
        Err(JournalTransactionError::JournalStateUnavailable)
    );

    let mut exhausted =
        JournalRing::new_clean(u32::MAX - 1, UUID, MAXIMUM_BLOCK, true, &[100, 101, 102]).unwrap();
    let mut final_sequence = exhausted.begin_transaction().unwrap();
    final_sequence.stage_metadata(2, &filled(2)).unwrap();
    assert_eq!(
        exhausted.prepare(&final_sequence),
        Err(JournalTransactionError::SequenceOverflow)
    );
}
