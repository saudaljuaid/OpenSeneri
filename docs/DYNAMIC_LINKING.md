<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native dynamic ELF loading

Phipia can execute a bounded x86-64 PIE root with authenticated shared-object
dependencies in Ring 3. The Rust boundary admits and relocates `ET_DYN` bytes;
the C process loader reads the root, catalog, and libraries from the read-only
System volume, chooses non-overlapping biases, installs final permissions and
TLS, runs constructors, and runs destructors before process cleanup.

The `native-dynamic` QEMU scenario is the end-to-end proof. Its installed
format-v2 package contains `DYNROOT.APP`, `DYN/DYNROOT.CAT`, and
`DYN/DYNLIB.SO`. The root manifest authenticates the executable and catalog;
the catalog binds the exact `DYNLIB.SO` SONAME to the library SHA-256. Package
inspection independently checks those relationships before building the System
image. It starts two process instances before entering the scheduler. Each
instance must produce the lifecycle order below, the Ring 3 pass marker must
appear exactly twice, and the kernel must report a positive count of immutable
RX pages reused by the second process:

```text
PHIPIA DYNAMIC LIB INIT
PHIPIA DYNAMIC ROOT INIT
PHIPIA DYNAMIC RING3 PASS
PHIPIA DYNAMIC ROOT FINI
PHIPIA DYNAMIC LIB FINI
Phipia: dynamic ELF shared RX, private TLS and lifecycle passed
```

The kernel also requires a zero exit status, at least seven real Ring 3
syscalls, a scheduler transition, and a clean page/handle/thread census.

## Trust boundary

The loader never searches ambient directories. A dependency name must be one
canonical uppercase 8.3 SONAME present in the manifest-authenticated 2 KiB
catalog. The corresponding System resource is read once, hashed, compared with
the catalog entry, parsed, and copied into private preparation memory. Missing,
duplicate, ambiguous, cyclic, changed, or out-of-bound dependencies are
refused before a user mapping is entered.

This proof package uses legacy format 2, whose body has integrity hashing but
no publisher signature. Phipia's Ed25519-signed format-v3 repository and trust
policy are separate. The dynamic proof covers authenticated System-image
loading; publisher provenance and in-guest installation belong to the package
service boundary.

## Admission policy and bounds

The parser accepts little-endian System V x86-64 `ET_DYN` only. It decodes
integers from checked slices and checks every file, virtual, table, relocation,
and signed PC-relative range.

| Bound | Limit |
| --- | ---: |
| Root or library file read by the guest | 16 MiB |
| Program headers | 32 |
| `PT_LOAD` segments | 16 |
| Mapped span per object | 256 MiB |
| Dynamic entries | 256 |
| Direct `DT_NEEDED` entries | 16 |
| Objects in one process scope | 16 |
| Dynamic symbols | 4,096 |
| Main plus PLT RELA records | 256 |
| TLS image per object | 1 MiB |
| Constructor or destructor addresses | 256 each |
| Dynamic preparation allocation | 8 MiB per launch |

`PT_LOAD` records must be ordered, page-disjoint, congruent, and use only R,
R-X, or RW permissions. W+X, differently protected shared pages, wrapped
addresses, invalid extents, executable stacks, and entries outside executable
content are refused. Exactly one RW `PT_DYNAMIC` and one RW non-executable
`PT_GNU_STACK` are required. `PT_INTERP` and unknown program types are refused.
Optional `PT_GNU_RELRO` is page-rounded and installed read-only after eager
relocation. Optional `PT_TLS` is bounded, read-only, aligned, and copied into a
variant-II per-thread template.

The admitted metadata includes `DT_NEEDED`, `DT_SONAME`, SysV or GNU hashes,
RELA/PLT RELA, NOW binding, initializers/finalizers, TLS, and a zero `DT_DEBUG`
slot. Versioned symbols, GNU unique/IFUNC, audit/filter records, RPATH/RUNPATH,
`DT_SYMBOLIC`, text relocations, lazy PLT binding, COPY, `DT_REL`, and
`DT_RELR` are refused rather than approximated.

## Relocation, scope, and lifecycle

The admitted relocation subset is `R_X86_64_RELATIVE`, `R_X86_64_64`,
`R_X86_64_GLOB_DAT`, `R_X86_64_JUMP_SLOT`, checked `R_X86_64_PC32`,
`R_X86_64_TPOFF64`, and the all-zero `R_X86_64_NONE`. Every target must be
wholly inside an RW load, and target ranges may not overlap. Undefined strong
symbols fail; unresolved weak symbols become zero.

Global preemption scope and lifecycle topology are separate. The
relocator searches the root first, then libraries in breadth-first
`DT_NEEDED` load order. Constructors run dependencies first and the root last;
destructors run the exact reverse. Missing/ambiguous SONAMEs and cycles are
refused. Generated constructor and finalizer trampolines obey the SysV stack
contract even when `SYS_EXIT` arrives through the public SDK wrapper.

Dependency TLS blocks are laid out before the root so local-exec root TLS ends
immediately below `FS:0`. The proof uses both a library initial-exec variable
and a root local-exec variable. The resulting combined template is copied into
the process's writable/NX native-TLS mapping; all ELF template pages remain
read-only.

Relocations are applied only in kernel-private heap buffers. Root-executable,
R, RW, RELRO, TLS-template, trampoline, stack, and anonymous pages remain
private. Immutable executable pages belonging to authenticated DSOs enter a
bounded global cache keyed by the catalog SHA-256 and page offset. A cache hit
must also match all 4 KiB of prepared page content before the same physical
frame is mapped into another process.

Each process still installs its own user mapping and narrows its private
identity alias. `paging.c` reference-counts the corresponding supervisor alias,
so the live identity mapping stays read-only/NX until the last process releases
the executable frame. Shared frames are returned to the frame allocator only
after the cache reference count reaches zero. Partial admission, allocation,
mapping, lifecycle, fault, and exit paths flow through the ordinary
native-process teardown census.

## Verification

`make dynamic-elf-tests` constructs genuine bounded ELF byte fixtures for both
hash families and covers dependency ordering, TLS, lifecycle tables,
relocations, authentication, hostile bounds, W+X, unsupported tags, malformed
hashes, relocation target/overflow failures, and missing/ambiguous/cyclic
graphs. `tools/phipia_package_host_test.py` covers catalog/resource digest
agreement and changed-library/catalog refusal.

`make native-dynamic-proof` builds and inspects the real PIE/DSO, creates the
package and FAT32 images, and runs the host parser controls. The Ubuntu native
workflow runs `qemu-test-native-dynamic`; its serial assertions require two
Ring 3 passes, a positive shared-page count, per-instance TLS/lifecycle success,
and a clean final cache/resource census. It retains the root, library, catalog,
`readelf` reports, serial log, and Data image from the same commit.

## Scope

- There is no `PT_INTERP`, `ld.so`, `dlopen`, `dlsym`, lazy binding, symbol
  versioning, unload API, or environment-controlled search path.
- Only authenticated DSO RX pages use the global cache. Read-only data is not
  shared, and there is no page deduplication for root executables or anonymous
  process memory.
- The QEMU proof covers two concurrent process instances, one direct library
  per instance, and one initial thread per process. Host tests cover transitive
  graphs and hostile inputs.
- This does not add a hosted libc ABI or make arbitrary Linux shared objects
  compatible with Phipia's native ABI.
