<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native platform scope

Native ABI v1 has this bounded hardware and compatibility profile:

- x86_64 static `ET_EXEC` plus a bounded authenticated PIE/shared-object
  profile; no interpreter, `dlopen`, symbol versions, lazy binding, `fork`,
  `exec`, Unix signals, or general POSIX process model;
- one core, four processes, eight threads per process, 128 handles, 256 MiB
  manifest memory ceiling, and a 16 MiB executable/file ceiling;
- FAT32 only, with the existing 64 MiB geometry, ASCII 8.3 names, no long file
  names, no journal, and application-rooted writable Data namespaces;
- one native Phipia window per process, xRGB8888 only, bounded 64-event queues,
  and no GPU command submission or direct framebuffer mapping;
- IPv4 on the deterministic virtio-net profile, with DNS, TCP, UDP, plain HTTP,
  and a bounded validated TLS 1.2/HTTPS client; no IPv6, firewall, routing,
  Wi-Fi, general Internet, or physical-NIC claim;
- no IOMMU and no userspace access to physical, MMIO, DMA, or page-table
  addresses;
- a scoped C runtime and Rust `no_std` crate, not complete libc, full libm,
  POSIX sockets, C++ runtime, or Rust `std` support;
- CMOS UTC has one-second resolution, a 1970-through-9999 range, no timezone
  database, and no persistent anti-rollback policy;
- HD Audio provides one 48 kHz, signed-16-bit stereo playback stream with two
  logical handles for one owning process.

Unsupported operations return explicit errors rather than success stubs.
