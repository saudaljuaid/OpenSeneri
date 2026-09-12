<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Phipia 2.1.0 networking evidence

These historical text receipts describe a QEMU 11.1.0 interaction with the
deterministic offline Ethernet peer. Keyboard and pointer events entered through
QEMU's guest input path. The Terminal performed device inspection, DHCP, ICMP
echo, DNS A resolution, TCP/HTTP, synchronized FAT32 download, and `netstat`.

The identity migration normalized names in these text files. Their manifest
digests cover the resulting committed bytes, not the original capture text.
The screenshot, video, and PCAP named in `SHA256SUMS` are external historical
artifacts and are not committed here; their retained hashes were not regenerated.
These receipts are not evidence for the current PR head. Current Linux workflow
release directories contain freshly generated reports, media, and manifests.

The screenshot shows the final guest framebuffer. The video is exactly 22.000
seconds. The PCAP contains guest and peer Ethernet frames; the independent JSON
audit reconstructs ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP and HTTP with no
malformed frame and `production_path: true`. The serial log is the guest's
concurrent diagnostic stream. The FAT32 report was produced after QEMU closed
the synchronized Data image and records `NETCAP.TXT` as a 30-byte file with
matching FAT copies and no cycle, cross-link or leak.

The local Windows capture used TCG with
`-icount shift=auto,align=off,sleep=on` to keep emulated clocks synchronized
while recording frames. The dedicated `networking-milestone.yml` workflow
regenerates the same class of evidence from each exact Linux CI commit under
TCG, runs the declared production scenario matrix, and publishes the full release
bundle.

No file here is evidence of TLS, HTTPS, a browser, IPv6, firewalling, Wi-Fi, or
physical NIC support.
