<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native application manifests and packages

`tools/phipia-package.py` builds, inspects, and installs Phipia application
packages without network access. JSON is only the host-side build input. The
guest validates a deterministic 1,024-byte binary manifest at the Rust
admission boundary.

## Manifest fields

| JSON field | Version 1 rule |
| --- | --- |
| `name` | Required ASCII display name, at most 31 bytes. |
| `identifier` | Required uppercase-normalized 1–8 character FAT short-name component. |
| `executable` | Required System-volume 8.3 path, at most 15 bytes in the binary field. |
| `abi_version` | Must be `1`. |
| `memory_limit` | Page multiple from 64 KiB through 256 MiB. |
| `max_handles` | 1–128, also bounded by the kernel table. |
| `max_threads` | 1–8. |
| `capabilities` | Names from `console`, `system-read`, `data-read`, `data-write`, `time`, `entropy`, `window`, `input`, `network`, `threads`, `audio`, and privileged `packages`. |
| `resource_directory` | Optional immutable System 8.3 directory identifier. |
| `data_namespace` | Required 1–8 character directory on Data; relative application paths are rooted here. |
| `icon` | Optional System 8.3 path. |
| `arguments` | At most eight nonempty printable-ASCII strings of at most 31 bytes each. They are opaque program arguments, so URL and option punctuation are permitted. |
| `resources` | Optional list of `{ "path": "NAME.EXT", "source": "host/path" }` records. Sources are resolved relative to the JSON file. |

The binary manifest starts with `PHIPIAA1`, format version and size, ABI
version, limits, capability bits, fixed-width text records, the executable
SHA-256, and zero-filled reserved space. Nonzero reserved bytes, unterminated
text, nonzero text tails, unknown capabilities, or unused argument records are
named refusals.

## Legacy package containers and installation

An `.SPK` file contains a 64-byte `PHIPPKG1` header, the 1,024-byte manifest,
and the exact static executable. Container version 1 ends there. Version 2
adds up to 13 deterministic resource records; each has a fixed 32-byte header,
an 8.3 path, a byte length, zero reserved bytes, and its exact payload. The
package header records all component lengths and the SHA-256 of the complete
body. The manifest contains a second digest of the executable. Inspection
verifies both digests and every resource boundary before returning content.
The legacy encoder remains the default. With the same manifest, executable,
and resources, it emits the exact version 1 or version 2 bytes produced before
format version 3 was added.

Typical commands are:

```sh
python3 tools/phipia-package.py build \
    --spec apps/my-app/manifest.json \
    --executable build/my-app/MYAPP.APP \
    --output build/my-app/MYAPP.SPK
python3 tools/phipia-package.py inspect build/my-app/MYAPP.SPK
python3 tools/phipia-package.py install-system \
    --output build/my-app/system.raw build/my-app/MYAPP.SPK
```

Installation rejects symlink inputs, duplicate identifiers, malformed short
names, package-length disagreement, and either digest mismatch. It atomically
replaces its host output through a sibling temporary file. The resulting FAT32
System image contains `IDENT.MAN`, the executable, and any packaged resource
files under `resource_directory`. Resource directories are one cluster and one
level deep by format contract. A process can address its own resources using
relative System paths; the kernel prefixes the admitted directory, so it cannot
escape into another package. The loader creates the named Data directory, and
the native path layer prevents `..`, absolute paths, backslashes, drive syntax,
and cross-namespace access.

## Package container version 3

Version 3 is the signed repository container built by host tooling and admitted
by the guest package manager. It does not reuse or reinterpret the version 1/2
header. It uses the same `PHIPPKG1` magic,
a 512-byte header, fixed-size canonical tables, and contiguous file payloads.
All integer fields are little-endian. Package and file sizes are checked before
slicing or allocating from their declared values.

The header contains:

| Field | Version 3 rule |
| --- | --- |
| Format/header/total sizes | Version `3`, header size 512, and an exact total byte count no larger than 256 MiB. |
| Architecture | NUL-terminated fixed field; currently exactly `x86_64`. |
| ABI range | Inclusive, ordered, positive 32-bit `abi_min` and `abi_max`. |
| Package identity | Canonical lowercase dotted identifier of at most 63 ASCII bytes. |
| Name/publisher | Nonempty printable ASCII strings of at most 63 bytes. |
| Version | Canonical SemVer 2.0.0 string of at most 63 bytes. |
| Capabilities | The same named, bounded bit set as the legacy manifest. Unknown bits are refused. |
| Table locations | File, dependency, conflict, and payload regions must be contiguous in that order. Offsets, counts, and record sizes have one canonical encoding. |
| Content digest | SHA-256 over every byte after the 512-byte header. |
| Publisher key ID | SHA-256 of the raw 32-byte Ed25519 public key. The key itself is not embedded as a trust root. |
| Signature envelope | Algorithm ID `1` (Ed25519), declared length 64, publisher key ID, and a 64-byte signature over the entire package with the signature field zeroed. |
| Reserved bytes | Must be zero. Header flags are currently zero. |

The exact header layout is:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | `PHIPPKG1` magic |
| 8 | 2 | format version |
| 10 | 2 | header bytes |
| 12 | 4 | flags |
| 16 | 8 | total bytes |
| 24 | 8 | file-table offset |
| 32 | 4 | file count |
| 36 | 4 | file-record bytes |
| 40 | 8 | dependency-table offset |
| 48 | 4 | dependency count |
| 52 | 4 | dependency-record bytes |
| 56 | 8 | conflict-table offset |
| 64 | 4 | conflict count |
| 68 | 4 | conflict-record bytes |
| 72 | 8 | payload offset |
| 80 | 8 | payload bytes |
| 88 | 4 | minimum ABI |
| 92 | 4 | maximum ABI |
| 96 | 16 | architecture |
| 112 | 64 | package identifier |
| 176 | 64 | display name |
| 240 | 64 | semantic version |
| 304 | 64 | publisher identity |
| 368 | 8 | capability bits |
| 376 | 32 | content SHA-256 |
| 408 | 32 | publisher key ID |
| 440 | 64 | Ed25519 signature |
| 504 | 2 | signature algorithm (`1` = Ed25519) |
| 506 | 2 | signature bytes (`64`) |
| 508 | 4 | reserved zero bytes |

There are at most 256 file records. Each 256-byte record owns one unique,
package-root-relative POSIX path, sorted by its ASCII bytes. It records a kind
(`executable`, `library`, `resource`, or `icon`), canonical mode (`0444` or
`0555`), exact contiguous payload offset and length, SHA-256, and a library
SONAME where applicable. A file is nonempty and at most 64 MiB. Backslashes,
absolute paths, empty components, `.`/`..`, gaps, overlaps, duplicate paths,
unknown flags, and nonzero reserved bytes are refused.

A file record stores path at bytes 0–127; kind, flags, and mode at 128–135;
payload offset and length at 136–151; SHA-256 at 152–183; SONAME at 184–247;
and eight reserved zero bytes at 248–255. A dependency or conflict record
stores identifier at bytes 0–63, constraint at 64–119, and eight reserved zero
bytes at 120–127. Every text field is NUL-terminated with a zero-filled tail.

Dependency and conflict tables each contain at most 64 unique package
identifiers. Records are sorted by identifier and constraint. Constraints are
`*` or comma-separated canonical clauses using `=`, `>`, `>=`, `<`, `<=`, `^`,
or `~` and a full SemVer value. The same identifier cannot be both a dependency
and a conflict.

Version 3 JSON names each packaged file explicitly; sources are resolved
relative to the JSON specification:

```json
{
  "format": 3,
  "architecture": "x86_64",
  "abi_min": 1,
  "abi_max": 1,
  "identifier": "org.phipia.example",
  "name": "Example",
  "version": "1.0.0",
  "publisher": "Phipia Project",
  "capabilities": ["console", "system-read"],
  "dependencies": [
    {"identifier": "org.phipia.libc", "constraint": "^1.0.0"}
  ],
  "conflicts": [],
  "files": [
    {"path": "bin/example", "kind": "executable", "source": "EXAMPLE.APP"},
    {"path": "lib/libexample.so.1", "kind": "library",
     "soname": "libexample.so.1", "source": "libexample.so.1"}
  ]
}
```

Build and inspect with a real Ed25519 private/public key pair:

```sh
python3 tools/phipia-package.py build --format 3 \
    --spec apps/example/package-v3.json \
    --signing-key keys/repository-ed25519-private.pem \
    --output build/example/example.spk
python3 tools/phipia-package.py inspect \
    --trusted-key keys/repository-ed25519-public.pem \
    build/example/example.spk
```

The key files may be PEM, or raw 32-byte private-seed/public-key files. Signing
and verification use Python's `cryptography` Ed25519 implementation. The
verification workflow installs that reviewed system package and requires the
real signature acceptance and negative tests to execute; an unavailable crypto
backend is a verification failure. The binary field parser itself has no
cryptographic dependency, but version 3 content is never returned as accepted
without signature verification. If real Ed25519 support is unavailable, no
trusted key is supplied, the key ID is unknown, or verification fails, the
operation is explicitly refused. There is no checksum-as-signature or
embedded-key fallback.

The existing `install-system` command writes the legacy FAT32 System image and
therefore refuses version 3 after authenticating it. The bounded guest
`package_manager.c` parser admits these bytes through caller-supplied trust
callbacks and binds them to signed repository metadata. The pinned guest
Ed25519 verifier and validated immutable key-table provider implement those
callbacks. The generation builder and package service consume the authenticated
extraction views, and the Phip client drives their install/update/remove/repair
transactions from signed HTTPS repository bytes. The ELF shared-library loader
consumes the older installed
System package/catalog profile independently. See
[`PACKAGE_MANAGER.md`](PACKAGE_MANAGER.md).
