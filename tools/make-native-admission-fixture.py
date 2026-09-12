#!/usr/bin/env python3
"""Build deterministic malformed native-admission System-volume fixtures."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct

import fat32_image


def load_package_module() -> object:
    path = Path(__file__).with_name("phipia-package.py")
    specification = importlib.util.spec_from_file_location(
        "phipia_package_tool", path)
    if specification is None or specification.loader is None:
        raise RuntimeError("could not load the Phipia package implementation")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def build_fixture(package_path: Path) -> tuple[bytes, tuple[tuple[str, bytes], ...]]:
    package_module = load_package_module()
    manifest, executable, _, _ = package_module.parse_package(
        package_path.read_bytes())

    bad_abi = bytearray(manifest)
    struct.pack_into("<I", bad_abi, 12, 2)

    bad_digest = bytearray(manifest)
    bad_digest[128] ^= 0x80

    bad_elf = bytearray(executable)
    bad_elf[0] = 0
    bad_elf_manifest = bytearray(manifest)
    bad_elf_manifest[112:128] = b"BADELF.APP\0" + bytes(6)
    bad_elf_manifest[128:160] = hashlib.sha256(bad_elf).digest()

    extras = (
        ("BADABI.MAN", bytes(bad_abi)),
        ("BADDGST.MAN", bytes(bad_digest)),
        ("BADELF.MAN", bytes(bad_elf_manifest)),
        ("BADELF.APP", bytes(bad_elf)),
        ("NATIVET.APP", executable),
    )
    image = fat32_image.build_image("system", (), extras)
    fat32_image.verify_system(image, (), extras)
    return image, extras


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    image, extras = build_fixture(arguments.package)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_bytes(image)
    print("native admission fixture: " + ", ".join(name for name, _ in extras))
    print("sha256=" + hashlib.sha256(image).hexdigest().upper())


if __name__ == "__main__":
    main()
