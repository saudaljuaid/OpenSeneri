<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Package admission and planning

`src/kernel/package_manager.c` is the bounded guest-side parser and planner for
signed repository-index v1, signed package v3, and installed-database v1 bytes.
It is the parser and resolver used by privileged package services.

## Trust boundary

The core never treats a digest as a signature and never accepts a key embedded
by the object being verified. Its caller supplies an immutable key lookup and a
real Ed25519 verifier. Repository and package admission fail closed when either
callback is absent, the SHA-256 ID of the returned raw public key differs from
the signed key ID, the key is unknown or revoked, or signature verification
fails. The verifier receives the complete object plus the one 64-byte signature
range that must be treated as zero.

Repository admission additionally enforces the caller's wall-clock freshness,
minimum repository version, native ABI, and `x86_64` policy. Package admission
binds exact downloaded bytes to the signed repository size and SHA-256 before
checking the package's publisher signature, identity, version, ABI, dependency
and conflict records, file layout, per-file digest, mode, kind, and SONAME.

`package_trust.c` supplies the guest Ed25519 verifier and immutable key-table
lookup. Its `PHIPKEY1` platform boundary copies a canonical, bounded table into
caller-owned storage, checks the exact version, length, record size, reserved
bytes, and record-table SHA-256, and then admits only sorted unique keys whose
SHA-256 IDs match. The digest detects corruption; trust comes from the platform
supplying these bytes immutably, never from that unkeyed digest. Any refusal
clears the whole destination table. Key records preserve explicit
trusted/revoked state and reject non-canonical or pure low-order public keys.
Provisioned keys must be generated Ed25519 keys. Verification incrementally
substitutes the 64-byte zero range and narrows pinned Monocypher 4.0.3's
cofactored profile with canonical encoding, negative-zero, pure-low-order, and
`S < L` requirements.
Signature bytes are not a uniqueness identifier. No signing or private key
enters the guest.

`platform/package-trust.json` is the default development/CI provisioning
source. `tools/make-package-trust.py` derives key IDs, sorts records, emits the
canonical table, audits it independently, and generates a const C array linked
into kernel read-only data. Boot admits that asset before any package state is
recovered; an invalid or empty platform table stops boot. Production builds
must set `PACKAGE_TRUST_SPEC` to their own public-key-only specification. The
default roots deliberately match the deterministic signed lifecycle fixtures
and are not a production release trust policy.

## Bounds and resolver behavior

The binary bounds match the canonical host formats:

- repository: 32 MiB, 1,024 packages, and 64 relations of each kind per
  package;
- package: 256 MiB, 256 files, 64 dependencies, and 64 conflicts;
- install plan: 128 packages, 256 provider bindings, and dependency depth 16;
- search result: 64 entries;
- installed state: the fixed limits in `package_state.h`.

Resolution selects one deterministic provider identity, tries that provider's
versions from highest SemVer precedence to lowest, backtracks within the fixed
bounds, applies conflicts, and iteratively emits dependencies before consumers.
Distinct packages providing the same requested identity are refused as
ambiguous. Missing constraints, cycles, and graphs beyond the plan/depth bounds
have separate statuses.

The planner checks publisher keys for every selected package, prevents implicit
downgrades unless policy explicitly permits them, and refuses publisher-key
rotation rather than silently changing trust. Removal starts from an explicit
installed root and removes only dependencies no longer reachable from another
explicit root; shared dependencies stay installed. Removal plans do not delete
mutable application data. Every successful plan retains the exact requested
target and its uniquely selected provider-package root separately from the
dependency-first changed-item list so a generation builder can preserve or
assign the explicit-root flag correctly, including virtual-capability requests. An
already-present automatic dependency is promoted by a successful zero-download
install plan rather than being misreported as explicitly installed.

Planning is serialized. Its roughly 9 KiB graph workspace is static rather than
placed on Phipia's 16 KiB syscall stack, reentrant entry is refused, candidate
selection uses bounded catalog scans instead of a per-recursion candidate
array, and topological ordering is iterative. The input byte buffers must remain
immutable and live while returned views or plans are used because text and
digest fields are slices into those authenticated buffers.

## Authenticated staging views

After `package_manager_package_open()` admits exact repository-bound and
publisher-signed bytes, `package_manager_package_file()` exposes one immutable
path, kind, mode, SONAME, SHA-256, and payload slice at a time. The accessor
rechecks record bounds, path and kind invariants, and the payload digest before
returning it. Dependency and conflict accessors expose the exact canonical
relations from those same admitted bytes. Out-of-range and structurally forged
views fail closed; no accessor writes a filesystem or reports installation.

These slices are the extraction boundary for a privileged generation builder.
They remain valid only while the admitted package buffer is live and unchanged.
`package_manager_plan_dependency_binding()` also exposes every planned
consumer's exact dependency identifier, constraint, and unique provider
identity. It revalidates the plan item against the authenticated repository and
uses the resolver's bounded ambiguity rule without adding another large binding
array to the already stack-sensitive plan. The provider may be an unchanged
installed package and therefore need no plan item of its own.

`package_builder.c` implements the allocation-free metadata half of the
generation boundary in a caller-owned heap workspace. It re-authenticates the
repository and every package, recomputes the supplied plan, merges unchanged
installed records with changed packages, assigns the requested explicit root,
prunes automatic dependencies that are no longer reachable, refuses cycles and
file ownership collisions, and emits canonical package, provider-edge, and file
arrays for `package_generation_encode()`. Each file retains an exact old-file or
signed-payload source. The builder performs no filesystem I/O; staging, durable
commit, and cleanup remain separate privileged-service work.

## Verification and integration

`make package-manager-tests` builds real deterministic Ed25519 packages and
indexes with the canonical host tools, verifies them with the independent host
crypto backend, and passes those exact bytes through the production
freestanding C trust provider and parser. `make package-trust-tests` adds a
fixed cross-provider vector and strict encoding, revocation, and table tests. It
covers package/repository admission, search, dependency-first install planning,
installed-state no-op and removal, unknown/revoked keys, unavailable crypto,
signature rejection, digest changes, freshness, rollback, ABI mismatch,
unsatisfied dependencies, conflicts, ambiguous providers, cycles, backtracking,
and the dependency-depth bound.

`package_service.c` recovers generations, durably prepares a complete builder
result without advancing package authority, and separately commits only a
revalidated prepared journal through an ordered authority switch. A distinct
generation-one bootstrap uses only admitted signed payload sources and a durable
authority receipt rather than inventing a generation-zero database. A bounded
repair builder preserves the installed graph and metadata while replacing only
sorted owned paths whose authenticated bytes match the installed length and
digest. The SDK's `phipia_package_fetch_stage()` supplies bounded HTTPS
streaming, incremental SHA-256, temporary-file cleanup, and two-barrier atomic
publication for inert repository/package bytes. Platform trust provisioning is
also wired at boot. The `packages` native capability and typed upload handle now
provide a non-path-based HTTPS ingress: four kernel-owned slots accept 4 KiB
writes up to the VFS's real 16 MiB file bound, seal only to an exact privileged
caller-supplied length and digest, flush before use, and clean up on final close
or process exit. Sealing proves stable bytes, not the authority of those
expected values. Transaction-control calls must consume the handle while
binding it to an admitted repository record. `package_control.c` now provides
that privileged internal boundary for install, update, removal, and repair: it authenticates a
sealed repository through platform trust and wall-clock policy, recovers and
snapshots installed authority, exposes a bounded eight-package plan, copies
only exact repository-bound sealed payloads, re-authenticates every package,
rebuilds canonical state, and invokes bootstrap or prepare/commit. Its signed
host lifecycle also proves retry of a prepared commit after a durability error
and zero controller allocations after close or refusal. Before repository
admission, install and repair read a checksummed, monotonic repository-version
floor from Data. Every accepted signed repository version is durably advanced
before package staging; older signed metadata is then refused after reboot.
The floor's current and crash-leftover candidates fail closed when malformed
and preserve the greatest accepted version across every write/sync/rename
prefix. The privileged native
ABI exposes this session as a typed control handle with item, attach, commit,
duplicate, final-close, and process-teardown semantics; repository and package
upload handles remain independently closeable after the controller copies them.
The `phip` client and Store presentation drive the same controller over HTTPS,
and the native QEMU lifecycle persists the resulting generations on writable
ext4.
