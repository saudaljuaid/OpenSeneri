# Phipia ext4plus port boundary

Phipia vendors the `src/` tree from ext4plus commit
`ec7e8443e474376977bb752cde370762226a5a50` as its sole ext4
implementation candidate. Upstream's MIT and Apache-2.0 notices are retained
unchanged.

The source was selected because it is designed for `no_std` + `alloc`, has a
caller-supplied block I/O contract, performs broad checked ext4 metadata
validation, supports extents and 64-bit files, and already implements the
required read/write object operations.

The unmodified commit does **not** provide journaled writes. It reads and
validates an existing JBD2 journal but sends mutations directly to home blocks.
Phipia therefore exposes read-write access only through the local retained
mutation stage and ordered, checksummed JBD2 writer over the explicit NVMe
Flush fence. The public path was admitted after deliberate power-loss tests.

Phipia's local journaling delta supplies that admission boundary.
`src/journal/transaction.rs` builds one bounded checksum-v3 descriptor,
metadata set, optional 64-bit revoke record, and commit record and returns an
ordered-data operation plan with explicit data, journal-payload, commit, and
checkpoint flush barriers. A ring admitted from a real journal-inode map wraps
that body with a checksummed nonzero live-superblock write and, after the home
checkpoint barrier, a clean or advanced-tail superblock write plus final state
flush. Its clean-journal ring coordinator bounds and validates a caller-supplied
physical slot map, wraps without collision, refuses overrun of uncheckpointed
records, sequences commit durability, reclaims only the oldest transaction
after its tail state is durable, and rolls back the newest reservation before
its live plan begins without leaving a sequence gap. Replay and public tests
cover corruption, every mapped-plan power-cut prefix, revocation suppression,
wrap, exhaustion, abort, and out-of-order reclamation.

`JournalSuperblockImage` validates and preserves the exact JBD2 v2 superblock,
admits Phipia's 4 KiB/checksum-v3/64-bit profile, emits checksummed clean/live
sequence-and-start images, and maps a complete clean journal-inode block list
into the ring. The mapping is bounded, distinct, in-range, and includes the
physical superblock in alias checks. Ring preparation also refuses revokes when
the admitted superblock does not advertise block revocations.

`FilesystemSuperblockImage` preserves the admitted ext4 primary superblock and
emits clean or incompat-recovery state with the metadata-checksum CRC32C
recomputed over the little-endian bytes before `s_checksum`, using the ext4
superblock's fixed all-ones seed. A ring discovered from a real clean filesystem
emits that marker write followed by `Flush(FilesystemState)` and refuses its
first mapped JBD2 commit plan until the caller acknowledges the flush. Synthetic
journal-only rings retain their existing host-test boundary.
An allocation transaction may journal the 4 KiB home block that contains this
primary superblock. The ring admits that updated image only when it retains the
recovery marker, has a valid metadata checksum, changes only the currently
supported allocation-counter fields, and occurs in the matching prepared
transaction's home-checkpoint operations. Durable acknowledgement atomically
reclaims that reservation and adopts its image, preventing the later clean
plan from restoring stale mount-time counters.

Block-bitmap checksums follow Linux `ext4_block_bitmap_csum_set()`: with
bigalloc excluded, exactly `blocks_per_group / 8` bytes are hashed at the
filesystem checksum seed. Padding to ext4plus's 4 KiB I/O block is deliberately
excluded.

Ordinary `load_with_writer` admission still discards a supplied writer whenever
the recovery marker is present. The explicit `load_with_recovery_writer` entry
point preserves it only when recovery is the sole read-only condition; the
on-disk read-only flag and unsupported read-only-compatible features continue
to discard it. This narrow API is for Phipia's coordinator after it has made the
marker durable and owns commit, checkpoint, and barrier ordering. It does not
make ext4plus's ordinary mutation API journal-aware.

`load_journal_inode_map` discovers the internal journal through ext4plus's own
checked inode and extent/block-map iterator, bounds the complete physical map,
reads logical block zero without the replay overlay, and requires its length to
agree with the JBD2 superblock. The public test consumes the real deterministic
e2fsprogs fixture produced immediately before the Cargo suite. Clean-ring
admission additionally requires ext4's incompat-recovery bit to be clear; a
zero JBD2 `s_start` is not treated as authoritative on its own. Superblock
admission reports header, feature, checksum-type, and checksum failures
separately so a real image is never weakened to fit an opaque refusal. The
unmounted fixture explicitly upgrades libext2fs's initial feature-zero journal
to the same revoke/64-bit/checksum-v3 profile that Phipia admits, then verifies
the clean start, UUID, geometry, feature masks, checksum type, and CRC32C before
the Rust handoff.

`recover_committed_ring` scans Phipia's bounded single-descriptor transaction
profile from the validated live start across at most one wrap. It validates
every record, discards an uncommitted tail, applies later revocations to older
pending images, and returns a collapsed checkpoint set plus the checksummed
clean superblock state. A clear ext4 recovery bit cannot accompany a nonzero
JBD2 start. A set recovery bit may accompany a zero start because it is the
durable crash state between the marker flush and the first live JBD2
superblock; that state yields an empty replay and still requires cleanup.

The public `JournalStorage` executor maps every ordered operation to a checked
absolute byte write and passes every named flush through without coalescing it.
Phipia's mount adapter implements that boundary with native NVMe writes and
`nvme_volume_flush()`. Dirty mounts checkpoint the validated replay set, flush
home metadata, persist and flush the clean JBD2 superblock, clear and flush the
checksummed ext4 recovery marker, then reload and re-admit the clean image. If
replay includes the primary-superblock home block, the final marker clear is
derived from that validated replay image rather than the stale mount-time
image, preserving recovered allocation counters.
For writable mounts, the ring exposes a separate final-clean plan only
after all reservations are checkpointed and the durable JBD2 start is zero. It
blocks new reservations until the checksummed ext4 marker clear is flushed, and
the next commit cycle must make the recovery marker durable again.
Pending recovery-marker activation and final marker clearing plans are both
retry-stable after a write or flush refusal; acknowledgement alone advances
their in-memory durability state.
Started commit plans and checkpoint tail-state plans also re-emit identical
operations after refusal and retain their ring slots until the corresponding
durability acknowledgement.

`JournalMutationStage` is the synchronous interception boundary for
ext4plus mutations. It accepts only an immutable backing reader, coalesces
partial writes into bounded complete 4 KiB copy-on-write images, serves reads
from that overlay, and can discard the entire stage without issuing a home
write. ext4plus reports freed block ranges through the writer before changing
allocation metadata. The journal stage bounds and deduplicates that set,
removes a same-transaction image when its block is freed, and emits the
corresponding JBD2 revoke record; ordinary direct writers use the default no-op
callback. Phipia reloads every clean admitted mount with the retained stage as
ext4plus's reader and only writer; both unmount phases refuse pending images or
revocations, so an unclassified mutation cannot be silently dropped. A focused
public test crosses a block boundary, proves coalescing and rollback, reaches
the 64-block image and revocation bounds, and proves the backing bytes never
change. The Linux fixture additionally runs a real upstream file write through
the stage and observes the overlay while the source image remains unchanged.
Phipia's directory delta initializes new directory inodes with checksummed
`.`/`..` entries and supplies a bounded empty-directory removal primitive.
Removal validates the complete single-block directory before changing the
stage, updates the parent link count, frees the inode and data block, and
returns that physical block so the platform adapter can require its exact JBD2
revocation. A same-parent rename primitive adds the destination and removes the
source without changing inode or parent link counts; it refuses replacement
and leaves cross-parent moves to a future, separately proven adapter.
The stage can also clone an input transaction into one atomic classified
snapshot: each explicitly named ordered-data block must be staged exactly once,
derived revocations are added, every remaining image becomes journaled
metadata, and staged/revoked overlap is refused. Classification and sealing
share one exclusive lock, so a successful snapshot cannot race a later
upstream write. Refused classification leaves the stage open; rollback clears
and reopens it, but the mutated filesystem object must still be discarded. An
open file can report the initialized physical block at a byte offset so Phipia
can derive the ordered-data set after a staged regular-file write without
exposing a writer.
The real Linux fixture persists the checksummed recovery marker, reloads the
upstream filesystem on that dirty-marker backing, performs an allocation-bearing
sparse extension, and verifies that its staged block-zero image cannot clear the
marker. One deliberately invalid ordered-data classification then proves that
discarding the mutated filesystem object and rolling back the stage restores
the original size and recovery-marked superblock. A fresh mutation binds its
block-zero image to the prepared reservation, executes the complete ordered
commit, and checks both normal checkpoint completion and mount replay after a
reset following the durable commit record. Both paths preserve the allocation
counter through tail cleanup, verify the group-zero bitmap checksum
independently, reopen the appended byte, and require read-only `e2fsck`
acceptance. A following real truncate must derive a revoke for that appended
data block, return the allocation count to its original value, and pass
`e2fsck` again. Public VFS writes use this same path.

The stage is connected to public VFS write, truncate, create, hard-link,
unlink, mkdir, rmdir, and same-parent no-overwrite rename operations. Each
mutation supplies its exact ordered-data classification and commits before the
VFS publishes success. The retry-safe final-clean plan is bound to both VFS
sync and unmount through a writable NVMe lease. Sync retains the mount,
acknowledges the marker clear only after its flush, and requires the next
mutation to re-arm recovery; a clean sync needs no extra write or flush because
every transaction already checkpoints synchronously. Sync and unmount
preparation also finish an already-started retained transaction plan; another
refusal keeps that exact plan retryable. Once the marker clear is durable, the
adapter reloads a clean staged view so the retained mount can arm a later
transaction; a failed reload is retried without rewriting durable state. The QEMU probe retains one
allocation-bearing transaction across an injected live-superblock write
failure and an injected ordered-data flush failure, including retry through VFS
sync, then retries injected final-clean marker-write and flush failures. Its
non-power-cut path commits a one-block truncate, requires the freed physical
block in the JBD2 revocation set, cleans, and re-appends to prove recovery-marker
re-arming after sync. It also creates an empty file, adds a hard link, unlinks
both names through staged inode/directory allocation metadata, synchronizes
every transaction, and requires the namespace to be absent before remount. A
separate Linux target
cuts ten independent VMs immediately after every named durability barrier,
reboots each disk, and requires namespace, data, resource, and read-only
`e2fsck` acceptance. The real fixture separately pins pre-commit allocation
rollback after a refused classification and derives revocations from a real
truncate. Open-handle busy rules, retry through VFS sync, mode-preserving file
creation, clean unmount, and resource census are exercised through the public
adapter.

Phipia also tightens upstream writer admission: an image carrying ext4's
`RO_COMPAT_READONLY` feature discards the supplied writer, as does an image
carrying an unsupported read-only-compatible feature. Recovery remains
read-only through the ordinary loader and can retain a writer only through the
explicit coordinator-only loader described above. Focused superblock tests pin
the permanent read-only refusal, and the real-fixture transaction test proves
that the ordinary loader still refuses a recovery-marked writer before using
the explicit coordinator loader. Writable admission is therefore limited to
the exact supported profile and never weakens those refusal cases.

Phipia-specific changes stay in reviewable commits and are summarized here as
they land. The intended port configuration is `--no-default-features
--features sync`; the asynchronous and hosted `std` surfaces are out of scope.
The standalone lockfile is generated from the repository's committed offline
crate mirror and matches the dependency versions in `src/rust/Cargo.lock`.
The vendored manifest replaces workspace-inherited edition/license fields and
removes upstream-only `xtask`/dev dependencies because those sources and test
fixtures are deliberately outside the runtime vendor boundary.

Vendored scope is intentionally limited to `Cargo.toml`, `Cargo.lock`, the two
license files, `README.md`, and `src/`. Upstream test disk images, `xtask`, and
host integration tests are not runtime build inputs and are not vendored.
