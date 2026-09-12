# Phipia BearSSL port boundary

The files below `inc/` and `src/` are byte-for-byte BearSSL 0.6 upstream
sources. Phipia does not patch its cryptographic primitives or protocol state
machine.

The SDK compiles the sources freestanding and disables host entropy and clock
adapters. It also disables architecture-specific SSE2, AES-NI, and POWER8
implementations so the first supported profile has no undeclared CPU-feature
dependency. Phipia integration must:

- inject entropy obtained through the native entropy service;
- call `br_x509_minimal_set_time()` with the validated realtime clock;
- require TLS 1.2 and an explicitly selected bounded cipher-suite list;
- pass a nonempty canonical DNS hostname to `br_ssl_client_reset()`;
- provide externally pinned trust anchors and refuse an empty trust store;
- drive transport through public native stream calls with monotonic deadlines;
- disable renegotiation and reclaim every context, buffer, and stream on all
  exits.

The vendored archive alone establishes a reproducible cryptographic dependency.
It does not establish working TLS, HTTPS, certificate validation, or package
transport until those integration requirements have QEMU evidence.
