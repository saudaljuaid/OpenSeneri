#!/usr/bin/env python3
"""Deterministic Phipia FAT32 formatter, inspector, and fixture mutator."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
from typing import Iterable


SECTOR_BYTES = 512
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 32
FAT_COPIES = 2
TOTAL_SECTORS = 131_072
ROOT_CLUSTER = 2
FSINFO_SECTOR = 1
BACKUP_BOOT_SECTOR = 6
MEDIA = 0xF8
FAT32_MIN_CLUSTERS = 65_525
FAT32_MAX_CLUSTER = 0x0FFFFFEF
FAT32_BAD = 0x0FFFFFF7
FAT32_EOC_MIN = 0x0FFFFFF8
FAT32_EOC = 0x0FFFFFFF
ENTRY_BYTES = 32
SYSTEM_VOLUME_ID = 0x2000_0001
DATA_VOLUME_ID = 0x2000_0002
SYSTEM_LABEL = b"PHIPIASYS  "
DATA_LABEL = b"PHIPIADATA "
OEM_NAME = b"PHIPIA22"
SYSTEM_FILES = (
    ("echo", b"BUSYBOX    ", 33_584,
     "B308F2CAD5B5CD0EEB92A622DEC8D71C1A08F628A22CDC5BCDE2B98B53220746"),
    ("uname", b"UNAMEBOX   ", 38_368,
     "389AD6B13804EB7307BA589C8E8A7C702F91302005A7C5FC6E9E99124FCEAF43"),
    ("cat", b"CATBOX     ", 38_632,
     "8191596A22778B575942895071A2E50CCEEE0F82F4D88B6D986584CE0914FC3E"),
)


class Fat32Error(ValueError):
    """A named, bounded FAT32 validation failure."""


def u16(data: bytes | bytearray | memoryview, offset: int) -> int:
    return int.from_bytes(data[offset:offset + 2], "little")


def u32(data: bytes | bytearray | memoryview, offset: int) -> int:
    return int.from_bytes(data[offset:offset + 4], "little")


def put_u16(data: bytearray, offset: int, value: int) -> None:
    data[offset:offset + 2] = value.to_bytes(2, "little")


def put_u32(data: bytearray, offset: int, value: int) -> None:
    data[offset:offset + 4] = value.to_bytes(4, "little")


def checked_add(left: int, right: int, limit: int = (1 << 64) - 1) -> int:
    if left < 0 or right < 0 or left > limit - right:
        raise Fat32Error("integer addition overflow")
    return left + right


def checked_mul(left: int, right: int, limit: int = (1 << 64) - 1) -> int:
    if left < 0 or right < 0 or (left and right > limit // left):
        raise Fat32Error("integer multiplication overflow")
    return left * right


def compute_fat_sectors(total_sectors: int) -> int:
    """Solve FAT/data geometry until the FAT covers every data cluster."""
    fat_sectors = 1
    while True:
        data_sectors = total_sectors - RESERVED_SECTORS - FAT_COPIES * fat_sectors
        if data_sectors <= 0:
            raise Fat32Error("volume is too small for FAT32 geometry")
        clusters = data_sectors // SECTORS_PER_CLUSTER
        entries = fat_sectors * SECTOR_BYTES // 4
        if entries >= clusters + 2:
            return fat_sectors
        fat_sectors += 1


FAT_SECTORS = compute_fat_sectors(TOTAL_SECTORS)
FIRST_FAT_SECTOR = RESERVED_SECTORS
FIRST_DATA_SECTOR = RESERVED_SECTORS + FAT_COPIES * FAT_SECTORS
CLUSTER_COUNT = (TOTAL_SECTORS - FIRST_DATA_SECTOR) // SECTORS_PER_CLUSTER
IMAGE_BYTES = TOTAL_SECTORS * SECTOR_BYTES


@dataclass(frozen=True)
class Geometry:
    """Checked FAT32 geometry derived from an untrusted boot sector."""

    bytes_per_sector: int
    sectors_per_cluster: int
    reserved_sectors: int
    fat_copies: int
    total_sectors: int
    fat_sectors: int
    root_cluster: int
    fsinfo_sector: int
    backup_boot_sector: int
    first_fat_sector: int
    first_data_sector: int
    cluster_count: int
    maximum_cluster: int
    volume_id: int
    volume_label: str

    def cluster_sector(self, cluster: int) -> int:
        if cluster < 2 or cluster > self.maximum_cluster:
            raise Fat32Error(f"cluster {cluster} is outside the data region")
        relative = checked_mul(cluster - 2, self.sectors_per_cluster)
        sector = checked_add(self.first_data_sector, relative)
        if sector >= self.total_sectors:
            raise Fat32Error("cluster translation escaped the volume")
        return sector

    def sector_offset(self, sector: int) -> int:
        if sector < 0 or sector >= self.total_sectors:
            raise Fat32Error("sector lies outside the volume")
        return checked_mul(sector, self.bytes_per_sector)


@dataclass(frozen=True)
class DirectoryRecord:
    """One checked ordinary 8.3 directory entry."""

    path: str
    short_name: bytes
    attributes: int
    first_cluster: int
    size: int
    entry_offset: int
    parent_cluster: int

    @property
    def is_directory(self) -> bool:
        return (self.attributes & 0x10) != 0


def _boot_sector(volume_id: int, label: bytes) -> bytes:
    if len(label) != 11:
        raise Fat32Error("volume label must be exactly eleven bytes")
    boot = bytearray(SECTOR_BYTES)
    boot[0:3] = b"\xEB\x58\x90"
    boot[3:11] = OEM_NAME
    put_u16(boot, 11, SECTOR_BYTES)
    boot[13] = SECTORS_PER_CLUSTER
    put_u16(boot, 14, RESERVED_SECTORS)
    boot[16] = FAT_COPIES
    put_u16(boot, 17, 0)
    put_u16(boot, 19, 0)
    boot[21] = MEDIA
    put_u16(boot, 22, 0)
    put_u16(boot, 24, 1)
    put_u16(boot, 26, 1)
    put_u32(boot, 28, 0)
    put_u32(boot, 32, TOTAL_SECTORS)
    put_u32(boot, 36, FAT_SECTORS)
    put_u16(boot, 40, 0)
    put_u16(boot, 42, 0)
    put_u32(boot, 44, ROOT_CLUSTER)
    put_u16(boot, 48, FSINFO_SECTOR)
    put_u16(boot, 50, BACKUP_BOOT_SECTOR)
    boot[64] = 0x80
    boot[66] = 0x29
    put_u32(boot, 67, volume_id)
    boot[71:82] = label
    boot[82:90] = b"FAT32   "
    boot[510:512] = b"\x55\xAA"
    return bytes(boot)


def _fsinfo(free_clusters: int, next_free: int) -> bytes:
    info = bytearray(SECTOR_BYTES)
    put_u32(info, 0, 0x41615252)
    put_u32(info, 484, 0x61417272)
    put_u32(info, 488, free_clusters)
    put_u32(info, 492, next_free)
    put_u32(info, 508, 0xAA550000)
    return bytes(info)


def _fat_offset(copy: int, cluster: int) -> int:
    if copy < 0 or copy >= FAT_COPIES or cluster < 0:
        raise Fat32Error("invalid FAT address")
    sector = FIRST_FAT_SECTOR + copy * FAT_SECTORS
    return sector * SECTOR_BYTES + cluster * 4


def _set_fat(image: bytearray, cluster: int, value: int) -> None:
    if cluster > CLUSTER_COUNT + 1:
        raise Fat32Error("FAT update lies outside allocated entries")
    for copy in range(FAT_COPIES):
        put_u32(image, _fat_offset(copy, cluster), value & 0x0FFFFFFF)


def _cluster_offset(cluster: int) -> int:
    if cluster < 2 or cluster > CLUSTER_COUNT + 1:
        raise Fat32Error("cluster lies outside the formatted volume")
    return (FIRST_DATA_SECTOR + cluster - 2) * SECTOR_BYTES


def _directory_entry(name: bytes, attributes: int, cluster: int, size: int) -> bytes:
    if len(name) != 11 or cluster < 0 or cluster > FAT32_MAX_CLUSTER:
        raise Fat32Error("invalid directory entry input")
    entry = bytearray(ENTRY_BYTES)
    entry[0:11] = name
    entry[11] = attributes
    put_u16(entry, 20, cluster >> 16)
    put_u16(entry, 26, cluster & 0xFFFF)
    put_u32(entry, 28, size)
    return bytes(entry)


def verify_busybox(binary: bytes, profile: str, size: int, digest: str) -> None:
    if len(binary) != size:
        raise Fat32Error(f"{profile} BusyBox byte count changed")
    if hashlib.sha256(binary).hexdigest().upper() != digest:
        raise Fat32Error(f"{profile} BusyBox SHA-256 changed")
    if binary[:16] != b"\x7fELF\x02\x01\x01\x00" + b"\x00" * 8:
        raise Fat32Error(f"{profile} BusyBox is not the measured ELF64 file")
    elf_type, machine, version = struct.unpack_from("<HHI", binary, 16)
    if (elf_type, machine, version) != (2, 62, 1):
        raise Fat32Error(f"{profile} BusyBox ELF64 identity changed")


def build_image(
    volume: str,
    binaries: Iterable[bytes] = (),
    extra_files: Iterable[tuple[str, bytes]] = (),
) -> bytes:
    """Construct a deterministic, byte-complete system or data image."""
    if volume not in ("system", "data"):
        raise Fat32Error("volume role must be system or data")
    label = SYSTEM_LABEL if volume == "system" else DATA_LABEL
    volume_id = SYSTEM_VOLUME_ID if volume == "system" else DATA_VOLUME_ID
    payloads = tuple(binaries)
    extras = tuple(extra_files)
    if volume == "system" and len(payloads) not in (0, len(SYSTEM_FILES)):
        raise Fat32Error("system volume needs either no legacy profiles or echo, uname, and cat")
    if volume == "data" and payloads:
        raise Fat32Error("data volume formatter does not accept payloads")
    if volume == "data" and extras:
        raise Fat32Error("data volume formatter does not accept extra files")
    prepared_extras: list[tuple[bytes, bytes]] = []
    prepared_directories: dict[bytes, list[tuple[bytes, bytes]]] = {}
    directory_names: set[bytes] = set()
    directory_files: set[tuple[bytes, bytes]] = set()
    occupied = ({record[1] for record in SYSTEM_FILES}
                if volume == "system" and payloads else set())
    for name, payload in extras:
        if not payload or len(payload) > 16 * 1024 * 1024:
            raise Fat32Error("extra system file violates the file-size bound")
        if name.count("/") == 0:
            short_name = short_name_bytes(name)
            if short_name in occupied or short_name in directory_names:
                raise Fat32Error("extra system filenames must be unique")
            occupied.add(short_name)
            prepared_extras.append((short_name, payload))
        elif name.count("/") == 1:
            directory_text, filename_text = name.split("/", 1)
            directory = short_name_bytes(directory_text)
            filename = short_name_bytes(filename_text)
            key = (directory, filename)
            if directory in occupied or key in directory_files:
                raise Fat32Error("extra system paths must be unique")
            directory_names.add(directory)
            directory_files.add(key)
            prepared_directories.setdefault(directory, []).append(
                (filename, payload))
        else:
            raise Fat32Error("system packages support one resource directory level")
    if any(len(files) > SECTOR_BYTES // ENTRY_BYTES - 3
           for files in prepared_directories.values()):
        raise Fat32Error("system resource directory exceeds one cluster")
    if 1 + len(payloads) + len(prepared_extras) + len(prepared_directories) \
            >= SECTOR_BYTES // ENTRY_BYTES:
        raise Fat32Error("system root entries exceed the one-cluster package bound")

    image = bytearray(IMAGE_BYTES)
    boot = _boot_sector(volume_id, label)
    image[0:SECTOR_BYTES] = boot
    backup = BACKUP_BOOT_SECTOR * SECTOR_BYTES
    image[backup:backup + SECTOR_BYTES] = boot
    _set_fat(image, 0, 0x0FFFFF00 | MEDIA)
    _set_fat(image, 1, FAT32_EOC)
    _set_fat(image, ROOT_CLUSTER, FAT32_EOC)
    root = _cluster_offset(ROOT_CLUSTER)
    image[root:root + ENTRY_BYTES] = _directory_entry(label, 0x08, 0, 0)

    next_cluster = ROOT_CLUSTER + 1
    slot = 1
    if volume == "system":
        for payload, (profile, name, expected_size, digest) in zip(payloads, SYSTEM_FILES):
            verify_busybox(payload, profile, expected_size, digest)
            cluster_count = (len(payload) + SECTOR_BYTES - 1) // SECTOR_BYTES
            first_cluster = next_cluster
            for ordinal in range(cluster_count):
                cluster = first_cluster + ordinal
                following = cluster + 1 if ordinal + 1 < cluster_count else FAT32_EOC
                _set_fat(image, cluster, following)
                offset = _cluster_offset(cluster)
                start = ordinal * SECTOR_BYTES
                image[offset:offset + min(SECTOR_BYTES, len(payload) - start)] = (
                    payload[start:start + SECTOR_BYTES]
                )
            entry = _directory_entry(name, 0x21, first_cluster, len(payload))
            image[root + slot * ENTRY_BYTES:root + (slot + 1) * ENTRY_BYTES] = entry
            slot += 1
            next_cluster += cluster_count
        for name, payload in prepared_extras:
            cluster_count = (len(payload) + SECTOR_BYTES - 1) // SECTOR_BYTES
            first_cluster = next_cluster
            if first_cluster + cluster_count - 1 > CLUSTER_COUNT + 1:
                raise Fat32Error("system packages exceed the volume capacity")
            for ordinal in range(cluster_count):
                cluster = first_cluster + ordinal
                following = cluster + 1 if ordinal + 1 < cluster_count else FAT32_EOC
                _set_fat(image, cluster, following)
                offset = _cluster_offset(cluster)
                start = ordinal * SECTOR_BYTES
                block = payload[start:start + SECTOR_BYTES]
                image[offset:offset + len(block)] = block
            entry = _directory_entry(name, 0x21, first_cluster, len(payload))
            image[root + slot * ENTRY_BYTES:root + (slot + 1) * ENTRY_BYTES] = entry
            slot += 1
            next_cluster += cluster_count
        directory_clusters: dict[bytes, int] = {}
        for directory in sorted(prepared_directories):
            directory_clusters[directory] = next_cluster
            _set_fat(image, next_cluster, FAT32_EOC)
            entry = _directory_entry(directory, 0x10, next_cluster, 0)
            image[root + slot * ENTRY_BYTES:root + (slot + 1) * ENTRY_BYTES] = entry
            slot += 1
            next_cluster += 1
        for directory in sorted(prepared_directories):
            directory_cluster = directory_clusters[directory]
            directory_offset = _cluster_offset(directory_cluster)
            image[directory_offset:directory_offset + ENTRY_BYTES] = (
                _directory_entry(b".          ", 0x10, directory_cluster, 0)
            )
            image[directory_offset + ENTRY_BYTES:
                  directory_offset + 2 * ENTRY_BYTES] = (
                _directory_entry(b"..         ", 0x10, ROOT_CLUSTER, 0)
            )
            for resource_slot, (name, payload) in enumerate(
                    sorted(prepared_directories[directory],
                           key=lambda item: item[0]), start=2):
                cluster_count = (len(payload) + SECTOR_BYTES - 1) // SECTOR_BYTES
                first_cluster = next_cluster
                if first_cluster + cluster_count - 1 > CLUSTER_COUNT + 1:
                    raise Fat32Error("system resources exceed the volume capacity")
                for ordinal in range(cluster_count):
                    cluster = first_cluster + ordinal
                    following = cluster + 1 if ordinal + 1 < cluster_count else FAT32_EOC
                    _set_fat(image, cluster, following)
                    offset = _cluster_offset(cluster)
                    start = ordinal * SECTOR_BYTES
                    block = payload[start:start + SECTOR_BYTES]
                    image[offset:offset + len(block)] = block
                entry = _directory_entry(name, 0x21, first_cluster, len(payload))
                location = directory_offset + resource_slot * ENTRY_BYTES
                image[location:location + ENTRY_BYTES] = entry
                next_cluster += cluster_count

    used_clusters = next_cluster - 2
    free_clusters = CLUSTER_COUNT - used_clusters
    next_free = next_cluster if next_cluster <= CLUSTER_COUNT + 1 else 0xFFFFFFFF
    info = _fsinfo(free_clusters, next_free)
    info_offset = FSINFO_SECTOR * SECTOR_BYTES
    image[info_offset:info_offset + SECTOR_BYTES] = info
    backup_info = (BACKUP_BOOT_SECTOR + FSINFO_SECTOR) * SECTOR_BYTES
    image[backup_info:backup_info + SECTOR_BYTES] = info
    return bytes(image)


def build_full_data_image() -> bytes:
    """Construct a deterministic full-volume recovery fixture."""
    image = bytearray(build_image("data"))
    root = _cluster_offset(ROOT_CLUSTER)
    counts = (1, 32_768, 32_768, 32_768,
              CLUSTER_COUNT - 1 - 1 - 3 * 32_768)
    names = (b"TINY    BIN", b"FILL1   BIN", b"FILL2   BIN",
             b"FILL3   BIN", b"FILL4   BIN")
    next_cluster = ROOT_CLUSTER + 1

    if any(count <= 0 or count * SECTOR_BYTES > 16 * 1024 * 1024
           for count in counts) or sum(counts) != CLUSTER_COUNT - 1:
        raise Fat32Error("full fixture does not satisfy the kernel file bound")
    for slot, (name, count) in enumerate(zip(names, counts), start=1):
        first = next_cluster
        for ordinal in range(count):
            cluster = first + ordinal
            _set_fat(image, cluster,
                     cluster + 1 if ordinal + 1 < count else FAT32_EOC)
        image[root + slot * ENTRY_BYTES:root + (slot + 1) * ENTRY_BYTES] = (
            _directory_entry(name, 0x20, first, count * SECTOR_BYTES)
        )
        next_cluster += count
    if next_cluster != CLUSTER_COUNT + 2:
        raise Fat32Error("full fixture allocation did not consume the volume")
    info = _fsinfo(0, 0xFFFFFFFF)
    for sector in (FSINFO_SECTOR, BACKUP_BOOT_SECTOR + FSINFO_SECTOR):
        offset = sector * SECTOR_BYTES
        image[offset:offset + SECTOR_BYTES] = info
    return bytes(image)


def short_name_bytes(name: str) -> bytes:
    """Encode the host staging subset as one canonical FAT 8.3 name."""
    upper = name.upper()
    if upper.count(".") > 1:
        raise Fat32Error("staged filename must be a single 8.3 name")
    parts = upper.split(".", 1)
    base = parts[0]
    extension = parts[1] if len(parts) == 2 else ""
    allowed = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
    if not 1 <= len(base) <= 8 or len(extension) > 3:
        raise Fat32Error("staged filename exceeds the 8.3 subset")
    if any(character not in allowed for character in base + extension):
        raise Fat32Error("staged filename contains an unsupported character")
    return (base.ljust(8) + extension.ljust(3)).encode("ascii")


def populate_data_files(
    source: bytes, files: list[tuple[str, bytes]]
) -> bytes:
    """Place bounded ordinary files in a fresh deterministic data image."""
    verify_data(source)
    if not files or len(files) > SECTOR_BYTES // ENTRY_BYTES - 1:
        raise Fat32Error("staged file count exceeds the bounded root subset")
    names: set[bytes] = set()
    prepared: list[tuple[bytes, bytes, int]] = []
    total_clusters = 0
    for name, payload in files:
        if not payload:
            raise Fat32Error("staged media file must not be empty")
        if len(payload) > 16 * 1024 * 1024:
            raise Fat32Error("staged media file exceeds the kernel file bound")
        short_name = short_name_bytes(name)
        if short_name in names:
            raise Fat32Error("staged filenames must be unique")
        names.add(short_name)
        clusters = (len(payload) + SECTOR_BYTES - 1) // SECTOR_BYTES
        total_clusters += clusters
        prepared.append((short_name, payload, clusters))
    first_free = ROOT_CLUSTER + 1
    if first_free + total_clusters - 1 > CLUSTER_COUNT + 1:
        raise Fat32Error("staged media files do not fit the data volume")

    image = bytearray(source)
    root = _cluster_offset(ROOT_CLUSTER)
    next_cluster = first_free
    for index, (short_name, payload, clusters) in enumerate(prepared):
        first_cluster = next_cluster
        for ordinal in range(clusters):
            cluster = first_cluster + ordinal
            following = cluster + 1 if ordinal + 1 < clusters else FAT32_EOC
            _set_fat(image, cluster, following)
            offset = _cluster_offset(cluster)
            start = ordinal * SECTOR_BYTES
            block = payload[start:start + SECTOR_BYTES]
            image[offset:offset + len(block)] = block
        entry = root + (index + 1) * ENTRY_BYTES
        image[entry:entry + ENTRY_BYTES] = _directory_entry(
            short_name, 0x20, first_cluster, len(payload)
        )
        next_cluster += clusters
    free_clusters = CLUSTER_COUNT - 1 - total_clusters
    next_free = next_cluster
    if next_free > CLUSTER_COUNT + 1:
        next_free = 0xFFFFFFFF
    info = _fsinfo(free_clusters, next_free)
    for sector in (FSINFO_SECTOR, BACKUP_BOOT_SECTOR + FSINFO_SECTOR):
        offset = sector * SECTOR_BYTES
        image[offset:offset + SECTOR_BYTES] = info
    populated = bytes(image)
    inspect_image(populated)
    return populated


def populate_data_image(source: bytes, name: str, payload: bytes) -> bytes:
    """Place one bounded ordinary file in a fresh deterministic data image."""
    return populate_data_files(source, [(name, payload)])


def populate_data_tree(
    source: bytes, files: list[tuple[str, bytes]]
) -> bytes:
    """Place files in deterministic one-level application namespaces."""
    verify_data(source)
    if not files:
        raise Fat32Error("staged application tree must not be empty")
    grouped: dict[bytes, list[tuple[bytes, bytes, int]]] = {}
    occupied: set[tuple[bytes, bytes]] = set()
    total_file_clusters = 0
    for path, payload in files:
        if path.count("/") != 1:
            raise Fat32Error("staged application paths must be DIRECTORY/NAME")
        directory_text, filename_text = path.split("/", 1)
        directory = short_name_bytes(directory_text)
        filename = short_name_bytes(filename_text)
        if not payload or len(payload) > 16 * 1024 * 1024:
            raise Fat32Error("staged application file violates the file-size bound")
        key = (directory, filename)
        if key in occupied:
            raise Fat32Error("staged application paths must be unique")
        occupied.add(key)
        clusters = (len(payload) + SECTOR_BYTES - 1) // SECTOR_BYTES
        total_file_clusters += clusters
        grouped.setdefault(directory, []).append((filename, payload, clusters))
    if len(grouped) > SECTOR_BYTES // ENTRY_BYTES - 1:
        raise Fat32Error("application namespace count exceeds the root bound")
    if any(len(entries) > SECTOR_BYTES // ENTRY_BYTES - 3
           for entries in grouped.values()):
        raise Fat32Error("application namespace exceeds one directory cluster")
    total_clusters = len(grouped) + total_file_clusters
    if ROOT_CLUSTER + total_clusters > CLUSTER_COUNT + 1:
        raise Fat32Error("staged application tree does not fit the data volume")

    image = bytearray(source)
    root = _cluster_offset(ROOT_CLUSTER)
    next_cluster = ROOT_CLUSTER + 1
    directory_clusters: dict[bytes, int] = {}
    for directory in sorted(grouped):
        directory_clusters[directory] = next_cluster
        _set_fat(image, next_cluster, FAT32_EOC)
        next_cluster += 1

    for root_slot, directory in enumerate(sorted(grouped), start=1):
        directory_cluster = directory_clusters[directory]
        root_entry = root + root_slot * ENTRY_BYTES
        image[root_entry:root_entry + ENTRY_BYTES] = _directory_entry(
            directory, 0x10, directory_cluster, 0
        )
        directory_offset = _cluster_offset(directory_cluster)
        image[directory_offset:directory_offset + ENTRY_BYTES] = (
            _directory_entry(b".          ", 0x10, directory_cluster, 0)
        )
        image[directory_offset + ENTRY_BYTES:directory_offset + 2 * ENTRY_BYTES] = (
            _directory_entry(b"..         ", 0x10, ROOT_CLUSTER, 0)
        )
        for slot, (filename, payload, clusters) in enumerate(
                sorted(grouped[directory], key=lambda item: item[0]), start=2):
            first_cluster = next_cluster
            for ordinal in range(clusters):
                cluster = first_cluster + ordinal
                following = cluster + 1 if ordinal + 1 < clusters else FAT32_EOC
                _set_fat(image, cluster, following)
                offset = _cluster_offset(cluster)
                start = ordinal * SECTOR_BYTES
                block = payload[start:start + SECTOR_BYTES]
                image[offset:offset + len(block)] = block
            entry = directory_offset + slot * ENTRY_BYTES
            image[entry:entry + ENTRY_BYTES] = _directory_entry(
                filename, 0x20, first_cluster, len(payload)
            )
            next_cluster += clusters

    free_clusters = CLUSTER_COUNT - 1 - total_clusters
    next_free = next_cluster if next_cluster <= CLUSTER_COUNT + 1 else 0xFFFFFFFF
    info = _fsinfo(free_clusters, next_free)
    for sector in (FSINFO_SECTOR, BACKUP_BOOT_SECTOR + FSINFO_SECTOR):
        offset = sector * SECTOR_BYTES
        image[offset:offset + SECTOR_BYTES] = info
    populated = bytes(image)
    inspect_image(populated)
    return populated


def parse_geometry(image: bytes | bytearray | memoryview) -> Geometry:
    if len(image) < SECTOR_BYTES:
        raise Fat32Error("truncated boot sector")
    if bytes(image[510:512]) != b"\x55\xAA":
        raise Fat32Error("invalid boot signature")
    bytes_per_sector = u16(image, 11)
    sectors_per_cluster = image[13]
    reserved = u16(image, 14)
    fats = image[16]
    root_entries = u16(image, 17)
    total16 = u16(image, 19)
    fat16 = u16(image, 22)
    total = u32(image, 32)
    fat_sectors = u32(image, 36)
    root_cluster = u32(image, 44)
    fsinfo_sector = u16(image, 48)
    backup_boot = u16(image, 50)
    if bytes_per_sector not in (512, 1024, 2048, 4096):
        raise Fat32Error("invalid FAT32 sector size")
    if sectors_per_cluster == 0 or sectors_per_cluster > 128 or (
            sectors_per_cluster & (sectors_per_cluster - 1)):
        raise Fat32Error("invalid FAT32 cluster size")
    if reserved < 2 or fats != 2 or root_entries != 0:
        raise Fat32Error("impossible FAT32 reserved/FAT/root geometry")
    if total16 != 0 or total == 0 or fat16 != 0 or fat_sectors == 0:
        raise Fat32Error("FAT32 legacy size fields are inconsistent")
    fat_span = checked_mul(fats, fat_sectors)
    first_data = checked_add(reserved, fat_span)
    if first_data >= total:
        raise Fat32Error("FAT32 metadata consumes the volume")
    data_sectors = total - first_data
    cluster_count = data_sectors // sectors_per_cluster
    if cluster_count < FAT32_MIN_CLUSTERS or cluster_count > FAT32_MAX_CLUSTER - 1:
        raise Fat32Error("cluster count does not classify as FAT32")
    maximum_cluster = checked_add(cluster_count, 1)
    fat_entries = checked_mul(fat_sectors, bytes_per_sector) // 4
    if fat_entries <= maximum_cluster:
        raise Fat32Error("FAT is too short for the data region")
    if root_cluster < 2 or root_cluster > maximum_cluster:
        raise Fat32Error("invalid FAT32 root cluster")
    if fsinfo_sector == 0 or fsinfo_sector >= reserved:
        raise Fat32Error("FSInfo sector lies outside reserved space")
    if backup_boot == 0 or backup_boot >= reserved or backup_boot == fsinfo_sector:
        raise Fat32Error("backup boot sector lies outside reserved space")
    image_bytes = checked_mul(total, bytes_per_sector)
    if image_bytes != len(image):
        raise Fat32Error("image length does not match BPB total sectors")
    label_bytes = bytes(image[71:82])
    if any(byte < 0x20 or byte > 0x7E for byte in label_bytes):
        raise Fat32Error("volume label contains invalid bytes")
    return Geometry(
        bytes_per_sector, sectors_per_cluster, reserved, fats, total,
        fat_sectors, root_cluster, fsinfo_sector, backup_boot, reserved,
        first_data, cluster_count, maximum_cluster, u32(image, 67),
        label_bytes.decode("ascii").rstrip(),
    )


def parse_fsinfo(image: bytes | bytearray | memoryview, geometry: Geometry) -> dict[str, int | bool]:
    offset = geometry.sector_offset(geometry.fsinfo_sector)
    info = image[offset:offset + geometry.bytes_per_sector]
    if len(info) < 512 or (u32(info, 0), u32(info, 484), u32(info, 508)) != (
            0x41615252, 0x61417272, 0xAA550000):
        raise Fat32Error("invalid FSInfo signatures")
    free = u32(info, 488)
    next_free = u32(info, 492)
    free_valid = free == 0xFFFFFFFF or free <= geometry.cluster_count
    next_valid = next_free == 0xFFFFFFFF or 2 <= next_free <= geometry.maximum_cluster
    if not free_valid or not next_valid:
        raise Fat32Error("invalid FSInfo free-space hint")
    return {"free_hint": free, "next_hint": next_free, "hint_only": True}


def fat_value(image: bytes | bytearray | memoryview, geometry: Geometry,
              copy: int, cluster: int) -> int:
    if copy < 0 or copy >= geometry.fat_copies:
        raise Fat32Error("FAT copy index is invalid")
    if cluster < 0 or cluster > geometry.maximum_cluster:
        raise Fat32Error("FAT cluster index is invalid")
    sector = geometry.first_fat_sector + copy * geometry.fat_sectors
    offset = geometry.sector_offset(sector) + cluster * 4
    if offset + 4 > len(image):
        raise Fat32Error("FAT lookup escaped the image")
    return u32(image, offset) & 0x0FFFFFFF


def compare_fats(image: bytes | bytearray | memoryview, geometry: Geometry) -> None:
    length = geometry.fat_sectors * geometry.bytes_per_sector
    first = geometry.sector_offset(geometry.first_fat_sector)
    second = geometry.sector_offset(geometry.first_fat_sector + geometry.fat_sectors)
    if bytes(image[first:first + length]) != bytes(image[second:second + length]):
        raise Fat32Error("FAT copies do not match")


def walk_chain(image: bytes | bytearray | memoryview, geometry: Geometry,
               first_cluster: int) -> tuple[int, ...]:
    if first_cluster == 0:
        return ()
    chain: list[int] = []
    seen: set[int] = set()
    cluster = first_cluster
    while True:
        if cluster < 2 or cluster > geometry.maximum_cluster:
            raise Fat32Error("chain contains an out-of-range cluster")
        if cluster in seen:
            raise Fat32Error("cyclic FAT chain")
        if len(chain) >= geometry.cluster_count:
            raise Fat32Error("FAT chain exceeds the volume bound")
        seen.add(cluster)
        chain.append(cluster)
        following = fat_value(image, geometry, 0, cluster)
        if following >= FAT32_EOC_MIN:
            return tuple(chain)
        if following == FAT32_BAD:
            raise Fat32Error("chain contains a bad cluster")
        if following < 2 or 0x0FFFFFF0 <= following <= 0x0FFFFFF6:
            raise Fat32Error("chain contains a free or reserved cluster")
        cluster = following


def short_name_text(name: bytes) -> str:
    if len(name) != 11:
        raise Fat32Error("short name has the wrong length")
    base = name[:8].decode("ascii", "strict").rstrip()
    extension = name[8:].decode("ascii", "strict").rstrip()
    if not base:
        raise Fat32Error("short name base is empty")
    return base + (("." + extension) if extension else "")


def _parse_directory(
    image: bytes | bytearray | memoryview,
    geometry: Geometry,
    cluster: int,
    path: str,
    owners: dict[int, str],
    directory_stack: set[int],
) -> list[DirectoryRecord]:
    if cluster in directory_stack:
        raise Fat32Error("directory graph contains a cycle")
    directory_stack.add(cluster)
    records: list[DirectoryRecord] = []
    lfn_pending = False
    terminated = False
    for directory_cluster in walk_chain(image, geometry, cluster):
        previous = owners.get(directory_cluster)
        if previous is not None and previous != path:
            raise Fat32Error(f"cross-linked directory cluster {directory_cluster}")
        owners[directory_cluster] = path
        offset = geometry.sector_offset(geometry.cluster_sector(directory_cluster))
        cluster_bytes = geometry.bytes_per_sector * geometry.sectors_per_cluster
        for entry_offset in range(offset, offset + cluster_bytes, ENTRY_BYTES):
            entry = image[entry_offset:entry_offset + ENTRY_BYTES]
            first = entry[0]
            if first == 0:
                terminated = True
                break
            if first == 0xE5:
                lfn_pending = False
                continue
            attributes = entry[11]
            if attributes == 0x0F:
                sequence = entry[0]
                if sequence & 0x1F == 0 or sequence & 0x1F > 20 or entry[12] != 0 or u16(entry, 26) != 0:
                    raise Fat32Error("malformed long filename sequence")
                for index in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30):
                    codepoint = u16(entry, index)
                    if codepoint not in (0, 0xFFFF) and (codepoint < 0x20 or codepoint > 0x7E):
                        raise Fat32Error("invalid UTF-16 long filename data")
                lfn_pending = True
                continue
            if lfn_pending:
                raise Fat32Error("long filenames are not enabled in the v2.0 kernel subset")
            name = bytes(entry[:11])
            if attributes & 0x08:
                continue
            text = short_name_text(name)
            first_cluster = (u16(entry, 20) << 16) | u16(entry, 26)
            size = u32(entry, 28)
            child_path = text if path == "/" else path.rstrip("/") + "/" + text
            record = DirectoryRecord(child_path, name, attributes,
                                     first_cluster, size, entry_offset, cluster)
            records.append(record)
        if terminated:
            break
    if not terminated:
        raise Fat32Error("truncated directory has no end marker")

    folded: set[bytes] = set()
    descendants: list[DirectoryRecord] = []
    for record in records:
        key = record.short_name.upper()
        if key in folded:
            raise Fat32Error("duplicate name under ASCII case-folding rules")
        folded.add(key)
        if record.short_name in (b".          ", b"..         "):
            continue
        if record.is_directory:
            if record.first_cluster < 2:
                raise Fat32Error("directory has no cluster")
            descendants.extend(_parse_directory(
                image, geometry, record.first_cluster, record.path, owners,
                directory_stack
            ))
        else:
            chain = walk_chain(image, geometry, record.first_cluster)
            capacity = len(chain) * geometry.bytes_per_sector * geometry.sectors_per_cluster
            expected = (record.size + geometry.bytes_per_sector *
                        geometry.sectors_per_cluster - 1) // (
                            geometry.bytes_per_sector *
                            geometry.sectors_per_cluster)
            if len(chain) != expected or record.size > capacity:
                raise Fat32Error("file size and chain are inconsistent")
            for owned in chain:
                previous = owners.get(owned)
                if previous is not None and previous != record.path:
                    raise Fat32Error(f"cross-linked cluster {owned}")
                owners[owned] = record.path
    directory_stack.remove(cluster)
    return records + descendants


def inspect_image(image: bytes | bytearray | memoryview) -> dict[str, object]:
    geometry = parse_geometry(image)
    parse_fsinfo(image, geometry)
    compare_fats(image, geometry)
    if fat_value(image, geometry, 0, 0) != (0x0FFFFF00 | MEDIA):
        raise Fat32Error("FAT media/reserved entry is invalid")
    if fat_value(image, geometry, 0, 1) < FAT32_EOC_MIN:
        raise Fat32Error("FAT reserved cluster one is invalid")
    owners: dict[int, str] = {}
    records = _parse_directory(image, geometry, geometry.root_cluster, "/", owners, set())
    allocated = 0
    leaked: list[int] = []
    for cluster in range(2, geometry.maximum_cluster + 1):
        value = fat_value(image, geometry, 0, cluster)
        if value != 0:
            allocated += 1
            if cluster not in owners:
                leaked.append(cluster)
    if leaked:
        raise Fat32Error(f"leaked allocated clusters: {leaked[:8]}")
    free_actual = geometry.cluster_count - allocated
    info = parse_fsinfo(image, geometry)
    hint = int(info["free_hint"])
    if hint != 0xFFFFFFFF and hint != free_actual:
        raise Fat32Error("FSInfo free-space hint disagrees with the FAT")
    return {
        "format": "FAT32",
        "sector_bytes": geometry.bytes_per_sector,
        "sectors_per_cluster": geometry.sectors_per_cluster,
        "total_sectors": geometry.total_sectors,
        "reserved_sectors": geometry.reserved_sectors,
        "fat_copies": geometry.fat_copies,
        "fat_sectors": geometry.fat_sectors,
        "first_data_sector": geometry.first_data_sector,
        "cluster_count": geometry.cluster_count,
        "root_cluster": geometry.root_cluster,
        "volume_id": f"{geometry.volume_id:08X}",
        "volume_label": geometry.volume_label,
        "free_clusters": free_actual,
        "allocated_clusters": allocated,
        "files": [
            {"path": record.path, "directory": record.is_directory,
             "size": record.size, "first_cluster": record.first_cluster}
            for record in records
        ],
        "fat_copies_match": True,
        "cycles": 0,
        "cross_links": 0,
        "leaked_clusters": 0,
    }


def verify_system(
    image: bytes,
    payloads: tuple[bytes, ...],
    extra_files: tuple[tuple[str, bytes], ...] = (),
) -> dict[str, object]:
    report = inspect_image(image)
    records = {item["path"]: item for item in report["files"]}  # type: ignore[index]
    geometry = parse_geometry(image)
    if geometry.volume_id != SYSTEM_VOLUME_ID or geometry.volume_label != SYSTEM_LABEL.decode().rstrip():
        raise Fat32Error("system volume identity changed")
    for payload, (profile, name, expected_size, digest) in zip(payloads, SYSTEM_FILES):
        verify_busybox(payload, profile, expected_size, digest)
        path = short_name_text(name)
        item = records.get(path)
        if item is None or item["directory"] or item["size"] != len(payload):
            raise Fat32Error(f"{profile} system entry changed")
        first_cluster = int(item["first_cluster"])
        body = bytearray()
        for cluster in walk_chain(image, geometry, first_cluster):
            offset = geometry.sector_offset(geometry.cluster_sector(cluster))
            body.extend(image[offset:offset + SECTOR_BYTES])
        if bytes(body[:len(payload)]) != payload or any(body[len(payload):]):
            raise Fat32Error(f"{profile} contents or zero padding changed")
    for name, payload in extra_files:
        item = records.get(name.upper())
        if item is None or item["directory"] or item["size"] != len(payload):
            raise Fat32Error(f"extra system entry {name} changed")
    rebuilt = build_image("system", payloads, extra_files)
    if image != rebuilt:
        raise Fat32Error("system image is not the deterministic reconstruction")
    return report


def verify_data(image: bytes) -> dict[str, object]:
    report = inspect_image(image)
    geometry = parse_geometry(image)
    if geometry.volume_id != DATA_VOLUME_ID or geometry.volume_label != DATA_LABEL.decode().rstrip():
        raise Fat32Error("data volume identity changed")
    if report["files"]:
        raise Fat32Error("fresh data image is not empty")
    if image != build_image("data"):
        raise Fat32Error("data image is not the deterministic reconstruction")
    return report


def verify_full_data(image: bytes) -> dict[str, object]:
    report = inspect_image(image)
    geometry = parse_geometry(image)
    if geometry.volume_id != DATA_VOLUME_ID or report["free_clusters"] != 0:
        raise Fat32Error("full data fixture identity or capacity changed")
    if image != build_full_data_image():
        raise Fat32Error("full data fixture is not the deterministic reconstruction")
    return report


def mutate_image(kind: str, source: bytes) -> bytes:
    geometry = parse_geometry(source)
    changed = bytearray(source)
    fat0 = geometry.sector_offset(geometry.first_fat_sector)
    fat1 = geometry.sector_offset(geometry.first_fat_sector + geometry.fat_sectors)
    root = geometry.sector_offset(geometry.cluster_sector(geometry.root_cluster))

    def truncate_directory() -> None:
        cluster_bytes = geometry.bytes_per_sector * geometry.sectors_per_cluster
        for offset in range(root + ENTRY_BYTES, root + cluster_bytes, ENTRY_BYTES):
            changed[offset] = 0xE5

    mutations = {
        "boot-signature": lambda: changed.__setitem__(slice(510, 512), b"\x00\x00"),
        "bpb-sector-size": lambda: put_u16(changed, 11, 768),
        "bpb-cluster-size": lambda: changed.__setitem__(13, 3),
        "bpb-overflow": lambda: put_u32(changed, 32, 0xFFFFFFFF),
        "root-cluster": lambda: put_u32(changed, 44, 1),
        "fsinfo-signature": lambda: put_u32(changed, geometry.sector_offset(geometry.fsinfo_sector), 0),
        "fsinfo-hint": lambda: put_u32(changed, geometry.sector_offset(geometry.fsinfo_sector) + 488, geometry.cluster_count + 1),
        "fat-mismatch": lambda: changed.__setitem__(fat1 + ROOT_CLUSTER * 4, changed[fat1 + ROOT_CLUSTER * 4] ^ 1),
        "reserved-cluster": lambda: put_u32(changed, fat0 + ROOT_CLUSTER * 4, 0x0FFFFFF2),
        "bad-cluster": lambda: put_u32(changed, fat0 + ROOT_CLUSTER * 4, FAT32_BAD),
        "out-of-range": lambda: put_u32(changed, fat0 + ROOT_CLUSTER * 4, geometry.maximum_cluster + 1),
        "cycle": lambda: put_u32(changed, fat0 + ROOT_CLUSTER * 4, ROOT_CLUSTER),
        "truncated-directory": truncate_directory,
        "malformed-lfn": lambda: changed.__setitem__(slice(root + ENTRY_BYTES, root + 2 * ENTRY_BYTES), bytes([0x41]) + b"X" * 10 + bytes([0x0F, 1]) + b"X" * 19),
    }
    action = mutations.get(kind)
    if action is None:
        raise Fat32Error(f"unknown malformed fixture kind: {kind}")
    action()
    if kind in ("reserved-cluster", "bad-cluster", "out-of-range", "cycle"):
        changed[fat1 + ROOT_CLUSTER * 4:fat1 + ROOT_CLUSTER * 4 + 4] = (
            changed[fat0 + ROOT_CLUSTER * 4:fat0 + ROOT_CLUSTER * 4 + 4]
        )
    return bytes(changed)


def read_regular(path: Path) -> bytes:
    if not path.is_file() or path.is_symlink():
        raise Fat32Error(f"not an ordinary input file: {path}")
    return path.read_bytes()


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.unlink(missing_ok=True)
    try:
        with temporary.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def parse_extra_file(value: str) -> tuple[str, bytes]:
    """Read one deterministic NAME=PATH system-volume input."""
    name, separator, path = value.partition("=")
    if not separator or not name or not path:
        raise Fat32Error("--extra requires one 8.3 NAME=PATH value")
    short_name_bytes(name)
    return name.upper(), read_regular(Path(path))


def parse_tree_file(value: str) -> tuple[str, bytes]:
    """Read one deterministic DIRECTORY/NAME=PATH data-volume input."""
    name, separator, path = value.partition("=")
    if not separator or not name or not path or name.count("/") != 1:
        raise Fat32Error("--file requires one DIRECTORY/NAME=PATH value")
    directory, filename = name.split("/", 1)
    short_name_bytes(directory)
    short_name_bytes(filename)
    return name.upper(), read_regular(Path(path))


def command_format(args: argparse.Namespace) -> None:
    payloads: tuple[bytes, ...] = ()
    if args.role == "system":
        payloads = tuple(read_regular(Path(path)) for path in (args.echo, args.uname, args.cat))
    extras = tuple(parse_extra_file(value) for value in args.extra)
    image = build_full_data_image() if args.full else build_image(args.role, payloads, extras)
    atomic_write(Path(args.output), image)
    report = verify_system(image, payloads, extras) if args.role == "system" else (
        verify_full_data(image) if args.full else verify_data(image))
    print(json.dumps({"output": str(Path(args.output)),
                      "sha256": hashlib.sha256(image).hexdigest().upper(),
                      **report}, sort_keys=True))


def command_inspect(args: argparse.Namespace) -> None:
    image = read_regular(Path(args.image))
    report = inspect_image(image)
    report["sha256"] = hashlib.sha256(image).hexdigest().upper()
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        atomic_write(Path(args.report), rendered.encode("utf-8"))
    sys.stdout.write(rendered)


def command_verify(args: argparse.Namespace) -> None:
    image = read_regular(Path(args.image))
    if args.role == "system":
        payloads = tuple(read_regular(Path(path)) for path in (args.echo, args.uname, args.cat))
        extras = tuple(parse_extra_file(value) for value in args.extra)
        report = verify_system(image, payloads, extras)
    else:
        report = verify_full_data(image) if args.full else verify_data(image)
    print(json.dumps({"sha256": hashlib.sha256(image).hexdigest().upper(), **report}, sort_keys=True))


def command_malform(args: argparse.Namespace) -> None:
    source = read_regular(Path(args.source))
    changed = mutate_image(args.kind, source)
    try:
        inspect_image(changed)
    except Fat32Error as error:
        atomic_write(Path(args.output), changed)
        print(json.dumps({"kind": args.kind, "detected": str(error),
                          "sha256": hashlib.sha256(changed).hexdigest().upper()}, sort_keys=True))
        return
    raise Fat32Error("malformed fixture was not rejected by the inspector")


def command_populate(args: argparse.Namespace) -> None:
    source = read_regular(Path(args.source))
    payload = read_regular(Path(args.input))
    populated = populate_data_image(source, args.name, payload)
    atomic_write(Path(args.output), populated)
    report = inspect_image(populated)
    print(json.dumps({"output": str(Path(args.output)), "name": args.name,
                      "payload_sha256": hashlib.sha256(payload).hexdigest().upper(),
                      "sha256": hashlib.sha256(populated).hexdigest().upper(),
                      **report}, sort_keys=True))


def command_populate_tree(args: argparse.Namespace) -> None:
    source = read_regular(Path(args.source))
    files = [parse_tree_file(value) for value in args.file]
    populated = populate_data_tree(source, files)
    atomic_write(Path(args.output), populated)
    report = inspect_image(populated)
    print(json.dumps({"output": str(Path(args.output)),
                      "inputs": sorted(name for name, _ in files),
                      "sha256": hashlib.sha256(populated).hexdigest().upper(),
                      **report}, sort_keys=True))


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    formatter = commands.add_parser("format")
    formatter.add_argument("role", choices=("system", "data"))
    formatter.add_argument("output")
    formatter.add_argument("--echo")
    formatter.add_argument("--uname")
    formatter.add_argument("--cat")
    formatter.add_argument("--extra", action="append", default=[])
    formatter.add_argument("--full", action="store_true")
    formatter.set_defaults(function=command_format)
    inspector = commands.add_parser("inspect")
    inspector.add_argument("image")
    inspector.add_argument("--report")
    inspector.set_defaults(function=command_inspect)
    verifier = commands.add_parser("verify")
    verifier.add_argument("role", choices=("system", "data"))
    verifier.add_argument("image")
    verifier.add_argument("--echo")
    verifier.add_argument("--uname")
    verifier.add_argument("--cat")
    verifier.add_argument("--extra", action="append", default=[])
    verifier.add_argument("--full", action="store_true")
    verifier.set_defaults(function=command_verify)
    malformed = commands.add_parser("malform")
    malformed.add_argument("kind", choices=(
        "boot-signature", "bpb-sector-size", "bpb-cluster-size",
        "bpb-overflow", "root-cluster", "fsinfo-signature", "fsinfo-hint",
        "fat-mismatch", "reserved-cluster", "bad-cluster", "out-of-range",
        "cycle", "truncated-directory", "malformed-lfn"))
    malformed.add_argument("source")
    malformed.add_argument("output")
    malformed.set_defaults(function=command_malform)
    populate = commands.add_parser("populate-data")
    populate.add_argument("source")
    populate.add_argument("output")
    populate.add_argument("--input", required=True)
    populate.add_argument("--name", required=True)
    populate.set_defaults(function=command_populate)
    tree = commands.add_parser("populate-tree")
    tree.add_argument("source")
    tree.add_argument("output")
    tree.add_argument("--file", action="append", required=True)
    tree.set_defaults(function=command_populate_tree)
    return result


def main() -> int:
    try:
        args = parser().parse_args()
        if args.command in ("format", "verify") and args.role == "system" and not all(
                (args.echo, args.uname, args.cat)):
            raise Fat32Error("system volume requires --echo, --uname, and --cat")
        if args.command in ("format", "verify") and args.full and args.role != "data":
            raise Fat32Error("--full is only valid for the data role")
        args.function(args)
    except (Fat32Error, OSError, UnicodeError, struct.error) as error:
        print(f"FAT32 operation refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
