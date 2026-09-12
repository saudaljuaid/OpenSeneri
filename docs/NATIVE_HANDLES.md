<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native handles and resource limits

Each process owns a fixed table of at most 128 handles; a manifest may select a
smaller limit. Handles encode a slot, type, and nonzero generation. Resolution
checks the active slot, generation, expected type, and referenced object. A
file handle therefore cannot be used as a timer or stream, and a closed value
cannot name a later object reusing the same slot.

Version 1 types are file, directory, Phipia window, event queue, TCP stream,
UDP endpoint, timer, thread, bounded PCM output, and kernel-owned package
upload and package transaction control. Process handles are not
exposed. Duplicate creates a new generated slot referencing the same typed object. Close
invalidates its slot immediately; the underlying object is released when its
last reference closes. A repeated close and every stale resolution fail.

There is no implicit inheritance, ambient global descriptor table, or numeric
compatibility with Linux descriptors. A process starts with no handles. The
manifest capabilities determine which creation operations are available.

Process teardown closes the complete table. Object-specific cleanup closes FAT
objects, releases directory cursors, destroys a surface after its window and
queue references disappear, closes/cancels network state by process owner,
releases thread and timer bookkeeping, and synchronously stops any HDA stream
owned by the dying process. A final package-upload close removes and flushes
its private staging file; cleanup failure keeps the handle live for retry. A
final package-control close releases its copied repository, installed-state,
and signed payload buffers; a prepared transaction already published to the
transaction service remains recoverable independently of that session. The
result records peak handles and is not
considered clean until the table, address space, syscall gate, interrupt gate,
windows, network ownership, and audio controller ownership are gone.
