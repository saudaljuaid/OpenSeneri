# Phipia Monocypher boundary

The retained files are byte-for-byte Monocypher 4.0.3 sources. Phipia does not
patch Monocypher's arithmetic, SHA-512, or EdDSA equation implementation. The
kernel compiles the two translation units freestanding with MMX/SSE disabled.

Monocypher intentionally uses the cofactored Ed25519-Zebra equation. Phipia's
package wrapper narrows that profile in `src/kernel/package_trust.c`: public
keys and signature `R` must use canonical field encodings, pure low-order points
and negative-zero encodings are refused, and Monocypher's `S < L` check remains
mandatory. Immutable public keys must be generated Ed25519 keys; the raw table
parser does not independently prove prime-subgroup membership. The cofactored
equation can admit mixed-order `R`, so callers must never use signature bytes as
a uniqueness identifier. This does not permit changing the authenticated
repository/package content. The wrapper hashes
`R || A || message` incrementally while substituting exactly 64 zero bytes for
the embedded signature envelope, then calls the upstream reduced-equation
check. It never signs or handles private key material.

The immutable key table validates each SHA-256 key ID, strict public-key
encoding, sort order, uniqueness, and trusted/revoked status before exposing
the package-manager callbacks. Unknown, revoked, malformed, unconfigured, and
cryptographically invalid inputs fail closed.

The host package-manager proof feeds Python `cryptography`-signed repository
and package bytes through this production C verifier. Focused negative tests
cover wrong zero ranges, message/signature changes, non-canonical and pure
low-order points, `S >= L`, revoked keys, key-ID mismatch, and table ordering.
This establishes package admission only; HTTPS download, generation staging,
transaction commit, CLI/Store presentation, and launch lifecycle require their
own integration evidence.
