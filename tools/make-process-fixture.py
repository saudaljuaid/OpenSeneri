#!/usr/bin/env python3
"""Build and independently verify the process proof's FAT16/NVMe fixture."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import stat
import sys

from elf64_fixture import FILE_BYTES, PAYLOAD_SHA256, build_payload, verify_payload


BLOCK_BYTES = 4096
TOTAL_SECTORS = 4096
IMAGE_BYTES = BLOCK_BYTES * TOTAL_SECTORS
RESERVED_SECTORS = 1
FAT_COUNT = 1
FAT_SECTORS = 2
ROOT_ENTRIES = 128
ROOT_SECTORS = 1
FIRST_FAT_SECTOR = 1
FIRST_ROOT_SECTOR = 3
FIRST_DATA_SECTOR = 4
FILE_CLUSTER = 2
MEDIA = 0xF8
SHORT_NAME = b"PHIPIA  BIN"
IMAGE_SHA256 = "F8730A9253C9EBECFABB0108714F4F59EC05D151C5ADB19B0C8E08279CEFE53E"


def put_u16(image: bytearray, offset: int, value: int) -> None:
    image[offset : offset + 2] = value.to_bytes(2, "little")


def put_u32(image: bytearray, offset: int, value: int) -> None:
    image[offset : offset + 4] = value.to_bytes(4, "little")


def build_image() -> bytes:
    """Construct the v0.6.0 geometry with the independently built ELF body."""
    image = bytearray(IMAGE_BYTES)
    image[0:3] = b"\xEB\x3C\x90"
    image[3:11] = b"PHIPIA  "
    put_u16(image, 11, BLOCK_BYTES)
    image[13] = 1
    put_u16(image, 14, RESERVED_SECTORS)
    image[16] = FAT_COUNT
    put_u16(image, 17, ROOT_ENTRIES)
    put_u16(image, 19, TOTAL_SECTORS)
    image[21] = MEDIA
    put_u16(image, 22, FAT_SECTORS)
    put_u16(image, 24, 1)
    put_u16(image, 26, 1)
    image[36] = 0x80
    image[38] = 0x29
    put_u32(image, 39, 0x0600_0001)
    image[43:54] = b"PHIPIA     "
    image[54:62] = b"FAT16   "
    image[510:512] = b"\x55\xAA"

    fat = FIRST_FAT_SECTOR * BLOCK_BYTES
    put_u16(image, fat, 0xFFF8)
    put_u16(image, fat + 2, 0xFFFF)
    put_u16(image, fat + FILE_CLUSTER * 2, 0xFFFF)

    root = FIRST_ROOT_SECTOR * BLOCK_BYTES
    image[root : root + 11] = SHORT_NAME
    image[root + 11] = 0x20
    put_u16(image, root + 26, FILE_CLUSTER)
    put_u32(image, root + 28, FILE_BYTES)

    data = FIRST_DATA_SECTOR * BLOCK_BYTES
    image[data : data + FILE_BYTES] = build_payload()
    return bytes(image)


def u16(image: bytes, offset: int) -> int:
    return int.from_bytes(image[offset : offset + 2], "little")


def u32(image: bytes, offset: int) -> int:
    return int.from_bytes(image[offset : offset + 4], "little")


def verify_image(image: bytes) -> None:
    """Reopen, derive geometry, validate the ELF, and byte-compare the image."""
    if len(image) != IMAGE_BYTES or image[510:512] != b"\x55\xAA":
        raise ValueError("fixture length or boot signature is invalid")
    bytes_per_sector = u16(image, 11)
    root_entries = u16(image, 17)
    root_sectors = (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector
    first_fat = u16(image, 14)
    first_root = first_fat + image[16] * u16(image, 22)
    first_data = first_root + root_sectors
    clusters = (u16(image, 19) - first_data) // image[13]
    geometry = (
        bytes_per_sector,
        image[13],
        first_fat,
        image[16],
        root_entries,
        u16(image, 19),
        image[21],
        u32(image, 32),
        u16(image, 22),
        u32(image, 28),
        root_sectors,
        first_root,
        first_data,
        clusters,
    )
    expected = (4096, 1, 1, 1, 128, 4096, 0xF8, 0, 2, 0, 1, 3, 4, 4092)
    if geometry != expected or not (4085 <= clusters < 65525):
        raise ValueError("derived FAT16 geometry differs from v0.6.0")
    fat = first_fat * bytes_per_sector
    if (u16(image, fat), u16(image, fat + 2), u16(image, fat + 4)) != (
        0xFFF8,
        0xFFFF,
        0xFFFF,
    ):
        raise ValueError("FAT entries are invalid")
    root = first_root * bytes_per_sector
    root_state = (
        image[root : root + 11],
        image[root + 11],
        u16(image, root + 20),
        u16(image, root + 26),
        u32(image, root + 28),
        image[root + 32],
    )
    if root_state != (SHORT_NAME, 0x20, 0, FILE_CLUSTER, FILE_BYTES, 0):
        raise ValueError("canonical root entry is invalid")
    body = image[first_data * bytes_per_sector : first_data * bytes_per_sector + FILE_BYTES]
    verify_payload(body)
    if hashlib.sha256(body).hexdigest().upper() != PAYLOAD_SHA256:
        raise ValueError("embedded ELF digest is invalid")
    if image != build_image():
        raise ValueError("fixture contains an unexpected byte")
    if hashlib.sha256(image).hexdigest().upper() != IMAGE_SHA256:
        raise ValueError("fixture image SHA-256 is invalid")


def checked_output(argument: str) -> Path:
    """Confine the ordinary-file fixture below the repository build tree."""
    repository = Path(__file__).resolve().parents[1]
    build = (repository / "build").resolve()
    output = Path(argument)
    if not output.is_absolute():
        output = repository / output
    resolved = output.resolve(strict=False)
    if build != resolved and build not in resolved.parents:
        raise ValueError("fixture output must remain under the repository build directory")
    if output.exists():
        mode = output.lstat().st_mode
        if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
            raise ValueError("fixture output must be an ordinary non-symlink file")
    output.parent.mkdir(parents=True, exist_ok=True)
    return output


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} OUTPUT", file=sys.stderr)
        return 2
    output = None
    try:
        output = checked_output(sys.argv[1])
        image = build_image()
        with output.open("wb") as stream:
            stream.write(image)
            stream.flush()
            os.fsync(stream.fileno())
        with output.open("rb") as stream:
            reopened = stream.read()
        verify_image(reopened)
    except (OSError, ValueError) as error:
        if output is not None:
            try:
                output.unlink(missing_ok=True)
            except OSError:
                pass
        print(f"process fixture refused: {error}", file=sys.stderr)
        return 1
    print(
        f"{output}: {TOTAL_SECTORS} sectors x {BLOCK_BYTES} bytes, "
        f"ELF64 PHIPIA.BIN {FILE_BYTES} bytes, SHA-256 {PAYLOAD_SHA256}, "
        f"image SHA-256 {IMAGE_SHA256}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
