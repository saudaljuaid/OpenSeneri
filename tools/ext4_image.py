#!/usr/bin/env python3
"""Build, inspect, verify, and deliberately damage Phipia ext4 fixtures.

The builder intentionally uses e2fsprogs instead of a mounted loop device, so it
can run in an unprivileged Linux CI job.  Every format-affecting input is pinned;
the integration test also builds the image twice and requires byte equality.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from typing import Callable, Iterable
import uuid


BLOCK_BYTES = 4096
BLOCK_COUNT = 32768  # 128 MiB; the logical sparse fixture is larger than 4 GiB.
INODE_COUNT = 4096
BLOCKS_PER_GROUP = 8192
INODE_BYTES = 256
JOURNAL_SUPERBLOCK_BYTES = 1024
JOURNAL_BLOCK_COUNT = 1024
JOURNAL_MAGIC = 0xC03B3998
JOURNAL_SUPERBLOCK_V2 = 4
JOURNAL_FEATURE_REVOKE = 0x1
JOURNAL_FEATURE_64BIT = 0x2
JOURNAL_FEATURE_CSUM_V3 = 0x10
JOURNAL_FEATURES = JOURNAL_FEATURE_REVOKE | JOURNAL_FEATURE_64BIT | JOURNAL_FEATURE_CSUM_V3
JOURNAL_CHECKSUM_TYPE_CRC32C = 4
FIXED_EPOCH = 1704067200  # 2024-01-01 00:00:00 UTC
FILESYSTEM_UUID = "5a706f74-652d-4558-5434-000000000001"
HASH_SEED_UUID = "5a706f74-652d-4841-5348-000000000001"
VOLUME_LABEL = "PHIPIAEXT4"
LAST_MOUNTED = "/phipia"
LARGE_SPARSE_BYTES = 5 * 1024 * 1024 * 1024 + 123
XATTR_NAME = "user.phipia"
XATTR_VALUE = "profile-v1"

FEATURES = (
    "has_journal",
    "ext_attr",
    "extents",
    "64bit",
    "metadata_csum",
    "metadata_csum_seed",
    "dir_index",
    "filetype",
    "sparse_super",
    "large_file",
    "huge_file",
    "dir_nlink",
    "extra_isize",
)

REQUIRED_TOOLS = ("mke2fs", "debugfs", "e2fsck")

COMPAT_FEATURES = {
    0x0004: "has_journal",
    0x0008: "ext_attr",
    0x0020: "dir_index",
}
INCOMPAT_FEATURES = {
    0x0002: "filetype",
    0x0004: "needs_recovery",
    0x0040: "extents",
    0x0080: "64bit",
    0x2000: "metadata_csum_seed",
}
RO_COMPAT_FEATURES = {
    0x0001: "sparse_super",
    0x0002: "large_file",
    0x0008: "huge_file",
    0x0020: "dir_nlink",
    0x0040: "extra_isize",
    0x0400: "metadata_csum",
}

MUTATIONS = (
    "magic",
    "block-size",
    "missing-extents",
    "missing-64bit",
    "missing-metadata-csum",
    "unsupported-incompat",
    "blocks-range",
    "superblock-checksum",
    "group-descriptor-checksum",
    "inode-checksum",
    "extent-header",
    "directory-record",
)


class Ext4ImageError(RuntimeError):
    """A deterministic fixture operation failed or an image was refused."""


def _read_u16(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _read_u32(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _write_u16(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value)


def _write_u32(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<I", data, offset, value)


def _read_u32be(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def _write_u32be(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into(">I", data, offset, value)


def _crc32c_raw(data: bytes | bytearray, seed: int = 0xFFFFFFFF) -> int:
    """Return the kernel/e2fsprogs CRC32C state without a final XOR."""
    crc = seed
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc & 0xFFFFFFFF


def _feature_names(mask: int, known: dict[int, str]) -> list[str]:
    return [name for bit, name in known.items() if mask & bit]


def _known_mask(known: dict[int, str]) -> int:
    result = 0
    for bit in known:
        result |= bit
    return result


def _refuse(message: str) -> None:
    raise Ext4ImageError(f"ext4 refusal: {message}")


def parse_superblock(image: bytes | bytearray) -> dict[str, object]:
    """Parse and strictly validate the Phipia ext4 superblock profile."""
    if len(image) < 2048:
        _refuse("image is too small to contain an ext4 superblock")
    sb = memoryview(image)[1024:2048]
    if _read_u16(sb, 0x38) != 0xEF53:
        _refuse("bad ext4 magic")

    log_block_size = _read_u32(sb, 0x18)
    if log_block_size > 6:
        _refuse(f"invalid log block size {log_block_size}")
    block_size = 1024 << log_block_size
    if block_size != BLOCK_BYTES:
        _refuse(f"block size {block_size} is not the supported 4096 bytes")

    compat = _read_u32(sb, 0x5C)
    incompat = _read_u32(sb, 0x60)
    ro_compat = _read_u32(sb, 0x64)
    unknown_compat = compat & ~_known_mask(COMPAT_FEATURES)
    unknown_incompat = incompat & ~_known_mask(INCOMPAT_FEATURES)
    unknown_ro = ro_compat & ~_known_mask(RO_COMPAT_FEATURES)
    if unknown_compat:
        _refuse(f"unsupported compatible feature bits 0x{unknown_compat:08x}")
    if unknown_incompat:
        _refuse(f"unsupported incompatible feature bits 0x{unknown_incompat:08x}")
    if unknown_ro:
        _refuse(f"unsupported read-only feature bits 0x{unknown_ro:08x}")

    enabled = set(
        _feature_names(compat, COMPAT_FEATURES)
        + _feature_names(incompat, INCOMPAT_FEATURES)
        + _feature_names(ro_compat, RO_COMPAT_FEATURES)
    )
    missing = sorted(set(FEATURES) - enabled)
    if missing:
        _refuse("missing required feature(s): " + ", ".join(missing))

    inode_size = _read_u16(sb, 0x58)
    if inode_size != INODE_BYTES:
        _refuse(f"inode size {inode_size} is not the supported {INODE_BYTES} bytes")

    blocks = _read_u32(sb, 0x04) | (_read_u32(sb, 0x150) << 32)
    free_blocks = _read_u32(sb, 0x0C) | (_read_u32(sb, 0x158) << 32)
    image_blocks = len(image) // block_size
    if len(image) % block_size:
        _refuse("image length is not block aligned")
    if blocks == 0 or blocks > image_blocks:
        _refuse(f"declared block count {blocks} exceeds image capacity {image_blocks}")
    if free_blocks > blocks:
        _refuse("free block count exceeds total block count")

    inodes = _read_u32(sb, 0x00)
    free_inodes = _read_u32(sb, 0x10)
    if inodes == 0 or free_inodes > inodes:
        _refuse("invalid inode counts")
    blocks_per_group = _read_u32(sb, 0x20)
    inodes_per_group = _read_u32(sb, 0x28)
    if blocks_per_group == 0 or inodes_per_group == 0:
        _refuse("zero group geometry")

    fs_uuid = str(uuid.UUID(bytes=bytes(sb[0x68:0x78])))
    if fs_uuid != FILESYSTEM_UUID:
        _refuse(f"UUID {fs_uuid} does not match the pinned Phipia UUID")
    label = bytes(sb[0x78:0x88]).split(b"\0", 1)[0].decode("ascii", "strict")
    if label != VOLUME_LABEL:
        _refuse(f"volume label {label!r} does not match {VOLUME_LABEL!r}")

    stored_checksum = _read_u32(sb, 0x3FC)
    calculated_checksum = _crc32c_raw(sb[:0x3FC])
    if stored_checksum != calculated_checksum:
        _refuse(
            "ext4 superblock checksum mismatch: "
            f"stored=0x{stored_checksum:08x}, calculated=0x{calculated_checksum:08x}"
        )

    return {
        "format": "ext4",
        "image_bytes": len(image),
        "block_size": block_size,
        "block_count": blocks,
        "free_blocks": free_blocks,
        "inode_count": inodes,
        "free_inodes": free_inodes,
        "inode_size": inode_size,
        "blocks_per_group": blocks_per_group,
        "inodes_per_group": inodes_per_group,
        "uuid": fs_uuid,
        "label": label,
        "last_mounted": bytes(sb[0x88:0xC8]).split(b"\0", 1)[0].decode("ascii", "replace"),
        "features": sorted(enabled),
        "needs_recovery": bool(incompat & 0x0004),
        "superblock_checksum": f"0x{stored_checksum:08x}",
        "feature_masks": {
            "compat": f"0x{compat:08x}",
            "incompat": f"0x{incompat:08x}",
            "ro_compat": f"0x{ro_compat:08x}",
        },
        "created_epoch": _read_u32(sb, 0x108),
        "written_epoch": _read_u32(sb, 0x30),
    }


def available_tools() -> dict[str, str]:
    """Return required e2fsprogs executables when all are installed."""
    found: dict[str, str] = {}
    for name in REQUIRED_TOOLS:
        path = shutil.which(name)
        if not path:
            return {}
        found[name] = path
    return found


def require_tools() -> dict[str, str]:
    found: dict[str, str] = {}
    for name in REQUIRED_TOOLS:
        path = shutil.which(name)
        if not path:
            raise Ext4ImageError(
                f"required e2fsprogs tool {name!r} was not found; "
                "install the e2fsprogs package (Linux CI: apt-get install e2fsprogs)"
            )
        found[name] = path
    return found


def _tool_env() -> dict[str, str]:
    env = os.environ.copy()
    env.update(
        {
            "E2FSCK_TIME": str(FIXED_EPOCH),
            "E2FSPROGS_FAKE_TIME": str(FIXED_EPOCH),
            "SOURCE_DATE_EPOCH": str(FIXED_EPOCH),
            "TZ": "UTC",
            "LC_ALL": "C",
            "LANG": "C",
        }
    )
    return env


def _run(
    argv: Iterable[str | Path],
    *,
    input_text: str | None = None,
    accepted: tuple[int, ...] = (0,),
    extra_env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [str(value) for value in argv]
    env = _tool_env()
    if extra_env:
        env.update(extra_env)
    result = subprocess.run(
        command,
        input=input_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        check=False,
    )
    if result.returncode not in accepted:
        rendered = " ".join(command)
        output = result.stdout.strip()
        raise Ext4ImageError(
            f"command failed with status {result.returncode}: {rendered}"
            + (f"\n{output}" if output else "")
        )
    return result


def _debugfs(tools: dict[str, str], image: Path, command: str) -> str:
    result = _run((tools["debugfs"], "-R", command, image))
    output = result.stdout
    bad_fragments = (
        "Filesystem not open",
        "Command not found",
        "File not found",
        "Ext2 inode is not a directory",
        "Not a hash-indexed directory",
    )
    if any(fragment in output for fragment in bad_fragments):
        raise Ext4ImageError(f"debugfs command {command!r} failed:\n{output.strip()}")
    return output


def _journal_superblock_offset(tools: dict[str, str], image: Path) -> int:
    """Return the byte offset of logical block zero in internal journal inode 8."""
    output = _debugfs(tools, image, "bmap <8> 0")
    numbers = re.findall(r"\b(\d+)\b", output)
    if not numbers:
        raise Ext4ImageError(f"could not map internal journal inode 8:\n{output.strip()}")
    return int(numbers[-1]) * BLOCK_BYTES


def _parse_journal_superblock(data: bytes | bytearray) -> dict[str, object]:
    """Validate the exact checksummed JBD2 profile required for writable work."""
    if len(data) != JOURNAL_SUPERBLOCK_BYTES:
        _refuse("journal superblock is not exactly 1024 bytes")
    if _read_u32be(data, 0x00) != JOURNAL_MAGIC:
        _refuse("bad JBD2 magic")
    if _read_u32be(data, 0x04) != JOURNAL_SUPERBLOCK_V2:
        _refuse("JBD2 superblock is not v2")
    if _read_u32be(data, 0x0C) != BLOCK_BYTES:
        _refuse("JBD2 block size does not match the ext4 profile")
    maximum_length = _read_u32be(data, 0x10)
    start_block = _read_u32be(data, 0x1C)
    if maximum_length != JOURNAL_BLOCK_COUNT or _read_u32be(data, 0x14) != 1:
        _refuse("JBD2 ring geometry is invalid")
    if start_block != 0:
        _refuse("deterministic fixture journal is not clean")
    compat = _read_u32be(data, 0x24)
    incompat = _read_u32be(data, 0x28)
    ro_compat = _read_u32be(data, 0x2C)
    if compat != 0 or incompat != JOURNAL_FEATURES or ro_compat != 0:
        _refuse(
            "JBD2 feature masks are outside the supported profile: "
            f"compat=0x{compat:08x}, incompat=0x{incompat:08x}, "
            f"ro_compat=0x{ro_compat:08x}"
        )
    if data[0x50] != JOURNAL_CHECKSUM_TYPE_CRC32C:
        _refuse(f"JBD2 checksum type {data[0x50]} is not CRC32C")
    journal_uuid = str(uuid.UUID(bytes=bytes(data[0x30:0x40])))
    if journal_uuid != FILESYSTEM_UUID:
        _refuse(f"JBD2 UUID {journal_uuid} does not match the ext4 UUID")
    stored_checksum = _read_u32be(data, 0xFC)
    check = bytearray(data)
    _write_u32be(check, 0xFC, 0)
    calculated_checksum = _crc32c_raw(check)
    if stored_checksum != calculated_checksum:
        _refuse(
            "JBD2 superblock checksum mismatch: "
            f"stored=0x{stored_checksum:08x}, calculated=0x{calculated_checksum:08x}"
        )
    return {
        "block_size": _read_u32be(data, 0x0C),
        "maximum_length": maximum_length,
        "first_block": _read_u32be(data, 0x14),
        "sequence": _read_u32be(data, 0x18),
        "start_block": start_block,
        "uuid": journal_uuid,
        "feature_masks": {
            "compat": f"0x{compat:08x}",
            "incompat": f"0x{incompat:08x}",
            "ro_compat": f"0x{ro_compat:08x}",
        },
        "checksum_type": "crc32c",
        "checksum": f"0x{stored_checksum:08x}",
    }


def _upgrade_journal_superblock(image: Path, tools: dict[str, str]) -> None:
    """Upgrade mke2fs's unmounted clean journal to Phipia's JBD2 profile."""
    offset = _journal_superblock_offset(tools, image)
    with image.open("r+b") as stream:
        stream.seek(offset)
        data = bytearray(stream.read(JOURNAL_SUPERBLOCK_BYTES))
        if len(data) != JOURNAL_SUPERBLOCK_BYTES:
            _refuse("internal journal superblock is truncated")
        if _read_u32be(data, 0x00) != JOURNAL_MAGIC:
            _refuse("bad JBD2 magic before profile upgrade")
        if _read_u32be(data, 0x04) != JOURNAL_SUPERBLOCK_V2:
            _refuse("JBD2 superblock is not v2 before profile upgrade")
        if _read_u32be(data, 0x0C) != BLOCK_BYTES or _read_u32be(data, 0x14) != 1:
            _refuse("JBD2 geometry is invalid before profile upgrade")
        _write_u32be(data, 0x24, 0)
        _write_u32be(data, 0x28, JOURNAL_FEATURES)
        _write_u32be(data, 0x2C, 0)
        data[0x50] = JOURNAL_CHECKSUM_TYPE_CRC32C
        _write_u32be(data, 0xFC, 0)
        _write_u32be(data, 0xFC, _crc32c_raw(data))
        _parse_journal_superblock(data)
        stream.seek(offset)
        stream.write(data)


def _inspect_journal_superblock(image: Path, tools: dict[str, str]) -> dict[str, object]:
    offset = _journal_superblock_offset(tools, image)
    with image.open("rb") as stream:
        stream.seek(offset)
        data = stream.read(JOURNAL_SUPERBLOCK_BYTES)
    return _parse_journal_superblock(data)


def _tool_versions(tools: dict[str, str]) -> dict[str, str]:
    versions: dict[str, str] = {}
    for name, executable in tools.items():
        output = _run((executable, "-V")).stdout
        first_line = next((line.strip() for line in output.splitlines() if line.strip()), "unknown")
        versions[name] = first_line
    return versions


def _write_debugfs_script(path: Path, payloads: dict[str, Path]) -> None:
    lines = [
        f"set_current_time @{FIXED_EPOCH}",
        "mkdir /system",
        "mkdir /packages",
        "mkdir /packages/demo",
        "mkdir /data",
        "mkdir /data/user",
        "mkdir /indexed",
        f"write {payloads['readme']} /system/README.TXT",
        f"write {payloads['app']} /packages/demo/APP.BIN",
        f"write {payloads['state']} /data/user/state.txt",
        "link /data/user/state.txt /data/user/state-hard.txt",
        "symlink /data/user/state-link state.txt",
        f"write {payloads['empty']} /data/user/large-sparse.bin",
        "fallocate /data/user/large-sparse.bin 0 0",
        f"fallocate /data/user/large-sparse.bin {(LARGE_SPARSE_BYTES - 1) // BLOCK_BYTES} "
        f"{(LARGE_SPARSE_BYTES - 1) // BLOCK_BYTES}",
        f"set_inode_field /data/user/large-sparse.bin size {LARGE_SPARSE_BYTES}",
        "set_inode_field / mode 040755",
        "set_inode_field /system mode 040755",
        "set_inode_field /packages mode 040755",
        "set_inode_field /packages/demo mode 040755",
        "set_inode_field /data mode 040755",
        "set_inode_field /data/user mode 040750",
        "set_inode_field /indexed mode 040755",
        "set_inode_field /system/README.TXT mode 0100444",
        "set_inode_field /packages/demo/APP.BIN mode 0100555",
        "set_inode_field /data/user/state.txt mode 0100640",
        "set_inode_field /data/user/state.txt uid 1000",
        "set_inode_field /data/user/state.txt gid 1000",
        "set_inode_field /data/user/large-sparse.bin mode 0100600",
        "set_inode_field /data/user/large-sparse.bin uid 1000",
        "set_inode_field /data/user/large-sparse.bin gid 1000",
        f"ea_set /data/user/state.txt {XATTR_NAME} {XATTR_VALUE}",
    ]
    # Enough deterministic entries to force e2fsck -D to build an htree.
    for index in range(256):
        lines.append(f"write {payloads['entry']} /indexed/entry-{index:04d}-phipia-fixture")
    lines.extend(
        [
            f"set_super_value mkfs_time @{FIXED_EPOCH}",
            f"set_super_value wtime @{FIXED_EPOCH}",
            f"set_super_value lastcheck @{FIXED_EPOCH}",
            "quit",
        ]
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def _format_image(image: Path, tools: dict[str, str], temporary: Path) -> None:
    with image.open("wb") as stream:
        stream.truncate(BLOCK_COUNT * BLOCK_BYTES)

    features = "none," + ",".join(FEATURES)
    extended = ",".join(
        (
            "nodiscard",
            "lazy_itable_init=0",
            "lazy_journal_init=0",
            "root_owner=0:0",
            "stride=0",
            "stripe_width=0",
            f"hash_seed={HASH_SEED_UUID}",
        )
    )
    # Isolate mke2fs from machine-local /etc/mke2fs.conf policy.  The command
    # line still repeats every layout-affecting value so the invocation is
    # auditable on its own.
    mke2fs_config = temporary / "mke2fs.conf"
    mke2fs_config.write_text(
        """[defaults]
base_features = sparse_super,large_file,filetype,dir_index,ext_attr
default_mntopts = acl,user_xattr
enable_periodic_fsck = 0
blocksize = 4096
inode_size = 256
inode_ratio = 16384

[fs_types]
small = {
inode_size = 256
}
ext4 = {
features = has_journal,extent,huge_file,metadata_csum,metadata_csum_seed,64bit,dir_nlink,extra_isize
}
""",
        encoding="ascii",
        newline="\n",
    )
    _run(
        (
            tools["mke2fs"],
            "-q",
            "-F",
            "-t",
            "ext4",
            "-b",
            str(BLOCK_BYTES),
            "-g",
            str(BLOCKS_PER_GROUP),
            "-I",
            str(INODE_BYTES),
            "-N",
            str(INODE_COUNT),
            "-m",
            "0",
            "-e",
            "remount-ro",
            "-L",
            VOLUME_LABEL,
            "-U",
            FILESYSTEM_UUID,
            "-M",
            LAST_MOUNTED,
            "-o",
            "linux",
            "-E",
            extended,
            "-O",
            features,
            "-J",
            "size=4",
            image,
            str(BLOCK_COUNT),
        ),
        extra_env={"MKE2FS_CONFIG": str(mke2fs_config)},
    )

    payloads = {
        "readme": temporary / "README.TXT",
        "app": temporary / "APP.BIN",
        "state": temporary / "state.txt",
        "empty": temporary / "empty",
        "entry": temporary / "entry",
    }
    payloads["readme"].write_bytes(b"Phipia deterministic ext4 fixture\n")
    payloads["app"].write_bytes(b"PHIPIA-APP\x00\x01\x02\x03\n")
    payloads["state"].write_bytes(b"counter=7\nmessage=persisted\n")
    payloads["empty"].write_bytes(b"")
    payloads["entry"].write_bytes(b"x\n")
    command_file = temporary / "debugfs.commands"
    _write_debugfs_script(command_file, payloads)
    _run((tools["debugfs"], "-w", "-f", command_file, image))

    # Fix link counts, refresh metadata checksums, and materialize /indexed's htree.
    _run((tools["e2fsck"], "-f", "-y", "-D", image), accepted=(0, 1))
    # libext2fs intentionally creates a feature-zero journal superblock and
    # normally relies on the first Linux mount to upgrade it. The deterministic
    # fixture is never mounted, so perform that small, checksummed upgrade here.
    _upgrade_journal_superblock(image, tools)


def build_image(output: Path) -> dict[str, object]:
    tools = require_tools()
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="phipia-ext4-", dir=output.parent) as raw:
        temporary = Path(raw)
        candidate = temporary / "phipia-ext4.img"
        _format_image(candidate, tools, temporary)
        report = inspect_image(candidate, tools=tools)
        os.replace(candidate, output)
    report["sha256"] = hashlib.sha256(output.read_bytes()).hexdigest()
    return report


def prepare_recovery_marker_image(source: Path, output: Path) -> dict[str, object]:
    """Clone a clean fixture at the crash point after ext4's marker flush.

    The JBD2 superblock deliberately remains clean.  This is the precise state
    reached when power is lost after the incompat-recovery marker becomes
    durable but before the first journal transaction starts.
    """
    tools = require_tools()
    source = source.resolve()
    output = output.resolve()
    source_report = inspect_image(source, tools=tools)
    if source_report["needs_recovery"]:
        _refuse("recovery-marker source is already dirty")
    if source_report["journal"]["start_block"] != 0:
        _refuse("recovery-marker source journal is not clean")
    if source == output:
        raise Ext4ImageError("recovery-marker output must not replace its source")
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, output)
    with output.open("r+b") as stream:
        stream.seek(1024)
        superblock = bytearray(stream.read(1024))
        if len(superblock) != 1024:
            _refuse("recovery-marker superblock is truncated")
        incompat = _read_u32(superblock, 0x60)
        if incompat & 0x0004:
            _refuse("recovery-marker output is already dirty")
        _write_u32(superblock, 0x60, incompat | 0x0004)
        _write_u32(superblock, 0x3FC, _crc32c_raw(superblock[:0x3FC]))
        stream.seek(1024)
        stream.write(superblock)
        stream.flush()
        os.fsync(stream.fileno())
    report = parse_superblock(output.read_bytes())
    journal = _inspect_journal_superblock(output, tools)
    if not report["needs_recovery"] or journal["start_block"] != 0:
        _refuse("recovery-marker crash state was not constructed exactly")
    report.update(
        {
            "crash_point": "ext4-recovery-marker-durable-before-journal-start",
            "journal": journal,
            "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        }
    )
    return report


def _parse_stat(output: str, path: str) -> dict[str, object]:
    inode = re.search(r"Inode:\s*(\d+).*?Mode:\s*([0-7]+)", output)
    owner = re.search(r"User:\s*(\d+)\s+Group:\s*(\d+).*?Size:\s*(\d+)", output)
    links = re.search(r"Links:\s*(\d+)\s+Blockcount:\s*(\d+)", output)
    if not inode or not owner or not links:
        raise Ext4ImageError(f"could not parse debugfs stat for {path!r}:\n{output.strip()}")
    times: dict[str, int] = {}
    for name in ("ctime", "atime", "mtime", "crtime"):
        match = re.search(rf"\b{name}:\s+0x([0-9a-fA-F]+)", output)
        if match:
            times[name] = int(match.group(1), 16)
    return {
        "inode": int(inode.group(1)),
        "mode": inode.group(2).zfill(4),
        "uid": int(owner.group(1)),
        "gid": int(owner.group(2)),
        "size": int(owner.group(3)),
        "links": int(links.group(1)),
        "block_count_512": int(links.group(2)),
        "times": times,
    }


def _e2fsck_read_only(image: Path, tools: dict[str, str]) -> None:
    result = _run((tools["e2fsck"], "-f", "-n", image), accepted=(0, 4))
    descriptor_checksum = re.search(
        r"(?:group descriptor \d+ checksum is|block group descriptor "
        r"checksums are invalid)", result.stdout, re.IGNORECASE)
    if result.returncode != 0 or descriptor_checksum is not None:
        raise Ext4ImageError("e2fsck refusal: filesystem metadata is inconsistent\n" + result.stdout.strip())


def inspect_image(image: Path, *, tools: dict[str, str] | None = None) -> dict[str, object]:
    tools = tools or require_tools()
    image = image.resolve()
    data = image.read_bytes()
    report = parse_superblock(data)
    _e2fsck_read_only(image, tools)

    paths = (
        "/data/user",
        "/system/README.TXT",
        "/packages/demo/APP.BIN",
        "/data/user/state.txt",
        "/data/user/state-hard.txt",
        "/data/user/state-link",
        "/data/user/large-sparse.bin",
        "/indexed",
    )
    fixtures = {path: _parse_stat(_debugfs(tools, image, f"stat {path}"), path) for path in paths}
    state = fixtures["/data/user/state.txt"]
    hard = fixtures["/data/user/state-hard.txt"]
    sparse = fixtures["/data/user/large-sparse.bin"]
    if fixtures["/data/user"]["mode"] != "0750":
        _refuse("writable data directory permissions changed")
    if fixtures["/system/README.TXT"]["mode"] != "0444":
        _refuse("system read-only fixture permissions changed")
    if fixtures["/packages/demo/APP.BIN"]["mode"] != "0555":
        _refuse("package executable fixture permissions changed")
    if state["inode"] != hard["inode"] or state["links"] != 2:
        _refuse("hard-link fixture does not share an inode with link count 2")
    if state["mode"] != "0640" or state["uid"] != 1000 or state["gid"] != 1000:
        _refuse("state fixture permissions or ownership changed")
    if sparse["mode"] != "0600" or sparse["uid"] != 1000 or sparse["gid"] != 1000:
        _refuse("sparse fixture permissions or ownership changed")
    if sparse["size"] != LARGE_SPARSE_BYTES:
        _refuse(f"large sparse fixture size is {sparse['size']}, expected {LARGE_SPARSE_BYTES}")
    if sparse["block_count_512"] == 0:
        _refuse("large-file fixture has no allocated extent blocks")
    if sparse["block_count_512"] >= LARGE_SPARSE_BYTES // 512:
        _refuse("large-file fixture is not sparse")
    for path in ("/data/user/state.txt", "/data/user/large-sparse.bin"):
        observed = fixtures[path]["times"]
        if any(observed.get(name) != FIXED_EPOCH for name in ("ctime", "atime", "mtime", "crtime")):
            _refuse(f"fixture timestamps are not pinned for {path}")

    symlink_stat = _debugfs(tools, image, "stat /data/user/state-link")
    if 'Fast link dest: "state.txt"' not in symlink_stat:
        _refuse("symlink fixture target is not state.txt")
    xattr = _debugfs(tools, image, f"ea_get /data/user/state.txt {XATTR_NAME}")
    if XATTR_VALUE not in xattr:
        _refuse(f"missing or incorrect {XATTR_NAME} fixture")
    extents = _debugfs(tools, image, "dump_extents -l /data/user/large-sparse.bin")
    if "Uninit" not in extents:
        _refuse("large sparse fixture does not contain unwritten extents")
    htree = _debugfs(tools, image, "htree_dump /indexed")
    if "Root node" not in htree:
        _refuse("indexed directory fixture has no htree root")

    report.update(
        {
            "e2fsck_clean": True,
            "journal": _inspect_journal_superblock(image, tools),
            "e2fsprogs": _tool_versions(tools),
            "fixed_epoch": FIXED_EPOCH,
            "profile": {
                "block_count": BLOCK_COUNT,
                "blocks_per_group": BLOCKS_PER_GROUP,
                "inode_count": INODE_COUNT,
                "inode_size": INODE_BYTES,
                "hash_seed": HASH_SEED_UUID,
                "journal_mebibytes": 4,
            },
            "large_sparse_bytes": LARGE_SPARSE_BYTES,
            "hardlink_inode": state["inode"],
            "symlink_target": "state.txt",
            "xattr": {"name": XATTR_NAME, "value": XATTR_VALUE},
            "fixtures": fixtures,
            "sha256": hashlib.sha256(data).hexdigest(),
        }
    )
    return report


def _locate_inode(tools: dict[str, str], image: Path, filespec: str) -> int:
    output = _debugfs(tools, image, f"imap {filespec}")
    match = re.search(r"located at block\s+(\d+),\s+offset\s+0x([0-9a-fA-F]+)", output)
    if not match:
        raise Ext4ImageError(f"could not locate inode for {filespec!r}:\n{output.strip()}")
    return int(match.group(1)) * BLOCK_BYTES + int(match.group(2), 16)


def _locate_data_block(tools: dict[str, str], image: Path, filespec: str) -> int:
    output = _debugfs(tools, image, f"bmap {filespec} 0")
    numbers = re.findall(r"\b(\d+)\b", output)
    if not numbers:
        raise Ext4ImageError(f"could not map first data block for {filespec!r}:\n{output.strip()}")
    return int(numbers[-1]) * BLOCK_BYTES


def _mutation(kind: str, data: bytearray, source: Path, tools: dict[str, str]) -> None:
    sb = 1024
    operations: dict[str, Callable[[], None]] = {
        "magic": lambda: _write_u16(data, sb + 0x38, 0),
        "block-size": lambda: _write_u32(data, sb + 0x18, 1),
        "missing-extents": lambda: _write_u32(data, sb + 0x60, _read_u32(data, sb + 0x60) & ~0x40),
        "missing-64bit": lambda: _write_u32(data, sb + 0x60, _read_u32(data, sb + 0x60) & ~0x80),
        "missing-metadata-csum": lambda: _write_u32(data, sb + 0x64, _read_u32(data, sb + 0x64) & ~0x400),
        "unsupported-incompat": lambda: _write_u32(data, sb + 0x60, _read_u32(data, sb + 0x60) | 0x10000),
        "blocks-range": lambda: (_write_u32(data, sb + 0x04, 0xFFFFFFFF), _write_u32(data, sb + 0x150, 0xFFFFFFFF)),
        "superblock-checksum": lambda: data.__setitem__(sb + 0x3FC, data[sb + 0x3FC] ^ 0x80),
        "group-descriptor-checksum": lambda: data.__setitem__(BLOCK_BYTES + 0x1E, data[BLOCK_BYTES + 0x1E] ^ 0x80),
    }
    if kind in operations:
        operations[kind]()
        return
    if kind in ("inode-checksum", "extent-header"):
        offset = _locate_inode(tools, source, "/data/user/large-sparse.bin")
        if kind == "inode-checksum":
            data[offset + 0x7C] ^= 0x80
        else:
            data[offset + 0x28] ^= 0x80
        return
    if kind == "directory-record":
        offset = _locate_data_block(tools, source, "/indexed")
        _write_u16(data, offset + 4, 0)
        return
    raise Ext4ImageError(f"unknown mutation {kind!r}")


def malform_image(kind: str, source: Path, output: Path) -> None:
    if kind not in MUTATIONS:
        raise Ext4ImageError(f"unknown mutation {kind!r}; choose from {', '.join(MUTATIONS)}")
    tools = require_tools()
    source = source.resolve()
    output = output.resolve()
    data = bytearray(source.read_bytes())
    parse_superblock(data)
    _mutation(kind, data, source, tools)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix="phipia-ext4-malform-", dir=output.parent, delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(data)
    try:
        refused = False
        try:
            inspect_image(temporary, tools=tools)
        except Ext4ImageError:
            refused = True
        if not refused:
            raise Ext4ImageError(f"mutation {kind!r} was not refused by the verifier")
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def _write_report(report: dict[str, object], destination: Path | None) -> None:
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if destination is None:
        sys.stdout.write(rendered)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(rendered, encoding="utf-8", newline="\n")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    build = subparsers.add_parser("build", help="build and verify the deterministic fixture")
    build.add_argument("output", type=Path)
    build.add_argument("--report", type=Path)
    inspect = subparsers.add_parser("inspect", help="inspect and verify a fixture")
    inspect.add_argument("image", type=Path)
    inspect.add_argument("--report", type=Path)
    verify = subparsers.add_parser("verify", help="verify a fixture and print its SHA-256")
    verify.add_argument("image", type=Path)
    recover = subparsers.add_parser(
        "prepare-recovery-marker",
        help="clone a clean fixture at the marker-durable, journal-clean crash point",
    )
    recover.add_argument("source", type=Path)
    recover.add_argument("output", type=Path)
    recover.add_argument("--report", type=Path)
    malform = subparsers.add_parser("malform", help="create one named refused image")
    malform.add_argument("kind", choices=MUTATIONS)
    malform.add_argument("source", type=Path)
    malform.add_argument("output", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "build":
            _write_report(build_image(args.output), args.report)
        elif args.command == "inspect":
            _write_report(inspect_image(args.image), args.report)
        elif args.command == "verify":
            report = inspect_image(args.image)
            print(f"verified ext4 {args.image}: sha256={report['sha256']}")
        elif args.command == "prepare-recovery-marker":
            _write_report(
                prepare_recovery_marker_image(args.source, args.output),
                args.report,
            )
        elif args.command == "malform":
            malform_image(args.kind, args.source, args.output)
            print(f"wrote refused ext4 mutation {args.kind}: {args.output}")
        return 0
    except (Ext4ImageError, OSError) as error:
        print(f"ext4_image.py: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
