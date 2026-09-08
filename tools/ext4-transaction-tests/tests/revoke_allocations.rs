// SPDX-License-Identifier: GPL-3.0-only
//! Large revocation sets must fit the kernel's allocation-record budget.
use ext4plus::{Ext4Read, Ext4Write, JournalCommitOperation, JournalMutationStage,
    JournalTransaction, JOURNAL_BLOCK_BYTES, JOURNAL_TRANSACTION_MAX_REVOKED_BLOCKS,
    replay_committed_transaction};
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

thread_local! {
    static TRACKING: Cell<bool> = const { Cell::new(false) };
    static CALLS: Cell<usize> = const { Cell::new(0) };
}

struct CountedAllocator;
fn allocated() {
    let _ = TRACKING.try_with(|tracking| {
        if tracking.get() { let _ = CALLS.try_with(|calls| calls.set(calls.get() + 1)); }
    });
}
unsafe impl GlobalAlloc for CountedAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        allocated();
        unsafe { System.alloc(layout) }
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        allocated();
        unsafe { System.alloc_zeroed(layout) }
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        allocated();
        unsafe { System.realloc(pointer, layout, size) }
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) }
    }
}
#[global_allocator]
static ALLOCATOR: CountedAllocator = CountedAllocator;

fn measured<T>(operation: impl FnOnce() -> T) -> (T, usize) {
    CALLS.with(|calls| calls.set(0));
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
