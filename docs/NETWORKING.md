<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Networking

Phipia 2.2.0 has a bounded IPv4 networking foundation for one modern
`virtio-net-pci` device under QEMU. Packets cross the normal PCI claim, mapped
BAR, MSI-X, split virtqueue, DMA-ownership, protocol, syscall or Terminal, and
FAT32/NVMe paths. The deterministic peer is a host-side Ethernet endpoint; it
does not inject results into private kernel helpers.

This is not an Internet-security claim. Phipia has no IPv6, general-purpose
transport TLS, firewall, routing, Wi-Fi, physical-NIC support, or browser. A
separate bounded TLS 1.2/HTTPS SDK profile is documented in `TLS.md` and
`HTTPS.md`.

## Device contract

Only PCI ID `1af4:1041` is accepted. The driver requires the modern PCI
capability layout, `VIRTIO_F_VERSION_1`, MAC and status features, at least two
queues, and MSI-X. Legacy/transitional transport, mergeable receive buffers,
offloads, multiqueue, control queues, and indirect descriptors are refused.

| Resource | Bound |
| --- | ---: |
| NICs | 1 |
| RX/TX queue descriptors | 16 / 16 |
| RX/TX packet reserves | 32 / 16 |
| packet arena | 48 × 2,048 bytes |
| accepted Ethernet frame | 1,514 bytes |
| MSI-X entries used | 1 |

Every packet has an explicit owner. Reset first stops the device, disables bus
mastering, unbinds MSI-X, returns DMA to the CPU, releases the allocations and
PCI claim, invalidates sockets and caches, and advances the device generation.
Stale handles cannot alias a later device generation.

## Protocol contract

- Ethernet II accepts only the configured unicast MAC, broadcast, and required
  IPv4 multicast forms. Unsupported EtherTypes and malformed lengths are
  counted and dropped.
- ARP has eight authenticated entries, three 500 ms attempts, a 60 s lifetime,
  conflict detection, and generation invalidation.
- IPv4 validates version, IHL, total length, TTL, header checksum, destination,
  and fragmentation flags before dispatch. Fragment reassembly is absent.
- ICMP implements bounded echo request/reply and reports timeouts. UDP validates
  pseudo-header checksums when present; IPv4 UDP zero-checksum datagrams are
  accepted as the protocol permits.
- DHCP performs DISCOVER/OFFER/REQUEST/ACK with three bounded attempts. It
  validates transaction, client identity, message type, server identity,
  subnet/router/DNS values, lease, renewal, and rebinding options. NAK and
  timeout leave no partial configuration.
- DNS supports bounded A and CNAME resolution, compression pointers with a
  16-pointer loop bound, four CNAME follows, 512-byte messages, eight cached
  entries, negative answers, TTL expiry, and configuration/device generations.
- TCP provides eight connections, 8,192 receive bytes and one 1,460-byte
  retransmission segment per connection, four retransmissions, checked sequence
  and acknowledgement state, active and passive open, FIN close, RST handling
  in both directions, polling, cancellation, and owner/generation isolation.
  Congestion control is limited to this bounded profile.
- HTTP/1.1 accepts `http://` URLs only. It bounds headers to 4,096 bytes and 32
  fields, supports `Content-Length`, chunked transfer, and four redirects, and
  rejects conflicting framing, malformed chunks/status/header lines, redirect
  loops, unsupported schemes, truncation, and bodies above 16 MiB.

## Passive open

Until 2.2.0 Phipia could only be a TCP client. It can now also be the side that
waits. A socket enters `LISTEN` on one port with a declared backlog of at most
four; a SYN arriving for that port with no connection already matching its
four-tuple produces a child connection in `SYN_RECEIVED`, drawn from the same
eight-slot table an outbound connection is drawn from, and `network_tcp_accept`
hands it over once the peer's acknowledgement completes the handshake.

Three bounds define listener behavior:

- **A listener costs nothing before acceptance.** A handshake completes on
  any pump -- the peer's acknowledgement is an inbound segment like any other --
  but *retransmission and reaping* of half-open children happen only inside
  `network_tcp_accept`. A peer that opens a connection and vanishes therefore
  leaves nothing durable behind, and a listener whose peer's hardware address is
  unknown makes progress only while an accept is outstanding. Listener work is
  driven by `network_tcp_accept` rather than a background timer.
- **Closing a listener refuses its unaccepted children.** Such a child belongs
  to the listener, and closing the listener resets those peers and reclaims
  their slots. A child already accepted is an independent connection with its
  own handle and is left alone.
- **The backlog is checked before a slot is taken, not after.** A SYN beyond
  the declared backlog, or beyond the connection table, is refused with a reset
  rather than queued.

`network_poll` reports a listener as `NETWORK_READY_ACCEPTABLE` when a
completed connection is waiting, and never as connected or writable.

The `network-tcp-listen` scenario proves both halves. Its first peer is
accepted, sends bytes, receives bytes, closes, and is closed. Its second peer is
left unaccepted: the listener is polled until it reports the waiting
connection as acceptable, then closed, and the peer reports the reset it
received back over UDP. Nothing is left allocated afterwards.

## Closed ports are answered

A TCP segment that matches no connection and no listener is now answered with a
reset instead of being dropped in silence, with the sequence numbers RFC 793
section 3.4 specifies. A reset is never answered with a reset -- that is the
rule that stops two closed ports from talking to each other forever -- and a
refusal is only sent when the stack is configured and the peer is unicast.

There is no rate limit on refusals beyond the one segment in, one segment out
that the receive loop already imposes and the transmit queue's own refusal when
it is full. That is a deliberate non-claim: this is not a stack hardened against
a flood, and a token bucket would need its own evidence rather than a comment.

## The pump runs alone

One receive buffer and one transmit buffer serve the whole stack. A handler
that answers the frame it is reading -- an ICMP echo, a TCP acknowledgement, a
refusal, the acknowledgement to a passive open's SYN -- reaches a send, and a
send that needs an unknown hardware address used to wait for the ARP reply by
pumping the device again, from inside the loop that owned the buffer being
parsed. That is a remote-triggerable buffer reuse: any peer whose hardware
address had expired could arrange it with one echo request.

`network_service` now refuses recursive entry, and while it holds,
`arp_resolve` turns its wait into a single ARP request and reports
`NETWORK_STATUS_WOULD_BLOCK`. The send does not happen; the caller's
retransmission carries it once the reply lands. The `network-tcp-listen`
scenario is arranged to take exactly that path -- it announces its port to the
gateway, so the peer's hardware address is still unknown when the peer's SYN
arrives -- and requires the deferral, the retransmission and the completed
handshake all to be visible in the statistics.

HTTP downloads use a temporary FAT32 path and synchronized replacement. A
failed transfer removes its temporary state; the previous destination remains
intact. Nested 8.3 paths, full-media refusal, clean reboot persistence, and the
immutable system volume are QEMU-tested.

## Public kernel and syscall bounds

`include/phipia/network.h` is native ABI version 1. It exposes explicit owners,
generation-authenticated handles, deadlines, readiness and cancellation. The
global bounds are eight UDP sockets, eight TCP connections, 32 timers, eight
poll handles per call, four queued datagrams per UDP socket, and 512 bytes per
datagram.

`include/phipia/network_syscall.h` is an experimental Phipia-private ABI version
1 for future native processes. At most four authenticated process contexts may
exist. A request transfers at most 4,096 bytes, random requests at most 256
bytes, and any deadline at most 30 seconds. Before the first copy, every page of
every user range is translated and checked for user access, leaf level,
writability where required, canonical range shape, overflow, and allocatable
physical backing. Process termination cancels owned work and invalidates its
token.

Operations cover monotonic time, bounded random bytes, DNS, TCP lifecycle and
I/O, poll, cancel, HTTP-to-memory, and HTTP-to-file. The
`network-http-length` scenario constructs a real private process address space,
dispatches HTTP-to-FAT32 through this boundary, authenticates the response,
invalidates the terminated token, and proves complete page/frame teardown.

## Terminal use

```text
network
dhcp
ip 10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.3
arp
ping 10.0.2.2 1
resolve phipia.test
http http://phipia.test/welcome.txt NETCAP.TXT
netstat
```

The `http` command writes only to the Data volume. `netstat` reports bounded
resource use, RX/TX counts, accepted Ethernet/IPv4/UDP traffic, malformed
packets, and IPv4 checksum failures.

## Entropy

`random.c` mixes RDSEED and RDRAND when available with calibrated timing and
monotonic state. Boot explicitly records `strong`, `hardware`, or `degraded`.
The API never claims cryptographic strength when only the degraded source is
available. DHCP/DNS/TCP identifiers still avoid fixed constants. The bounded
TLS client instead uses the fail-closed `RANDOM_STRONG` call, which bypasses the
non-cryptographic generator and samples repetition-checked RDSEED/RDRAND output
directly. Other cryptographic protocols remain outside this networking profile.

## Deterministic evidence

`tools/network_fixture.py` is an offline unicast Ethernet peer with deterministic
DHCP, ARP, ICMP, UDP, DNS, TCP, and HTTP behavior. Its negative modes cover
silence/timeouts, NAK, NXDOMAIN, truncation, CNAME, bad checksum, ARP conflict,
TCP reset/retransmission, HTTP chunking/redirect/truncation/malformed framing,
redirect loops, and malformed floods. Two modes reverse the roles: the guest
announces a port over UDP and the peer opens a TCP connection *to* it, either to
a port Phipia is listening on or to one with no listener. It writes
classic PCAP with deterministic packet timestamps.

`tools/network_packet_audit.py` independently reconstructs the captured
Ethernet frames and requires traffic in both directions plus ARP, IPv4, ICMP,
UDP, DHCP, DNS, TCP, and HTTP. The production proof is false if any layer is
missing. `tools/run_network_scenario.py` owns fixture/QEMU lifecycle, isolated
ports, per-test storage copies, link-down QMP control, stable exit codes, serial
markers, and packet audit.

## Measured reference run

These are diagnostic measurements from the 30-byte offline fixture under QEMU
TCG on the v2.1.0 development host, not general throughput claims:

| Path | HTTP elapsed | FAT32 sync | payload rate | polling CPU | interrupt CPU | drops |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| chunked | 44.53 ms | 16.10 ms | 673 B/s | 2.34 ms | 0.087 ms | 0 |
| one redirect | 46.62 ms | 16.06 ms | 643 B/s | 4.42 ms | 0.136 ms | 0 |

The 22-second evidence interaction completed DHCP, ping, DNS, HTTP streaming,
FAT32 synchronization, screen updates, keyboard/pointer input, and `netstat`
without packet drops.
