#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Audit or emit the deterministic BearSSL test trust anchor."""

from __future__ import annotations

import argparse
import hashlib
import re
import ssl
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "tests" / "fixtures" / "tls" / "anchor.txt"
HEADER = ROOT / "apps" / "native-https" / "trust_anchor.h"
SOURCE_SHA256 = "23cbda48ee8b659919b25760011481d7cf7db444af21adc9d47095a24bdc4503"
FIELDS = ("dn", "n", "e")
CERTIFICATES = {
    "ca.pem": "d13a4f5b79fbebfe1bd69401cea778fa8e1827c4d1ec16cab1a5d06ec9d5dac5",
    "valid.pem": "966961e7cbd7922bc6ab207dfc60bd221de24fb17d6119cb42d1fb52935ca20f",
    "valid-key.pem": "55c0533e694dc6e038cb8cc533e132e74d356a40972a684ca69d094bb59764fe",
    "expired.pem": "1896a926080114f6fd04960cc31c7234a79be0e7b27844ff336b005f1bb26d21",
    "expired-key.pem": "f207af1bc263e1acee9d40f12f58856a5e107b56e6da45fbc10ab72b23839936",
    "future.pem": "ff3696429950a6aefd347e49c4f1f0e725ccdfac5f779c45a2732b233de8a8c4",
    "future-key.pem": "83401a2d3db347b41b35014e384dd601b42ef81587daf063d3ca357b112ffa51",
    "untrusted.pem": "aab065072e062305cbc0775e64df403e345304c6df62ec97bcc8bcc9fd738a82",
    "untrusted-key.pem": "4db57fbd96f5810ed07c17c9cb032ece5b4a3aec59f97736abb5c2b469afea8e",
}


def load_source() -> dict[str, bytes]:
    raw = SOURCE.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        raise ValueError("offline root anchor digest changed")
    records: dict[str, bytes] = {}
    for line in raw.decode("ascii").splitlines():
        name, separator, encoded = line.partition("=")
        if not separator or name not in FIELDS or name in records:
            raise ValueError("non-canonical anchor record")
        if not encoded or len(encoded) % 2 or encoded != encoded.lower():
            raise ValueError(f"non-canonical {name} hex")
        try:
            records[name] = bytes.fromhex(encoded)
        except ValueError as error:
            raise ValueError(f"invalid {name} hex") from error
    if tuple(records) != FIELDS:
        raise ValueError("anchor fields are missing or reordered")
    if not (1 <= len(records["dn"]) <= 4096):
        raise ValueError("anchor DN is out of bounds")
    if not (256 <= len(records["n"]) <= 512):
        raise ValueError("anchor RSA modulus is out of bounds")
    if records["e"] != b"\x01\x00\x01":
        raise ValueError("anchor RSA exponent is not the pinned value")
    return records


def extract_array(header: str, symbol: str) -> bytes:
    match = re.search(
        rf"static unsigned char {re.escape(symbol)}\[\] =\s*(.*?);",
        header,
        flags=re.DOTALL,
    )
    if match is None:
        raise ValueError(f"missing generated array {symbol}")
    encoded = "".join(re.findall(r'"([^"\n]*)"', match.group(1)))
    if re.sub(r"\\x[0-9a-f]{2}", "", encoded):
        raise ValueError(f"non-canonical C escaping in {symbol}")
    return bytes.fromhex(encoded.replace("\\x", ""))


def audit(records: dict[str, bytes]) -> None:
    header = HEADER.read_text(encoding="ascii")
    for field, symbol in (
        ("dn", "phipia_https_test_dn"),
        ("n", "phipia_https_test_n"),
        ("e", "phipia_https_test_e"),
    ):
        if extract_array(header, symbol) != records[field]:
            raise ValueError(f"{symbol} does not match the pinned root")
    required = (
        "static const br_x509_trust_anchor phipia_https_test_anchors[]",
        "BR_X509_TA_CA",
        "BR_KEYTYPE_RSA",
    )
    if any(item not in header for item in required):
        raise ValueError("generated anchor metadata is incomplete")
    fixture = SOURCE.parent
    for name, expected in CERTIFICATES.items():
        if hashlib.sha256((fixture / name).read_bytes()).hexdigest() != expected:
            raise ValueError(f"offline certificate fixture changed: {name}")
    for name in ("valid", "expired", "future", "untrusted"):
        decoded = ssl._ssl._test_decode_cert(str(fixture / f"{name}.pem"))
        if decoded.get("subjectAltName") != (("DNS", "repo.phipia.test"),):
            raise ValueError(f"offline certificate SAN changed: {name}")
    valid = ssl._ssl._test_decode_cert(str(fixture / "valid.pem"))
    if valid.get("notBefore") != "Jan  1 00:00:00 2026 GMT" or \
            valid.get("notAfter") != "Dec 31 00:00:00 2027 GMT":
        raise ValueError("valid offline certificate interval changed")
    print(
        "HTTPS certificate/anchor audit passed: sha256="
        f"{SOURCE_SHA256} dn={len(records['dn'])} rsa={len(records['n']) * 8}"
    )


def emit(records: dict[str, bytes]) -> None:
    for field in FIELDS:
        data = records[field]
        print(f"{field}={''.join(f'\\x{value:02x}' for value in data)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("audit", "emit"))
    args = parser.parse_args()
    records = load_source()
    if args.command == "audit":
        audit(records)
    else:
        emit(records)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
