<!-- SPDX-License-Identifier: GPL-3.0-only -->

# TLS profile and release gate

Phipia includes a pinned BearSSL TLS 1.2 SDK client for bounded,
single-response HTTPS downloads. The desktop HTTP parser remains a separate
plaintext facility. `TLS.md` and `HTTPS.md` define the implemented profile.

## Platform requirements

The SDK profile depends on kernel entropy, validated realtime, DNS, streams,
deadlines, cancellation, and handle services. General Internet release
qualification requires:

- a cryptographically strong, continuously health-checked entropy source and
  DRBG with explicit failure behavior;
- the available validated RTC calendar time plus a policy for rollback;
- a checksum-pinned root store with a documented update/revocation process;
- strict DNS-name/SAN matching, SNI, certificate-chain limits, signature and
  key-size policy, expiry checks, unknown-critical-extension refusal, and test
  vectors for every parser;
- secret-zeroization rules, bounded stacks/buffers, constant-time primitive
  selection, and no secret-dependent logging;
- nonblocking TLS state integrated with authenticated handles, readiness,
  deadlines, cancellation, reset, partial I/O, close-notify and process death;
- reproducible source pinning, license review, fuzzing, interoperability tests,
  malformed-record/certificate controls, and independent security review.

## Library assessment

BearSSL fits the bounded client. Its
[documented design](https://bearssl.org/) uses a caller-driven state machine,
does no dynamic allocation, is intended for small and bare-metal systems, and
can integrate with polling outside the engine. Its
[API documentation](https://bearssl.org/apidoc/index.html) makes callers own
all context and record buffers, which is compatible with Phipia's explicit
bounds.

BearSSL supplies the TLS 1.2 engine, not the surrounding web stack. Phipia owns
entropy, time, trust anchors, hostname policy, I/O adaptation, updates,
zeroization, and security response.

## Implemented bounded profile

The profile is a client-only static build with one connection, TLS 1.2
ECDHE, AEAD-only cipher suites, SNI, external bounded trust anchors, strict
hostname/chain/time validation, fixed record and header buffers, no client
certificates, no renegotiation, no session persistence, and explicit refusal
of unsupported or oversized HTTP records.

## Release gate

HTTPS may be named as a capability only after an exact-commit QEMU capture shows
DNS, TCP, handshake, certificate validation, encrypted HTTP, close and resource
teardown; the packet audit confirms that HTTP plaintext is absent from PCAP;
expired, wrong-name, unknown-root, malformed-chain, weak-algorithm, bad-record,
truncation, replay, entropy-failure, timeout and reset controls all fail closed;
and an independent review has no unresolved high-severity finding.

The current SDK/QEMU evidence covers the bounded profile above.
