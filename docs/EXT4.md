<!-- SPDX-License-Identifier: GPL-3.0-only -->

# ext4 storage boundary

The baseline capability matrix, public call graph, additional measured limits,
and unresolved failure cases are recorded in the
[Milestone 2 writable audit](EXT4-WRITABLE-AUDIT.md). Its stage gates distinguish
existing bounded evidence from everyday filesystem support that remains unproven.

Phipia has one ext4 implementation: the audited, pinned `ext4plus` source under
`vendor/ext4plus`. It is built `no_std`, synchronously, and with default
features disabled. Cargo is locked and forced offline through `.cargo/config.toml`;
the exact registry closure is under `vendor/rust-crates`.

The integrated backend is writable through the VFS. VFS remains the sole owner
of mounts, vnodes, open-file descriptions, generations, and directory iterators.
The ext4 backend owns one opaque Rust `Ext4` object per admitted volume and
generation-authenticated C file/directory cookies. It implements root and nested
lookup, open, read, offset-preserving pread, 64-bit seek/stat, and directory
enumeration, journaled regular-file writes and truncation, file and directory
creation/removal, hard links, regular-file no-overwrite rename across parents,
and directory rename across parents. Directory moves journal the
`..` entry and both parent link counts with the namespace changes; ancestry is
checked by inode identity, including symlink aliases, with a 1,024-ancestor
refusal bound. Indexed directory moves preserve their htree checksum and
passed Linux fixture verification at `7b47c59`.
File handles perform reads, writes, append and sync refresh by inode identity.
No-replace file/directory rename can therefore preserve open handles, including
descendants of moved directories; directory iterators own their snapshots. Hard-linked
paths report the same inode identity to the vnode table. Symlinks are resolved
by ext4plus for lookup and open. VFS `phipfs_symlink` journals literal targets
shorter than 128 bytes; `phipfs_readlink` copies the literal target without a
trailing NUL and supports short output buffers. The Rust coordinator accepts
targets up to 4,095 bytes. Dangling and looping links remain valid on remount;
unlink/rename and hard-link creation inspect the final link itself. Unlinking
a non-final hard link preserves open inode handles. Removing an open regular
inode's final link journals it onto the legacy orphan chain; inode-based I/O
continues until its final handle is released. Native
FILE_TRUNCATE and SDK ftruncate also use the
handle's inode identity, preserve the cursor, and refresh all matching handles'
sizes after a successful checkpoint.
Native `PATH_SYMLINK`/`PATH_READLINK` and SDK `symlink()`/`readlink()` expose
these operations to applications. Native unlink examines the entry itself
before trying empty-directory removal, so dangling links can be removed.
The remove-any API resumes the retained file or directory transaction directly;
SDK unlink and rmdir select file-only and directory-only semantics. VFS mutations
defer component validation to the journal coordinator, allowing retries while
the staged view is hidden. Native PATH_LINK and SDK link expose hard links.
`phipfs_rename` retains no-replace behavior; `phipfs_rename_replace` publishes
replacement in one transaction, preserving same-inode no-ops and refusing
nonempty directory destinations or incompatible file/directory types. The
destination's freed blocks are revoked with the namespace change. An open regular
destination is retained on the orphan chain, while source handles keep their
inode identity. Empty directory destinations and rmdir targets with open
snapshots now use the same retention path. Their inode numbers stay reserved
until the last snapshot closes; captured entries remain readable. Directory
retention and Linux-kernel recovery fixtures passed `make verify` at `f9ae36e`. The existing image/revoke limits still
apply. Open-file replacement and Linux-kernel recovery/roundtrip fixtures
passed Linux verification at f61d344.
The existing native `PATH_REPLACE` syscall selects this
operation when ext4 is admitted, and SDK `rename()` uses that syscall. Errors
from the ext4 transaction return directly; they never trigger the multi-step
backup-name replacement used by backends without atomic replacement.

VFS `phipfs_unlink_held_file` removes a name only if it still names the caller's
writable regular-file handle. The coordinator compares the final entry without
following a symlink before arming recovery and binds the inode into a distinct
retry key. Final-link removal uses the existing open-orphan chain; ordinary
unlink cannot take over its retained plan. This supports owned scratch cleanup
after draining failed application writes. New identity, storage-failure, replay,
allocation and fsck fixtures require Linux verification before gate credit.

C normally closes each NVMe filesystem session before returning. Failed
teardown retains the ownership cookie and freezes ordinary I/O; explicit sync
or unmount can retry teardown before resuming the journal coordinator. Ordinary reads acquire a
read-only session and each synchronous mutation acquires a writable session.
Mount also acquires a writable session so validated JBD2 recovery can checkpoint
before the volume becomes visible. Rust points to the
stable per-mount state and C refuses read, write, or flush callbacks without an
active lease, and refuses writes and flushes without the writable mount lease.
Every byte request checks the namespace capacity before calculating or issuing
an LBA operation. A failed mount drops any allocated Rust object and closes the
session before the mount becomes visible.

Admission accepts exactly the deterministic profile produced by
`tools/ext4_image.py`: 4096-byte blocks, 256-byte inodes, 64-byte group
descriptors, a zero first-data-block field, `has_journal`,
`ext_attr`, `dir_index`, `filetype`, extents, 64-bit block counts,
`metadata_csum` plus its seed, sparse/large/huge files, `dir_nlink`, and
`extra_isize`, with no additional feature bits other than the transient ext4
incompat-recovery marker. The declared block count must
fit the NVMe namespace and all free/total geometry is checked. Legacy orphan
cleanup is bounded to 128 allocated, zero-link regular inodes or empty
directories. Retained directories must have valid dot records, checksummed
blocks, a block-aligned size, and at most 8192 blocks. Zero-size Linux directory
orphans are admitted only with an inline extent root and a contiguous,
initialized logical prefix whose remaining blocks validate as empty. A fully
data-free root must have zero allocation accounting and no external xattr.
These states passed recovery/failure fixtures at `7b39693`; multi-level
zero-size trees and linked Linux truncation orphans remain refused. Cycles,
invalid allocation/checksum state, and reachable
zero-link inodes are refused before recovery home writes. Recovery validates
the post-replay view, checkpoints the journal while retaining the marker,
then journals each orphan deletion before clearing recovery state.
Ext4plus then
validates the superblock, group descriptors, and existing journal. Phipia walks
the reachable namespace (at most 8,192 entries and 512 queued directories),
validating directory blocks,
inode metadata and timestamps, extended-attribute names and values, symlink
targets, and the first and last mapped byte of non-empty files. Remaining file
extent/data checks are lazy and occur on pread.

The drive report derives `free_bytes` from ext4plus's checked, in-memory
superblock allocator counters, multiplies it by the admitted 4 KiB block size,
and rejects any result outside the filesystem image. Committed mutations reload
that view after checkpointing, so package capacity checks observe filesystem
space rather than unused bytes at the end of the NVMe namespace.

Phipia's VFS currently admits ASCII mount-relative paths shorter than 128 bytes
and at most 16 components. Directory names may be 255 bytes on disk; entries
that cannot fit the current VFS path contract can be enumerated but cannot be
opened through ABI v1. VFS directory handles own bounded snapshots of up to
8192 entries, retaining captured inode identities and order across mutations.
The legacy ordinal probe remains separate from snapshot iteration.

## Ownership and lock order

The kernel executes this path synchronously on one core. The required
order is VFS object/generation state, ext4 backend handle state, ext4plus's
internal synchronous read lock, native byte callback, active NVMe volume
session. Code must not call back into VFS while it owns an ext4plus object or an
NVMe session. Heap allocation may occur inside ext4plus, but no allocation
survives a successfully cleaned-up rejected mount. Failed NVMe teardown retains
its session until cleanup succeeds. File cursor changes and shared EOF updates
occur under the operation guard. Close makes a handle stale immediately and
defers backing-state/snapshot destruction when an active operation still uses
it. Snapshot reads and seek also reserve this guard without opening storage
unless SEEK_END needs a checked inode refresh. Reentrant-callback tests passed
locally before the final guard-release adjustment; Windows Application Control
blocked that rebuilt binary. Final Linux verification is pending at `d2affe4`.
This lock
order applies to the current single-core execution model.

## Read-write admission

Upstream reads an existing JBD2 journal but does not journal new mutations, so
Phipia gives ext4plus only a bounded `JournalMutationStage` as its reader and
writer. That copy-on-write overlay cannot write through to its immutable
NVMe-backed reader. After Phipia has durably set the recovery marker, the
coordinator retains the same overlay while continuing to refuse permanent or
unsupported read-only conditions. The ordered journal executor is the only
platform writer. Public VFS mutations route through that coordinator. Directory
removal refuses live children and requires freed directory blocks in the
transaction's revoke record before commit.

The VFS write path was admitted only after all of the following were present:

1. ordered-data JBD2 descriptor, data, revoke, and checksummed commit records;
2. an NVMe Flush after journal data and before acknowledging the commit;
3. replay with sequence, checksum, and revoke validation;
4. deliberate power cuts at every journal commit point, followed by replay and
   namespace/resource census checks; and
5. refusal tests for unsupported feature combinations and corrupt metadata.

The vendored port record in `vendor/ext4plus/PHIPIA-PORT.md` tracks the delta
from the pinned upstream commit.

## Ordered transaction foundation

The vendored crate contains a lower-level `no_std` transaction primitive.
`JournalTransaction`
accepts only complete 4 KiB block images, rejects duplicate or out-of-range
home blocks, bounds ordered-data and metadata images to 64 blocks each and
revocations to 8192 block numbers, and
refuses metadata that would require the unsupported JBD2 magic-escape rule. A
transaction serializes one checksum-v3/64-bit descriptor, its checksummed
metadata images, up to 17 checksummed 64-bit revoke records (509 entries per
full record), and one
checksummed commit block. The serializer feeds the same descriptor-tag,
revocation, and commit validators used by the existing replay reader.
Independent e2fsprogs journal-only replay on disposable cut images additionally
checks interoperability before read-only full fsck. Large truncate/unlink can
now exceed the former 64-revoke limit while retaining the 64-image stage bound.

`JournalRing` admits a distinct, bounded physical data-slot map for a clean
journal and assigns those records without collision across wrap. It refuses
more than 8,192 slots, duplicate/out-of-range slots, stale transaction
sequences, and reservations that would overrun uncheckpointed data. Commit
durability is sequence ordered. A mapped ring starts each durable plan with the
checksummed nonzero live-tail superblock, and only the oldest committed
reservation can build a clean/advanced-tail update after its home checkpoint
flush. Slots are reclaimed only after that final journal-state flush is
acknowledged. The newest reservation can be aborted before its live plan starts
without leaving a sequence gap. Public tests cover wrap, full-ring refusal,
early/out-of-order reclamation refusal, abort, revoke corruption, and replay
suppression.

`JournalSuperblockImage` validates
the exact 1,024-byte JBD2 v2 superblock header, checksum-v3/64-bit feature set,
CRC32C checksum, 4 KiB block size, sequence, clean/live start, and a bounded
logical journal length. Header, feature, checksum-type, and stored/calculated
checksum refusals remain distinct at the public boundary. Deterministic tooling
can build the canonical clean image or derive checksummed clean/live
sequence-and-start images without changing other admitted bytes. Clean
admission requires a complete, distinct, in-range physical map of the journal
inode (including its superblock), and the mapped ring refuses home metadata
that aliases that superblock or revoke records when the corresponding
incompatible-feature bit is absent.

`load_journal_inode_map` follows the admitted ext4 journal inode through the
same bounded extent/block-map iterator used by ext4plus, refuses holes,
duplicates, truncation, excess blocks, and superblock-length disagreement, and
reads logical block zero without consulting the replay overlay. The public host
suite passes the deterministic e2fsprogs image from the Python profile test into
Rust and proves that its real journal inode maps into the clean ring. Libext2fs
creates a feature-zero journal and normally upgrades it on mount. The fixture
generator performs that deterministic upgrade without mounting the image. It
installs the admitted
revoke/64-bit/checksum-v3 bits and CRC32C, then both e2fsck and the independent
Python/Rust parsers revalidate the result. The fixture verifier pins the clean
start, 4 MiB journal length, UUID match, feature masks, checksum type, and
stored checksum.

The Rust mount path performs the same journal-inode discovery and JBD2 admission
before exposing the filesystem to VFS. A clean filesystem must map into a
complete clean ring. For a filesystem carrying ext4's recovery bit, Phipia
reads the bounded physical ring, independently validates and collapses every
committed transaction, checkpoints the returned home images, flushes them,
persists and flushes the returned clean JBD2 superblock, and only then clears
and flushes the checksummed ext4 recovery marker. If the collapsed replay set
contains primary-superblock block zero, the marker clear is derived from that
validated replay image so recovered allocation counters cannot be replaced by
the mount-time snapshot. It reloads and re-admits the clean filesystem before
walking the namespace or exposing the mount.

`recover_committed_ring` implements the bounded live-ring reader for Phipia's
single-descriptor transaction profile. It starts at the admitted JBD2 sequence
and live block, follows consecutive committed records across one wrap, validates
every descriptor, data tag, optional revoke, and commit checksum, discards an
uncommitted tail, and collapses later images and revocations into the final
checkpoint set. A corrupt record that claims the expected transaction is
refused. Recovery also requires the caller to provide ext4's incompat-recovery
feature state. A clear ext4 recovery bit with nonzero JBD2 `s_start` is refused.
A set ext4 recovery bit with zero `s_start` is the valid marker-only crash state
before the first live journal-superblock write; it replays nothing but must pass
the same ordered cleanup before the marker clears. Tests cover wrap, cross-
transaction revocation, an uncommitted tail, a corrupt committed record, and
ext4/JBD2 state disagreement.

`qemu-test-ext4-recovery` builds that exact marker-only crash image from the
deterministic fixture, boots it as a writable 4 KiB NVMe namespace, and requires
the guest to recover it, then drives one allocation-bearing sparse extension
through the journal transaction path. The guest reopens the appended byte,
exercises public VFS write, truncate, create, hard-link, unlink, directory, and
rename entry points, cleanly unmounts, remounts with zero replay, and revalidates
the byte and resource census. After QEMU closes the disk, the host independently
runs the strict fixture inspector and read-only e2fsck over the resulting bytes.

`qemu-test-ext4-powercuts` gives every flush an explicit Rust/C ABI boundary
identifier and repeats the journal mutation on ten independent fixture copies. It
terminates QEMU without guest cleanup immediately after each durable recovery,
commit, checkpoint, journal-state, and final filesystem-state barrier. Every
copy is then rebooted without a cut, required to pass the guest namespace,
sparse-data, clean-unmount, remount, and resource census, and independently
checked with debugfs plus read-only `e2fsck`. Per-cut serial transcripts, disk
reports, and hashes are retained as a Linux workflow artifact. The same mounted
backend is writable through ordinary VFS calls after the boundary sweep.

The caller must supply a distinct physical journal block for the descriptor,
each metadata image, and the commit. The resulting operation list has one legal
order:

```text
checksummed ext4 primary-superblock write (set incompat-recovery)
Flush(FilesystemState)
live JBD2 superblock write (nonzero s_start)
ordered file-data home writes
Flush(OrderedData)
descriptor and metadata journal writes
Flush(JournalPayload)
commit journal write
Flush(Commit)
metadata home-block checkpoint writes
Flush(Checkpoint)
clean or advanced-tail JBD2 superblock write
Flush(JournalState)
```

Mount recovery has its own terminal ordering:

```text
validated committed metadata home-block checkpoint writes
Flush(Checkpoint)
clean JBD2 superblock write
Flush(JournalState)
checksummed ext4 primary-superblock write (clear incompat-recovery)
Flush(FilesystemState)
```

This ordering was checked against Linux's primary JBD2/ext4 paths: JBD2 waits
for journal payload before the commit record and places a preflush/FUA fence on
that record; checkpoint cleanup flushes filesystem home writes before advancing
the journal tail; recovery synchronizes replayed home blocks before journal
cleanup; and ext4 flushes the journal before clearing and committing its
needs-recovery feature. See Linux
[`commit.c`](https://github.com/torvalds/linux/blob/master/fs/jbd2/commit.c),
[`checkpoint.c`](https://github.com/torvalds/linux/blob/master/fs/jbd2/checkpoint.c),
[`recovery.c`](https://github.com/torvalds/linux/blob/master/fs/jbd2/recovery.c),
and [`super.c`](https://github.com/torvalds/linux/blob/master/fs/ext4/super.c).
Allocation checksum handling is pinned to Linux
[`bitmap.c`](https://github.com/torvalds/linux/blob/master/fs/ext4/bitmap.c)
and e2fsprogs
[`csum.c`](https://github.com/tytso/e2fsprogs/blob/master/lib/ext2fs/csum.c):
the block-bitmap CRC32C covers `clusters_per_group / 8` bytes, not the padded
remainder of the bitmap's 4 KiB home block. Phipia rejects bigalloc, so this is
`blocks_per_group / 8` at the retained filesystem checksum seed.

Metadata home blocks appear only after `Flush(Commit)`, and slots are reused
only after `Flush(JournalState)`. A ring discovered from a real clean ext4
image refuses its first mapped commit plan until the caller acknowledges the
dedicated recovery-marker flush. `FilesystemSuperblockImage` changes only the
little-endian incompatibility word and the primary-superblock CRC32C, whose
all-ones seed is independent of ext4's general metadata checksum seed. A
complete, checksummed commit is required before
`replay_committed_transaction()` returns any home image. Pure tests cut the
mapped operation list at every boundary and prove each durable prefix is either
clean, contains only an uncommitted tail, or replays the complete metadata
image. They also corrupt descriptor, data, and commit bytes independently.
`make ext4-tests` runs the public transaction checks from
`tools/ext4-transaction-tests` with
`--no-default-features --features sync`, followed by the hostile-image suite.

The C backend host test also exercises repeated public truncate refusals while
Rust hides its pending view. Truncate retries reach the coordinator before
stat, then refresh all matching open-handle sizes from checkpointed metadata.
The test checks read/write lease balance, shared-handle sizes, stale cookies,
and preservation of FULL and READ_ONLY errors across the C boundary.

The ring planner validates the journal map and produces recovery-marker, live,
checkpoint, recovery-cleanup, and tail-state operations. A shared executor maps
every operation to checked absolute byte writes and preserves every flush. The
kernel mount adapter binds those writes to `nvme_volume_write()` and each
barrier to `nvme_volume_flush()` for recovery and final clean-plan execution.
The mutation coordinator makes the recovery
marker durable, reloads the overlay through the recovery-only writer admission,
runs one upstream regular-file write, classifies initialized touched blocks as
ordered data and every other staged block as journaled metadata, then executes
and acknowledges commit, checkpoint, and tail-state durability before reloading
the checkpointed view. When a transaction journals the primary-superblock home
block, the ring binds its recovery-marked, checksummed image to that exact
prepared reservation. Only the matching durable checkpoint can replace the
ring's retained superblock state, so the later clean-marker write preserves the
checkpointed free-block and free-inode counters instead of restoring the
mount-time snapshot. A failed platform write or flush retains the exact
pending phase and request bytes for an identical retry; a different request is
refused. The QEMU recovery scenario injects a refusal at the live-journal
superblock write after an allocation-bearing upstream mutation, then at the
ordered-data flush while retrying that same pending request. Only the third,
byte-identical attempt is allowed to complete. Requests up to the existing
256 KiB limit now split into transactions touching at most 32 file-data blocks
each. When fragmented metadata exceeds the shared stage budget, an explicit
capacity refusal discards the entire attempt and halves the touched-block
count. The reduced chunk size survives storage retries. Real fixtures tested
12-image pressure, byte-identical suffix retries, and one-image rollback at
`7b39693`. The coordinator retains the full
request and its checkpointed byte count across a storage refusal; the identical
request or sync resumes only the unfinished chunk and remaining suffix. A
precommit refusal such as ENOSPC after completed chunks returns their durable
byte count as a short write. A power cut can preserve a prefix of a split write;
this is not whole-request atomicity. The C mutation limit is now 64 MiB;
Linux verification of that expanded VFS boundary is pending. Writable namespace methods use
the same retained planner through the public VFS table.
A bounded `JournalMutationStage`
now gives synchronous ext4plus mutations an immutable backing reader and a
copy-on-write overlay: the first partial write reads a complete 4 KiB home
block, later writes coalesce into it, reads see the overlay, and at most 64
complete images can be exported without any home write. ext4plus reports every
freed block range through its writer boundary before changing allocation
metadata; the stage bounds and deduplicates 64 revocations and removes any
same-transaction image of a block that was subsequently freed. Rollback clears
both images and revocations and requires discarding the mutated ext4plus object
as well. An atomic snapshot builder requires every caller-named ordered file-
data block to exist exactly once in the stage, adds the derived revocation set,
journals every remaining image as metadata, refuses a staged/revoked overlap,
and leaves the input transaction unchanged on error.
Successful classification takes the stage's exclusive lock and atomically
seals the complete snapshot, so no later ext4plus write can race the transaction
plan. A refused classification remains open for correction; explicit rollback
discards every staged image and reopens the overlay, while still requiring the
mutated ext4plus object to be discarded.
An open regular file exposes its initialized physical block at a byte offset so
the mutation adapter can derive that ordered-data set after the upstream write;
holes and uninitialized extents are never misclassified.
The deterministic Linux fixture now drives one real upstream sparse-extending
file write through that classifier after persisting the recovery marker. It
proves the allocation-updated block-zero superblock image retains a valid
metadata checksum and the recovery bit, independently checks the block-bitmap
CRC over the non-padding bytes, executes the ordered commit, and models both
normal checkpoint completion and a reset after the durable commit record.
Mount recovery replays the block-zero allocation counters before journal-tail
cleanup and derives the final marker clear from the replayed image. Both paths
prove the persisted free-block count changed once, reopen the appended byte,
and require read-only `e2fsck` acceptance. Before that commit, the same real
fixture performs an allocation-bearing extension, injects an invalid ordered-
data classification, discards the mutated ext4plus object, rolls back the
stage, and reloads the original size and recovery-marked superblock exactly. It
then truncates the committed extension, requires the freed data block in the
JBD2 revoke record, returns the free-space counter to its original value, and
requires a second clean `e2fsck` result. This is a
host integration proof over the same operation executor used by VFS and QEMU.

The stage is retained for the full Rust mount lifetime and both unmount phases
refuse pending images or revocations, so no unclassified upstream mutation can
be silently dropped. Setup failures before a prepared commit discard the
overlay and reload ext4plus so in-memory allocation counters cannot escape. The
real fixture pins that rollback across an allocation-bearing, deliberately
refused pre-commit classification. Once a storage operation has started,
rollback would be unsafe because an unknown prefix may already be durable; the
retained plan is retried instead. VFS writes pass their exact handle offset and
bounded source bytes into the same classifier and advance the cursor only after
the commit, checkpoint, journal-tail update, and storage lease close succeed.
The admitted
`JournalRing` is also retained for the full Rust mount lifetime. C opens a
writable NVMe lease before sync or unmount preparation; Rust
re-emits the same pending clean plan after a failed write or flush, acknowledges
the checksummed marker clear only after `Flush(FilesystemState)`, and refuses to
drop the mount unless the marker, JBD2 start block, reservations, and used-slot
census are all clean with matching sequence and head/tail state.
VFS sync now executes that same retry-stable plan without releasing the mount.
If a transaction has already started, sync or unmount preparation first resumes
its retained commit/checkpoint/reload phase; a further refusal leaves that exact
plan owned for another retry instead of discarding staged state.
After the marker clear is durable, Rust reloads a clean staged filesystem view;
a refused reload is retried by the next sync without rewriting durable state.
The QEMU probe injects both its marker write and flush failures, retries the
same plan through both the original mutation request and VFS sync, and only then
accepts sync. Its non-power-cut path then shrinks the sparse extension, requires
the freed physical block to appear in the prepared JBD2 revocation set, commits
and cleans it, and appends again to prove that a post-sync mutation durably
re-arms the recovery marker. Because every current transaction checkpoints
synchronously, a clean sync needs no extra write or flush.
The same non-power-cut scenario then creates an empty file under `/data/user`,
adds a hard link, removes the original name while the linked inode remains, and
removes the final name, synchronizing every transaction and requiring both
names to disappear before the final clean remount. This exercises inode
allocation, inode and directory bitmaps, group descriptors, directory
checksums, link counts, and their primary-superblock counters through platform
storage using the public create/link/unlink table entries.
The recovery-marker activation plan is equally retry-stable: its exact
checksummed write and filesystem-state flush are re-emitted after an I/O refusal
and acknowledged only after the flush completes. Sync and unmount preparation
also finish a refused activation before preparing the final marker clear, even
when the upstream mutation never started. This prevents a marker-only failure
from stranding a mount until the original request is retried. Started commit plans and
pending journal-tail writes likewise re-emit byte-identical operations until
their final flushes are acknowledged; slots remain reserved throughout. The
lease is closed before the separate Rust release, and either failure leaves the
mount live. The QEMU proof arms the marker, syncs it clean, and then unmounts
without another write. Its public truncate and namespace round trip are removed
before unmount, and the clean remount revalidates both bytes and resources.

If NVMe lease teardown fails, the backend retains its ownership cookie and
freezes new filesystem operations. The operation that encountered the failure
can already be durable; it must not be blindly repeated. Close application
handles and explicitly unmount to retry controller teardown and finish the
journal before releasing the mount. Failed mount attempts likewise retain
their owners until a mount retry or explicit unmount cleans them up. Host
fault tests cover repeated DMA-release and lease-close failures; bare-metal
teardown failure injection remains a separate required gate.

SDK fsync now passes a native file handle through FILE_SYNC and VFS to the
ext4 operation lease. The backend rechecks the handle after storage acquisition;
a stale descriptor or mount generation cannot redirect sync to a later mount.
Its current guarantee is mount-wide: drain retained work, checkpoint committed
transactions with the coordinator's NVMe flush barriers, preserve live open
orphans, and refresh shared EOFs before releasing the lease. Errors remain
retryable and may follow durable writes; fsync does not undo earlier failures.
The changed C tests pass for read-handle sync, storage refusal/retry, shared EOF,
closure during acquisition, and stale mount rejection. Linux SDK verification
and the complete durability/concurrency stage remain pending.

Close releases a descriptor and tries to reclaim unreferenced orphans. It is
not a durability barrier: refused cleanup remains owned by the mount, with its
exact pending transaction available to sync or unmount. Sync reports cleanup
errors and receives the complete live inode list under the volume lease; it
preserves orphan data held by other handles and keeps the recovery marker set
while those orphans exist. Unmount requires all handles closed, completes any
remaining cleanup, and only then clears the marker. The new orphan coordinator,
repeated-recovery, and final-close fault fixtures passed Linux verification at
07d45ea; they do not establish the full Milestone 2 crash/concurrency profile.

Inode-based access checks the checksummed allocation bitmap before reading an
inode body. A freed inode can retain valid mode and checksum fields after Linux
deletion; those fields alone do not authorize access. The manual filesystem
workflow also enables disposable Linux loop-mount tests for replace-rename
recovery and bidirectional file modification. Those fixtures passed Linux
verification at f61d344; they cover that operation profile, not the full
Milestone 2 interoperability and crash matrix.

The append backend selects the live inode's EOF under the exclusive writable
volume lease, ignoring the handle's seek position. Retained journal retries
reuse the original append offset and payload. Native `PHIPIA_OPEN_APPEND`,
POSIX `O_APPEND`, and stdio append modes use this operation. Native append
syscalls return at most one 4096-byte copied chunk; callers must handle short
writes. Direct backend requests remain bounded to 256 KiB and can checkpoint
multiple transactions, so crash atomicity for the entire request is not claimed.
The C mutation cap is now 64 MiB, matching the bounded split-reclaim profile.
The changed C boundary/ownership host test passes; the corresponding Linux
64 MiB sparse growth, tail, remount and reclamation fixture is pending.

Successful writes publish their cursor and shared EOF while still owning the
volume lease. A later writer cannot be followed by an older EOF update from
the previous writer. If controller teardown then fails, that committed cursor
and EOF remain visible while the mount refuses further storage operations.

Drive capacity reports use the last readable allocator count captured before
operation teardown. They do not borrow the Rust coordinator during a mutation;
a frozen storage owner is reported unhealthy until explicit teardown succeeds.

Append-only ext4 inodes accept the append operation, including bounded split
requests, but reject ordinary writes (even at EOF), truncate, unlink, rename,
new hard links, and explicit mode/time/xattr changes. Append-only directories
accept new entries and incoming non-replacing moves; removal and replacement
are refused. These checks run in the journal mutation path; the current open
interface carries access rights but no append flag, so refusal occurs at the
mutation rather than at open. Pending requests retain append mode in their
retry identity.

Directory handles own a snapshot captured under one read lease, bounded by
8192 visible entries. Create/delete/rename after open do not change the names
or metadata in that snapshot; reopen observes the new namespace. Close frees
the snapshot, and handle-allocation failure frees it before returning. Native
DIRECTORY_READ_LONG and SDK readdir carry full 255-byte ext4 names while the
existing pathname length limit still applies to operations on those names.

The VFS preserves dot components for ext4 to resolve in traversal order,
including symlink targets before `..` and lookup failures before later dots.
It also passes non-ASCII filename bytes through unchanged for this backend.
Root mode/time/xattr operations use the metadata path rather than namespace
creation checks. Mount-relative syntax and existing path/depth bounds apply.

VFS chmod replaces permission/special bits (0000–07777) through JBD2; immutable inodes
are refused. The writable branch now updates an existing access ACL's owner,
mask (or owning group), and other permissions in that same transaction. New
files and directories inherit a parent's default ACL with requested permissions
masked as in Linux; directories also retain the default for their children.
Hard links, rename and symlinks do not acquire a destination parent's ACL.
ACL parsing, inheritance, chmod, allocation rollback and crash fixtures await
Linux verification; this does not claim complete multiuser ACL enforcement.
Creation also inherits a setgid parent's group (including symlinks), and new
subdirectories retain setgid. A non-setgid parent's group is not inherited;
the current VFS supplies caller uid/gid 0. Linux comparison fixtures include
group IDs above 65535 and nested default-ACL/setgid inheritance.
POSIX mkdir now carries its mode through the native syscall and VFS into the
creation transaction, including mode 0000 and sticky. Legacy native mkdir calls
retain mode 0755. Requested setuid/setgid bits are stripped for mkdir as in
Linux; a setgid parent can still supply the directory's setgid bit. Pending
mkdir retries bind both path and requested mode. This extension awaits Linux
verification alongside the inherited-ACL fixtures.
POSIX open with O_CREAT now carries its variadic mode through an optional
native request flag to VFS creation; legacy requests retain mode 0644.
Permissions and special bits enter the new inode's journal transaction before
default-ACL masking. Native open now uses a prepared backend operation under
one volume lease: resolve/create, optional inode-bound truncate, then register
the handle. VFS does not pre-stat this path, so a retained create can retry.
Failed opens bind path/access/flags/mode and any selected truncate inode;
sync or an external exact retry retires that identity before inode reuse.
Known handle exhaustion refuses before mutation, and failed opens release
their handles. Storage failures may still commit the requested mutation.
Every-write/flush retry and old-or-new crash fixtures await Linux verification.
Creation follows dangling final symlinks using the existing component resolver,
permitting only a missing final name. Missing ancestors, trailing directory
separators and link loops still refuse. O_CREAT|O_EXCL reaches the same leased
operation and refuses any existing final name, including dangling symlinks,
before allocation or truncation. Exact retries retain the resolved creation
target as well as the original request. These additions await Linux verification.
The additive PATH_METADATA syscall supplies SDK stat/lstat with inode identity,
mode, uid/gid, link count and signed access/modify/change seconds plus nanoseconds.
lstat keeps the final symlink's own metadata, including dangling links. The older
PATH_STAT layout is unchanged. Pre-1970 dates are preserved; undeclared extended
timestamp bytes are ignored, and declared invalid nanoseconds refuse admission
before recovery writes. Backends without Unix metadata retain their old mode
projection and omit the native Unix-fields validity flag. Linux verification of
this metadata extension is pending.
FILE_METADATA supplies SDK fstat through a checked native handle and VFS
generation to the backend's inode lookup. It shares stat/lstat field conversion
and preserves metadata for an unlinked inode while its file remains open.
The C tests cover zero-link metadata, refusal outputs, closure during storage
acquisition and stale mount rejection; the new SDK route awaits Linux testing.
The kernel VFS publication operation takes a writable file handle and two paths.
Under one ext4 lease it checks that the temporary name still names that regular
inode, then performs the existing journaled replace. A replaced temporary name
or symlink refuses before marker writes. Pending publication binds both names
and the expected inode, preserving open destination orphans. Callers must finish
writing and syncing the temporary file before publication. C forwarding/lease
tests pass; exact retry, old-or-new crash, inode-identity and fsck fixtures await
Linux verification before applications can claim this save protocol is proved.
The admitted explicit xattr mutation namespace is
`user.*`, with names up to 255 bytes. Set/replace/remove journal the inode and
any allocated, rewritten or released external attribute block. The writer
packs small values in the inode and remaining values in one external block,
copying shared blocks before mutation. Entry hashes, block hashes, CRC32C,
inode block counts and allocator changes belong to the same transaction.
Sets that cannot be packed return FULL with the old attributes and allocation
counters preserved; external value inodes remain refused. The new allocation,
shared-copy, replay and e2fsprogs export fixtures await Linux verification.
Reads support size queries and refuse
undersized buffers. Native PATH_CHMOD/PATH_XATTR and SDK wrappers expose these
operations with Data write capability checks; POSIX chmod uses PATH_CHMOD.
Mutations sample validated RTC seconds before staging; inode changes record
ctime, file write/truncate records mtime, and directory entry changes record
mtime. Chmod preserves mtime; rename records the moved inode's ctime. New
inodes receive atime/mtime/ctime/crtime. Reads use noatime behavior.
Retries retain the staged timestamp bytes. Invalid RTC readings refuse a new
mutation before storage writes. The admitted write-time range is Unix epoch
through 2446-05-10 (0x37fffffff seconds). VFS/native/SDK set-times accepts explicit
atime/mtime in this range with nanoseconds below one billion; ctime still
comes from the transaction clock. The upstream Duration metadata API reports pre-epoch dates as zero
while preserving their raw fields unless the timestamp is changed.
