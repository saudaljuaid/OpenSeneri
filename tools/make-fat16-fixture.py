#!/usr/bin/env python3
"""Build and independently verify Phipia's bounded FAT16 QEMU fixture."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import stat
import sys


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
FILE_BYTES = 128
MEDIA = 0xF8
SHORT_NAME = b"PHIPIA  BIN"
PAYLOAD_SHA256 = "D399F065C9F21E2FD51E2AEADB7768EAB7E6E45E5150F31227C9711934A4D1D3"
IMAGE_SHA256 = "4B6072C4762E3D59B372CCB0FB83C47F901E2FC1E465C45C438CFD2A6BCD3528"


def payload() -> bytes:
    """Return the deterministic file body without reading any host resource."""
    return bytes((index * 73 + 19) & 0xFF for index in range(FILE_BYTES))


def put_u16(image: bytearray, offset: int, value: int) -> None:
    image[offset : offset + 2] = value.to_bytes(2, "little")


def put_u32(image: bytearray, offset: int, value: int) -> None:
    image[offset : offset + 4] = value.to_bytes(4, "little")


def build_image() -> bytes:
    """Construct the exact supported superfloppy as ordinary bytes."""
    image = bytearray(IMAGE_BYTES)

    # Microsoft FAT General Overview 1.03, "Boot Sector and BPB Structure".
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
    put_u32(image, 28, 0)
    put_u32(image, 32, 0)

    # FAT12/FAT16 extended BPB at byte 36. The filesystem label is
    # informational; the reader classifies solely from the cluster count.
    image[36] = 0x80
    image[37] = 0
    image[38] = 0x29
    put_u32(image, 39, 0x0600_0001)
    image[43:54] = b"PHIPIA     "
    image[54:62] = b"FAT16   "
    image[510:512] = b"\x55\xAA"

    fat = FIRST_FAT_SECTOR * BLOCK_BYTES
    put_u16(image, fat + 0, 0xFFF8)
    put_u16(image, fat + 2, 0xFFFF)
    put_u16(image, fat + FILE_CLUSTER * 2, 0xFFFF)

    root = FIRST_ROOT_SECTOR * BLOCK_BYTES
    image[root : root + 11] = SHORT_NAME
    image[root + 11] = 0x20
    put_u16(image, root + 20, 0)
    put_u16(image, root + 26, FILE_CLUSTER)
    put_u32(image, root + 28, FILE_BYTES)
    image[root + 32] = 0

    data = FIRST_DATA_SECTOR * BLOCK_BYTES
    image[data : data + FILE_BYTES] = payload()
    return bytes(image)


def u16(image: bytes, offset: int) -> int:
    return int.from_bytes(image[offset : offset + 2], "little")


def u32(image: bytes, offset: int) -> int:
    return int.from_bytes(image[offset : offset + 4], "little")


def verify_image(image: bytes) -> None:
    """Parse the reopened bytes independently and reject any disagreement."""
    if len(image) != IMAGE_BYTES:
        raise ValueError("fixture length is not exactly 16 MiB")
    if image[510:512] != b"\x55\xAA" or image[38] != 0x29:
        raise ValueError("boot-sector signatures are invalid")

    bytes_per_sector = u16(image, 11)
    sectors_per_cluster = image[13]
    reserved = u16(image, 14)
    fats = image[16]
    root_entries = u16(image, 17)
    total16 = u16(image, 19)
    media = image[21]
    fat16 = u16(image, 22)
    hidden = u32(image, 28)
    total32 = u32(image, 32)
    root_sectors = (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector
    first_fat = reserved
    first_root = reserved + fats * fat16
    first_data = first_root + root_sectors
    data_sectors = total16 - first_data
    clusters = data_sectors // sectors_per_cluster

    geometry = (
        bytes_per_sector,
        sectors_per_cluster,
        reserved,
        fats,
        root_entries,
        total16,
        media,
        total32,
        fat16,
        hidden,
        root_sectors,
        first_fat,
        first_root,
        first_data,
        clusters,
    )
    expected = (
        BLOCK_BYTES,
        1,
        RESERVED_SECTORS,
        FAT_COUNT,
        ROOT_ENTRIES,
        TOTAL_SECTORS,
        MEDIA,
        0,
        FAT_SECTORS,
        0,
        ROOT_SECTORS,
        FIRST_FAT_SECTOR,
        FIRST_ROOT_SECTOR,
        FIRST_DATA_SECTOR,
        4092,
    )
    if geometry != expected or not (4085 <= clusters < 65525):
        raise ValueError("derived FAT16 geometry disagrees with the contract")

    fat = first_fat * bytes_per_sector
    if u16(image, fat) != 0xFFF8 or u16(image, fat + 2) != 0xFFFF:
        raise ValueError("FAT reserved entries are invalid")
    if u16(image, fat + FILE_CLUSTER * 2) < 0xFFF8:
        raise ValueError("file cluster is not terminated by FAT16 EOC")

    root = first_root * bytes_per_sector
    if image[root : root + 11] != SHORT_NAME or image[root + 11] != 0x20:
        raise ValueError("canonical root entry is absent")
    if u16(image, root + 20) != 0 or u16(image, root + 26) != FILE_CLUSTER:
        raise ValueError("root entry cluster is invalid")
    if u32(image, root + 28) != FILE_BYTES or image[root + 32] != 0:
        raise ValueError("root entry size or end marker is invalid")

    file_lba = first_data + (FILE_CLUSTER - 2) * sectors_per_cluster
    body = image[file_lba * bytes_per_sector : file_lba * bytes_per_sector + FILE_BYTES]
    digest = hashlib.sha256(body).hexdigest().upper()
    if body != payload() or digest != PAYLOAD_SHA256:
        raise ValueError("file payload or SHA-256 is invalid")

    # This byte-for-byte comparison also proves all unused/reserved bytes are
    # deterministic zeroes rather than uninitialised host data.
    if image != build_image():
        raise ValueError("fixture contains an unexpected byte")
    image_digest = hashlib.sha256(image).hexdigest().upper()
    if image_digest != IMAGE_SHA256:
        raise ValueError("fixture image SHA-256 is invalid")


def checked_output(argument: str) -> Path:
    """Confine the ordinary-file fixture to this repository's build tree."""
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
        print(f"FAT16 fixture refused: {error}", file=sys.stderr)
        return 1
    print(
        f"{output}: {TOTAL_SECTORS} sectors x {BLOCK_BYTES} bytes, "
        f"PHIPIA.BIN {FILE_BYTES} bytes, SHA-256 {PAYLOAD_SHA256}, "
        f"image SHA-256 {IMAGE_SHA256}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
