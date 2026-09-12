<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Verification

Phipia checks the kernel on the host and in QEMU. Hardware coverage is limited
to the documented emulated configurations.

## Commands

```sh
make lint
make verify
make smoke
make qemu-tests
```

`make verify` performs a clean build, host tests, ELF and linker inspection,
asset validation, filesystem reconstruction checks, and Rust parser tests. It
rejects warnings, unresolved symbols, unexpected sections, W+X mappings,
floating-point or SIMD instructions in the kernel, modified pinned assets, and
non-reproducible filesystem images.

`make qemu-tests` runs the complete 117-scenario guest suite. The Makefile is
the source of truth for scenario names and expected results.

The `native-canvas` scenario captures `canvas.png` and `canvas.mp4` directly
from QEMU's guest framebuffer while two independently loaded native Canvas
processes are alive. The same run injects hardware keyboard and pointer input;
the serial proof requires both applications to report focus and partial-damage
activity before their resource census is checked.

The `native-sdl` scenario launches the SDL 2 proof application twice. It
captures a screenshot and video, injects keyboard and pointer input into the
first launch, validates non-silent PCM output when QEMU provides its WAV
backend with exact frame identities and per-callback minimum coverage even when
backend silence fragments a callback, checks synchronized preference state on the second launch, and then
requires clean process, window, audio, and handle censuses.

The `native-dynamic` scenario installs one package containing a PIE root,
an authenticated dependency catalog, and `DYNLIB.SO`, then starts two process
instances before scheduling either. It requires a positive immutable DSO RX
reuse count, exactly two Ring 3 TLS passes, dependency-before-root constructors,
reverse destructors, private TLS behavior, and a clean shared-cache/resource
census.

The `native-https` scenario uses a deterministic offline Ethernet/DHCP/DNS/
TCP/TLS peer. Ring 3 validates the pinned chain, hostname, wall-clock validity,
HTTP/1.1 framing and authenticated close, synchronizes the exact body to Data,
and then passes only after the kernel observes a clean process/network census.
Its independent PCAP audit requires port-443 TLS records and rejects plaintext
request, response-line, or body bytes. The scenario uses QEMU's `max` CPU and a
pinned in-certificate RTC, and it requires the strong-hardware-entropy marker.

The `native-phip` scenario serves a deterministic sequence of root-signed
repositories and publisher-signed v3 packages over that same authenticated
HTTPS path. Across four boots, the Ring 3 `phip` client installs version 1,
reopens the persisted authority and updates it to version 2, proves that a
signed version-1 rollback is refused without changing generation 2, then
repairs deliberately damaged immutable bytes into generation 3. Each
accepted transaction copies the repository into a sealed upload, asks the
privileged controller for an authenticated plan, streams every exact package
directly into a digest-bound upload, commits the generation, removes its staging
file, synchronizes journaled ext4, and reboots. The damaged-generation boot
proves an ordinary snapshot quarantines the application before repair. On the
final boot the kernel independently parses the authority-selected database,
launches SDL 2.32.10's byte-exact upstream Chess Board application from repaired
generation 3, runs its bounded
event/software-render loop, and verifies its exact SDL preference-file output.
The scenario requires clean process, network, file, upload, controller,
package-service, NVMe, heap, and VFS censuses. The retained Data image must also
pass read-only `e2fsck`, and the PCAP is independently audited for encrypted TLS
traffic.

[`NATIVE_SCENARIOS.md`](NATIVE_SCENARIOS.md) maps each required native
application proof to its Ring 3 action, exact QEMU scenario, and retained
evidence.

## Coverage

The QEMU suite covers:

- boot, exception entry, APIC routing, clocks, paging, heap, and threads;
- framebuffer, cached surfaces, keyboard, pointer, shell, and Phipia;
- PCI, MSI-X, DMA, xHCI, NVMe, FAT32, and filesystem recovery;
- ext4 recovery and commit power cuts at every named NVMe flush boundary,
  followed by reboot, namespace/data checks, resource census, and `e2fsck`;
- ELF64 loading, isolated processes, user faults, and BusyBox profiles;
- virtio-net, DHCP, DNS, ICMP, UDP, TCP, HTTP, and downloads to FAT32;
- PCI driver binding, HD Audio codec transport, and NVIDIA device-model
  validation.

Each scenario has an expected exit status and required serial output. Writable
storage tests receive private image copies except for tests that explicitly
reboot with the same synchronized image.

## Focused checks

The repository also contains focused workflows for BusyBox reproduction,
storage, networking, drivers, Media Editor, and Phipia capture. BusyBox binaries
are built twice and compared, then checked against their ELF shape and syscall
profiles. Network tests use an offline peer and retain packet captures when
protocol-level inspection is needed.

## Failure checks

Tests for critical boundaries should also be exercised with an isolated bad
input. Useful examples include a missing boot dependency, a modified fixture,
an invalid user range, an RWX mapping, a wrong guest exit value, or a changed
reference pixel. These temporary mutations are never committed.

## Visual captures

```sh
make capture-phipia-proof
make screenshot-proof
make capture-boot-video
python3 tools/capture-networking.py --iso build/phipia.iso \
    --system build/userspace/phipia-system-fat32.raw \
    --data build/userspace/phipia-data-fat32.raw \
    --output build/networking-capture
```

Screenshots and videos confirm the visible guest interaction. Serial output,
packet captures, retained disk images, and structural checks remain the source
for behavior that cannot be established from a picture.

## Pull requests

A pull request should list the commands run, any relevant QEMU or hardware
limits, and the important behavior not covered by its tests. The required
`build-and-boot` check must pass on the latest commit before merge.
