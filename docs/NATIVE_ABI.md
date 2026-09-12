<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native userspace ABI v1

The Phipia native ABI is the kernel contract for static Ring 3 applications.
It is separate from the three measured Linux/BusyBox profiles. The canonical
machine-readable definitions are under `include/phipia/abi/`; the SDK installs
the same headers under `sdk/include/phipia/abi/`.

## Calling and result convention

On x86_64, `RAX` contains the syscall number. Arguments zero through five use
`RDI`, `RSI`, `RDX`, `R10`, `R8`, and `R9`. `SYSCALL` destroys `RCX` and `R11`.
Native syscall entry switches to a dedicated, bounded 64 KiB kernel stack.
Its low-end canary is checked during disarm; the TSS `RSP0` stack remains
reserved for privilege-changing interrupts and is not reused by deep filesystem
or package-service syscall paths.
Results are returned in `RAX`: zero or a positive value is success and a
negative `phipia_errno` value is failure. Unknown numbers return `-ENOSYS`.

Every public record uses fixed-width fields, begins with `size` and `version`
where evolution is expected, and names its reserved fields. Callers set every
reserved field and unused flag bit to zero. Version 1 rejects unknown flags;
it never silently accepts a future meaning. ABI records are packed and have
compile-time size assertions, so their layout does not depend on compiler
padding.

The kernel copies user data through checked aliases. It validates the entire
range with checked arithmetic before a promised all-or-nothing copy. A range
is rejected if it wraps, is noncanonical, crosses an unmapped page, has the
wrong direction permission, or names a guard, kernel, MMIO, DMA, page-table,
foreign-process, immutable executable, or read-only output mapping.

## Service groups

| Range | Calls | Blocking and ownership |
| --- | --- | --- |
| `0x0000` | ABI version, exit, console read/write, handle close/duplicate | Writes are immediate. Console read parks the thread until keyboard input. Close consumes one handle immediately; duplicate creates another reference to the same typed object. |
| `0x0100` | anonymous map and unmap | Synchronous. Maps are private, page-granular, RW/NX or R/NX, optionally guarded, and charged to the manifest limit. Successful unmap consumes the complete named mapping. |
| `0x0200` | file, directory, path mutation, stat, seek, sync, free space | Synchronous bounded FAT32 operations. Open calls return owned typed handles. Reads and writes may return a documented partial byte count; metadata mutations either publish a valid result or fail. |
| `0x0300` | monotonic/realtime clocks, sleep, wait, entropy, timers, cancellation | Realtime is UTC Unix seconds. Sleep, wait, and every deadline remain monotonic. Sleep and wait park only the calling native thread. Wait copies at most eight items into the kernel and copies the full set back on completion. Timeout is `-ETIMEDOUT`; cancellation is `-ECANCELED`. |
| `0x0400` | window creation, surface present, event read, pointer capture | Window creation returns owned window and event-queue handles plus one process-local RW/NX surface mapping. Present consumes no ownership. Event read is nonblocking; wait on the queue before retrying. |
| `0x0500` | DNS, TCP, UDP, address query | Open calls return owned stream/datagram handles. Deadlines are absolute monotonic nanoseconds. Shutdown changes stream direction state but does not close the handle. |
| `0x0600` | thread create/exit/join, FS-base TLS, futex wait/wake | Create returns an owned thread handle. Join parks and reports the target exit status; the handle is still closed explicitly. Futex wait compares one aligned user `u32` before parking. |
| `0x0700` | PCM output open, submit, volume, drain | Open returns a typed HDA output handle. Submission is one fixed 4 KiB PCM chunk; drain may park the calling thread. |
| `0x0800` | package upload and transaction control | Requires the privileged `packages` capability. Upload bytes live only in a kernel-private Data path; the controller separately authenticates repository metadata, binds each payload, snapshots installed authority, and enters bootstrap or prepare/commit. |

## Per-call contract

The signatures below name registers in syscall-argument order; `*record` means
the exact packed record from the corresponding ABI header. Pointer arguments
are borrowed for the call only, and the kernel never retains a userspace
pointer. `I` is an immediate scheduler-serialized transition, `K` performs
synchronous kernel or device work while no other native userspace thread runs,
`P` parks only the calling thread and permits another runnable native thread or
process, and `X` does not return. Process teardown closes every surviving
handle and mapping; the lifetime column states the earlier explicit release.

### Core and memory

| Number and signature | Result | Mode | Ownership, concurrency, and cleanup |
| --- | --- | --- | --- |
| `0x0000 ABI_VERSION()` | ABI version `1` | I | No resource or ownership change. |
| `0x0001 EXIT(status)` | Does not return | X | Atomically begins process-wide teardown; all threads, handles, mappings, queues, windows, and network objects are reclaimed. |
| `0x0002 CONSOLE_WRITE(bytes, length)` | Bytes written | I | Borrows a readable range; no retained state. Calls from native threads are serialized in syscall order. |
| `0x0003 CONSOLE_READ(buffer, length)` | Bytes read | P | Borrows a writable range until completion; cancellation occurs only through process teardown. Buffered input wakes one eligible reader. |
| `0x0004 HANDLE_CLOSE(handle)` | `0` | K | Consumes this table entry immediately. The generation changes before resource cleanup; a repeated or stale close fails. The object dies when its final duplicate closes. |
| `0x0005 HANDLE_DUPLICATE(handle)` | New typed handle | I | Creates an independently closeable reference to the same object. No cross-process transfer or inheritance is introduced. |
| `0x0100 MEMORY_MAP(*request, *response)` | `0` | K | Creates one owned private mapping, optionally with guards; the response is all-or-nothing. Release with `MEMORY_UNMAP` or process teardown. |
| `0x0101 MEMORY_UNMAP(address, length)` | `0` | K | Consumes exactly one complete anonymous mapping and its guards. Partial, foreign, executable, surface, or mismatched ranges fail without mutation. |

### FAT32 storage

Storage calls are restricted by the manifest capability and namespace. System
is immutable; writable Data paths are rooted below the application namespace.

| Number and signature | Result | Mode | Ownership, concurrency, and cleanup |
| --- | --- | --- | --- |
| `0x0200 FILE_OPEN(*request)` | Owned file handle | K | Captures open mode and current offset in a process-local typed object. Close explicitly or at process exit. |
| `0x0201 FILE_READ(*io)` | Bytes read | K | Borrows the output range. `offset == UINT64_MAX` advances the handle offset; an explicit offset is positional and leaves it unchanged. Partial reads are permitted at EOF. |
| `0x0202 FILE_WRITE(*io)` | Bytes written | K | Borrows the input range. Current-offset and positional rules match read. A positive partial result is possible after bounded device progress; unreported bytes are not claimed written. |
| `0x0203 FILE_SEEK(*request)` | New offset | I | Mutates only the named file handle's cursor; duplicates share the underlying open object and cursor. |
| `0x0204 PATH_STAT(*path, *stat)` | `0` | K | Borrows the path and all-or-nothing output record; creates no persistent kernel object. |
| `0x0205 DIRECTORY_OPEN(*path)` | Owned directory handle | K | Creates a typed enumeration cursor. Close explicitly or at process exit. |
| `0x0206 DIRECTORY_READ(directory, *entry)` | `1` entry, `0` end | K | Advances that directory cursor and writes one complete entry or none. The handle remains owned by the caller. |
| `0x0207 PATH_MKDIR(*path, reserved_zero)` | `0` | K | Creates one Data directory; namespace and FAT32 publication are serialized. No handle is returned. |
| `0x0208 PATH_RENAME(*request)` | `0` | K | Moves the source only when the destination is absent. The mutation is serialized and either publishes the valid rename or fails. |
| `0x0209 PATH_REPLACE(*request)` | `0` | K | Publishes the source at the destination using the recovery-safe replacement path. No user handle is consumed. |
| `0x020a PATH_UNLINK(*path, reserved_zero)` | `0` | K | Removes the named Data file or empty directory. Existing open handles retain their bounded kernel object until closed. |
| `0x020b PATH_TRUNCATE(*path, length)` | `0` | K | Resizes one Data file subject to FAT32 and manifest limits; no handle or buffer ownership changes. |
| `0x020c VOLUME_SYNC(volume)` | `0` | K | Completes pending FAT32 and device synchronization for the authorized volume; owns no object after return. |
| `0x020d VOLUME_SPACE(volume, *space)` | `0` | K | Writes one all-or-nothing free-space record and retains no pointer or resource. |

### Time, waiting, and entropy

| Number and signature | Result | Mode | Ownership, concurrency, and cleanup |
| --- | --- | --- | --- |
| `0x0300 TIME_MONOTONIC()` | Nanoseconds | I | Reads the monotonic clock; no resource change. |
| `0x0301 SLEEP_UNTIL(deadline_ns)` | `0` | P | Parks the caller until the absolute deadline. Process teardown cancels the saved wait state. A past deadline returns immediately. |
| `0x0302 WAIT(*request)` | Ready count or `-ETIMEDOUT` | P | Copies at most eight items before parking and copies the complete set back on wake. Handles remain caller-owned; close or cancellation makes the observed item report its defined state. |
| `0x0303 RANDOM(buffer, length)` | Bytes written | K | Borrows a writable range and fills it from the bounded non-cryptographic generator. A positive partial result is reported only for bytes already copied; cryptographic callers use `RANDOM_STRONG`. |
| `0x0304 TIMER_CREATE()` | Owned timer handle | I | Creates an initially disarmed typed wait object. Close explicitly or at process exit. |
| `0x0305 TIMER_SET(*request)` | `0` | I | Replaces the timer's absolute deadline. The timer remains owned and reusable; setting does not transfer it to a waiter. |
| `0x0306 CANCEL(handle)` | `0` | K | Disarms a timer or sets cancellation on a stream/datagram object. It neither closes nor consumes the handle; unsupported handle types fail. |
| `0x0307 TIME_REALTIME()` | UTC Unix seconds or `-EIO` | K | Performs one bounded coherent CMOS/RTC read. It owns no object. RTC validity does not affect monotonic deadlines. |
| `0x0308 RANDOM_STRONG(buffer, length)` | Bytes written or `-EIO` | K | Bypasses the non-cryptographic generator and copies only repetition-checked RDSEED/RDRAND output. It fails closed when strong hardware entropy is unavailable. |

### Phipia window, surface, and input

| Number and signature | Result | Mode | Ownership, concurrency, and cleanup |
| --- | --- | --- | --- |
| `0x0400 WINDOW_CREATE(*request, *response)` | `0` | K | Atomically returns owned window and event-queue handles plus a process-local RW/NX surface mapping. Closing the window or process teardown destroys the UI object and releases the mapping. |
| `0x0401 SURFACE_PRESENT(*request)` | Pixels presented | K | Borrows one to eight damage rectangles and copies only those pixels from the owned surface. No ownership transfer; presents from native threads are serialized. |
| `0x0402 EVENT_READ(queue, *event)` | `1` or `-EAGAIN` | I | Removes one bounded queued event into an all-or-nothing record. The queue handle remains owned; overflow is reported as an explicit event. |
| `0x0403 POINTER_CAPTURE(window, capture)` | `0` | I | Changes capture for the focused owned window. Release with `capture == 0`, focus loss, window close, fault, or process teardown. |

### IPv4 networking

Direct network operations use absolute deadlines and perform bounded protocol
progress inside the syscall. Readiness through `WAIT` is the cooperative path
for applications that must keep another userspace thread runnable.

| Number and signature | Result | Mode | Ownership, concurrency, and cleanup |
| --- | --- | --- | --- |
| `0x0500 DNS_RESOLVE(name, length, deadline_ns)` | IPv4 address | K | Borrows an ASCII hostname, owns no resolver object after return, and maps timeout, malformed response, or cancellation to a negative error. |
| `0x0501 STREAM_OPEN()` | Owned stream handle | K | Creates a typed TCP object. Close explicitly or at process exit; no descriptor inheritance exists. |
| `0x0502 STREAM_CONNECT(stream, *peer, deadline_ns)` | `0` | K | Mutates only the named stream. The endpoint is borrowed; timeout, reset, link loss, and prior cancellation leave an explicitly failed stream state. |
| `0x0503 STREAM_READ(*io)` | Bytes read | K | Borrows a writable range. Positive partial reads are valid; orderly peer close is `-EPIPE`. The stream remains owned. |
| `0x0504 STREAM_WRITE(*io)` | Bytes written | K | Borrows a readable range. Positive partial writes cover only accepted bytes; the stream remains owned. |
| `0x0505 STREAM_SHUTDOWN(stream, flags, deadline_ns)` | `0` | K | Changes the requested stream direction without consuming the handle. Close remains required. |
| `0x0506 DATAGRAM_OPEN()` | Owned datagram handle | K | Creates a typed UDP object. Close explicitly or at process exit. |
| `0x0507 DATAGRAM_BIND(datagram, port)` | `0` | K | Associates the local port with the object; the handle remains owned and the binding is released on final close. |
| `0x0508 DATAGRAM_SEND(*io)` | Datagram bytes | K | Borrows the complete input and destination. UDP is all-or-error for the bounded datagram; no endpoint ownership is created. |
| `0x0509 DATAGRAM_RECEIVE(*io)` | Datagram bytes | K | Borrows a writable buffer and writes the source endpoint back into the request atomically with success. The datagram handle remains owned. |
| `0x050a NETWORK_ADDRESS(handle, peer, *endpoint)` | `0` | K | Borrows one output record and queries a stream/datagram local address or connected peer. No state or ownership changes. |

### Threads, TLS, and futexes

| Number and signature | Result | Mode | Ownership, concurrency, and cleanup |
| --- | --- | --- | --- |
| `0x0600 THREAD_CREATE(*request)` | Owned thread handle | K | Creates a private guarded stack and initial FPU/TLS context. The new thread becomes runnable after admission; close the handle after join. Process teardown reclaims both live and exited threads. |
| `0x0601 THREAD_EXIT(status)` | Does not return | X | Marks only the caller exited, wakes joiners, and keeps its result until the thread object is closed or the process exits. The final live thread triggers process teardown. |
| `0x0602 THREAD_JOIN(thread)` | Exit status | P | Parks until the target exits. It neither consumes nor closes the thread handle; self-join and stale or wrong-type handles fail. |
| `0x0603 TLS_SET(fs_base)` | `0` | I | Replaces only the calling thread's saved FS base after validating that it is zero or user-mapped. It is restored on every return to that thread. |
| `0x0604 TLS_GET()` | FS base | I | Reads only the calling thread's saved value; no ownership or state change. |
| `0x0605 FUTEX_WAIT(*request)` | `0`, `-EAGAIN`, or timeout | P | Atomically validates and compares one aligned mapped `u32` before parking. The word remains userspace-owned; process teardown cancels the wait. |
| `0x0606 FUTEX_WAKE(*request)` | Threads woken | I | Wakes up to the bounded requested count for one exact process-local address. It never owns or modifies the futex word. |

### Package upload and transaction control

| Number and signature | Result | Mode | Ownership, concurrency, and cleanup |
| --- | --- | --- | --- |
| `0x0800 PACKAGE_UPLOAD_OPEN()` | Owned package-upload handle | K | Allocates one of four kernel slots and a private 8.3-safe Data file. The file is inaccessible through the caller's application namespace. |
| `0x0801 PACKAGE_UPLOAD_WRITE(*request)` | Bytes written | K | Copies at most 4 KiB into the private file and advances an incremental kernel SHA-256. Any storage failure poisons the upload so it can only be closed. |
| `0x0802 PACKAGE_UPLOAD_SEAL(*request)` | `0` | K | Closes the writer, requires exact nonzero length and SHA-256 from the privileged caller, then flushes Data. The all-or-nothing output reports actual length, digest, and sealed/durable flags; it does not authenticate the metadata's origin. |
| `0x0803 PACKAGE_CONTROL_OPEN_INSTALL(*request)` | Owned package-control handle | K | With `OPEN_INSTALL`, copies and authenticates the sealed repository upload with platform trust, realtime freshness, and the durable repository rollback floor, snapshots recovered installed authority, and produces one bounded install/update plan. `OPEN_REPAIR` retains authenticated installed metadata even when owned bytes are damaged and plans exact signed replacements. `OPEN_REMOVE` requires an invalid repository handle and derives a dependency-safe removal plan solely from authenticated installed state. |
| `0x0804 PACKAGE_CONTROL_ITEM(*request)` | `0` | K | Copies one exact plan item, including its repository-bound length, SHA-256, and download path. No pointer into privileged parser state is exposed. |
| `0x0805 PACKAGE_CONTROL_ATTACH(*request)` | `0` | K | Requires a sealed upload whose length and digest match the named plan item, copies it into the controller, and re-authenticates the signed package against the repository entry. The upload handle remains caller-owned. |
| `0x0806 PACKAGE_CONTROL_COMMIT(*request)` | `0` | K | Rebuilds and encodes canonical state, then bootstraps generation one or prepares and commits an update or removal. A durability refusal after prepare leaves the control handle retryable; `PREPARED` and `COMMITTED` report the exact state. |

The installed VFS currently bounds one staged file at 16 MiB, so the upload
ABI publishes that real limit even though package format v3 can represent a
larger host-side container. A final close durably unlinks the upload; duplicate
handles share it and only the last close performs cleanup.

One package-control session may be live system-wide. Its native handle may be
duplicated; the final close releases the authenticated repository snapshot,
installed-state snapshot, and copied payloads. Process teardown performs the
same close. The v1 control profile admits at most eight changed packages with
4 MiB of aggregate package bytes. Removal is exposed without a repository or
payload upload. `phip repair` uses the repair flag with a signed repository and
the same payload-binding path. Install/update/repair advance a checksummed,
monotonic repository-version floor before package staging, so a signed older
index remains refused across reboot and crash recovery.

Directory enumeration reports the canonical printable form of each accepted
ASCII 8.3 name in lower case. Path lookup remains case-insensitive.

Network readiness waits park a native thread in the scheduler. Synchronous
DNS and stream/datagram operations pump bounded protocol state, recheck their
absolute deadline and completion state, then halt the core until a device or
timer interrupt. They never poll in a userspace or kernel spin loop.

`include/phipia/abi/base.h` is the syscall-number and error-number registry.
The service-specific headers define exact records, limits, flags, event values,
pixel format, IPv4 endpoint encoding, and static size checks.

## Concurrency and cleanup

Handles and mappings are process-local. Version 1 has no implicit inheritance
and no cross-process transfer. Syscalls may be issued by any live thread;
kernel handle resolution and resource mutation occur with the native scheduler
in kernel context. Wait, sleep, join, console read, and futex wait save the
thread context and let another runnable thread or process proceed.

Normal exit, the final thread exiting, a userspace exception, or a failed
admission path converges on process teardown. Teardown closes every handle,
destroys windows and queues, cancels network ownership, releases every private
frame and surface mapping, clears saved FPU/TLS state, and restores the kernel
address space before the result is published.
