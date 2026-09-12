<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Signed package repositories

`tools/phipia-repository.py` defines Phipia's deterministic repository index and
host lock format. It builds and verifies a canonical binary index, authenticates
it against an external immutable Ed25519 root, checks freshness and
repository-version floors, verifies downloaded package bytes, and emits an exact
dependency-first install plan and lock representation.

The guest parser/planner in `package_manager.c` consumes the same canonical
index and package-v3 metadata behind the fail-closed `package_trust.c` immutable-key
and Ed25519 callbacks. The platform table provisions those keys, the Phip
client fetches the index and payloads over HTTPS, and the package controller
binds them to staged generation commits. Store actions queue that same client
path. See
[`PACKAGE_MANAGER.md`](PACKAGE_MANAGER.md) for the integration boundary.

## Repository index version 1

An index starts with the eight-byte `PHIPIDX1` magic and a fixed 512-byte header.
All integers are little-endian. Fixed-width text is printable ASCII or the more
restrictive package identifier/path/SemVer grammar, NUL-terminated, and followed
only by zero tail bytes. The whole index is at most 32 MiB.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | `PHIPIDX1` magic |
| 8 | 2 | format version (`1`) |
| 10 | 2 | header bytes (`512`) |
| 12 | 4 | flags (`0`) |
| 16 | 8 | exact total bytes |
| 24 | 8 | monotonic repository version |
| 32 | 8 | generated-at Unix time |
| 40 | 8 | expires-at Unix time |
| 48 | 4 | minimum native ABI |
| 52 | 4 | maximum native ABI |
| 56 | 16 | architecture (`x86_64`) |
| 72 | 64 | canonical repository identifier |
| 136 | 8 | package-table offset (`512`) |
| 144 | 4 | package count |
| 148 | 4 | package-record bytes (`512`) |
| 152 | 8 | relation-table offset |
| 160 | 4 | relation count |
| 164 | 4 | relation-record bytes (`128`) |
| 168 | 32 | SHA-256 of all bytes after the header |
| 200 | 32 | SHA-256 key ID of the raw root public key |
| 232 | 64 | Ed25519 signature |
| 296 | 2 | signature algorithm (`1` = Ed25519) |
| 298 | 2 | signature bytes (`64`) |
| 300 | 212 | reserved zero bytes |

The signature covers the complete index with bytes 232–295 zeroed. The root
public key is absent from the index: callers supply an immutable
trusted root, and its SHA-256 key ID must match the signed header. An embedded
key, an unknown key, an altered signature, or a missing real Ed25519 backend is
never accepted as trust.

Generated-at must be earlier than expires-at. Verification refuses an index
before generated-at, at or after expires-at, or below a caller-provided immutable
minimum repository version. Time values and version floors are explicit API/CLI
inputs in tests and automation, so resolution itself adds no timestamps.

## Package and relation records

An index has 1–1,024 package records, sorted by the ASCII bytes of package
identifier and version. Exact identifier/version pairs, download paths, and
SemVer versions that differ only in ignored build metadata are unique.

| Package-record offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 64 | lowercase dotted package identifier |
| 64 | 64 | canonical SemVer 2.0.0 version |
| 128 | 128 | package-root-relative download path |
| 256 | 8 | exact package bytes, 1 through 256 MiB |
| 264 | 32 | package SHA-256 |
| 296 | 32 | package-v3 publisher key ID |
| 328 | 4 | dependency start record |
| 332 | 4 | dependency count |
| 336 | 4 | conflict start record |
| 340 | 4 | conflict count |
| 344 | 4 | provider start record |
| 348 | 4 | provider count |
| 352 | 160 | reserved zero bytes |

Each package owns at most 64 dependencies, 64 conflicts, and 64 virtual
providers. Its relation ranges are contiguous in dependency/conflict/provider
order, and the global relation table contains no gaps or unowned records.

A dependency or conflict record stores an identifier in bytes 0–63, a canonical
constraint in 64–119, and eight reserved zero bytes. Constraints are `*` or
comma-separated clauses using `=`, `>`, `>=`, `<`, `<=`, `^`, or `~` with a full
SemVer value. Provider records use the same layout but store the provided SemVer
in bytes 64–119. Relations are unique and sorted. A package cannot list the same
identifier as both dependency and conflict, or redundantly provide its own ID.

The root signature authenticates download size, SHA-256, relative path, and the
package publisher key ID. `verify-download` checks size before digest. Package-v3
signature verification remains the package admission layer's responsibility;
the repository lock preserves its expected publisher key ID for that handoff.

## Deterministic resolution and locks

Requirements use `identifier@constraint`; omitting `@constraint` means `*`.
Resolution checks repository architecture and ABI first. For each dependency it:

1. Finds direct packages and declared virtual providers whose package/provided
   version satisfies every canonical constraint.
2. Refuses multiple distinct provider package IDs. Multiple versions of one
   package ID are tried from highest SemVer precedence to lowest.
3. Uses bounded deterministic backtracking to reconcile constraints.
4. Refuses unsatisfied requirements, selected-package conflicts, dependency
   cycles, or more than the configured package bound (128 by default, 256 hard
   maximum).
5. Topologically orders dependencies before consumers, with ASCII package ID as
   the stable order for independent nodes.

The lock is canonical ASCII JSON: sorted object keys, no insignificant
whitespace, and exactly one trailing newline. It records lock format `1`, the
repository identity/version/index SHA-256/root key ID, selected architecture and
ABI, normalized requested constraints, and the exact dependency-first package
list. Every selected item pins identifier, version, path, bytes, SHA-256, and
publisher key ID. Repeating resolution against the same authenticated index and
inputs produces identical plan objects and lock bytes.

## Host commands

A JSON build specification mirrors the fields above. Package entries use
`dependencies`, `conflicts`, and `provides` lists; `sha256`,
`publisher_key_id`, and optional `root_key_id` are 64 hexadecimal digits.

```sh
python3 tools/phipia-repository.py build \
    --spec repository/index.json \
    --signing-key keys/repository-root-private.pem \
    --output build/repository/index.sri

python3 tools/phipia-repository.py inspect \
    --trusted-root keys/repository-root-public.pem \
    --minimum-version 42 \
    build/repository/index.sri

python3 tools/phipia-repository.py resolve \
    --trusted-root keys/repository-root-public.pem \
    --minimum-version 42 --abi 1 \
    --lock-output build/repository/desktop.lock \
    build/repository/index.sri org.phipia.desktop@^3.0.0

python3 tools/phipia-repository.py verify-download \
    --trusted-root keys/repository-root-public.pem \
    --identifier org.phipia.desktop --version 3.0.0 \
    --file downloads/desktop-3.0.0.spk \
    build/repository/index.sri
```

Root keys may be unencrypted PEM or raw 32-byte private-seed/public-key files.
The tool reuses the package-v3 SemVer, canonical identifier/path, bounded file,
and Ed25519 helpers. Python's `cryptography` is optional for legacy tooling but
mandatory for signing or accepting repository indexes. When unavailable, the
repository operations explicitly fail closed.
