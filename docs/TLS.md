<!-- SPDX-License-Identifier: GPL-3.0-only -->

# TLS client boundary

Phipia's first transport-TLS profile uses the pinned BearSSL 0.6 archive in the
SDK. `phipia/tls.h` is a native userspace client API; it uses only public Phipia
DNS, stream, entropy, realtime, monotonic-deadline, and handle services.

The profile is bounded:

- TLS 1.2 only;
- `ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256`, followed by
  `ECDHE_RSA_WITH_AES_128_GCM_SHA256`;
- RSA certificate keys of at least 2,048 bits, with RSA trust anchors admitted
  only through 4,096 bits; P-256 EC trust anchors are also accepted;
- lowercase canonical DNS hostnames, 253 bytes total and 63 bytes per label;
- at most 16 external CA trust anchors and 80 KiB of admitted DN/key bytes;
- no renegotiation or session resumption;
- a fixed 4,096-step handshake work bound plus monotonic deadlines on every
  underlying transport operation.

`phipia_tls_client_open()` refuses an empty or malformed trust store. It passes
the same nonempty hostname to DNS, SNI, and BearSSL's minimal X.509 validator,
so a valid chain for another host is not accepted. It reads the kernel's
validated realtime seconds and converts them to BearSSL's proleptic-Gregorian
day count for certificate validity. Monotonic time remains the only deadline
source. Thirty-two bytes from `RANDOM_STRONG` seed each independent client
engine. That call bypasses Phipia's non-cryptographic generator, samples
RDSEED/RDRAND directly with a continuous repetition check, and fails closed
when the hardware source is absent or stops producing fresh words. Host random
and clock adapters are disabled at compile time.

Every failure after stream creation shuts down and closes the stream and wipes
the client and anchor allocations through non-elidable volatile stores.
`phipia_tls_client_close()` attempts authenticated
`close_notify`, then tears down the transport even when the peer omits its
reply. BearSSL and native transport error values remain separately queryable
while the client is alive.

## Trust-anchor snapshot

The API accepts BearSSL's public `br_x509_trust_anchor` records as immutable
input. Before entropy, DNS, or stream work, it validates every record and makes
a bounded private snapshot of the anchor records, DNs, and key bytes. The
caller may therefore release its input after `phipia_tls_client_open()`
returns. Proof packages embed a fixed test anchor audited against the
deterministic offline CA; a future system trust-store service must still make
an explicit publisher/policy decision before constructing these records.

`phipia_tls_client_open_diagnostic()` preserves the BearSSL and Phipia
transport refusal values even though a failed open returns no client object.
`phipia_tls_client_cancel()` atomically publishes cancellation and routes it to
the underlying Phipia stream handle, so a second thread can interrupt a
blocking TLS operation. The owner waits for that operation to return before it
closes the client. Every open and application operation uses the caller's
absolute monotonic deadline. Realtime is accepted only in the explicit
2020--2099 plausibility window before BearSSL applies each certificate's exact
interval.

## Current evidence boundary

`make tls-tests` compiles the same wrapper and pinned BearSSL source against a
POSIX adapter, then connects it over real loopback TCP to a Python TLS 1.2 peer
using the committed offline CA. It proves a valid chain, hostname, fixed
certificate time, application bytes, and authenticated close. Separate peers
prove wrong-host, unknown-root, expired, not-yet-valid, truncated-handshake, and
deadline refusal. The certificates and public test keys are fixed inputs with
recorded checksums; no Internet service or host trust store is consulted.

The SDK now also contains the bounded HTTPS profile documented in
`HTTPS.md`. Its host evidence uses the same wrapper and BearSSL archive. The
`native-https` scenario wires a Ring 3 proof application to a raw QEMU
Ethernet/TLS fixture and requires the exact durable body plus clean process and
network censuses. Passing that scenario is the repository's scoped in-guest
HTTPS claim; it is not a general Internet or browser-security claim.
