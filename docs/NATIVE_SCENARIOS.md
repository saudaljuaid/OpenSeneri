<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native application QEMU scenarios

`make qemu-port-tests` runs the native platform proof set. Some related checks
share one boot so expensive device initialization is not repeated, but each
Ring 3 check has its own required serial marker or kernel assertion. A missing
marker, wrong debug-exit value, guest panic, or `ST FAIL` fails the scenario.

For every native launch, the kernel records the initial resource state and
requires both `result.resources_released` and
`native_process_resources_released()` before the scenario can pass. Admission
refusals perform the same census immediately before and after validation. The
negative checks below therefore identify both the exact error and the common
clean-census assertion rather than treating continued boot as success.

| Required proof | QEMU scenario | Ring 3 action and required evidence |
| --- | --- | --- |
| 1. General load and clean exit | `native` | Loads `NATIVET.MAN` from System and requires `PHIPIA NATIVE PASS` plus the kernel loader/SDK/TLS/thread/FPU completion line. |
| 2. Initial stack | `native` | The C entry code validates exact `argc`, `argv`, three environment entries, terminators, and Phipia auxiliary entries before `PHIPIA STARTUP argc argv environment auxiliary PASS`. |
| 3. Malformed ELF | `native-elf-refusal` | Rust validation refuses `BADELF.MAN`; the exact marker names malformed ELF and an unchanged resource census. |
| 4. Digest mismatch | `native-digest-refusal` | The manifest/executable SHA-256 disagreement is refused before mapping; the exact marker names the mismatch and unchanged census. |
| 5. Unsupported ABI | `native-abi-refusal` | ABI version 2 is refused with the exact unsupported-version and unchanged-census marker. |
| 6. Capability denial | `native` | A manifest without `network` calls `STREAM_OPEN`, requires `-EACCES`, and emits the consolidated `capability EACCES` refusal marker before clean exit. |
| 7. Stale handle | `native` | Duplicates and closes a file handle, then requires `-ESTALE` for use and repeated close before `PHIPIA STORAGE typed duplicate stale-handle PASS`. |
| 8. Invalid ranges | `native` | Requires `-EFAULT` for an unmapped pointer and a two-byte range crossing into an unmapped page while preserving the mapped edge byte. |
| 9. Anonymous memory exhaustion | `native` | Maps private RW/NX regions to the manifest limit, requires `-ENOMEM`, releases every successful mapping, and emits the memory exhaustion marker. |
| 10. Crash containment | `native-crash` | Faults at null with live mappings, state, and resources; a second native process must then pass. The kernel names reclaimed mappings, handles, threads, windows, FS, x87, and SSE state. |
| 11. File mutation lifecycle | `native` | Through the ABI, creates, writes, seeks, truncates, renames, replaces, enumerates, synchronizes, unlinks, and removes the directory; each storage phase and final composite marker is required. |
| 12. Clean-reboot persistence | `native-sqlite` | Creates and synchronizes the database, resets the platform, reopens it from the retained Data image, and requires both phase markers. |
| 13. Traversal refusal | `native` | Data and System `../` paths must return `-EINVAL`; the consolidated refusal marker names traversal and the launch ends with a clean census. |
| 14. Surface and partial present | `native-canvas` | Two apps map xRGB8888 surfaces and repeatedly present only a 70x14 live region. Two performance lines, a real framebuffer PNG, and an MP4 are required. |
| 15. Keyboard and pointer delivery | `native-canvas` | Hardware injection must produce positive key and pointer counts in a Ring 3 `PHIPIA CANVAS PASS` line. |
| 16. Multiple application windows | `native-canvas` | Two independently admitted generations remain visible together; the kernel requires both to close and both native UI slots to be empty. |
| 17. Event wait and timeout | `native` | A timer wakes `WAIT`, a past absolute deadline returns `-ETIMEDOUT`, cancellation disarms the timer, and the event marker is required. |
| 18. TCP client | `network-native` | Resolves the offline host, connects, writes HTTP/1.1, validates framing/body, and stores the exact body on Data. |
| 19. UDP endpoint | `network-native` | Binds port 50010, verifies its local address, sends to the offline echo peer, and validates payload and source endpoint. |
| 20. Network failure paths | `network-native` | Requires reset, timeout, prior cancellation, and malformed-DNS errors, then checks zero TCP, UDP, and timer objects after teardown. |
| 21. Two userspace threads | `native` | Creates two SDK threads, schedules both through blocking waits, joins each, and requires all four create/join markers. |
| 22. TLS isolation | `native` | Main and worker `_Thread_local` values remain distinct through 32 switches per worker; the general/FS/x87/SSE marker is required. |
| 23. x87/SSE isolation | `native` | Assembly pins callee-saved GPRs, x87 `1.0`, and XMM15 across blocking syscalls in both threads; initial `FCW`/`MXCSR` and cleared vector state are also checked. |
| 24. Lua port | `native-lua` | Upstream Lua reads injected stdin, loads `SCRIPT.LUA` from Data, computes and writes the exact result, and exits with the Lua marker and clean census. |
| 25. SQLite persistence | `native-sqlite` | Upstream SQLite creates a table, inserts/commits three rows, observes busy locking, reboots, runs integrity/query checks, and verifies the result file. |
| 26. Every handle type live | `network-native` | The teardown fixture leaves file, directory, window, event queue, stream, datagram, timer, and thread handles plus an anonymous mapping live; the kernel requires all classes gone after exit. |
| 27. Repeated launch | `native-relaunch` | Launches the same unmodified package twice, requires a strictly newer process generation, and checks a clean census after both exits. |
| 28. Public audio | `native-audio` | Proves capability refusal, two typed Ring 3 PCM handles, deterministic Q15 mixing, readiness, drain, cancellation, close and process-exit cleanup; verifies a QEMU WAV artifact when that backend is available. |
| 29. SDL 2 application lifecycle | `native-sdl` | Runs the real SDL Phipia video, event, timer, thread, filesystem, and audio backends twice; requires partial presentation, injected key and pointer input, non-silent queued PCM, synchronized preference state on relaunch, and a clean resource census. |
| 30. Dynamic shared object, shared RX, TLS, and lifecycle | `native-dynamic` | Starts two instances of one PIE/DSO package, authenticates its catalog and SONAME digest, relocates with root-first lookup scope, requires a positive immutable DSO RX reuse count, proves private library initial-exec plus root local-exec TLS in both processes, and requires dependency/root constructors and reverse destructors before the clean cache/resource census. |
| 31. Validated HTTPS download | `native-https` | Uses fail-closed strong hardware entropy plus the pinned-RTC offline Ethernet/DHCP/DNS/TCP/TLS peer, validates the pinned chain, hostname, certificate time, strict Content-Length body, authenticated close and encrypted PCAP, synchronizes the exact body to Data, then repeats the transfer into an exact-digest, flushed kernel package-upload handle and requires clean process/network/upload censuses. |
| 32. Signed HTTPS package lifecycle | `native-phip` | Runs the Ring 3 `phip` client through a deterministic four-boot version-1 install, version-2 update, signed version-1 downgrade refusal, and authenticated repair after deliberate immutable-file damage. Accepted install, update, and repair generations download every digest-bound payload over TLS and commit to journaled ext4; each is synchronized and rebooted. The damage phase proves normal launch is quarantined before repair. The final boot proves generation 3 is authoritative, launches SDL 2.32.10's byte-exact upstream Chess Board application from ext4, runs its event/software-render loop, verifies its SDL preference-file output, runs read-only `e2fsck`, and requires clean process/window/network/file/upload/controller/service/NVMe censuses plus an encrypted PCAP audit. |

The Makefile owns the expected debug-exit values and exact serial expressions.
The native-porting workflow retains serial logs, writable Data images, Canvas
and SDL media, available native-audio and SDL WAV captures, the Lua ELF/map,
the dynamic root/library/catalog and `readelf` reports, HTTPS and signed-package
encrypted packet captures and peer logs, and the native-network packet capture
and peer log from the same commit.
