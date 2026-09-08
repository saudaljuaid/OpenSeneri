// SPDX-License-Identifier: GPL-3.0-only
//! Large revocation sets must fit the kernel's allocation-record budget.
use ext4plus::{Ext4Read, Ext4Write, JournalCommitOperation, JournalMutationStage,
    JournalTransaction, JOURNAL_BLOCK_BYTES, JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS,
    JournalSuperblockImage, recover_committed_ring, replay_committed_transaction};
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

thread_local! {
    static TRACKING: Cell<bool> = const { Cell::new(false) };
    static CALLS: Cell<usize> = const { Cell::new(0) };
    static FAIL_AT: Cell<usize> = const { Cell::new(0) };
    static LIVE_DELTA: Cell<isize> = const { Cell::new(0) };
}

struct CountedAllocator;
fn live_changed(change: isize) {
    let _ = TRACKING.try_with(|tracking| {
        if tracking.get() { let _ = LIVE_DELTA.try_with(|live| live.set(live.get() + change)); }
    });
}

fn allocated() -> bool {
    let mut denied = false;
    let _ = TRACKING.try_with(|tracking| {
        if tracking.get() {
            let _ = CALLS.try_with(|calls| {
                calls.set(calls.get() + 1);
                let _ = FAIL_AT.try_with(|at| { denied = at.get() == calls.get(); });
            });
        }
    });
    denied
}
unsafe impl GlobalAlloc for CountedAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if allocated() { return std::ptr::null_mut(); }
        let pointer = unsafe { System.alloc(layout) };
        if !pointer.is_null() { live_changed(1); }
        pointer
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        if allocated() { return std::ptr::null_mut(); }
        let pointer = unsafe { System.alloc_zeroed(layout) };
        if !pointer.is_null() { live_changed(1); }
        pointer
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        if allocated() { return std::ptr::null_mut(); }
        unsafe { System.realloc(pointer, layout, size) }
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        live_changed(-1);
        unsafe { System.dealloc(pointer, layout) }
    }
}
#[global_allocator]
static ALLOCATOR: CountedAllocator = CountedAllocator;

fn measured<T>(operation: impl FnOnce() -> T) -> (T, usize) {
    CALLS.with(|calls| calls.set(0));
    LIVE_DELTA.with(|live| live.set(0));
    TRACKING.with(|tracking| tracking.set(true));
    let value = operation();
    TRACKING.with(|tracking| tracking.set(false));
    (value, CALLS.with(Cell::get))
}

struct ZeroReader;
impl Ext4Read for ZeroReader {
    fn read(&self, _offset: u64, bytes: &mut [u8]) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        bytes.fill(0);
        Ok(())
    }
}

#[test]
fn maximum_revokes_use_bounded_allocations_and_preserve_wire_order_and_replay() {
    let limit = JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS;
    let stage = JournalMutationStage::new(Box::new(ZeroReader), 100_001 * JOURNAL_BLOCK_BYTES as u64).unwrap();
    let (_, staging_calls) = measured(|| {
        Ext4Write::revoke_blocks(&stage, (limit / 2 + 1) as u64, (limit / 2) as u32).unwrap();
        Ext4Write::revoke_blocks(&stage, 1, (limit / 2) as u32).unwrap();
        Ext4Write::revoke_blocks(&stage, 1, limit as u32).unwrap();
    });
    assert!(staging_calls <= 4, "revocation staging allocated {staging_calls} times");
    assert_eq!(stage.revoked_block_count(), limit);
    assert!(Ext4Write::revoke_blocks(&stage, limit as u64 + 1, 1).is_err());
    assert_eq!(stage.revoked_block_count(), limit);
    let bytes = [0x33; JOURNAL_BLOCK_BYTES];
    Ext4Write::write(&stage, 9000 * JOURNAL_BLOCK_BYTES as u64, &bytes).unwrap();
    let base = JournalTransaction::new(17, [0x5a; 16], 100_000).unwrap();
    let (transaction, classification_calls) = measured(|| stage.build_transaction(&base, &[]).unwrap());
    assert!(classification_calls <= 32, "classification allocated {classification_calls} times");
    let mut reference = base;
    for block in 1..=limit { reference.stage_revocation(block as u64).unwrap(); }
    reference.stage_metadata(9000, &bytes).unwrap();
    let slots: Vec<_> = (90_000..90_000 + transaction.required_journal_slots().unwrap() as u64).collect();
    let plan = transaction.commit_plan(&slots).unwrap();
    assert_eq!(plan, reference.commit_plan(&slots).unwrap());
    let journal: Vec<_> = plan.iter().filter_map(|operation| match operation {
        JournalCommitOperation::WriteJournal { bytes, .. } => Some(bytes.as_slice()),
        _ => None,
    }).collect();
    let (replay, recovery_calls) = measured(|| replay_committed_transaction([0x5a; 16], 17, 100_000, &journal).unwrap());
    assert!(recovery_calls <= 32, "recovery allocated {recovery_calls} times");
    assert_eq!(replay.len(), 1);
    assert_eq!(replay[0].block_index(), 9000);
    stage.rollback();
    assert!(stage.is_empty() && !stage.is_sealed());
    println!("maximum revokes allocations: staging {staging_calls}, classification {classification_calls}, recovery {recovery_calls}");
}

#[test]
fn recovery_allocation_refusals_return_errors_and_preserve_identical_retry() {
    let mut journal = Vec::new();
    for (sequence, blocks, revokes) in [
        (17, vec![100, 110, 120], vec![]),
        (18, vec![110], vec![100]),
        (19, vec![100], vec![120]),
    ] {
        let mut transaction = JournalTransaction::new(sequence, [0x5a; 16], 100_000).unwrap();
        for block in blocks { transaction.stage_metadata(block, &[sequence as u8; JOURNAL_BLOCK_BYTES]).unwrap(); }
        for block in revokes { transaction.stage_revocation(block).unwrap(); }
        let slots: Vec<_> = (1000..1000 + transaction.required_journal_slots().unwrap() as u64).collect();
        for operation in transaction.commit_plan(&slots).unwrap() {
            if let JournalCommitOperation::WriteJournal { bytes, .. } = operation { journal.push(bytes); }
        }
    }
    let superblock = JournalSuperblockImage::new_clean(17, [0x5a; 16], (journal.len() + 2) as u32)
        .unwrap().with_state(17, 1).unwrap();
    journal.push(vec![0; JOURNAL_BLOCK_BYTES]);
    let input = journal.clone();
    let references: Vec<_> = journal.iter().map(Vec::as_slice).collect();
    let (recovered, parse_calls) = measured(|| recover_committed_ring(&superblock, true, 100_000, &references).unwrap());
    assert_eq!(recovered.committed_transactions(), 3);
    assert_eq!(recovered.replay_images().iter().map(|image| image.block_index()).collect::<Vec<_>>(), [100, 110]);
    assert_eq!(recovered.replay_images()[0].bytes(), &[19; JOURNAL_BLOCK_BYTES]);
    assert_eq!(recovered.replay_images()[1].bytes(), &[18; JOURNAL_BLOCK_BYTES]);
    for ordinal in 1..=parse_calls {
        FAIL_AT.with(|at| at.set(ordinal));
        let (result, calls) = measured(|| recover_committed_ring(&superblock, true, 100_000, &references));
        FAIL_AT.with(|at| at.set(0));
        assert!(result.is_err(), "allocation {ordinal} was not refused after {calls} calls");
        assert_eq!(LIVE_DELTA.with(Cell::get), 0, "parser allocation refusal leaked buffers");
        assert_eq!(journal, input);
        assert_eq!(recover_committed_ring(&superblock, true, 100_000, &references).unwrap(), recovered);
    }
    let (projected, copy_calls) = measured(|| recovered.try_replay_images().unwrap());
    for ordinal in 1..=copy_calls {
        FAIL_AT.with(|at| at.set(ordinal));
        let (result, _) = measured(|| recovered.try_replay_images());
        FAIL_AT.with(|at| at.set(0));
        assert!(result.is_err());
        assert_eq!(LIVE_DELTA.with(Cell::get), 0, "projected-view allocation refusal leaked buffers");
        assert_eq!(recovered.try_replay_images().unwrap(), projected);
    }
    println!("recovery allocation refusals: {parse_calls} parser points, {copy_calls} projected-view points; unchanged input and exact retry");
}

#[test]
#[ignore = "requires the Linux-created PHIPIA_EXT4_RUST_FIXTURE; required by Linux CI"]
fn recovery_checkpoint_allocation_refusals_preserve_marker_and_retry() {
    use ext4plus::{Ext4, JournalFlush, load_journal_inode_map};
    let path = std::env::var("PHIPIA_EXT4_RUST_FIXTURE").expect("real ext4 fixture is required");
    let filesystem = Ext4::load(Box::new(std::fs::read(path).unwrap())).unwrap();
    let map = load_journal_inode_map(&filesystem).unwrap();
    let marker = map.filesystem_superblock().with_recovery_state(true);
    let marker_before = marker.clone();
    let mut transaction = JournalTransaction::new(17, [0x5a; 16], 100_000).unwrap();
    // Include the real primary superblock so marker cleanup also exercises
    // validation of a checkpointed successor, not only the mount-time image.
    let mut home = [0; JOURNAL_BLOCK_BYTES];
    home[1024..2048].copy_from_slice(marker.bytes());
    transaction.stage_metadata(0, &home).unwrap();
    transaction.stage_metadata(100, &[0x33; JOURNAL_BLOCK_BYTES]).unwrap();
    let slots: Vec<_> = (1000..1000 + transaction.required_journal_slots().unwrap() as u64).collect();
    let mut journal: Vec<_> = transaction.commit_plan(&slots).unwrap().into_iter().filter_map(|operation| {
        if let JournalCommitOperation::WriteJournal { bytes, .. } = operation { Some(bytes) } else { None }
    }).collect();
    let superblock = JournalSuperblockImage::new_clean(17, [0x5a; 16], (journal.len() + 2) as u32)
        .unwrap().with_state(17, 1).unwrap();
    journal.push(vec![0; JOURNAL_BLOCK_BYTES]);
    let references: Vec<_> = journal.iter().map(Vec::as_slice).collect();
    let recovered = recover_committed_ring(&superblock, true, 100_000, &references).unwrap();
    let original = recovered.clone();
    let (plan, calls) = measured(|| recovered.checkpoint_plan(999, &marker).unwrap());
    assert_eq!(calls, 4, "operation vector, two home images, and journal superblock");
    assert!(matches!(&plan[0], JournalCommitOperation::WriteHomeMetadata(image) if image.block_index() == 0));
    assert!(matches!(&plan[1], JournalCommitOperation::WriteHomeMetadata(image) if image.block_index() == 100));
    assert_eq!(plan[2], JournalCommitOperation::Flush(JournalFlush::Checkpoint));
    assert!(matches!(&plan[3], JournalCommitOperation::WriteJournalSuperblock { image, .. } if image.start_block() == 0));
    assert_eq!(plan[4], JournalCommitOperation::Flush(JournalFlush::JournalState));
    assert!(matches!(&plan[5], JournalCommitOperation::WriteFilesystemSuperblock { image, .. } if !image.needs_recovery()));
    assert_eq!(plan[6], JournalCommitOperation::Flush(JournalFlush::FilesystemState));
    for ordinal in 1..=calls {
        FAIL_AT.with(|at| at.set(ordinal));
        let (result, _) = measured(|| recovered.checkpoint_plan(999, &marker));
        FAIL_AT.with(|at| at.set(0));
        assert!(result.is_err(), "checkpoint allocation {ordinal} must refuse before execution");
        assert_eq!(LIVE_DELTA.with(Cell::get), 0, "checkpoint refusal leaked buffers");
        assert_eq!(marker, marker_before);
        assert_eq!(recovered, original);
        assert_eq!(recovered.checkpoint_plan(999, &marker).unwrap(), plan);
    }
    println!("recovery checkpoint: all {calls} allocation refusals preserve marker, release buffers, and retry identically");
}
