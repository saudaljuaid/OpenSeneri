<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Rust in Phipia

Rust has two separate roles in Phipia: validating selected byte
streams at the kernel boundary, and supporting freestanding native
applications through the public ABI. C and x86_64 assembly continue to own
hardware, page tables, interrupt entry, context switches, and kernel resource
lifecycles.

## Boundary

Rust is used where attacker- or fixture-controlled lengths, offsets, counts,
and encodings dominate the risk. Today it validates:

| Module | Input |
| --- | --- |
| `font.rs`, `ui_font.rs` | packed bitmap-font headers and glyph data |
| `logo.rs` | the deterministic runtime Phipia S-mark stream |
| `fat16.rs`, `linux_fat16.rs` | FAT16 geometry, chains, root entries, and payload digests |
| `fat32.rs` | FAT32 BPB/FSInfo geometry, cluster classes, paths, names, and directory entries |
| `ext4.rs` | checked ext4 superblock, group-descriptor, namespace, journal-inode map, and JBD2 profile admission through pinned `ext4plus` |
| `elf64.rs`, `linux_elf64.rs` | bounded native and static BusyBox ELF64 records |
| `abi.rs` | the explicit C/Rust calling boundary and embedded assets |

Rust returns checked scalar metadata and fixed-size records. It does not own
allocation, mappings, DMA, devices, processes, or teardown. C supplies bounded
slices only after it owns the underlying memory; Rust never retains a borrowed
pointer across the call.

For v1.1.0, `linux_fat16.rs` accepts at most the three frozen canonical root
entries, validates each entry against its own byte count and digest contract,
and returns only the requested profile. It does not expose a path lookup, VFS,
or reusable file API. The profile-specific ELF parser then validates the whole
static image before C allocates or maps process pages. The checked C/Rust
copy-out boundary validates the complete cat read destination before any byte
is copied, so invalid or cross-boundary ranges cannot produce partial input.

For v2.0.0, `fat32.rs` validates pointer-free metadata before C publishes it.
The C filesystem remains responsible for NVMe sessions, mount and handle
generations, FAT and directory mutation, cache ownership, allocation, and
teardown. The kernel accepts only the deterministic geometry and the documented
ASCII 8.3 subset; malformed or unsupported long-name records are named
refusals. See [`FAT32.md`](FAT32.md).

## Freestanding build

`src/rust/lib.rs` is compiled as a static library for
`x86_64-unknown-none` with:

- `#![no_std]` and `panic=abort`;
- static relocation and no red zone;
- no MMX, SSE, AVX, or floating-point kernel state;
- abort-only panics routed to `console_panic`, with no unwinder or exception
  personality linked;
- warnings denied;
- `unsafe_op_in_unsafe_fn` denied;
- linker rejection of unexpected sections, relocations, GOT growth, and W+X.

The resulting archive links directly into the kernel ELF. There is no kernel
allocator supplied by Rust, unwinder, hosted runtime, dynamic loader, or Rust
kernel entry point.

## Native application crate

`rust/phipia` is a separate `#![no_std]` application crate. It supplies the
native entry shim, panic-to-exit behavior, a page-mapping global allocator,
typed handle cleanup, and wrappers for files, monotonic time, sleeping,
entropy, event waits, Phipia windows and surfaces, DNS, TCP, threads, futexes,
and FS-base TLS control. It uses `alloc` without a `std` runtime.

`apps/native-rust` builds with Cargo's `x86_64-unknown-none` target in locked
offline mode. Its static `ET_EXEC` image uses the same linker and manifest
rules as C applications. The installed guest allocates a `Vec`, obtains
entropy, creates and joins a native thread, writes `RUSTAPP/RUST.TXT` on Data,
sleeps to a monotonic deadline, and exits with a clean resource census.

## Safety rules

- Keep pointer construction and raw slice creation in `abi.rs`.
- Every `unsafe` block names the condition that makes it sound.
- Validate lengths and arithmetic before indexing or slicing.
- Refuse truncated, overlapping, wrapped, noncanonical, executable-writable,
  or otherwise ambiguous input instead of repairing it.
- Return errors without partially publishing decoded state.
- Test both acceptance and deliberate corruption using host-side Rust tests and
  installed QEMU proofs.

## Boundary rule

Rust cannot make port I/O, MMIO, page-table mutation, register programming, or
context switching safe; those operations remain `unsafe` regardless of
language. The Rust boundary is reserved for structured, untrusted input where
checked parsing reduces risk. Machine-facing work stays in C and assembly.

## Adding a parser

1. Define the exact accepted byte shape and maximum sizes.
2. Add a safe Rust parser with named refusals and host tests.
3. Expose the smallest pointer-free result through `abi.rs`.
4. Let C retain resource ownership and lifecycle control.
5. Add an installed proof and a negative control capable of breaking it.
6. Run `make verify` and the affected QEMU scenarios.
