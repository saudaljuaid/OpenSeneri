#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the exact authenticated manifest/catalog for the dynamic Ring 3 proof."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct


CATALOG_BYTES = 2048
CATALOG_ENTRY_BYTES = 96
LIBRARY_NAME = "DYNLIB.SO"


def text_field(value: str, size: int, field: str) -> bytes:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError(f"{field} must be ASCII") from error
    if not encoded or len(encoded) >= size or any(
            not (character.isalnum() or character in "._-/ ")
            for character in value):
        raise ValueError(f"{field} does not fit the Phipia text profile")
    return encoded + bytes(size - len(encoded))


def bounded_file(path: Path, field: str) -> bytes:
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"{field} is not an ordinary file: {path}")
    data = path.read_bytes()
    if not data or len(data) > 16 * 1024 * 1024:
        raise ValueError(f"{field} is empty or exceeds 16 MiB")
    return data


def make_catalog(library: bytes) -> bytes:
    catalog = bytearray(CATALOG_BYTES)
    catalog[:8] = b"PHIPDYN1"
    struct.pack_into("<HHIHH", catalog, 8, 1, 64, CATALOG_BYTES, 1,
                     CATALOG_ENTRY_BYTES)
    catalog[64:128] = text_field(LIBRARY_NAME, 64, "library SONAME")
    catalog[128:160] = hashlib.sha256(library).digest()
    return bytes(catalog)


def make_package_spec(spec: dict[str, object]) -> dict[str, object]:
    required = {
        "name", "identifier", "executable", "dynamic_catalog",
        "resource_directory", "data_namespace", "memory_limit",
        "max_handles", "max_threads", "capabilities",
    }
    if set(spec) != required or spec["capabilities"] != ["console"]:
        raise ValueError("dynamic proof spec has unknown fields or capabilities")
    memory_limit = spec["memory_limit"]
    max_handles = spec["max_handles"]
    max_threads = spec["max_threads"]
    if (not isinstance(memory_limit, int) or memory_limit % 4096 != 0
            or not 64 * 1024 <= memory_limit <= 256 * 1024 * 1024
            or not isinstance(max_handles, int) or not 1 <= max_handles <= 128
            or not isinstance(max_threads, int) or not 1 <= max_threads <= 8):
        raise ValueError("dynamic proof resource bounds are invalid")

    if spec["dynamic_catalog"] != "DYN/DYNROOT.CAT" or \
            spec["resource_directory"] != "DYN":
        raise ValueError("dynamic proof resources must remain in DYN")
    return {
        **spec,
        "format": 2,
        "resources": [
            {"path": "DYNROOT.CAT", "source": "DYNROOT.CAT"},
            {"path": LIBRARY_NAME, "source": LIBRARY_NAME},
        ],
    }


def atomic_write(path: Path, data: bytes) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.unlink(missing_ok=True)
    try:
        with temporary.open("xb") as stream:
            stream.write(data)
            stream.flush()
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spec", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    spec = json.loads(args.spec.read_text(encoding="utf-8"))
    if not isinstance(spec, dict):
        raise ValueError("dynamic proof spec must be an object")
    root = bounded_file(args.root, "root executable")
    library = bounded_file(args.library, "shared library")
    catalog = make_catalog(library)
    package_spec = make_package_spec(spec)
    args.output.mkdir(parents=True, exist_ok=True)
    atomic_write(args.output / "DYNROOT.CAT", catalog)
    atomic_write(args.output / "package.json",
                 (json.dumps(package_spec, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    print(
        "dynamic proof manifest/catalog: "
        f"root_sha256={hashlib.sha256(root).hexdigest().upper()} "
        f"library_sha256={hashlib.sha256(library).hexdigest().upper()} "
        f"catalog_sha256={hashlib.sha256(catalog).hexdigest().upper()}"
    )


if __name__ == "__main__":
    main()
