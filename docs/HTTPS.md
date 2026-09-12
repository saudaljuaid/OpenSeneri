<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Bounded native HTTPS profile

`phipia_https_get()` and `phipia_https_get_stream()` are bounded HTTP/1.1
download operations over Phipia's BearSSL TLS 1.2 client. They are Ring 3 SDK
facilities; they reach the network only through `phipia_dns_resolve()` and the
Phipia stream API. The first fills one caller buffer. The streaming form uses a
bounded 4 KiB transfer buffer and requires a sink to accept each complete chunk,
so repository indexes and packages do not need one contiguous application
allocation. A short or failed sink write aborts the connection with a distinct
status; callers can therefore stage to a temporary file, hash, sync, and publish
only after the complete authenticated response succeeds.

`phipia_package_fetch_stage()` applies that pattern to the Data volume. It
streams into a temporary path while computing SHA-256, optionally requires an
exact signed length and digest, closes and flushes the temporary file, atomically
replaces an inert staging path, and flushes the namespace change. Every
pre-publish refusal removes and syncs the temporary file. A failure after replace
reports `published=true` and `durable=false` so the caller must re-open and
authenticate the staged bytes after reboot; it never treats transport success as
package admission or advances installed-package authority.

The caller supplies one lowercase DNS hostname, a port, an origin-form path,
an immutable external trust-anchor set, one absolute monotonic deadline, and a
fixed output buffer or maximum streamed body length. The TLS layer snapshots at
most 16 anchors and 80 KiB of
anchor bytes before performing external work. DNS, SNI, the HTTP `Host` field,
and X.509 hostname validation use the same hostname. BearSSL checks the chain
against the explicitly supplied anchors and checks validity against the
kernel's wall clock.

The accepted response profile is intentionally strict:

- `HTTP/1.1` and status `200` only;
- at most 4,096 response-header bytes;
- one decimal `Content-Length`, with no duplicate and no overflow;
- no `Transfer-Encoding`;
- a length no larger than the caller's output buffer;
- exactly that many authenticated body bytes;
- no additional application byte after the body; and
- an authenticated TLS `close_notify` after the exact body.

The request sends `Accept-Encoding: identity` and `Connection: close`. It does
not follow redirects, decode chunked framing or compression, upload request
bodies, pool connections, negotiate HTTP/2, or consult the host/firmware trust
store. These are security bounds, not a claim of general Internet
compatibility.

## Refusals and teardown

The public HTTPS status names distinguish invalid trust input, implausible
wall clock, entropy and DNS failures, deadline, cancellation, reset before any
TLS record bytes, authenticated-stream truncation, hostname mismatch,
certificate-time failure, unknown/bad authentication, TLS handshake/I/O,
HTTP version/status/header framing, missing/oversized length, short/extra body,
and close failure. `phipia_https_response` retains the BearSSL and raw Phipia
transport values for diagnostics.

Every failure after stream creation cancels, shuts down, and closes the stream
before returning. A reset after TLS record bytes is reported as
TLS truncation; Phipia ABI v1 otherwise exposes orderly TCP closure and reset
through overlapping stream errors. Cancellation is also available directly
through `phipia_tls_client_cancel()` on the lower-level client. The synchronous
`phipia_https_get()` helper does not expose its internal client, so callers
cancel that complete operation through native process termination; teardown
closes its typed network handles.

## Deterministic offline evidence

`tools/https_anchor.py audit` verifies the fixed certificate/key hashes,
certificate SAN and validity interval, and the generated BearSSL anchor bytes.
No key or certificate is generated during a test. These are public test-only
keys and must never be installed as production roots.

`tools/https_host_test.py` drives the compiled SDK source and pinned BearSSL
archive over real loopback TCP. Its buffered and streaming success cases verify
the canonical request, chain, hostname, time, exact body, and authenticated
close; a separate case proves sink refusal tears down without accepting a
partial result. Negative
peers name and prove hostname, expired, future, untrusted, timeout,
reset, truncated TLS, wrong HTTP version/status, duplicate or transfer framing,
non-identity content encoding, malformed line endings, missing/oversized
length, short body, and extra body refusal. The same harness interrupts a
blocking lower-level TLS read from a second thread. The host adapter reports
`HTTPS HANDLES clean` after every case.

`apps/native-https/` is the freestanding Ring 3 proof client. It exercises the
streaming SHA-256 staging path, both durability barriers, atomic publication,
and a second HTTPS transfer directly into a kernel-owned package-upload handle.
The upload is accepted only after its privileged caller-supplied length and
SHA-256 match and the data-volume flush completes; closing the typed handle
durably removes the private staging file. This proof uses a fixed expected
digest. The package controller binds those same upload bytes to an admitted
signed repository record before installation; the `native-phip` proof carries
that path through update, rollback refusal, damage quarantine, and repair.
`tools/https_network_fixture.py` is its offline raw-Ethernet peer for QEMU's
dgram backend: it supplies DHCP/ARP/DNS, a TCP peer on 10.0.2.20:443, a Python
`ssl.MemoryBIO` TLS 1.2 server using the committed certificate, strict request
bytes, a fixed response, and `close_notify`. Network topology, certificate,
request, and response are fixed; TLS nonces and ciphertext are intentionally
fresh on each run.

## Guest verification

`make https-tests` audits the immutable anchor, self-tests the raw network
fixture, and runs the compiled 20-case host matrix, including expiry between
TLS operations. It is part of `make verify`.
`make native-https-proof` additionally builds the Ring 3 application, package,
and isolated System/Data images.

`qemu-test-native-https` boots that package with the offline peer under QEMU's
`max` CPU profile and pins the virtual RTC to 2026-08-31 inside the committed
test certificate interval. The guest
must emit its start, authenticated-download, durable-output, kernel-upload, and final success
markers exactly once, including the strong-hardware-entropy marker. The kernel
reopens `HTTPSAPP/HTTPS.TXT`, checks the exact
33-byte body, requires normal process exit, and requires zero native-process,
TCP, UDP, and network-timer resources before the scenario may pass. The native
workflow retains the serial log, Data image, encrypted packet capture, packet
audit, fixture log, application ELF, and link map from the same commit. The
packet audit reconstructs the Ethernet/IP/TCP path, requires TLS 1.2 records on
port 443, and refuses captures containing the request, HTTP status line, or
body in plaintext.

This is an in-guest TLS 1.2/HTTPS download over Phipia DNS and TCP. The client
writes an exact-digest, durably staged inert body to its Data namespace and
separately proves the non-path-based privileged upload boundary without
installing or executing it. There is no general
Internet, redirect, chunked/compressed response, HTTP/2, mutable system trust
store, OCSP/CRL, certificate pin update, or persistent clock anti-rollback
claim.
