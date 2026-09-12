<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Package transaction state and recovery foundation

`tools/phipia-transaction.py` defines a bounded installed-database format, an
atomic-generation authority record, an immutable transaction journal, and a
deterministic in-memory reference model for package state transitions. The model
exercises staging, commit recovery, rollback, removal, dependency retention,
integrity verification, and repair.

The host tool defines the format and reference state machine; it does not write
a System image or install packages. The guest has a privileged, VFS-backed
prepare/commit/recovery service plus a bounded parser and dependency planner for
signed repository/package bytes. These components meet at an authenticated
staging boundary described in
[`PACKAGE_MANAGER.md`](PACKAGE_MANAGER.md).

## Trust and namespace boundary

The reference API starts after repository and package admission. Candidate
metadata uses package-v3 identity, SemVer, publisher key ID, immutable file path,
kind, mode, length, and SHA-256 semantics. Dependency records retain the exact
identifier/constraint/provider binding chosen by the signed repository resolver.
The transaction layer does not resolve versions again, accept embedded keys, or
turn a SHA-256 into a signature. The staging caller must authenticate the
repository index, download digest, publisher key, package-v3 signature, and file
payload before calling the equivalent staging operation.

The installed database and journal use SHA-256 for corruption detection and
cross-record binding, not publisher or repository authenticity. Repair payloads
in the host model are described as authenticated because the API verifies them
against the already authenticated installed file length and digest; the guest
service also obtains those bytes from a trusted cache or repeats the
repository/package verification chain.

Immutable package files and mutable user data are distinct namespaces. Package
transactions own only the paths listed in the installed database. The model's
`user_data` mapping is never copied into a package generation, verified against a
package digest, removed, rolled back, or repaired. The guest filesystem layout
and authorization checks preserve that separation.

## Complete immutable generations

Each committed generation consists of:

1. one canonical installed database;
2. exactly the immutable owned files named by that database; and
3. no unowned immutable files.

Installation, update, removal, and repair build a complete generation at
`current + 1` without changing the selected generation. Every staged byte and
digest is validated before the commit point. A 128-byte authority record is then
atomically replaced to select the complete new generation. Old immutable state
is retained until authority replacement and cleanup are durable.

The host model charges free space for the complete old generation, staged
generation, and journal at once. A filesystem may share immutable extents or
content-addressed objects only with equivalent reference accounting. It must
retain the old generation until the new one is complete and authoritative.

## Installed database version 1

The database is at most 32 MiB and starts with `PHIPDB01`. Integers are
little-endian. Fixed-width text is ASCII, NUL-terminated, and zero-tailed. Tables
are contiguous in package, dependency-edge, and owned-file order. SHA-256 covers
every byte following the 512-byte header.

| Header offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | `PHIPDB01` magic |
| 8 | 2 | format version (`1`) |
| 10 | 2 | header bytes (`512`) |
| 12 | 4 | flags (`0`) |
| 16 | 8 | exact total bytes |
| 24 | 8 | positive authoritative generation |
| 32 | 4 | positive native ABI |
| 36 | 4 | reserved zero |
| 40 | 16 | architecture (`x86_64`) |
| 56 | 8 | package-table offset (`512`) |
| 64 | 4 | package count, at most 256 |
| 68 | 4 | package-record bytes (`256`) |
| 72 | 8 | dependency-edge-table offset |
| 80 | 4 | edge count, at most 4,096 |
| 84 | 4 | edge-record bytes (`192`) |
| 88 | 8 | owned-file-table offset |
| 96 | 4 | file count, at most 4,096 |
| 100 | 4 | file-record bytes (`256`) |
| 104 | 32 | SHA-256 of all table bytes |
| 136 | 376 | reserved zero bytes |

Package records are sorted by identifier. The same package identifier cannot
appear twice; an update replaces that identity in the next generation.

| Package offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 64 | canonical lowercase dotted identifier |
| 64 | 64 | canonical SemVer 2.0.0 version |
| 128 | 32 | authenticated package-v3 SHA-256 |
| 160 | 32 | authenticated publisher Ed25519 key ID |
| 192 | 4 | flags (`1` means explicitly installed root) |
| 196 | 4 | first owned dependency-edge index |
| 200 | 4 | dependency-edge count |
| 204 | 4 | owned-file count |
| 208 | 48 | reserved zero bytes |

Dependency edges are contiguous for each package and sorted by requested
identifier, constraint, and selected provider. The 192-byte record stores the
requested identifier at 0–63, its package-v3 canonical constraint at 64–119,
the exact provider package identifier at 120–183, and eight reserved zero bytes.
Every provider must exist in the same database. Duplicate requested identifiers
and dependency cycles are refused. Every non-explicit package must be reachable
from an explicit root, so the database cannot canonically retain an orphaned
dependency. The record intentionally preserves virtual provider binding rather
than guessing it during recovery.

Owned-file records are globally sorted by path, so ownership collisions have one
unambiguous refusal before any staging mutation.

| File offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 128 | canonical package-root-relative path |
| 128 | 4 | owner package-table index |
| 132 | 2 | package-v3 file kind |
| 134 | 2 | flags (`0`) |
| 136 | 4 | canonical mode (`0444` or `0555`) |
| 140 | 4 | reserved zero |
| 144 | 8 | exact nonzero length, at most 64 MiB |
| 152 | 32 | file SHA-256 |
| 184 | 64 | library SONAME, empty for other kinds |
| 248 | 8 | reserved zero bytes |

An executable must be `0555`; other current package-v3 kinds may be `0444` or
`0555`. Paths, kinds, modes, and bounds intentionally match package v3.

## Authoritative-generation record

The authority record is exactly 128 bytes and starts with `PHIPGN01`.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | `PHIPGN01` magic |
| 8 | 2 | version (`1`) |
| 10 | 2 | record bytes (`128`) |
| 12 | 4 | flags (`0`) |
| 16 | 8 | selected generation |
| 24 | 8 | exact database bytes |
| 32 | 32 | selected database SHA-256 |
| 64 | 32 | SHA-256 of bytes 0–63 |
| 96 | 32 | reserved zero bytes |

The guest durability contract is stronger than encoding these bytes: it must
flush the staged generation and journal before atomically replacing authority,
then flush authority before reclaiming old state. An implementation may use an
atomic rename, two fixed slots plus a monotonically selected sector, or another
filesystem-specific primitive only if recovery can observe the complete old
record or complete new record, never a synthesized mixture.

## Prepared journal version 1

The journal is exactly 512 bytes and immutable after creation. Authority, not a
mutable journal phase, identifies the commit side. This avoids requiring several
independently durable phase writes.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | `PHIPTX01` magic |
| 8 | 2 | version (`1`) |
| 10 | 2 | journal bytes (`512`) |
| 12 | 4 | flags (`0`) |
| 16 | 2 | operation: install `1`, update `2`, remove `3`, repair `4` |
| 18 | 2 | phase (`1`, prepared) |
| 20 | 4 | reserved zero |
| 24 | 8 | base generation |
| 32 | 8 | target generation (`base + 1`) |
| 40 | 8 | conservative required staging space |
| 48 | 8 | base database bytes |
| 56 | 8 | target database bytes |
| 64 | 32 | base database SHA-256 |
| 96 | 32 | target database SHA-256 |
| 128 | 32 | transaction ID: SHA-256 of the record with this field zero |
| 160 | 64 | optional single target package identifier |
| 224 | 288 | reserved zero bytes |

The target field is empty for repair and multi-package plans. It is diagnostic;
the complete target database is authoritative for state.

## Repository rollback floor

`pkgstate/repo.bin` is a checksummed 128-byte `PHIPREP1` record containing the
greatest signed repository version accepted for an install, update, or repair.
Repository admission reads the maximum valid version from `repo.bin` and a
possible crash-leftover `repo.new`; any present malformed candidate fails
closed. A lower signed repository can therefore never regain authority after a
reboot.

Advancement writes and syncs `repo.new`, then replaces `repo.bin` and syncs
again before package-generation staging starts. Recovery treats a greater
temporary candidate as accepted and promotes it, while a stale duplicate is
discarded only when the current record remains present. Every injected failure
at either durability boundary retains at least the previously accepted floor;
retry is idempotent and lowering the value is refused.

## Recovery decision

Recovery validates authority, journal, both database digests, database
generation numbers, exact owned-file sets, file lengths, and file digests.

| Observed authority | Complete states | Result |
| --- | --- | --- |
| base | complete base, any target | select base; discard target and journal |
| target | complete target | select target; discard base and journal |
| target or invalid | incomplete target, complete base | repair authority to base |
| any | no complete base and no authoritative complete target | refuse recovery |

Thus an interruption before authority replacement rolls back an update or
remove. An interruption after replacement completes it. Recovery never merges
some old files with some new metadata. Cancellation is permitted only while the
base authority is still selected; afterward normal recovery is required.

`include/phipia/package_state.h` and `src/kernel/package_state.c` provide the
allocation-free parsing and decision core. They are freestanding and
allocation-free: all untrusted integers are decoded from checked little-endian
byte fields, SHA-256 is computed incrementally in fixed storage, dependency
cycle/reachability state is bounded by the 256-package maximum, and no on-disk
structure is overlaid with a C struct. The parser enforces the exact v1 table
locations, counts, record sizes, canonical text and SemVer grammars, package-v3
per-package limits, unique sorting, provider existence, file ownership, modes,
SONAMEs, zero reserved bytes, and both content and envelope digests.

The same core emits canonical authority and prepared-journal records for the
privileged generation committer. Both encoders re-parse their database
inputs, enforce adjacent generations and bounded canonical target identifiers,
and clear the entire output on refusal. Recovery uses the public authority
encoder too, so repair and installation cannot drift into different record
encodings.

Typed immutable accessors expose each admitted package, dependency edge, and
owned-file record without making downstream staging code repeat fixed offsets.
They validate the view's complete table shape and the selected record before
returning slices; the parsed database buffer remains caller-owned and must stay
live and unchanged.

`package_generation.c` is the allocation-free canonical database encoder at the
other side of those views. A privileged builder supplies bounded package,
provider-edge, and owned-file arrays plus an exact caller-owned output span. The
encoder writes no filesystem, signs nothing, and does not resolve dependencies;
it clears failures and returns success only after the normal installed-state
parser accepts the complete generated database. Its host proof requires exact
byte equality with the independently assembled two-package fixture and refuses
bad ordering, file collisions, missing digests, and output-length mismatch.
The companion verifier re-parses already encoded bytes and compares every
header, package, provider edge, and file field to the source specification. A
stager can therefore bind file-source entries to the exact database it persists
without allocating and trusting a second encoded copy.

`package_builder.c` supplies those arrays from an authenticated plan without
performing filesystem I/O. It reopens the signed repository and package bytes,
recomputes the plan, preserves unchanged package metadata and file sources,
binds changed dependency edges to the selected providers, promotes the requested
root, and removes automatic packages no longer reachable after an update. Its
caller-owned heap workspace records whether every output file must be copied
from the old generation or written from an admitted package payload. The host
proof covers fresh install, unchanged-file reuse, zero-download root promotion,
full removal, dependency-changing update/orphan pruning, plan mismatch, and
wrong payload ordering before passing the result through the canonical encoder.

The SHA-256 context API is public so the filesystem service can hash immutable
files in bounded chunks; update-after-finish, finish-twice, and counter overflow
are refused.

The C recovery core accepts two database candidates and one explicit
`owned_files_complete` proof per candidate. A database checksum alone does not
establish that proof. `include/phipia/package_service.h` and
`src/kernel/package_service.c` now produce it by enumerating the immutable root,
refusing extra files and multiply linked files, then checking each declared
file's type, exposed mode, stable object identity, exact length, EOF, and SHA-256
through 4 KiB reads. A missing or changed file leaves that candidate incomplete.
The core then re-parses and re-hashes both databases itself, binds them to the
journal, and selects only the complete base or authoritative complete target.
An authority generation, database length, or database digest unrelated to the
prepared journal is refused as a state mismatch. With no journal, exactly one
complete candidate must match authority.

## Guest filesystem recovery service

The service uses the data volume and this 8.3-safe metadata layout. Generation
numbers are fixed-width lowercase hexadecimal, split into high and low 32-bit
directory components:

```text
pkgstate/auth.bin
pkgstate/auth.new
pkgstate/auth.old
pkgstate/txn.bin
pkgstate/txn.new
pkgstate/gen/00000000/00000001/state.db
pkgstate/gen/00000000/00000001/root/<database-owned path>
```

The privileged call is
`package_service_recover(struct package_service_report *)`. The heap and data
volume must already be online, and package-owned executables must not be opened
until it returns `PACKAGE_SERVICE_STATUS_OK`. The source is picked up by the
kernel's existing `src/kernel/*.c` rule without another kernel source list; the
Makefile additions are the focused host target and its `verify` dependency.
On a normal (non-test-scenario) boot, `kernel_main` initializes the VFS, mounts
the data volume, and calls this entry point before native-process self-tests or
the shell. An absent data volume or entirely absent transaction state is
reported and leaves the package subsystem unavailable. Once any transaction
envelope exists, malformed or incomplete state is a boot refusal rather than a
fallback into package-owned execution. Focused host verification is wired as
`make package-service-tests`. There is intentionally no shell command or native
ABI endpoint that could report installation success.

With a prepared journal, the service loads the exact base and target database
lengths named by the journal, validates both candidates, and calls the recovery
decision core. A guest-specific 4 MiB database cap keeps two candidates, the
directory scratch list, and existing kernel allocations within the 16 MiB heap;
larger format-valid databases are refused as a resource bound. Each directory
is limited by the VFS list bound of 64 entries, each tree walk by 8,192 entries,
and path depth by the VFS depth bound. Candidate buffers, scratch storage, and
file handles are counted in the returned report and released on every exit.

When authority must be replaced, the service writes and closes `auth.new`, syncs,
moves an existing `auth.bin` to `auth.old`, syncs that fallback, moves
`auth.new` to `auth.bin`, and syncs the selection. Only then may it recursively
remove the discarded fixed generation and authority fallback. Those removals
are synced before `txn.bin` is unlinked, and a final sync makes journal removal
durable. The extra barrier guarantees that any durable no-journal state has
already durably reclaimed the discarded generation. A cleanup or sync failure
before journal removal leaves the journal present so a later recovery can retry;
it is never converted to success. If a crash leaves `auth.bin` absent, a
canonical `auth.old` is an eligible conservative fallback and is rewritten
through the same protocol. Malformed journals are never discarded automatically.

Cleanup is confined to the computed discarded generation directory and fixed
transaction envelope paths. Mutable user data remains outside
`pkgstate/gen/.../root` and is never traversed. The recovery entry point does
not create package files, accept package payload bytes, or modify dependency
state.

The separate privileged prepare entry point accepts an exact builder/database
pair only after the canonical equivalence verifier binds them. It refuses
fresh-store bootstrap, requires current authority to select the builder's base,
accounts for all target content against free space, and preflights path length,
depth, file/descendant conflicts, and the 64-entry recovery-list bound before
writing. The service also reloads the authoritative on-disk `state.db` and
requires byte equality with the builder's base rather than trusting a detached
caller view.

Prepare writes and syncs `txn.new` before creating the target and reads the
marker back byte-for-byte. It then writes each signed payload or verified
old-generation copy, writes `state.db`, verifies the complete immutable tree
through the normal recovery verifier, and syncs it. Only then does it rename
`txn.new` to `txn.bin` and sync again. It deliberately leaves `auth.bin`
unchanged, so success is not installation success and immediate recovery
chooses the old generation and removes the prepared target. A failure before
journal publication removes the target and temporary marker and syncs that
cleanup; a failure after publication leaves the journal for recovery. On the
short-name FAT backend, metadata paths work, but a database path whose
components cannot be represented by that backend is refused. General package-v3
paths and canonical installed modes still require the writable ext4/VFS path.

At boot, an unpublished `txn.new` with no `txn.bin` is a power-cut receipt. A
valid current authority makes its adjacent generation unselected, so recovery
removes that bounded tree and the temporary marker, syncs the cleanup, and then
validates the old generation normally. Coexisting temporary and published
journals are contradictory and refused without deletion.

The separate privileged commit entry point accepts only that fully published
prepared state: `txn.bin` must exist, while `txn.new`, `auth.new`, and `auth.old`
must not. It re-parses the journal, loads and fully verifies both immutable
generations, requires current authority to select the journal base, and asks the
normal recovery core to prove both the pre-switch `OLD` and encoded post-switch
`NEW` decisions before writing authority. It then uses the ordered replacement
protocol above. Once the new authority sync completes, `report.committed` is
true and reboot recovery must select the target even if later cleanup reports an
error. Platform trust, fresh-store bootstrap, repair construction, and durable
HTTPS staging now exist as separate bounded layers. There is still no shell or
native ABI endpoint or end-user lifecycle that can present this private service
as a completed installation feature.

Fresh-store bootstrap is a distinct privileged operation because the normal
journal format intentionally requires a real base database and never invents a
generation zero. It accepts only an authenticated fresh-install builder for
generation one, requires every file source to be an admitted signed payload,
and refuses every existing authority, journal, transient, or generation-one
tree. After namespace and free-space preflight, it writes canonical `auth.new`
and syncs that recovery receipt before creating any generation content. It then
stages and verifies the complete generation, syncs it, renames `auth.new` to
`auth.bin`, and syncs authority.

Boot recovery recognizes only a canonical generation-one `auth.new` when no
authority, fallback, or journal exists. An incomplete target is removed with the
receipt and recovery returns the store to the absent state; a complete target is
fully verified and promoted to `auth.bin`. A malformed receipt is retained and
refused rather than silently discarded. Host tests persist the receipt-only,
complete-target, and selected-authority prefixes independently and inject a sync
failure at each of the three durability boundaries.

## Removal, references, and repair

The explicit bit distinguishes requested roots from automatically selected
dependencies. Transactional removal clears a requested root by constructing a
new generation from every other explicit root and its exact provider edges.
Dependencies reachable from another root remain installed. Dependencies that
become unreachable are removed in the same generation. A direct attempt to
remove a package still required by another root is refused rather than silently
breaking the graph.

Verification reports missing, size-mismatched, digest-mismatched, and unowned
immutable paths deterministically. Repair builds another complete generation;
it accepts a replacement only when its length and SHA-256 match installed
authenticated metadata. Extra replacement paths are refused. Mutable user data
is outside both operations.

`package_builder_repair()` implements that bounded C repair construction. It
re-parses the authoritative installed database, copies every package/provider
edge/file metadata record into generation `current + 1`, and accepts a strictly
sorted replacement list. Each replacement must name exactly one owned path and
match its installed length and SHA-256; unchanged files remain individually
verified old-generation copy sources. The result passes through the same
canonical generation encoder and filesystem staging path as install/update.

Repair is the only prepared operation allowed to start with damaged base files;
the base database itself must still be present, canonical, and selected by
authority. If the complete repair target is published while the base remains
incomplete, recovery selects the repair target even before authority replacement
because rolling back would leave no usable generation. If the base is complete,
the ordinary pre-authority rollback rule still applies. Commit revalidates this
operation-specific decision before switching authority. Missing or bad repair
payloads fail before journal publication and leave the selected base unchanged.

## Host reference commands and tests

The CLI exposes byte-format construction and inspection only:

```sh
python3 tools/phipia-transaction.py build-database \
    --spec build/installed-state.json --output build/installed.db
python3 tools/phipia-transaction.py inspect-database build/installed.db
python3 tools/phipia-transaction.py inspect-authority build/installed.authority
python3 tools/phipia-transaction.py inspect-journal build/installed.journal

python3 tools/phipia_transaction_host_test.py

gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Iinclude \
    tools/package-state-host-test.c src/kernel/package_state.c \
    -o build/package-state-host-test
build/package-state-host-test

gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Iinclude \
    tools/package-service-host-test.c src/kernel/package_service.c \
    src/kernel/package_state.c -o build/package-service-host-test
build/package-service-host-test
```

The test creates only in-memory reference stores. It covers deterministic
encoding, zero reserved fields, interrupted commit on both sides of authority
replacement, incomplete-new fallback, update rollback, transactional removal,
shared dependency retention, ownership collision, cancellation cleanup,
low-space refusal, verify/repair, and preserved user data.

The parser C host test independently constructs canonical empty-base and two-package
target byte fixtures, then mutates reserved fields, table bounds, canonical
package text, provider bindings, ownership paths, content digests, authority,
and journal records. It exercises recovery before and after the authority switch,
incomplete-new fallback, absent-journal selection, corrupt-authority fallback,
and unrelated-authority refusal under `-Werror`.

The service host test links the real service against a bounded in-memory VFS and
heap. It covers no-journal verification, pre-authority rollback,
post-authority completion, exact file digest tamper, an unowned extra file,
authority repair ordering, recursive cleanup, no-complete-generation refusal,
bootstrap/prepare/commit/repair refusal, every bootstrap/prepare/commit durability
boundary, persisted bootstrap and authority-switch prefixes, journal-last
cleanup ordering, monotonic repository-floor advancement at every sync prefix,
corrupt floor refusal, durability failure with recoverable state, and zero live
handle/allocation census on every exit.

`package_control.c` connects install, update, removal, and repair to the native
client and Store without exposing private staging paths. It consumes only sealed
kernel upload handles, authenticates repository bytes with platform trust and
wall-clock freshness, enforces the durable repository floor, recovers the
authority-selected installed snapshot,
binds each payload to the exact planned repository digest and length,
re-authenticates packages, rebuilds the canonical generation, and enters this
service's bootstrap or prepare/commit path. The controller retains a prepared
session after a commit durability failure so the same token retries only the
authority commit. Its real signed-fixture host proof covers fresh install,
persisted-state update, wrong-owner and wrong-payload refusal, stale handles,
and allocation census.

The privileged native ABI exposes this core through a typed control handle.
Callers can enumerate exact plan items, attach sealed uploads, commit, retry a
prepared durability failure, duplicate the session handle, and release it by
final close or process teardown without gaining filesystem paths to private
staging. The `native-phip` QEMU path exercises this endpoint as a real Ring 3
client. It downloads a signed version-1 repository and payload over HTTPS,
commits and reboots from writable ext4, updates to signed version 2 and reboots
again, then refuses the signed version-1 downgrade while retaining generation
2. The kernel deliberately damages an immutable manifest, proves ordinary
snapshot and launch quarantine it, and uses `phip repair` to authenticate and
commit generation 3 from the signed repository. It then launches SDL 2.32.10's
upstream Chess Board application from the repaired authority, verifies its
bounded render loop and exact persistent SDL preference output, and checks the
retained image with `e2fsck`. Removal is exposed by the Phip client, and the
graphical Store's signed SDL Chess listing queues that same bounded client for
its real Install / Update action.
