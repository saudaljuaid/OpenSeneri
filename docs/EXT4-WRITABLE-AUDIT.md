<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Writable ext4 Milestone 2: foundation audit

Baseline: `804c6ac065d92813e604b971f912383c3addf350` (PR #72).
This is a source audit of the existing writable implementation, not a release
certification. The matrix below preserves that baseline, not current status.
Stage 1 subsequently passed its host/e2fsprogs gates at `771ab6c`; milestone
status remains **1/5 verified**. Stage 2 implementation continues. See
[the current ext4 boundary](EXT4.md) for the tested feature profile and later
Linux evidence; stages 2–5 and release/review completion remain unproven.
The acceptance contract is everyday VFS semantics for the exact profile below;
neither upstream APIs nor synthetic journal tests alone establish that contract.

## Public operation call graph

The public entry points live in `src/kernel/vfs.c`; the backend dispatch contract
is `include/phipia/vfs_backend.h`. For ext4, its table selects
`src/kernel/ext4_fs.c`. Rust ABI wrappers in `src/rust/abi.rs` and
`src/rust/lib.rs` reach `src/rust/ext4.rs`.

| VFS entry point | C backend / Rust endpoint | ext4plus endpoint |
| --- | --- | --- |
| mount | `ext4_backend_mount` / `mount` | profile validation, `Ext4::load`, journal map, recovery, staged view, namespace validation |
| open | `ext4_backend_open` / `stat` | metadata with symlink following; C retains path/inode/access/size cookie |
| read, pread | `ext4_backend_read`, `ext4_backend_pread` / `pread` | `Ext4::open`, file offset read; C advances read cursor only for read |
| write | `ext4_backend_write` / `transaction_probe` | `File::write_bytes_at`, initialized physical block classification |
| seek | `ext4_backend_seek` | C cookie offset arithmetic; no disk mutation |
| stat_path | `ext4_backend_stat_path` / `stat` | checked metadata, inode identity/mode/uid/gid/link count |
| list, directory_open/read/close | backend list/cookie and `directory_entry` | fresh `read_dir`, rescan to visible ordinal, inode metadata read |
| create, create_mode | `ext4_backend_create` / `create_file_probe` | inode allocation, parent `Dir::link` |
| truncate | `ext4_backend_truncate` / `truncate_probe` | `File::truncate`, block release callbacks |
| mkdir | `ext4_backend_mkdir` / `create_directory_probe` | initialized directory inode, dot entries, parent link |
| rmdir | `ext4_backend_rmdir` / `remove_directory_probe` | `Dir::remove_empty_directory`, required freed-block revoke |
| link | `ext4_backend_link` / `link_file_probe` | `Dir::link`, regular inode link count |
| unlink | `ext4_backend_unlink` / `unlink_file_probe` | `Dir::unlink`, delete inode at last link |
| rename | `ext4_backend_rename` / `rename_probe` | same-parent `Dir::rename_entry`; cross-parent files use staged link/unlink; destination must be absent |
| sync | `ext4_backend_sync` / `sync`, `prepare_unmount` | resume pending mutation, retained clean-marker plan, clean reload |
| close | `ext4_backend_close` | clear C cookie; no storage sync |
| unmount | `ext4_backend_unmount` / `prepare_unmount`, `unmount` | finish pending plan, clean-state census, release Rust mount after lease close |
| drive, completion_count | backend report | checked allocator counters, C session completion count |

Every mutation joins `commit_staged_mutation`, either directly or through
`commit_namespace_mutation`. `load_staged_view` supplies
`JournalMutationStage` as both reader and writer. Its backing `PhipiaReader`
implements only `Ext4Read`; ext4plus cannot invoke a device write through it.
Partial writes coalesce in complete 4 KiB stage images. Freed ranges call the
writer's revoke hook. `build_transaction` classifies ordered data, journals all
remaining images, validates revokes, and seals under its exclusive lock.

`arm_recovery_marker` executes/acknowledges the recovery-marker plan before the
upstream mutation. `JournalRing::prepare` reserves slots; the coordinator retains
the request kind, paths, bytes, offset, prepared transaction, and exact phase.
`prepare_commit_plan` emits live journal state, ordered data, journal payload,
commit and home checkpoint operations. `prepare_checkpoint_plan` emits the tail
update. `execute_commit_operations` invokes `PhipiaJournalStorage` exclusively;
ABI callbacks require an active writable C lease. C bounds each byte request,
uses `nvme_volume_write` (read/modify/write for partial sectors), and maps every
`JournalFlush` to `nvme_volume_flush`. Completion advances the ring, adopts
checkpointed superblock allocation counters, and reloads before publishing success.

Mount recovery uses `recover_committed_ring`, then home replay + checkpoint
flush, clean journal superblock + journal-state flush, then checksummed ext4
marker clear + filesystem-state flush. If replay updates block zero, marker
cleanup derives from that replayed image. Only then does mount reload and expose
the namespace. Ordinary sync/unmount use the retained ring's clean plan.
See `EXT4.md` for the precise operation sequences and existing primary sources.
No ordering or checksum algorithm changes are part of this audit.

## Capability matrix

Implemented means the public bounded path exists; partial means a required
daily-use semantic or scale proof is missing. Refused means no public operation
exists or a specific operation returns an error. Existing evidence names below
are test locations, not a claim that this audit head has passed them.

| Capability | Baseline status | Measured contract / remaining gap |
| --- | --- | --- |
| read / pread | Implemented, bounded | offset read, lazy extent validation; pread preserves cursor; no concurrent-read proof |
| aligned / unaligned write, overwrite | Partial | one staged transaction; initialized touched blocks are ordered data; no large-request splitting |
| append | Refused as an atomic operation | seek-to-end followed by write exists; access enum has only read/write/read-write |
| sparse extension / hole read | Partial | hole-backed growth and zero reads are covered by the focused coordinator case; unaligned boundary writes, exact retries, and the 64 MiB/256 KiB bounds are exercised when the Linux fixture is available; no broad fragmented-scale claim |
| truncate grow / shrink | Partial | grow, partial-block shrink, re-extension, revoke/reclaim, retained retries, shared EOF, and bounded refusal are covered by the focused case; broader large-free and power-cut coverage remains outside this scope |
| create | Implemented, bounded | empty regular file; mode limited to 0777, default 0644 |
| mkdir / rmdir | Partial | dot/dotdot and counts handled; removal explicitly requires one-block empty directory |
| hard link | Implemented, bounded | regular-file path; same inode identity; bounded u16 links and transaction size |
| symlink / readlink | Refused through VFS | existing symlinks followed and mount-validated; no creation/readlink method in backend contract |
| unlink | Partial | live inode handles return BUSY; no deferred last-close deletion/orphan lifecycle |
| rename | Partial | no-overwrite files cross parents; directories same-parent; open source inode BUSY; replacement and cross-parent directories absent |
| stat | Partial | size, identity, uid/gid/mode/links; no timestamps/xattrs/sparse map in public structure |
| directory iteration | Partial | bounded ordinal rescan; quadratic; no stable mutation cookies or snapshot semantics |
| sync | Implemented, bounded | finish retained plan and durably clean marker; clean view reload required |
| fsync(file) | Implemented, bounded | inode-identity retry through the existing commit/checkpoint executor; unrelated retained work remains owned by its mutation or volume sync |
| close | Partial | releases cookie only; a previously failed write is not implicitly completed |
| clean unmount | Implemented, bounded | VFS references must be zero; pending plan and clean census checked; retries retained |
| access / permissions | Partial | handle access bits enforced; mode preserved on create/stat; backend open does not authorize credentials against inode mode |
| timestamps | Partial | mount validates stored timestamps; public stat/set-time interface absent; mutation timestamp policy unproven |
| xattrs | Refused for public access/mutation | mount validates names/values; admitted ext_attr bit is not a public xattr API |
| ENOSPC / inode exhaustion | Partial | block/inode ENOSPC and read-only refusals now retain dedicated Rust/C/VFS errors; real low-space rollback test added, full exhaustion matrix pending |
| crash recovery | Implemented, bounded | checksum/sequence/revoke validation, wrap and marker-only state; ten bounded QEMU cuts are not the release matrix |
| multiple handles / concurrency | Partial | bounded generation-authenticated handles; synchronous single-core path; no append race or parallel writer proof |

## Exact limits and failure gaps

- Admission requires 4 KiB blocks, 256-byte inodes, 64-byte group descriptors,
  first data block zero, compat `0x002c`, incompat `0x20c2` plus optional recovery
  `0x0004`, ro-compat `0x046b`. Unknown bits are refused before mounting. Native
  ext4plus ordinary/recovery writer admission also refuses permanent readonly
  and unsupported ro-compat states. No bigalloc, encryption, inline data,
  casefold, external journal, fast-commit or arbitrary ext4 profile support.
- Stage has **64 total images**, shared by file data and metadata, and 64 revokes.
  The transaction format separately bounds ordered data and metadata to 64 each;
  that does not double stage capacity. Requests now split into transactions
  touching at most 32 data blocks, with unaligned first/last blocks included.
  Metadata can still exhaust the remaining stage capacity on complex extent
  changes; adaptive chunk sizing is not yet implemented.
- Public C and Rust mutation paths cap resulting files at **64 MiB**
  (`PHIPIA_EXT4_MAX_MUTABLE_FILE_BYTES` / the matching coordinator bound).
  Direct probes reject an overflowing offset or truncate size before starting a
  lease or transaction. Reads/stat/seek use 64-bit values; those do not prove
  writable 64-bit file scale. Rust and C request limits are 256 KiB, with writes
  split into bounded transactions of at most 32 data blocks each.
- Ring: at most 8,192 slots, bounded physical journal map, one descriptor per
  transaction, checksum-v3/64-bit tags; JBD2 magic escaping refused. Sequence
  overflow refused rather than treated as an indefinitely wrapping counter.
- VFS: two volumes, 128 vnodes, 64 open files, 32 directory iterators; ext4
  backend has its own 64 handle cookies shared by files/directories. Paths are
  mount-relative ASCII shorter than 128 bytes, at most 16 components; backslash,
  colon, empty components and traversal above root refused. On-disk names may be
  255 bytes. Bulk list capacity is 64; streaming does not imply that limit.
- Namespace validation bounds 8,192 entries and 512 pending directories. It runs
  in **every `load_staged_view`**, including mutation reloads, not only mount.
  Repeated full namespace validation compounds directory rescan costs.
- VFS owns public objects, but offsets currently reside in C backend cookies,
  alongside mount/handle generations and path resolution state. The milestone's
  stronger exclusive VFS ownership rule is an implementation gap, not an
  already-established invariant. ext4plus still owns no global VFS policy.
- Failed rollback/reload now drops the old Ext4 allocator view before a fallible
  replacement load. Public reads refuse an absent view or a retained mutation;
  sync and later mutations can retry loading the checkpointed state. The host
  coordinator tests include the production Rust source and inject failed
  rollback, commit reload, clean reload, and every create storage operation.
  Their real-fixture/e2fsck results remain required before this gate passes.
- Rename checks whether the source inode is open, not descendant path cookies.
  Renaming an ancestor of an open file needs an explicit test and VFS policy.
- Ordered data can reach its home block before metadata commit. Existing journal
  prefix tests prove metadata atomicity; they do **not** prove all-or-nothing
  existing-file data overwrite at every sector-level power cut. Release claims
  must distinguish data visibility from metadata consistency.
- `end_operation` marks the mount unhealthy on lease-close failure. A durable
  Rust mutation followed by C close failure must be covered separately from a
  retryable journal write/flush refusal; the cursor has not advanced.

## Evidence and gate

`tools/ext4-transaction-tests/src/lib.rs` already covers immutable stage backing,
partial-block coalescing, 64-image/revoke bounds, atomic classification/sealing,
ring ordering/abort/wrap, corruption, replay and mapped durability prefixes.
Its deterministic fixture covers real journal discovery, allocation-bearing
write, failed classification followed by object discard and rollback, bitmap/
superblock checksums, truncate revoke, replay counters and read-only e2fsck.
The vendored superblock readonly unit test references an upstream
`src/test_data/raw_superblock.bin` that is absent from this tree; its presence in
source is not runnable evidence. The real-fixture test now exercises both
ordinary and coordinator loaders with checksummed permanent-readonly and unknown
ro-compat images, with/without recovery, and requires mutation refusal with an
empty stage and unchanged backing bytes.

The audit adds operation-by-operation executor refusal tests retaining byte-exact
plans and reservations, a shared-stage alias sealing test, and 96 single-bit
feature-word changes in the independent Python inspector. A configured but
unreadable integration fixture must fail, not silently count as a passing test.
Unconfigured local fixture tests still do not establish an e2fsprogs gate.

Stage 1 requires `make ext4-tests` with real tools and an actual fixture, the
applicable host/kernel checks, and inspected e2fsck output. Windows toolchain
failures or missing e2fsprogs are unavailable evidence, never passing evidence.
Subsequent semantic changes require their own commits and updated matrix rows;
Stages 2–5 remain unproven. No new PR is opened while foundation PR #72 is pending.
