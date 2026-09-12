<!-- SPDX-License-Identifier: GPL-3.0-only -->

# NVIDIA register probes

Phipia has fifteen bounded probes for NVIDIA graphics and audio functions.
They identify a device through PCI configuration, BAR layout, documented MMIO
registers, and the video BIOS. The probes release every claimed resource and
leave bus mastering disabled.

The QEMU model in `tools/qemu/phipia_nvidia_model.c` exercises the complete bind
path. It supplies the register interface, while scenario parameters provide the
boot identity, straps, board identity, timer scale, and ROM image.

## Sources

NVIDIA does not publish a complete register manual for these devices. The
implementation uses the following public sources:

| Source | Used for |
| --- | --- |
| [envytools](https://github.com/envytools/envytools) | register names, offsets, and apertures |
| Nouveau (`drivers/gpu/drm/nouveau`) | identity decode, family table, ROM shadowing, and timer reads |
| Mesa / NVK | architecture names and generation boundaries |
| NVIDIA open GPU kernel modules | modern register-offset cross-checks |
| PCI Firmware Specification 3.0 | expansion ROM and PCIR structures |
| PCI Local Bus Specification 3.0 | ROM BAR, capability alignment, and MSI control |
| PCI Bus Power Management Interface 1.2 | power capability and state |
| PCI Express Base Specification | capability type, link, and payload fields |
| HD Audio 1.0a | audio version and stream counts |

Unspecified fields are reported without assigning them a meaning.

## Probe matrix

| # | Probe | Access | Check |
| --- | --- | --- | --- |
| 0 | GPU master control | BAR0 | `PMC_BOOT_0` names a known architecture and `PMC_BOOT_1` reports little-endian access |
| 1 | GPU configuration mirror | BAR0 | mirrored vendor/device ID matches PCI enumeration |
| 2 | GPU timer | BAR0 | the 64-bit counter advances during a bounded wait |
| 3 | GPU video BIOS | BAR0 | PROM contains an NVIDIA PCI ROM and BIT table |
| 4 | HD Audio function | BAR0 | version 1.0 with at least one output stream |
| 5 | GPU boot straps | BAR0 | `PEXTDEV_BOOT_0` is present and distinct from `PMC_BOOT_0` |
| 6 | GPU master-control engines | BAR0 | enable and interrupt registers are distinct |
| 7 | GPU memory apertures | BAR sizes | register BAR is non-prefetchable and framebuffer BAR is prefetchable and large enough |
| 8 | GPU PCI Express link | config | negotiated width does not exceed advertised width |
| 9 | GPU board identity | config | subsystem identity names an add-in-board partner |
| 10 | GPU power management | config | capability version is valid and the function is in D0 |
| 11 | GPU message interrupts | config | MSI count encodings are valid and MSI is disabled |
| 12 | GPU PCI Express endpoint | config | capability type is an endpoint and payload size is supported |
| 13 | GPU expansion ROM declaration | config | ROM decoding is disabled and reserved bits are clear |
| 14 | GPU timer scale | BAR0 | rate registers are stable and non-degenerate while the timer advances |

Probes 0–7 and 14 are NVIDIA-specific. Probes 8, 10, 11, and 12 validate
standard PCI capabilities. Probes 9 and 13 combine standard PCI fields with
the NVIDIA-specific identity and ROM checks.

Probe 0 runs first because probes 1 and 3 select offsets from the decoded
architecture. Probe 6 depends on probe 0, probe 13 on probe 3, and probe 14 on
probe 2. The foundation test validates this order.

## Register writes and DMA

Fourteen probes are read-only. The video-BIOS probe clears the PROM shadow bit,
reads the ROM window, restores the original value, and verifies the restored
state. Verification pins that write sequence and rejects additional writes.

No probe enables bus mastering or allocates DMA. Phipia has no IOMMU, so these
paths remain configuration- and register-only.

## Identity decode

`PMC_BOOT_0` uses the same decode as Nouveau:

```text
chipset  = (boot0 & 0x1ff00000) >> 20
revision =  boot0 & 0x000000ff
family   =  chipset & 0x1f0
```

| family | architecture | family | architecture |
| --- | --- | --- | --- |
| 0x010 | Celsius | 0x0e0/0x0f0/0x100 | Kepler |
| 0x020 | Kelvin | 0x110/0x120 | Maxwell |
| 0x030 | Rankine | 0x130 | Pascal |
| 0x040/0x060 | Curie | 0x140 | Volta |
| 0x050/0x080/0x090/0x0a0 | Tesla | 0x160 | Turing |
| 0x0c0/0x0d0 | Fermi | 0x170 | Ampere |
| | | 0x190 | Ada |

Zero, all-ones, and unknown family values return named errors. The `nvidia` and
`nvidia-builtin` scenarios re-derive thirteen encodings independently of the
driver table.

## Video BIOS

`src/rust/nvbios.rs` validates the ROM bytes. It checks:

- the `0xAA55` signature and PCIR pointer;
- the `PCIR` signature, NVIDIA vendor ID, image length, and x86 code type;
- the NVIDIA BIT signature and token table;
- every referenced byte range against the admitted image.

The reference ROM is synthetic and uses device ID `0x5341`. Independent C,
Rust, and Python descriptions must produce identical bytes. Sixteen corruption
controls each alter one structural field and require a specific parser error.

## Verification

The `nvidia` scenario attaches non-NVIDIA display and audio functions and
requires all of them to remain unbound. It also checks the Boot Ledger receipt
and the frame, paging, DMA, PCI, vector, and MSI-X census.

The `nvidia-builtin` scenario runs without a matching device. It requires the
foundation stage to pass and the probe stage to record a neutral skip.

With the QEMU model, fourteen probes bind; the audio probe remains absent
because the model exposes no audio function. The run checks the command-line
identity values, configuration mirror, advancing timer, synthetic ROM ID,
strap register, BAR layout, PCI capabilities, and exact read/write counts.

The model supports these negative controls:

| Model change | Expected refusal |
| --- | --- |
| unknown `boot0` family | NVIDIA device identity |
| straps equal to `boot0` | aliased register aperture |
| all-ones straps | board straps |
| zero subsystem ID | board identity |
| all-ones enable register | device identity |
| altered PCIR vendor byte | video BIOS image |
| zero timer numerator | timer scale |
| all-ones timer denominator | timer scale |
| enabled ROM BAR | ROM declaration |
| reserved ROM BAR bit | ROM declaration |
| missing power capability | power capability |
| enabled MSI | message-interrupt state |
| root-port capability type | endpoint type |
| timer-rate alias | timer scale |

## Scope

These probes identify functions and validate register contracts. They do not
perform mode setting, framebuffer programming, display-link setup, command
submission, firmware loading, interrupt handling, GPU memory management, or
power-state changes.

The ROM parser reads an 8 KiB PROM prefix and accepts BIT-based images. The
probe stage runs only in the dedicated scenario because it claims a live
graphics function and temporarily changes the PROM shadow bit.
