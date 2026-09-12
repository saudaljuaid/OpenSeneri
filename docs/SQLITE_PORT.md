<!-- SPDX-License-Identifier: GPL-3.0-only -->

# SQLite 3.46.0 port

The official SQLite 3.46.0 amalgamation is compiled unchanged with
`SQLITE_OS_OTHER=1` and the Phipia VFS in `ports/sqlite/phipia_vfs.c`.
Threading, loadable extensions, WAL, shared cache, UTF-16, and deprecated
interfaces are disabled. The supported profile uses rollback journals and the
single-process locking rules implemented by the VFS.

Phase one creates `SQLITE/PORT.DB`, creates a table, inserts three rows in a
transaction, commits, verifies the expected lock refusal, closes the database,
and syncs Data. QEMU then performs a clean guest reboot using the same writable
image. Phase two reopens the database, runs an integrity check, queries the
three retained rows and sum, and writes its guest result marker. The kernel
checks that marker and the workflow retains the rebooted Data image.

The guest reports monotonic timings for the committed insert transaction and
for the post-reboot open, query, integrity check, and close path. These are
regression diagnostics from the public SDK/VFS path, not host-side timings.

No SQLite-specific code exists in the loader, FAT layer, or syscall dispatch.
Build with `make build/ports/sqlite/SQLITE.SPK`; run the two-boot proof with
`make qemu-test-native-sqlite`.
