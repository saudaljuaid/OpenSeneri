<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Device identification drivers

Phipia has bounded identification drivers for thirteen PCI devices from Intel,
Realtek, AMD, and Cirrus Logic, plus the Bochs Display Interface. Each driver
matches the device, establishes a known state, checks its identity against the
hardware specification, and releases its resources.

## Common behavior

Every driver here:

- matches on vendor, device, class and subclass together, never on one of them;
- reaches its device either through configuration space only, or through
  exactly one memory BAR claimed and mapped uncached by the typed PCI
  substrate;
- performs the reset its specification defines, where one exists, and waits for
  the device's own completion signal under a one-second monotonic deadline;
- reads the registers that identify the device and checks them against a
  property the specification guarantees, not against a value that happened to
  be observed;
- unmaps and releases everything, and the matrix compares the frame, paging,
  DMA, PCI, vector and MSI-X census from before the first bind to after the
  last one.

These drivers keep bus mastering disabled. Phipia has no IOMMU, so a
bus-mastering device could reach all physical memory. The four chipset drivers
also leave configuration space unchanged while inspecting the machine's legacy
I/O setup.

## The thirteen drivers

| # | Device | ID | Access | Reset | Identity checked against |
| --- | --- | --- | --- | --- | --- |
| 1 | Intel 82441FX host bridge | `8086:1237` | configuration | none | PAM0's reserved attribute field reads zero |
| 2 | Intel 82371SB PIIX3 ISA bridge | `8086:7000` | configuration | none | every routed PCI interrupt names a legal ISA interrupt |
| 3 | Intel 82371SB PIIX3 IDE | `8086:7010` | configuration | none | a decodable programming interface, per-channel decode enables |
| 4 | Intel 82371AB PIIX4 power management | `8086:7113` | configuration | none | an enabled power-management base is a real I/O base |
| 5 | Intel 82540EM Gigabit Ethernet | `8086:100E` | BAR0 memory | `CTRL.RST` self-clears | EEPROM checksum 0xBABA, EEPROM and receive-address MAC agree, MAC is unicast |
| 6 | Intel 82574L Gigabit Ethernet | `8086:10D3` | BAR0 memory | `CTRL.RST` self-clears | as above, through the 82574's different EEPROM read encoding |
| 7 | Intel 82801IR ICH9 SATA AHCI | `8086:2922` | BAR5 memory | `GHC.HR` self-clears | major version 1, at least one implemented port, no more ports than `CAP.NP` allows |
| 8 | Intel 82801I ICH9 HD Audio | `8086:293E` | BAR0 memory | `GCTL.CRST` low then high | major version 1, a non-zero capability register |
| 9 | Realtek RTL8139 Fast Ethernet | `10EC:8139` | BAR1 memory | `CR.RST` self-clears | a hardware version identifier in the transmit configuration register, MAC neither zero nor broadcast |
| 10 | Intel 82801DB USB 2.0 EHCI | `8086:24CD` | BAR0 memory | `USBCMD.HCRESET` self-clears | interface version 1.0, a capability block long enough to hold itself, operational registers inside the window, at least one root port, and a halted controller after reset |
| 11 | Cirrus Logic GD5446 display | `1013:00B8` | BAR1 memory | none | the extension lock reads back the key when open and 0x0F when closed, and the CRTC chip identifier names a GD5446 |
| 12 | Bochs Display Interface | `1234:1111` (subclass 0x80) | BAR2 memory | none | a documented interface version, and a memory-size register that agrees to the byte with the prefetchable framebuffer BAR |
| 13 | AMD Am79C970A PCnet-PCI II | `1022:2000` | BAR1 memory | `S_RESET` read | JEDEC manufacturer 0x001 (AMD), fixed bit set, the chip identity reads the same through the 16-bit and 32-bit register files |

Nine entries are Intel parts, with one each from Realtek, AMD, and Cirrus
Logic. The final entry is the Bochs Display Interface implemented by Bochs,
QEMU, and VirtualBox. Seven devices perform a hardware reset; the matrix marks
the others as having no reset operation.

The Bochs entry matches subclass 0x80 rather than 0x00. A machine may expose a
standard VGA controller with the same vendor and device ID, so the
identification path avoids changing the firmware-selected display.

The Intel Gigabit driver reads the station address from both the receive-address
registers and EEPROM, then validates the checksum across all 64 EEPROM words.
The PCnet driver performs its read-triggered reset and requires the identity to
match through both 16-bit and 32-bit register access. The Cirrus driver selects
the active CRTC address pair, reads the chip ID, and restores the address and
extension-lock state.

## Where it runs

`src/kernel/driver.c` contains the drivers and binding matrix;
`include/phipia/driver.h` defines their interface. Two Boot Ledger stages cover
the matrix:

- **bounded PCI driver matrix foundation** validates duplicate identities,
  absent vendor IDs, access modes, class codes, accessor bounds, reset
  deadlines, and configuration-space decoding.
- **installed PCI driver matrix probe** binds every declared device that is
  present. It is a neutral-skip stage: a machine carrying none of the thirteen
  is a machine this stage has nothing to do on.

The probe stage runs only in scenarios that attach this hardware because
binding resets seven of the devices.

## Evidence

| Scenario | Machine | What it proves |
| --- | --- | --- |
| `driver-matrix` | i440fx with all thirteen devices attached and four pinned station addresses | all thirteen present, all thirteen bound, seven resets observed, every pinned address read back, census equal |
| `driver-matrix-builtin` | i440fx as it comes | the five built-in devices bind and the eight absent ones are reported absent with no identity and no driver bound |

Both scenarios compare the Boot Ledger receipt with the matrix result.

### Device-backed identity checks

The `driver-matrix` scenario assigns fixed station addresses to the four
network devices from the QEMU command line. Each driver must read back its
assigned address through the device's native register or EEPROM path. `make
verify` checks that the scenario and kernel test use the same values.

## Scope

These drivers bind and identify hardware through configuration or MMIO access.
They do not configure DMA, descriptor rings, interrupts, or data transfer. The
matrix uses a fixed declaration order and covers the devices available on the
QEMU test machine.

The AHCI controller finishes enabled, HD Audio finishes out of reset, EHCI
finishes halted, and PCnet finishes in 32-bit register mode. The display drivers
restore the state they inspected.
