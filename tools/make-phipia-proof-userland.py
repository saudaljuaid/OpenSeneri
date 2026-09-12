#!/usr/bin/env python3
"""Build and independently verify the v1.1.0 three-profile FAT16 volume."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import stat
import struct
import sys


BLOCK_BYTES = 4096
TOTAL_SECTORS = 4096
IMAGE_BYTES = BLOCK_BYTES * TOTAL_SECTORS
RESERVED_SECTORS = 1
FAT_COUNT = 1
FAT_SECTORS = 2
ROOT_ENTRIES = 128
FIRST_FAT_SECTOR = 1
FIRST_ROOT_SECTOR = 3
FIRST_DATA_SECTOR = 4
MEDIA = 0xF8
IMAGE_SHA256 = "C2A2B2FEC703C654E1260EF07A91FF1DD7808F8D83734C0D7AFD3967525B34B9"
NO_CAT_IMAGE_SHA256 = "F7DB823EE1CB7FF2A05E7020DB0F4502656B9950EFBBE79E23ED0EA755FC8478"

ECHO_NAME = b"BUSYBOX    "
ECHO_BYTES = 33_584
ECHO_CLUSTERS = 9
ECHO_FIRST_CLUSTER = 2
ECHO_SHA256 = "B308F2CAD5B5CD0EEB92A622DEC8D71C1A08F628A22CDC5BCDE2B98B53220746"

UNAME_NAME = b"UNAMEBOX   "
UNAME_BYTES = 38_368
UNAME_CLUSTERS = 10
UNAME_FIRST_CLUSTER = ECHO_FIRST_CLUSTER + ECHO_CLUSTERS
UNAME_SHA256 = "389AD6B13804EB7307BA589C8E8A7C702F91302005A7C5FC6E9E99124FCEAF43"

CAT_NAME = b"CATBOX     "
CAT_BYTES = 38_632
CAT_CLUSTERS = 10
CAT_FIRST_CLUSTER = UNAME_FIRST_CLUSTER + UNAME_CLUSTERS
CAT_SHA256 = "8191596A22778B575942895071A2E50CCEEE0F82F4D88B6D986584CE0914FC3E"


PROFILES = (
    ("echo", ECHO_NAME, ECHO_BYTES, ECHO_CLUSTERS, ECHO_FIRST_CLUSTER, ECHO_SHA256),
    ("uname", UNAME_NAME, UNAME_BYTES, UNAME_CLUSTERS, UNAME_FIRST_CLUSTER, UNAME_SHA256),
    ("cat", CAT_NAME, CAT_BYTES, CAT_CLUSTERS, CAT_FIRST_CLUSTER, CAT_SHA256),
)


def put_u16(image: bytearray, offset: int, value: int) -> None:
    image[offset : offset + 2] = value.to_bytes(2, "little")


def put_u32(image: bytearray, offset: int, value: int) -> None:
    image[offset : offset + 4] = value.to_bytes(4, "little")


def u16(image: bytes, offset: int) -> int:
    return int.from_bytes(image[offset : offset + 2], "little")


def u32(image: bytes, offset: int) -> int:
    return int.from_bytes(image[offset : offset + 4], "little")


def verify_elf(binary: bytes, profile: str) -> None:
    expected_bytes = {"echo": ECHO_BYTES, "uname": UNAME_BYTES, "cat": CAT_BYTES}[profile]
    expected_sha = {"echo": ECHO_SHA256, "uname": UNAME_SHA256, "cat": CAT_SHA256}[profile]
    if len(binary) != expected_bytes:
        raise ValueError(f"{profile} BusyBox byte count changed")
    if hashlib.sha256(binary).hexdigest().upper() != expected_sha:
        raise ValueError(f"{profile} BusyBox SHA-256 changed")
    if binary[:16] != b"\x7fELF\x02\x01\x01\x00" + b"\x00" * 8:
        raise ValueError(f"{profile} BusyBox ELF identification changed")
    header = struct.unpack_from("<HHIQQQIHHHHHH", binary, 16)
    expected_shoff = {"echo": 33_072, "uname": 37_856, "cat": 38_120}[profile]
    if header != (
        2, 62, 1, 0x40000100107A, 64, expected_shoff, 0,
        64, 56, 5, 64, 8, 7,
    ):
        raise ValueError(f"{profile} BusyBox ELF header changed")
    headers = [
        struct.unpack_from("<IIQQQQQQ", binary, 64 + index * 56)
        for index in range(5)
    ]
    if profile == "echo":
        expected = [
            (1, 4, 0x0, 0x400001000000, 0x400001000000, 0x158, 0x158, 0x1000),
            (1, 5, 0x1000, 0x400001001000, 0x400001001000, 0x5563, 0x5563, 0x1000),
            (1, 4, 0x7000, 0x400001007000, 0x400001007000, 0xED1, 0xED1, 0x1000),
            (1, 6, 0x8000, 0x400001008000, 0x400001008000, 0xFE, 0xB38, 0x1000),
            (0x6474E551, 6, 0, 0, 0, 0, 0, 0x10),
        ]
    elif profile == "uname":
        expected = [
            (1, 4, 0x0, 0x400001000000, 0x400001000000, 0x158, 0x158, 0x1000),
            (1, 5, 0x1000, 0x400001001000, 0x400001001000, 0x6D7F, 0x6D7F, 0x1000),
            (1, 4, 0x8000, 0x400001008000, 0x400001008000, 0x1181, 0x1181, 0x1000),
            (1, 6, 0x91A0, 0x40000100A1A0, 0x40000100A1A0, 0x20E, 0xC70, 0x1000),
            (0x6474E551, 6, 0, 0, 0, 0, 0, 0x10),
        ]
    else:
        expected = [
            (1, 4, 0x0, 0x400001000000, 0x400001000000, 0x158, 0x158, 0x1000),
            (1, 5, 0x1000, 0x400001001000, 0x400001001000, 0x6F72, 0x6F72, 0x1000),
            (1, 4, 0x8000, 0x400001008000, 0x400001008000, 0x118E, 0x118E, 0x1000),
            (1, 6, 0x91A0, 0x40000100A1A0, 0x40000100A1A0, 0x316, 0x1188, 0x1000),
            (0x6474E551, 6, 0, 0, 0, 0, 0, 0x10),
        ]
    if headers != expected:
        raise ValueError(f"{profile} BusyBox program headers changed")


def build_image(echo: bytes, uname: bytes, cat: bytes | None) -> bytes:
    verify_elf(echo, "echo")
    verify_elf(uname, "uname")
    if cat is not None:
        verify_elf(cat, "cat")
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
    put_u32(image, 39, 0x1000_0000)
    image[43:54] = b"PHIPIAUSER "
    image[54:62] = b"FAT16   "
    image[510:512] = b"\x55\xAA"

    fat = FIRST_FAT_SECTOR * BLOCK_BYTES
    put_u16(image, fat, 0xFFF8)
    put_u16(image, fat + 2, 0xFFFF)
    root = FIRST_ROOT_SECTOR * BLOCK_BYTES
    included = PROFILES if cat is not None else PROFILES[:2]
    binaries = {"echo": echo, "uname": uname, "cat": cat}
    for index, (profile, name, size, clusters, first, _) in enumerate(included):
        for ordinal in range(clusters):
            cluster = first + ordinal
            value = cluster + 1 if ordinal + 1 < clusters else 0xFFFF
            put_u16(image, fat + cluster * 2, value)
        entry = root + index * 32
        image[entry : entry + 11] = name
        image[entry + 11] = 0x20
        put_u16(image, entry + 26, first)
        put_u32(image, entry + 28, size)
        binary = binaries[profile]
        if binary is None:
            raise ValueError(f"{profile} payload is absent")
        data = (FIRST_DATA_SECTOR + first - 2) * BLOCK_BYTES
        image[data : data + size] = binary
    return bytes(image)


def verify_image(
    image: bytes,
    echo: bytes,
    uname: bytes,
    cat: bytes | None,
    check_digest: bool = True,
) -> str:
    if len(image) != IMAGE_BYTES or image[510:512] != b"\x55\xAA":
        raise ValueError("userspace volume length or boot signature changed")
    geometry = (
        u16(image, 11), image[13], u16(image, 14), image[16],
        u16(image, 17), u16(image, 19), image[21], u16(image, 22),
    )
    if geometry != (4096, 1, 1, 1, 128, 4096, 0xF8, 2):
        raise ValueError("userspace volume FAT16 geometry changed")
    fat = FIRST_FAT_SECTOR * BLOCK_BYTES
    if (u16(image, fat), u16(image, fat + 2)) != (0xFFF8, 0xFFFF):
        raise ValueError("userspace volume FAT16 reserved entries changed")
    root = FIRST_ROOT_SECTOR * BLOCK_BYTES
    binaries = {"echo": echo, "uname": uname, "cat": cat}
    included = PROFILES if cat is not None else PROFILES[:2]
    for index, (profile, name, size, clusters, first, digest) in enumerate(included):
        entry = root + index * 32
        state = (
            image[entry : entry + 11], image[entry + 11],
            u16(image, entry + 20), u16(image, entry + 26), u32(image, entry + 28),
        )
        if state != (name, 0x20, 0, first, size):
            raise ValueError(f"{profile} root metadata changed")
        body = bytearray()
        cluster = first
        for ordinal in range(clusters):
            expected_next = cluster + 1 if ordinal + 1 < clusters else 0xFFFF
            if u16(image, fat + cluster * 2) != expected_next:
                raise ValueError(f"{profile} FAT16 chain changed")
            lba = FIRST_DATA_SECTOR + cluster - 2
            body.extend(image[lba * BLOCK_BYTES : (lba + 1) * BLOCK_BYTES])
            cluster = expected_next
        payload = bytes(body[:size])
        if binaries[profile] is None or payload != binaries[profile] or hashlib.sha256(payload).hexdigest().upper() != digest:
            raise ValueError(f"{profile} payload checksum changed")
        if any(body[size:]):
            raise ValueError(f"{profile} final cluster padding changed")
    if image[root + len(included) * 32] != 0:
        raise ValueError("userspace volume root terminator changed")
    rebuilt = build_image(echo, uname, cat)
    if image != rebuilt:
        raise ValueError("userspace volume contains an unexpected byte")
    digest = hashlib.sha256(image).hexdigest().upper()
    expected_digest = IMAGE_SHA256 if cat is not None else NO_CAT_IMAGE_SHA256
    if check_digest and digest != expected_digest:
        raise ValueError("userspace volume SHA-256 changed")
    return digest


def negative_self_test(image: bytes, echo: bytes, uname: bytes, cat: bytes) -> None:
    root = FIRST_ROOT_SECTOR * BLOCK_BYTES
    fat = FIRST_FAT_SECTOR * BLOCK_BYTES
    data = FIRST_DATA_SECTOR * BLOCK_BYTES
    mutations = (
        ("missing cat profile", root + 2 * 32, 0),
        ("FAT16 metadata", 12, 0),
        ("FAT16 chain", fat + ECHO_FIRST_CLUSTER * 2, 0),
        ("checksum", data + ECHO_BYTES // 2, image[data + ECHO_BYTES // 2] ^ 1),
        ("ELF64", data, 0),
    )
    for name, offset, value in mutations:
        changed = bytearray(image)
        changed[offset] = value
        try:
            verify_image(bytes(changed), echo, uname, cat, check_digest=False)
        except ValueError:
            continue
        raise ValueError(f"negative {name} mutation was accepted")


def checked_file(argument: str, output: bool) -> Path:
    repository = Path(__file__).resolve().parents[1]
    build = (repository / "build").resolve()
    path = Path(argument)
    if not path.is_absolute():
        path = repository / path
    resolved = path.resolve(strict=False)
    if build != resolved and build not in resolved.parents:
        raise ValueError("userspace volume files must remain under build")
    if path.exists():
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
            raise ValueError("userspace volume inputs must be regular files")
    elif not output:
        raise ValueError("userspace volume input is absent")
    if output:
        path.parent.mkdir(parents=True, exist_ok=True)
    return path


def main() -> int:
    if len(sys.argv) != 5:
        print(
            f"usage: {Path(sys.argv[0]).name} ECHO UNAME CAT|--without-cat OUTPUT",
            file=sys.stderr,
        )
        return 2
    output = None
    try:
        echo_path = checked_file(sys.argv[1], output=False)
        uname_path = checked_file(sys.argv[2], output=False)
        cat_path = (
            None if sys.argv[3] == "--without-cat"
            else checked_file(sys.argv[3], output=False)
        )
        output = checked_file(sys.argv[4], output=True)
        echo = echo_path.read_bytes()
        uname = uname_path.read_bytes()
        cat = None if cat_path is None else cat_path.read_bytes()
        image = build_image(echo, uname, cat)
        if cat is not None:
            negative_self_test(image, echo, uname, cat)
        with output.open("wb") as stream:
            stream.write(image)
            stream.flush()
            os.fsync(stream.fileno())
        digest = verify_image(output.read_bytes(), echo, uname, cat)
    except (OSError, ValueError, struct.error) as error:
        if output is not None:
            try:
                output.unlink(missing_ok=True)
            except OSError:
                pass
        print(f"Phipia userspace volume refused: {error}", file=sys.stderr)
        return 1
    print(
        f"{output}: {TOTAL_SECTORS} sectors x {BLOCK_BYTES} bytes, "
        f"echo {ECHO_BYTES} bytes, uname {UNAME_BYTES} bytes, "
        f"cat {CAT_BYTES if cat is not None else 0} bytes, SHA-256 {digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
