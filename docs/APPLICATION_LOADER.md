<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native application loader and security model

`native_process_spawn()` accepts a manifest path on the read-only System
volume. The privileged `native_process_launch_installed()` path accepts only
the canonical `pkgstate/gen/<high>/<low>/root/...` generation namespace on
Data. In either case the executable must be a safe basename beside its
manifest. The loader reads both files once, passes their byte slices to the safe
Rust validator, and installs mappings only after validation succeeds. This
allows a package-service-verified immutable generation to launch after reboot
without copying its executable back to System.

Version 1 admits x86_64 little-endian static `ET_EXEC` images. It permits at
most 32 program headers and 16 `PT_LOAD` segments inside
`0x0000400000000000..0x0000400100000000`. It rejects interpreters, dynamic
segments, relocation sections, an executable stack, W+X loads, unsupported
program types, invalid alignment, wrapped or out-of-file ranges, overlapping
page ranges, an entry outside executable content, invalid TLS, files over
16 MiB, and manifest/executable digest disagreement.

The loader allocates a private page table, copies admitted file bytes through
temporary checked aliases, zeroes BSS, installs final W^X permissions, and
never revalidates the file after admission. Executable pages are immutable.
Anonymous pages, TLS, stacks, and surfaces have distinct mapping kinds. The
main stack is 16 pages with an unmapped guard; created threads receive their
own bounded stack and guard.

Initial registers pass `argc`, `argv`, and environment in `RDI`, `RSI`, and
`RDX`. The deterministic stack contains the manifest arguments, a
`PHIPIA_ABI=1` environment entry, and an auxiliary vector containing page size,
entry address, Phipia ABI version, and TLS image/size/alignment records. All
padding is zero and the resulting stack obeys the x86_64 alignment contract.

Every partial failure unwinds installed pages, aliases, frames, handles, TLS,
and address-space state. A userspace exception marks only its thread/process as
faulted; the scheduler restores the kernel CR3 and FS base before cleanup. A
failed image or crashed application must leave the native resource census equal
to the pre-launch census.

## Dynamic native images

When static admission refuses only the ELF type, the loader reuses the
manifest/executable authentication boundary and attempts bounded `ET_DYN`
admission. A root with dependencies must name an authenticated System-volume
catalog. Each exact SONAME is resolved relative to that catalog's packaged
resource directory, hashed before parsing, and loaded once in breadth-first
`DT_NEEDED` order.

Installed Data-volume images are currently restricted to static executables;
dynamic dependency catalogs remain System-volume-only until package generation
resolution binds every DSO to the same authenticated installed authority.

Relocation occurs in private kernel heap buffers. The mapper then installs
non-overlapping root/library mappings with final R, RX, RW, and RELRO
permissions, creates the combined variant-II TLS template, removes writable
executable aliases, and enters constructors through a generated RX trampoline.
`SYS_EXIT` is redirected once through the reverse destructor trampoline before
ordinary teardown. See [`DYNAMIC_LINKING.md`](DYNAMIC_LINKING.md) for admitted
relocations, trust relationships, bounds, evidence, and deliberate limits.
