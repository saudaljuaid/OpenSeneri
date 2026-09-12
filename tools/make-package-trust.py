#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build and audit Phipia's immutable platform package trust table."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile
from typing import Any


MAGIC = b"PHIPKEY1"
VERSION = 1
HEADER_BYTES = 128
RECORD_BYTES = 96
MAX_KEYS = 64
MAX_SPEC_BYTES = 64 * 1024
STATUS = {"trusted": 1, "revoked": 2}


class TrustTableError(ValueError):
    """A trust-table input is not canonical or within bounds."""


def _read_bounded(path: Path, maximum: int, description: str) -> bytes:
    if not path.is_file():
        raise TrustTableError(f"{description} is not a regular file")
    size = path.stat().st_size
    if size > maximum:
        raise TrustTableError(f"{description} exceeds {maximum} bytes")
    data = path.read_bytes()
    if len(data) != size:
        raise TrustTableError(f"{description} changed while reading")
    return data


def _load_spec(path: Path) -> list[tuple[bytes, bytes, int]]:
    try:
        value: Any = json.loads(
            _read_bounded(path, MAX_SPEC_BYTES, "trust specification")
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise TrustTableError(
            "trust specification is not valid UTF-8 JSON"
        ) from error
    if not isinstance(value, dict) or set(value) != {"format", "keys"}:
        raise TrustTableError("trust specification fields are not canonical")
    if type(value["format"]) is not int or value["format"] != VERSION:
        raise TrustTableError("unsupported trust specification format")
    keys = value["keys"]
    if not isinstance(keys, list) or not 1 <= len(keys) <= MAX_KEYS:
        raise TrustTableError("trust specification key count is out of bounds")
    records: list[tuple[bytes, bytes, int]] = []
    names: set[str] = set()
    for entry in keys:
        if not isinstance(entry, dict) or set(entry) != {
            "name", "public_key", "status"
        }:
            raise TrustTableError("trust key fields are not canonical")
        name = entry["name"]
        public_text = entry["public_key"]
        status_text = entry["status"]
        if (not isinstance(name, str) or not 1 <= len(name) <= 64 or
                any(ord(character) < 0x20 or ord(character) > 0x7e
                    for character in name)):
            raise TrustTableError(
                "trust key name is not bounded printable ASCII"
            )
        if name in names:
            raise TrustTableError("trust key names must be unique")
        names.add(name)
        if (not isinstance(public_text, str) or len(public_text) != 64 or
                public_text != public_text.lower()):
            raise TrustTableError(
                "public key must be 64 lowercase hexadecimal digits"
            )
        try:
            public_key = bytes.fromhex(public_text)
        except ValueError as error:
            raise TrustTableError("public key is not hexadecimal") from error
        if status_text not in STATUS:
            raise TrustTableError("trust key status must be trusted or revoked")
        records.append((hashlib.sha256(public_key).digest(), public_key,
                        STATUS[status_text]))
    records.sort(key=lambda record: record[0])
    if any(records[index - 1][0] == records[index][0]
           for index in range(1, len(records))):
        raise TrustTableError("trust keys must be unique")
    return records


def build_table(specification: Path) -> bytes:
    """Encode one deterministic canonical trust table from a JSON spec."""
    encoded_records = bytearray()
    records = _load_spec(specification)
    for key_id, public_key, status in records:
        encoded_records += key_id
        encoded_records += public_key
        encoded_records += struct.pack("<H", status)
        encoded_records += bytes(RECORD_BYTES - 66)
    total = HEADER_BYTES + len(encoded_records)
    header = bytearray(HEADER_BYTES)
    header[0:8] = MAGIC
    struct.pack_into("<HHIQII", header, 8, VERSION, HEADER_BYTES, 0,
                     total, len(records), RECORD_BYTES)
    header[32:64] = hashlib.sha256(encoded_records).digest()
    return bytes(header + encoded_records)


def audit_table(data: bytes) -> int:
    """Reject every non-canonical table and return its admitted key count."""
    if not HEADER_BYTES <= len(data) <= HEADER_BYTES + MAX_KEYS * RECORD_BYTES:
        raise TrustTableError("trust table length is out of bounds")
    if data[0:8] != MAGIC:
        raise TrustTableError("trust table magic is invalid")
    version, header_bytes, flags, total, count, record_bytes = struct.unpack_from(
        "<HHIQII", data, 8
    )
    if (version != VERSION or header_bytes != HEADER_BYTES or flags != 0 or
            total != len(data) or record_bytes != RECORD_BYTES):
        raise TrustTableError("trust table header is invalid")
    if count > MAX_KEYS or len(data) != HEADER_BYTES + count * RECORD_BYTES:
        raise TrustTableError("trust table record bounds are invalid")
    if any(data[64:HEADER_BYTES]):
        raise TrustTableError("trust table header reserved bytes are nonzero")
    records = data[HEADER_BYTES:]
    if hashlib.sha256(records).digest() != data[32:64]:
        raise TrustTableError("trust table record digest is invalid")
    previous: bytes | None = None
    for index in range(count):
        record = records[index * RECORD_BYTES:(index + 1) * RECORD_BYTES]
        key_id = record[0:32]
        public_key = record[32:64]
        status = struct.unpack_from("<H", record, 64)[0]
        if status not in STATUS.values() or any(record[66:]):
            raise TrustTableError("trust table record is invalid")
        if hashlib.sha256(public_key).digest() != key_id:
            raise TrustTableError("trust table key ID does not match")
        if previous is not None and previous >= key_id:
            raise TrustTableError("trust table keys are not strictly ordered")
        previous = key_id
    return count


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_bytes() == data:
        return
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)


def emit_c(data: bytes) -> bytes:
    """Emit the audited table as a const C object in kernel read-only data."""
    audit_table(data)
    lines = [
        "/* SPDX-License-Identifier: GPL-3.0-only */",
        "/* Generated by tools/make-package-trust.py; do not edit. */",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f"const uint8_t phipia_package_trust_asset[{len(data)}] = {{",
    ]
    for offset in range(0, len(data), 12):
        chunk = data[offset:offset + 12]
        lines.append(
            "    " + ", ".join(f"0x{byte:02x}U" for byte in chunk) + ","
        )
    lines.extend([
        "};",
        "const size_t phipia_package_trust_asset_bytes =",
        "    sizeof(phipia_package_trust_asset);",
        "",
    ])
    return "\n".join(lines).encode("ascii")


def self_test() -> None:
    """Exercise deterministic ordering and all outer integrity fields."""
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        first = root / "first.json"
        second = root / "second.json"
        entries = [
            {"name": "second", "public_key": "22" * 32,
             "status": "revoked"},
            {"name": "first", "public_key": "11" * 32,
             "status": "trusted"},
        ]
        first.write_text(json.dumps({"format": 1, "keys": entries}), "utf-8")
        second.write_text(
            json.dumps({"format": 1, "keys": entries[::-1]}), "utf-8"
        )
        table = build_table(first)
        if table != build_table(second) or audit_table(table) != 2:
            raise TrustTableError("trust table is not deterministic")
        changed = bytearray(table)
        changed[-1] ^= 1
        try:
            audit_table(bytes(changed))
        except TrustTableError:
            pass
        else:
            raise TrustTableError("changed trust table was accepted")
        if b"phipia_package_trust_asset" not in emit_c(table):
            raise TrustTableError("C asset output is incomplete")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build")
    build.add_argument("specification", type=Path)
    build.add_argument("output", type=Path)
    audit = commands.add_parser("audit")
    audit.add_argument("table", type=Path)
    c_output = commands.add_parser("emit-c")
    c_output.add_argument("table", type=Path)
    c_output.add_argument("output", type=Path)
    commands.add_parser("self-test")
    arguments = parser.parse_args()
    try:
        if arguments.command == "build":
            table = build_table(arguments.specification)
            audit_table(table)
            _atomic_write(arguments.output, table)
            print(f"package trust table: {len(table)} bytes")
        elif arguments.command == "audit":
            data = _read_bounded(
                arguments.table, HEADER_BYTES + MAX_KEYS * RECORD_BYTES,
                "trust table"
            )
            print(f"package trust table: {audit_table(data)} keys admitted")
        elif arguments.command == "emit-c":
            data = _read_bounded(
                arguments.table, HEADER_BYTES + MAX_KEYS * RECORD_BYTES,
                "trust table"
            )
            _atomic_write(arguments.output, emit_c(data))
        else:
            self_test()
            print("Phipia immutable package trust-table tests passed")
    except (OSError, TrustTableError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
