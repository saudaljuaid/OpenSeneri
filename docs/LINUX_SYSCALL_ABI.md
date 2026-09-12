<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Measured Linux x86_64 syscall boundary

Phipia runs three pinned static BusyBox programs as bounded compatibility
proofs:

- v0.8.0: `busybox echo PHIPIA`;
- v0.9.0: `busybox uname -s`;
- v1.1.0: `busybox cat`.

Version 2.0.0 preserves those executables and the bounded interactive
`linux cat` lifecycle unchanged while moving the current system-volume path to
immutable FAT32. This does not establish a broad userspace ABI.

This is not POSIX, a native Phipia ABI, or a general Linux personality. Each
profile has a distinct executable, configuration, storage contract, initial stack,
syscall allowlist, output sink, lifecycle, and checksum.

## Entry and return

The kernel programs and reads back `IA32_EFER.SCE`, `IA32_STAR`, `IA32_LSTAR`,
and `IA32_FMASK`. On `syscall`, assembly saves the user stack, closes the
interrupt window, switches to the validated TSS kernel stack, and calls C only
after authenticating the active process, generation, private CR3, CPL3 entry,
and executable range.

The syscall number is in `RAX`; arguments use `RDI`, `RSI`, `RDX`, `R10`, `R8`,
and `R9`; the signed result returns in `RAX`. Refusals use negative Linux errno
values. Return uses a checked `iretq` after validating the complete user frame
and lifecycle state. `int 0x80`, the native proof gate, direct handler calls,
and `sysretq` cannot satisfy this boundary.

## Echo profile

The committed trace is `userspace/busybox/syscall-allowlist.txt`.

| Number | Call | Accepted operation |
| ---: | --- | --- |
| 1 | `write` | fd 1, exactly `PHIPIA\n`, once |
| 9 | `mmap` | the measured anonymous guard and RW page only |
| 11 | `munmap` | the preceding measured RW page only |
| 12 | `brk` | fixed-base query and one exact 8192-byte growth |
| 158 | `arch_prctl` | measured `ARCH_SET_FS` address only |
| 218 | `set_tid_address` | measured writable address only |
| 231 | `exit_group` | status zero only |

All other numbers return `-ENOSYS` without widening process state.

## Uname profile

The normalized sequence in
`userspace/busybox/uname-syscall-sequence.txt` is:

```text
arch_prctl
set_tid_address
uname
ioctl
writev
exit_group
```

The exact arguments are pinned in
`userspace/busybox/uname-syscall-allowlist.txt`. `ioctl` is only the measured fd
1 `TIOCGWINSZ` probe returning `-ENOTTY`; `writev` accepts only the measured
two-element `Linux\n` output. Neither creates a terminal, descriptor table, or
general vector-I/O service.

Linux x86_64 syscall 63 writes one complete 390-byte `new_utsname` record:

| Field | Offset | Width |
| --- | ---: | ---: |
| `sysname` | 0 | 65 |
| `nodename` | 65 | 65 |
| `release` | 130 | 65 |
| `version` | 195 | 65 |
| `machine` | 260 | 65 |
| `domainname` | 325 | 65 |

The whole destination must be canonical, mapped, user-owned, and RW/NX before
any byte changes. Null, wrapped, supervisor, executable, read-only, unmapped,
MMIO, DMA, page-table, guard, foreign, and cross-resource ranges return
`-EFAULT` with no partial output. The record is immutable and Phipia-owned;
there is no hostname mutation or UTS namespace.

## Cat profile

The measured executable is 38,632 bytes with SHA-256
`8191596A22778B575942895071A2E50CCEEE0F82F4D88B6D986584CE0914FC3E`.
It is static `ET_EXEC`, enters at `0x40000100107a`, and has four `PT_LOAD`
segments: R at `0x400001000000`, RX at `0x400001001000`, R at
`0x400001008000`, and RW/NX from `0x40000100a1a0`. The guarded initial stack
contains `argc=2`, `argv={"busybox", "cat", NULL}`, empty `envp`,
`AT_PAGESZ=4096`, and `AT_NULL`.

The measured clean sequence is:

```text
arch_prctl(ARCH_SET_FS, 0x40000100b178) = 0
set_tid_address(0x40000100b324) = positive-proof-tid
read(0, 0x400001203f00, 4096) = delivered-line-bytes
write(1, 0x400001203f00, delivered-line-bytes) = delivered-line-bytes
read(0, 0x400001203f00, 4096) = 0
exit_group(0) = no-return
```

Only cat may call `read`, only fd 0 is accepted, and the buffer address and
count are exact. The complete 4096-byte destination must be inside its
authenticated RW/NX stack mapping before a read can wait. A wrong fd returns
`-EBADF`; an invalid complete range returns `-EFAULT` without copying; an
unexpected syscall returns `-ENOSYS`. A successful write must use fd 1, the
same measured buffer, the exact preceding read length, and bytes identical to
the line supplied by Phipia. Those bytes are copied back from userspace
before they are published to the terminal and serial stream.

At read entry the kernel authenticates the generation, CR3, ordinal, register
frame, selectors, flags, destination, and ownership. It saves one resumable
frame, restores the kernel CR3 and launch stack, and yields to the Phipia
event loop. A complete line or EOF revalidates those invariants, performs one
all-or-nothing copy-out, and resumes immediately after the authentic
`SYSCALL`. A read cannot be completed or resumed twice. The lifecycle is
candidate, building, installed, running, waiting-for-input, ready-to-resume,
running, exiting, stopping, released; the wait/resume section repeats only
inside the fixed input bounds.

Cat accepts at most four complete lines, 64 printable ASCII bytes plus newline
per line, and 256 bytes total per launch. Enter completes a line; Backspace
edits only the current line; left Ctrl-D on an empty line returns zero and is
not supplied as a byte. This is a measured foreground input contract, not a
general stdin ABI, canonical mode, or a TTY.

## Image, stack, and storage limits

All three programs are static, position-fixed x86_64 `ET_EXEC` images parsed by
Rust before allocation or mapping. Interpreter, dynamic, relocation, PIE,
executable-stack, and W+X shapes are refused.

Each profile receives its exact measured argument vector, an empty environment,
and the measured `AT_PAGESZ`/`AT_NULL` auxiliary vector in a guarded RW/NX stack. The
historical scenarios keep their separate read-only 16 MiB FAT16 fixtures.
The v2.0.0 Phipia path uses one deterministic read-only 64 MiB FAT32 system
image with the exact `BUSYBOX`, `UNAMEBOX`, and `CATBOX` entries. It is attached
through ordinary emulated NVMe; DMA ownership returns to the CPU before Rust
inspects metadata or complete file bytes. Filesystem writes are rejected below
the shell for this mount.

The Phipia owner assigns a fresh generation, invokes only the selected
profile's measured launcher, and accepts success only after private CPL3 entry,
the architectural `SYSCALL` instruction, exact stdout, status-zero exit, kernel
CR3 restoration, mapping teardown, and an equal resource census. Failed and
completed generations retain no mappings or ownership, so a later launch starts
cleanly.

Checksums, source provenance, and reproducible build instructions are in
[`BUSYBOX_REPRODUCIBLE_BUILD.md`](BUSYBOX_REPRODUCIBLE_BUILD.md).

## Deliberate limits

There are no paths, writable files, signals, multiple processes, dynamic
linking, PIE, sockets, native Phipia syscalls, general mappings, general
descriptors, a descriptor table, job control, a userspace scheduler, hostname
mutation, `int 0x80`, POSIX claim, production-readiness claim, or general Linux
binary promise. Adding another program means measuring and pinning a new
profile rather than silently widening this contract.
