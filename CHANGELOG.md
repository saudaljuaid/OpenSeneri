<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Changelog

## Phipia

- Replaced the former shell presentation with the Phipia desktop and the
  canonical Phipia identity.
- Added the six-application 3D Dock with magnification, reflections, tooltips,
  launch feedback, and light and dark shelf colours.
- Added overlapping movable windows with focus, stacking, close controls, and
  spring opening animation.
- Added fourteen photographic desktops and a functional Settings application.
- Added Files and Notes over the writable FAT32 data volume.
- Added the Camera application and double-buffered frame-provider interface.
  The default QEMU profile reports that no camera is connected.
- Added the native Media Editor window with BMP import, timeline editing, project
  persistence, and BMP export.
- Updated the vendored Media Editor source to commit
  `034ba9336f6dee3cd5a524a42b740b41013ca852`.
- Added high-resolution screenshots and a 25-second QEMU demonstration.

## 2.2.0

- Added up to four isolated user processes with private address spaces and a
  round-robin scheduler.
- Added fault containment and full saved-register restoration for user tasks.
- Added thirteen PCI drivers for Intel, Realtek, AMD, Cirrus Logic, and Bochs
  devices.
- Added an HD Audio command/response transport and codec discovery.
- Added a TCP listener with accepted child connections, retransmission limits,
  cleanup, readiness, and closed-port resets.
- Added fifteen NVIDIA register and configuration readers plus a Rust VBIOS
  validator and device-model tests.
- Increased the QEMU suite to 101 scenarios.

This release omitted `fork`, `exec`, signals, process IDs, IPC, preemptive user
scheduling, HD Audio streaming, NVIDIA graphics acceleration, and mode setting.

## 2.1.0

- Added modern virtio-net PCI, MSI-X, and DMA support.
- Added Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP, and HTTP/1.1.
- Added native networking handles with polling, cancellation, time, and random
  byte services.
- Added Terminal networking commands and streamed downloads to FAT32.
- Added an offline network peer, PCAP reconstruction, and 34 QEMU scenarios.
- Added browser-port and TLS prerequisite documents.

This release is IPv4-only and does not include TLS, HTTPS, a browser, Wi-Fi,
firewalling, routing, or physical-NIC support.
