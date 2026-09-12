#!/usr/bin/env python3
"""Canonical Phipia installed-state and transaction-recovery reference model.

This host tool defines bytes and deterministic state transitions.  It does not
install files into a host or guest filesystem and is not a guest package service.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
from typing import Any


PACKAGE_TOOL_PATH = Path(__file__).with_name("phipia-package.py")
PACKAGE_TOOL_SPEC = importlib.util.spec_from_file_location(
    "phipia_transaction_package_tool", PACKAGE_TOOL_PATH)
if PACKAGE_TOOL_SPEC is None or PACKAGE_TOOL_SPEC.loader is None:
    raise RuntimeError("could not load Phipia package tool")
PACKAGE = importlib.util.module_from_spec(PACKAGE_TOOL_SPEC)
PACKAGE_TOOL_SPEC.loader.exec_module(PACKAGE)


class TransactionError(ValueError):
    """A named installed-state or transaction refusal."""


DATABASE_MAGIC = b"PHIPDB01"
DATABASE_HEADER_BYTES = 512
DATABASE_PACKAGE_RECORD_BYTES = 256
DATABASE_EDGE_RECORD_BYTES = 192
DATABASE_FILE_RECORD_BYTES = 256
DATABASE_MAX_BYTES = 32 * 1024 * 1024
DATABASE_MAX_PACKAGES = 256
DATABASE_MAX_EDGES = 4096
DATABASE_MAX_FILES = 4096
DATABASE_FLAG_EXPLICIT = 1

AUTHORITY_MAGIC = b"PHIPGN01"
AUTHORITY_BYTES = 128

JOURNAL_MAGIC = b"PHIPTX01"
JOURNAL_BYTES = 512
JOURNAL_PHASE_PREPARED = 1
JOURNAL_OPERATIONS = {"install": 1, "update": 2, "remove": 3, "repair": 4}
JOURNAL_OPERATION_NAMES = {value: key for key, value in JOURNAL_OPERATIONS.items()}

FILE_KIND_NAMES = dict(PACKAGE.V3_FILE_KIND_NAMES)
FILE_KINDS = dict(PACKAGE.V3_FILE_KINDS)


def _uint(value: Any, maximum: int, field: str, *, positive: bool = False) -> int:
    if type(value) is not int or value < int(positive) or value > maximum:
        qualifier = "positive " if positive else ""
        raise TransactionError(f"{field} must be a {qualifier}bounded integer")
    return value


def _digest(value: Any, field: str) -> str:
    if not isinstance(value, str) or len(value) != 64:
        raise TransactionError(f"{field} must be 64 hexadecimal digits")
    try:
        raw = bytes.fromhex(value)
    except ValueError as error:
        raise TransactionError(f"{field} must be hexadecimal") from error
    if len(raw) != 32:
        raise TransactionError(f"{field} must be a SHA-256 value")
    return raw.hex().upper()


def _text(value: str, width: int, field: str, *, required: bool) -> bytes:
    try:
        return PACKAGE.text_field(value, width, field, required=required)
    except PACKAGE.PackageError as error:
        raise TransactionError(str(error)) from error


def _decode_text(value: bytes, field: str) -> str:
    try:
        return PACKAGE.decode_text(value, field)
    except PACKAGE.PackageError as error:
        raise TransactionError(str(error)) from error


def _identifier(value: Any, field: str) -> str:
    try:
        return PACKAGE.package_identifier(value, field)
    except PACKAGE.PackageError as error:
        raise TransactionError(str(error)) from error


def _version(value: Any, field: str) -> str:
    try:
        return PACKAGE.semantic_version(value, field)
    except PACKAGE.PackageError as error:
        raise TransactionError(str(error)) from error


def _constraint(value: Any, field: str) -> str:
    try:
        return PACKAGE.version_constraint(value, field)
    except PACKAGE.PackageError as error:
        raise TransactionError(str(error)) from error


def _path(value: Any, field: str) -> str:
    try:
        return PACKAGE.package_path(value, field)
    except PACKAGE.PackageError as error:
        raise TransactionError(str(error)) from error


def _soname(value: Any, field: str) -> str:
    try:
        return PACKAGE.soname(value, field)
    except PACKAGE.PackageError as error:
        raise TransactionError(str(error)) from error


def _architecture(value: Any) -> str:
    try:
        result = PACKAGE.canonical_printable(value, 16, "architecture")
    except PACKAGE.PackageError as error:
        raise TransactionError(str(error)) from error
    if result != "x86_64":
        raise TransactionError("architecture must be x86_64")
    return result


def _normalize_dependency(value: Any, field: str) -> dict[str, str]:
    if not isinstance(value, dict) or set(value) != {"identifier", "constraint", "provider"}:
        raise TransactionError(
            f"{field} must contain only identifier, constraint, and provider")
    return {
        "identifier": _identifier(value["identifier"], f"{field}.identifier"),
        "constraint": _constraint(value["constraint"], f"{field}.constraint"),
        "provider": _identifier(value["provider"], f"{field}.provider"),
    }


def _normalize_file_metadata(value: Any, field: str) -> dict[str, Any]:
    required = {"path", "kind", "mode", "bytes", "sha256", "soname"}
    if not isinstance(value, dict) or set(value) != required:
        raise TransactionError(f"{field} must contain exactly {sorted(required)}")
    path = _path(value["path"], f"{field}.path")
    kind = value["kind"]
    if not isinstance(kind, str) or kind not in FILE_KINDS:
        raise TransactionError(f"{field}.kind is invalid")
    mode = _uint(value["mode"], 0xffffffff, f"{field}.mode")
    if mode not in (0o444, 0o555) or kind == "executable" and mode != 0o555:
        raise TransactionError(f"{field}.mode is not a package-v3 canonical mode")
    soname = value["soname"]
    if kind == "library":
        soname = _soname(soname, f"{field}.soname")
    elif soname != "":
        raise TransactionError(f"{field}.soname is only valid for libraries")
    length = _uint(value["bytes"], PACKAGE.V3_MAX_FILE_BYTES,
                   f"{field}.bytes", positive=True)
    return {
        "path": path,
        "kind": kind,
        "mode": mode,
        "bytes": length,
        "sha256": _digest(value["sha256"], f"{field}.sha256"),
        "soname": soname,
    }


def _normalize_package_metadata(value: Any, field: str) -> dict[str, Any]:
    required = {
        "identifier", "version", "package_sha256", "publisher_key_id",
        "explicit", "dependencies", "files",
    }
    if not isinstance(value, dict) or set(value) != required:
        raise TransactionError(f"{field} must contain exactly {sorted(required)}")
    if type(value["explicit"]) is not bool:
        raise TransactionError(f"{field}.explicit must be boolean")
    dependencies = value["dependencies"]
    files = value["files"]
    if not isinstance(dependencies, list) \
            or len(dependencies) > PACKAGE.V3_MAX_RELATIONS:
        raise TransactionError(f"{field}.dependencies exceeds the package-v3 bound")
    if not isinstance(files, list) or not 1 <= len(files) <= PACKAGE.V3_MAX_FILES:
        raise TransactionError(f"{field}.files is outside the package-v3 bound")
    normalized_dependencies = [
        _normalize_dependency(item, f"{field}.dependencies[{index}]")
        for index, item in enumerate(dependencies)
    ]
    normalized_dependencies.sort(
        key=lambda item: (item["identifier"], item["constraint"], item["provider"]))
    if len({item["identifier"] for item in normalized_dependencies}) \
            != len(normalized_dependencies):
        raise TransactionError(f"{field}.dependencies contains a duplicate identifier")
    normalized_files = [
        _normalize_file_metadata(item, f"{field}.files[{index}]")
        for index, item in enumerate(files)
    ]
    normalized_files.sort(key=lambda item: item["path"].encode("ascii"))
    if len({item["path"] for item in normalized_files}) != len(normalized_files):
        raise TransactionError(f"{field}.files contains a duplicate path")
    if any(current["path"].startswith(previous["path"] + "/")
           for previous, current in zip(normalized_files, normalized_files[1:])):
        raise TransactionError(f"{field}.files contains an ancestor file path")
    return {
        "identifier": _identifier(value["identifier"], f"{field}.identifier"),
        "version": _version(value["version"], f"{field}.version"),
        "package_sha256": _digest(value["package_sha256"],
                                  f"{field}.package_sha256"),
        "publisher_key_id": _digest(value["publisher_key_id"],
                                    f"{field}.publisher_key_id"),
        "explicit": value["explicit"],
        "dependencies": normalized_dependencies,
        "files": normalized_files,
    }


def _normalize_packages(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list) or len(value) > DATABASE_MAX_PACKAGES:
        raise TransactionError("packages exceeds the installed-database bound")
    packages = [
        _normalize_package_metadata(item, f"packages[{index}]")
        for index, item in enumerate(value)
    ]
    packages.sort(key=lambda item: item["identifier"].encode("ascii"))
    identifiers = [item["identifier"] for item in packages]
    if len(set(identifiers)) != len(identifiers):
        raise TransactionError("installed database contains a duplicate package identifier")
    available = set(identifiers)
    edge_count = 0
    owners: dict[str, str] = {}
    for package in packages:
        edge_count += len(package["dependencies"])
        for dependency in package["dependencies"]:
            if dependency["provider"] not in available:
                raise TransactionError(
                    f"{package['identifier']} has missing provider {dependency['provider']}")
        for file_value in package["files"]:
            prior = owners.get(file_value["path"])
            if prior is not None:
                raise TransactionError(
                    f"file ownership collision at {file_value['path']}: {prior} and "
                    f"{package['identifier']}")
            owners[file_value["path"]] = package["identifier"]
    if edge_count > DATABASE_MAX_EDGES:
        raise TransactionError("dependency edge count exceeds its bound")
    if len(owners) > DATABASE_MAX_FILES:
        raise TransactionError("owned file count exceeds its bound")
    sorted_paths = sorted(owners)
    if any(current.startswith(previous + "/")
           for previous, current in zip(sorted_paths, sorted_paths[1:])):
        raise TransactionError("an owned file cannot be another file's ancestor")

    package_map = {item["identifier"]: item for item in packages}
    state: dict[str, int] = {}

    def visit(identifier: str) -> None:
        if state.get(identifier) == 1:
            raise TransactionError("installed dependency bindings contain a cycle")
        if state.get(identifier) == 2:
            return
        state[identifier] = 1
        for dependency in package_map[identifier]["dependencies"]:
            visit(dependency["provider"])
        state[identifier] = 2

    for identifier in identifiers:
        visit(identifier)
    reachable: set[str] = set()
    pending = [item["identifier"] for item in packages if item["explicit"]]
    while pending:
        identifier = pending.pop()
        if identifier in reachable:
            continue
        reachable.add(identifier)
        pending.extend(
            dependency["provider"] for dependency in package_map[identifier]["dependencies"])
    if reachable != available:
        orphan = min(available - reachable)
        raise TransactionError(f"non-explicit package is unreferenced: {orphan}")
    return packages


def _encode_database_normalized(generation: int, architecture: str, abi: int,
                                packages: list[dict[str, Any]]) -> bytes:
    package_indices = {item["identifier"]: index for index, item in enumerate(packages)}
    package_records = bytearray()
    edge_records = bytearray()
    file_rows: list[tuple[str, int, dict[str, Any]]] = []
    edge_cursor = 0
    for package_index, package in enumerate(packages):
        record = bytearray(DATABASE_PACKAGE_RECORD_BYTES)
        record[:64] = _text(package["identifier"], 64, "identifier", required=True)
        record[64:128] = _text(package["version"], 64, "version", required=True)
        record[128:160] = bytes.fromhex(package["package_sha256"])
        record[160:192] = bytes.fromhex(package["publisher_key_id"])
        flags = DATABASE_FLAG_EXPLICIT if package["explicit"] else 0
        struct.pack_into("<IIII", record, 192, flags, edge_cursor,
                         len(package["dependencies"]), len(package["files"]))
        package_records.extend(record)
        for dependency in package["dependencies"]:
            edge = bytearray(DATABASE_EDGE_RECORD_BYTES)
            edge[:64] = _text(dependency["identifier"], 64,
                              "dependency identifier", required=True)
            edge[64:120] = _text(dependency["constraint"], 56,
                                 "dependency constraint", required=True)
            edge[120:184] = _text(dependency["provider"], 64,
                                  "dependency provider", required=True)
            edge_records.extend(edge)
            edge_cursor += 1
        for file_value in package["files"]:
            file_rows.append((file_value["path"], package_index, file_value))
    file_rows.sort(key=lambda item: item[0].encode("ascii"))
    file_records = bytearray()
    for path, owner, file_value in file_rows:
        record = bytearray(DATABASE_FILE_RECORD_BYTES)
        record[:128] = _text(path, 128, "file path", required=True)
        struct.pack_into("<IHHIIQ", record, 128, owner, FILE_KINDS[file_value["kind"]],
                         0, file_value["mode"], 0, file_value["bytes"])
        record[152:184] = bytes.fromhex(file_value["sha256"])
        record[184:248] = _text(file_value["soname"], 64, "soname", required=False)
        file_records.extend(record)

    package_offset = DATABASE_HEADER_BYTES
    edge_offset = package_offset + len(package_records)
    file_offset = edge_offset + len(edge_records)
    body = bytes(package_records + edge_records + file_records)
    total = DATABASE_HEADER_BYTES + len(body)
    if total > DATABASE_MAX_BYTES:
        raise TransactionError("installed database exceeds its 32 MiB bound")
    header = bytearray(DATABASE_HEADER_BYTES)
    header[:8] = DATABASE_MAGIC
    struct.pack_into("<HHIQQII", header, 8, 1, DATABASE_HEADER_BYTES, 0, total,
                     generation, abi, 0)
    header[40:56] = _text(architecture, 16, "architecture", required=True)
    struct.pack_into("<QIIQIIQII", header, 56,
                     package_offset, len(packages), DATABASE_PACKAGE_RECORD_BYTES,
                     edge_offset, edge_cursor, DATABASE_EDGE_RECORD_BYTES,
                     file_offset, len(file_rows), DATABASE_FILE_RECORD_BYTES)
    header[104:136] = hashlib.sha256(body).digest()
    return bytes(header) + body


def encode_database(*, generation: int, architecture: str, abi: int,
                    packages: list[dict[str, Any]]) -> bytes:
    """Encode one canonical installed-database generation."""
    generation = _uint(generation, 0xffffffffffffffff, "generation", positive=True)
    architecture = _architecture(architecture)
    abi = _uint(abi, 0xffffffff, "abi", positive=True)
    normalized = _normalize_packages(packages)
    return _encode_database_normalized(generation, architecture, abi, normalized)


def parse_database(data: bytes) -> dict[str, Any]:
    """Strictly parse a canonical installed-database-v1 byte string."""
    if not isinstance(data, bytes) or not DATABASE_HEADER_BYTES <= len(data) <= DATABASE_MAX_BYTES \
            or data[:8] != DATABASE_MAGIC:
        raise TransactionError("installed database is truncated, oversized, or has invalid magic")
    version, header_bytes, flags, total, generation, abi, reserved = struct.unpack_from(
        "<HHIQQII", data, 8)
    if (version, header_bytes, flags, total, reserved) != (
            1, DATABASE_HEADER_BYTES, 0, len(data), 0):
        raise TransactionError("installed database header is not canonical")
    architecture = _decode_text(data[40:56], "architecture")
    _architecture(architecture)
    generation = _uint(generation, 0xffffffffffffffff, "generation", positive=True)
    abi = _uint(abi, 0xffffffff, "abi", positive=True)
    (package_offset, package_count, package_bytes,
     edge_offset, edge_count, edge_bytes,
     file_offset, file_count, file_bytes) = struct.unpack_from("<QIIQIIQII", data, 56)
    if package_count > DATABASE_MAX_PACKAGES or edge_count > DATABASE_MAX_EDGES \
            or file_count > DATABASE_MAX_FILES:
        raise TransactionError("installed database table count exceeds its bound")
    expected_edge = DATABASE_HEADER_BYTES + package_count * DATABASE_PACKAGE_RECORD_BYTES
    expected_file = expected_edge + edge_count * DATABASE_EDGE_RECORD_BYTES
    expected_total = expected_file + file_count * DATABASE_FILE_RECORD_BYTES
    if (package_offset, package_bytes, edge_offset, edge_bytes, file_offset, file_bytes,
            expected_total) != (
                DATABASE_HEADER_BYTES, DATABASE_PACKAGE_RECORD_BYTES,
                expected_edge, DATABASE_EDGE_RECORD_BYTES,
                expected_file, DATABASE_FILE_RECORD_BYTES, len(data)):
        raise TransactionError("installed database tables are not canonically contiguous")
    if any(data[136:DATABASE_HEADER_BYTES]):
        raise TransactionError("installed database reserved bytes are nonzero")
    if data[104:136] != hashlib.sha256(data[DATABASE_HEADER_BYTES:]).digest():
        raise TransactionError("installed database content SHA-256 mismatch")

    packages: list[dict[str, Any]] = []
    edge_cursor = 0
    owned_counts: list[int] = []
    previous_identifier: bytes | None = None
    for index in range(package_count):
        start = package_offset + index * DATABASE_PACKAGE_RECORD_BYTES
        record = data[start:start + DATABASE_PACKAGE_RECORD_BYTES]
        identifier = _decode_text(record[:64], f"packages[{index}].identifier")
        version_value = _decode_text(record[64:128], f"packages[{index}].version")
        if _identifier(identifier, "package identifier") != identifier \
                or _version(version_value, "package version") != version_value:
            raise TransactionError("installed package identity or version is not canonical")
        identifier_bytes = identifier.encode("ascii")
        if previous_identifier is not None and identifier_bytes <= previous_identifier:
            raise TransactionError("installed packages are not uniquely sorted")
        previous_identifier = identifier_bytes
        package_sha = record[128:160].hex().upper()
        publisher_key = record[160:192].hex().upper()
        flags_value, edge_start, package_edges, owned_files = struct.unpack_from(
            "<IIII", record, 192)
        if flags_value & ~DATABASE_FLAG_EXPLICIT or edge_start != edge_cursor \
                or package_edges > edge_count - edge_cursor or any(record[208:]):
            raise TransactionError(f"packages[{index}] metadata is invalid")
        dependencies: list[dict[str, str]] = []
        previous_dependency: tuple[str, str, str] | None = None
        seen_dependencies: set[str] = set()
        for dependency_index in range(package_edges):
            edge_start_bytes = edge_offset + edge_cursor * DATABASE_EDGE_RECORD_BYTES
            edge = data[edge_start_bytes:edge_start_bytes + DATABASE_EDGE_RECORD_BYTES]
            dependency = {
                "identifier": _decode_text(edge[:64], "dependency identifier"),
                "constraint": _decode_text(edge[64:120], "dependency constraint"),
                "provider": _decode_text(edge[120:184], "dependency provider"),
            }
            dependency = _normalize_dependency(
                dependency, f"packages[{index}].dependencies[{dependency_index}]")
            key = (dependency["identifier"], dependency["constraint"], dependency["provider"])
            if previous_dependency is not None and key <= previous_dependency \
                    or dependency["identifier"] in seen_dependencies or any(edge[184:]):
                raise TransactionError("dependency records are not canonical")
            previous_dependency = key
            seen_dependencies.add(dependency["identifier"])
            dependencies.append(dependency)
            edge_cursor += 1
        owned_counts.append(owned_files)
        packages.append({
            "identifier": identifier,
            "version": version_value,
            "package_sha256": package_sha,
            "publisher_key_id": publisher_key,
            "explicit": bool(flags_value & DATABASE_FLAG_EXPLICIT),
            "dependencies": dependencies,
            "files": [],
        })
    if edge_cursor != edge_count:
        raise TransactionError("installed database has unowned dependency records")

    previous_path: bytes | None = None
    for index in range(file_count):
        start = file_offset + index * DATABASE_FILE_RECORD_BYTES
        record = data[start:start + DATABASE_FILE_RECORD_BYTES]
        path = _decode_text(record[:128], f"files[{index}].path")
        if _path(path, "owned file path") != path:
            raise TransactionError("owned file path is not canonical")
        path_bytes = path.encode("ascii")
        if previous_path is not None and (path_bytes <= previous_path or
                path_bytes.startswith(previous_path + b"/")):
            raise TransactionError("owned files are not uniquely sorted")
        previous_path = path_bytes
        owner, kind_value, file_flags, mode, file_reserved, length = struct.unpack_from(
            "<IHHIIQ", record, 128)
        if owner >= package_count or kind_value not in FILE_KIND_NAMES or file_flags != 0 \
                or file_reserved != 0 or mode not in (0o444, 0o555) \
                or kind_value == FILE_KINDS["executable"] and mode != 0o555 \
                or not 1 <= length <= PACKAGE.V3_MAX_FILE_BYTES or any(record[248:]):
            raise TransactionError(f"files[{index}] metadata is invalid")
        soname_value = _decode_text(record[184:248], f"files[{index}].soname")
        if kind_value == FILE_KINDS["library"]:
            if _soname(soname_value, f"files[{index}].soname") != soname_value:
                raise TransactionError(f"files[{index}] SONAME is not canonical")
        elif soname_value:
            raise TransactionError(f"files[{index}] non-library SONAME is nonempty")
        packages[owner]["files"].append({
            "path": path,
            "kind": FILE_KIND_NAMES[kind_value],
            "mode": mode,
            "bytes": length,
            "sha256": record[152:184].hex().upper(),
            "soname": soname_value,
        })
    if [len(item["files"]) for item in packages] != owned_counts:
        raise TransactionError("installed package owned-file counts do not match")
    normalized = _normalize_packages(packages)
    canonical = _encode_database_normalized(generation, architecture, abi, normalized)
    if canonical != data:
        raise TransactionError("installed database has a noncanonical representation")
    return {
        "format": 1,
        "generation": generation,
        "architecture": architecture,
        "abi": abi,
        "packages": normalized,
        "database_sha256": hashlib.sha256(data).hexdigest().upper(),
    }


def encode_authority(database: bytes) -> bytes:
    """Encode the atomic authoritative-generation selector for one database."""
    report = parse_database(database)
    record = bytearray(AUTHORITY_BYTES)
    record[:8] = AUTHORITY_MAGIC
    struct.pack_into("<HHIQQ", record, 8, 1, AUTHORITY_BYTES, 0,
                     report["generation"], len(database))
    record[32:64] = hashlib.sha256(database).digest()
    record[64:96] = hashlib.sha256(record[:64]).digest()
    return bytes(record)


def parse_authority(data: bytes) -> dict[str, Any]:
    if not isinstance(data, bytes) or len(data) != AUTHORITY_BYTES \
            or data[:8] != AUTHORITY_MAGIC:
        raise TransactionError("generation authority has invalid length or magic")
    version, size, flags, generation, database_bytes = struct.unpack_from("<HHIQQ", data, 8)
    if (version, size, flags) != (1, AUTHORITY_BYTES, 0) or generation == 0 \
            or database_bytes < DATABASE_HEADER_BYTES or database_bytes > DATABASE_MAX_BYTES \
            or any(data[96:]):
        raise TransactionError("generation authority metadata or reserved bytes are invalid")
    if data[64:96] != hashlib.sha256(data[:64]).digest():
        raise TransactionError("generation authority SHA-256 mismatch")
    return {
        "format": 1,
        "generation": generation,
        "database_bytes": database_bytes,
        "database_sha256": data[32:64].hex().upper(),
    }


def encode_journal(*, operation: str, base_database: bytes, target_database: bytes,
                   required_space: int, target_identifier: str = "") -> bytes:
    """Encode one immutable prepared-transaction journal."""
    if operation not in JOURNAL_OPERATIONS:
        raise TransactionError("journal operation is invalid")
    base = parse_database(base_database)
    target = parse_database(target_database)
    if target["generation"] != base["generation"] + 1:
        raise TransactionError("journal target generation must immediately follow its base")
    required_space = _uint(required_space, 0xffffffffffffffff,
                           "required_space", positive=True)
    if target_identifier:
        target_identifier = _identifier(target_identifier, "target_identifier")
    journal = bytearray(JOURNAL_BYTES)
    journal[:8] = JOURNAL_MAGIC
    struct.pack_into("<HHIHHIQQQQQ", journal, 8, 1, JOURNAL_BYTES, 0,
                     JOURNAL_OPERATIONS[operation], JOURNAL_PHASE_PREPARED, 0,
                     base["generation"], target["generation"], required_space,
                     len(base_database), len(target_database))
    journal[64:96] = hashlib.sha256(base_database).digest()
    journal[96:128] = hashlib.sha256(target_database).digest()
    journal[160:224] = _text(target_identifier, 64, "target_identifier", required=False)
    journal[128:160] = hashlib.sha256(journal).digest()
    return bytes(journal)


def parse_journal(data: bytes) -> dict[str, Any]:
    if not isinstance(data, bytes) or len(data) != JOURNAL_BYTES \
            or data[:8] != JOURNAL_MAGIC:
        raise TransactionError("transaction journal has invalid length or magic")
    (version, size, flags, operation, phase, reserved, base_generation,
     target_generation, required_space, base_bytes, target_bytes) = struct.unpack_from(
        "<HHIHHIQQQQQ", data, 8)
    if (version, size, flags, phase, reserved) != (
            1, JOURNAL_BYTES, 0, JOURNAL_PHASE_PREPARED, 0) \
            or operation not in JOURNAL_OPERATION_NAMES \
            or base_generation == 0 \
            or target_generation != base_generation + 1 \
            or required_space == 0 \
            or not DATABASE_HEADER_BYTES <= base_bytes <= DATABASE_MAX_BYTES \
            or not DATABASE_HEADER_BYTES <= target_bytes <= DATABASE_MAX_BYTES \
            or any(data[224:]):
        raise TransactionError("transaction journal metadata or reserved bytes are invalid")
    transaction_id = data[128:160]
    canonical = bytearray(data)
    canonical[128:160] = bytes(32)
    if transaction_id != hashlib.sha256(canonical).digest():
        raise TransactionError("transaction journal identifier mismatch")
    target_identifier = _decode_text(data[160:224], "target_identifier")
    if target_identifier and _identifier(target_identifier, "target_identifier") \
            != target_identifier:
        raise TransactionError("journal target identifier is not canonical")
    return {
        "format": 1,
        "operation": JOURNAL_OPERATION_NAMES[operation],
        "phase": "prepared",
        "base_generation": base_generation,
        "target_generation": target_generation,
        "required_space": required_space,
        "base_database_bytes": base_bytes,
        "target_database_bytes": target_bytes,
        "base_database_sha256": data[64:96].hex().upper(),
        "target_database_sha256": data[96:128].hex().upper(),
        "transaction_id": transaction_id.hex().upper(),
        "target_identifier": target_identifier,
    }


class Snapshot:
    """One immutable reference generation used by the host model."""

    def __init__(self, database: bytes, files: dict[str, bytes]):
        self.database = bytes(database)
        self.files = {path: bytes(payload) for path, payload in files.items()}


class Store:
    """In-memory crash-state model; not a filesystem installer."""

    def __init__(self, *, capacity_bytes: int, authority: bytes,
                 generations: dict[int, Snapshot], user_data: dict[str, bytes]):
        self.capacity_bytes = capacity_bytes
        self.authority = authority
        self.generations = generations
        self.user_data = {path: bytes(payload) for path, payload in user_data.items()}
        self.journal: bytes | None = None
        self.staged: tuple[int, Snapshot] | None = None


def _snapshot_bytes(snapshot: Snapshot) -> int:
    return len(snapshot.database) + sum(len(value) for value in snapshot.files.values())


def used_space(store: Store) -> int:
    result = len(store.authority) + sum(
        _snapshot_bytes(snapshot) for snapshot in store.generations.values())
    if store.staged is not None:
        result += _snapshot_bytes(store.staged[1])
    if store.journal is not None:
        result += len(store.journal)
    return result


def create_store(*, capacity_bytes: int, architecture: str = "x86_64", abi: int = 1,
                 user_data: dict[str, bytes] | None = None) -> Store:
    """Create an empty generation-one reference store."""
    capacity_bytes = _uint(capacity_bytes, 0xffffffffffffffff,
                           "capacity_bytes", positive=True)
    database = encode_database(generation=1, architecture=architecture, abi=abi, packages=[])
    snapshot = Snapshot(database, {})
    normalized_user_data: dict[str, bytes] = {}
    for path, payload in (user_data or {}).items():
        normalized_path = _path(path, "user data path")
        if normalized_path in normalized_user_data or not isinstance(payload, bytes):
            raise TransactionError("user data paths must be unique and payloads must be bytes")
        normalized_user_data[normalized_path] = bytes(payload)
    store = Store(capacity_bytes=capacity_bytes, authority=encode_authority(database),
                  generations={1: snapshot}, user_data=normalized_user_data)
    if used_space(store) > capacity_bytes:
        raise TransactionError("capacity is too small for the initial authoritative state")
    return store


def _authoritative_snapshot(store: Store) -> tuple[dict[str, Any], Snapshot]:
    authority = parse_authority(store.authority)
    snapshot = store.generations.get(authority["generation"])
    if snapshot is None or len(snapshot.database) != authority["database_bytes"] \
            or hashlib.sha256(snapshot.database).hexdigest().upper() \
            != authority["database_sha256"]:
        raise TransactionError("authoritative generation database is absent or mismatched")
    database = parse_database(snapshot.database)
    if database["generation"] != authority["generation"]:
        raise TransactionError("authority and database generation disagree")
    return database, snapshot


def _expected_files(database: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        file_value["path"]: file_value
        for package in database["packages"] for file_value in package["files"]
    }


def verify_snapshot(snapshot: Snapshot) -> list[str]:
    """Return deterministic integrity findings for one immutable generation."""
    try:
        database = parse_database(snapshot.database)
    except TransactionError as error:
        return [f"database:{error}"]
    expected = _expected_files(database)
    issues: list[str] = []
    for path in sorted(expected):
        payload = snapshot.files.get(path)
        metadata = expected[path]
        if payload is None:
            issues.append(f"missing:{path}")
        elif len(payload) != metadata["bytes"]:
            issues.append(f"size:{path}")
        elif hashlib.sha256(payload).hexdigest().upper() != metadata["sha256"]:
            issues.append(f"digest:{path}")
    for path in sorted(set(snapshot.files) - set(expected)):
        issues.append(f"unowned:{path}")
    return issues


def verify(store: Store) -> list[str]:
    """Verify the selected database and all immutable owned files."""
    try:
        _, snapshot = _authoritative_snapshot(store)
    except TransactionError as error:
        return [f"authority:{error}"]
    return verify_snapshot(snapshot)


def installed(store: Store) -> dict[str, Any]:
    """Return the parsed authoritative installed database."""
    database, _ = _authoritative_snapshot(store)
    return database


def _normalize_candidate(value: Any, field: str,
                         old_explicit: bool | None) -> tuple[dict[str, Any], dict[str, bytes]]:
    required = {
        "identifier", "version", "package_sha256", "publisher_key_id",
        "dependencies", "files",
    }
    allowed = required | {"explicit"}
    if not isinstance(value, dict) or set(value) - allowed or not required <= set(value):
        raise TransactionError(f"{field} has missing or unknown package-v3 transaction fields")
    explicit = value.get("explicit", old_explicit if old_explicit is not None else False)
    if type(explicit) is not bool:
        raise TransactionError(f"{field}.explicit must be boolean")
    raw_files = value["files"]
    if not isinstance(raw_files, list) or not raw_files \
            or len(raw_files) > PACKAGE.V3_MAX_FILES:
        raise TransactionError(f"{field}.files must contain 1-{PACKAGE.V3_MAX_FILES} entries")
    metadata_files: list[dict[str, Any]] = []
    payloads: dict[str, bytes] = {}
    for index, file_value in enumerate(raw_files):
        file_field = f"{field}.files[{index}]"
        required_file = {"path", "kind", "mode", "payload", "soname"}
        allowed_file = required_file | {"bytes", "sha256"}
        if not isinstance(file_value, dict) or set(file_value) - allowed_file \
                or not required_file <= set(file_value):
            raise TransactionError(f"{file_field} has missing or unknown fields")
        payload = file_value["payload"]
        if not isinstance(payload, bytes) or not payload \
                or len(payload) > PACKAGE.V3_MAX_FILE_BYTES:
            raise TransactionError(f"{file_field}.payload is empty or exceeds its bound")
        metadata = _normalize_file_metadata({
            "path": file_value["path"],
            "kind": file_value["kind"],
            "mode": file_value["mode"],
            "bytes": file_value.get("bytes", len(payload)),
            "sha256": file_value.get(
                "sha256", hashlib.sha256(payload).hexdigest().upper()),
            "soname": file_value["soname"],
        }, file_field)
        if metadata["bytes"] != len(payload) \
                or metadata["sha256"] != hashlib.sha256(payload).hexdigest().upper():
            raise TransactionError(f"{file_field} payload metadata mismatch")
        if metadata["path"] in payloads:
            raise TransactionError(f"{field}.files contains a duplicate path")
        payloads[metadata["path"]] = payload
        metadata_files.append(metadata)
    metadata = _normalize_package_metadata({
        "identifier": value["identifier"],
        "version": value["version"],
        "package_sha256": value["package_sha256"],
        "publisher_key_id": value["publisher_key_id"],
        "explicit": explicit,
        "dependencies": value["dependencies"],
        "files": metadata_files,
    }, field)
    return metadata, payloads


def _stage(store: Store, *, operation: str, target_identifier: str,
           packages: list[dict[str, Any]], files: dict[str, bytes],
           allow_current_damage: bool = False) -> dict[str, Any]:
    if store.journal is not None or store.staged is not None:
        raise TransactionError("another transaction is already prepared")
    base, current = _authoritative_snapshot(store)
    if not allow_current_damage:
        issues = verify_snapshot(current)
        if issues:
            raise TransactionError(f"current generation is damaged: {issues[0]}")
    target_database = encode_database(
        generation=base["generation"] + 1,
        architecture=base["architecture"], abi=base["abi"], packages=packages)
    target_snapshot = Snapshot(target_database, files)
    issues = verify_snapshot(target_snapshot)
    if issues:
        raise TransactionError(f"staged generation is incomplete: {issues[0]}")
    required_space = _snapshot_bytes(target_snapshot) + JOURNAL_BYTES + AUTHORITY_BYTES
    journal = encode_journal(
        operation=operation, base_database=current.database,
        target_database=target_database, required_space=required_space,
        target_identifier=target_identifier)
    if used_space(store) + required_space > store.capacity_bytes:
        raise TransactionError("insufficient space for a complete recoverable generation")
    store.staged = (base["generation"] + 1, target_snapshot)
    store.journal = journal
    return parse_journal(journal)


def stage_install(store: Store, candidates: list[dict[str, Any]]) -> dict[str, Any]:
    """Stage an authenticated package-v3/repository plan into a new generation."""
    if not isinstance(candidates, list) or not candidates:
        raise TransactionError("install candidates must be a nonempty list")
    current, snapshot = _authoritative_snapshot(store)
    current_issues = verify_snapshot(snapshot)
    if current_issues:
        raise TransactionError(f"current generation is damaged: {current_issues[0]}")
    current_map = {item["identifier"]: item for item in current["packages"]}
    candidate_metadata: dict[str, dict[str, Any]] = {}
    candidate_payloads: dict[str, bytes] = {}
    replacing: set[str] = set()
    for index, candidate in enumerate(candidates):
        candidate_id = _identifier(
            candidate.get("identifier") if isinstance(candidate, dict) else None,
            f"candidates[{index}].identifier")
        if candidate_id in candidate_metadata:
            raise TransactionError("install candidates contain a duplicate identifier")
        old = current_map.get(candidate_id)
        metadata, payloads = _normalize_candidate(
            candidate, f"candidates[{index}]", old["explicit"] if old else None)
        candidate_metadata[candidate_id] = metadata
        candidate_payloads.update(payloads)
        if old is not None:
            replacing.add(candidate_id)
    packages = [item for item in current["packages"] if item["identifier"] not in replacing]
    packages.extend(candidate_metadata.values())
    packages = _normalize_packages(packages)
    files = dict(snapshot.files)
    replaced_paths = {
        file_value["path"] for identifier in replacing
        for file_value in current_map[identifier]["files"]
    }
    files = {path: payload for path, payload in files.items() if path not in replaced_paths}
    for path, payload in candidate_payloads.items():
        if path in files:
            owner = next(
                item["identifier"] for item in current["packages"]
                if any(file_value["path"] == path for file_value in item["files"]))
            raise TransactionError(f"file ownership collision at {path}: {owner} and candidate")
        files[path] = payload
    explicit_candidates = sorted(
        item["identifier"] for item in candidate_metadata.values() if item["explicit"])
    target_identifier = explicit_candidates[0] if len(explicit_candidates) == 1 else ""
    operation = "update" if replacing else "install"
    return _stage(store, operation=operation, target_identifier=target_identifier,
                  packages=packages, files=files)


def stage_remove(store: Store, identifiers: list[str]) -> dict[str, Any]:
    """Stage root removal and deterministic unreachable-dependency collection."""
    if not isinstance(identifiers, list) or not identifiers:
        raise TransactionError("remove identifiers must be a nonempty list")
    requested = sorted({_identifier(value, "remove identifier") for value in identifiers})
    if len(requested) != len(identifiers):
        raise TransactionError("remove identifiers contain duplicates")
    current, snapshot = _authoritative_snapshot(store)
    package_map = {item["identifier"]: item for item in current["packages"]}
    missing = [value for value in requested if value not in package_map]
    if missing:
        raise TransactionError(f"package is not installed: {missing[0]}")
    roots = sorted(
        item["identifier"] for item in current["packages"]
        if item["explicit"] and item["identifier"] not in requested)
    keep: set[str] = set()
    pending = list(reversed(roots))
    while pending:
        identifier = pending.pop()
        if identifier in keep:
            continue
        keep.add(identifier)
        dependencies = package_map[identifier]["dependencies"]
        for dependency in reversed(dependencies):
            pending.append(dependency["provider"])
    if any(value in keep for value in requested):
        raise TransactionError("requested package is still required by an installed root")
    packages = [item for item in current["packages"] if item["identifier"] in keep]
    kept_paths = {
        file_value["path"] for item in packages for file_value in item["files"]
    }
    files = {path: payload for path, payload in snapshot.files.items() if path in kept_paths}
    target_identifier = requested[0] if len(requested) == 1 else ""
    return _stage(store, operation="remove", target_identifier=target_identifier,
                  packages=packages, files=files)


def stage_repair(store: Store, replacements: dict[str, bytes]) -> dict[str, Any]:
    """Stage verified replacements for damaged immutable owned files."""
    if not isinstance(replacements, dict):
        raise TransactionError("repair replacements must be a path-to-bytes mapping")
    current, snapshot = _authoritative_snapshot(store)
    expected = _expected_files(current)
    normalized_replacements: dict[str, bytes] = {}
    for path, payload in replacements.items():
        path = _path(path, "repair path")
        if path not in expected or not isinstance(payload, bytes):
            raise TransactionError("repair replacement is unowned or is not bytes")
        normalized_replacements[path] = bytes(payload)
    files: dict[str, bytes] = {}
    for path, metadata in expected.items():
        payload = snapshot.files.get(path)
        valid = payload is not None and len(payload) == metadata["bytes"] \
            and hashlib.sha256(payload).hexdigest().upper() == metadata["sha256"]
        if not valid:
            payload = normalized_replacements.get(path)
        if payload is None or len(payload) != metadata["bytes"] \
                or hashlib.sha256(payload).hexdigest().upper() != metadata["sha256"]:
            raise TransactionError(f"no authenticated repair payload for {path}")
        files[path] = payload
    return _stage(store, operation="repair", target_identifier="",
                  packages=current["packages"], files=files,
                  allow_current_damage=True)


def _snapshot_matches(snapshot: Snapshot | None, *, generation: int,
                      database_bytes: int, database_sha256: str) -> bool:
    if snapshot is None or len(snapshot.database) != database_bytes \
            or hashlib.sha256(snapshot.database).hexdigest().upper() != database_sha256:
        return False
    try:
        database = parse_database(snapshot.database)
    except TransactionError:
        return False
    return database["generation"] == generation and not verify_snapshot(snapshot)


def commit(store: Store, *, interrupt: str | None = None) -> str:
    """Advance the reference commit through its one atomic authority switch.

    `interrupt` is a host-test fault point: `before-authority` leaves a complete
    staged generation selected by neither authority nor recovery; `after-authority`
    leaves the new complete generation authoritative while the journal remains.
    """
    if interrupt not in (None, "before-authority", "after-authority"):
        raise TransactionError("unknown commit interruption point")
    if store.journal is None or store.staged is None:
        raise TransactionError("no fully staged transaction is available")
    journal = parse_journal(store.journal)
    target_generation, target_snapshot = store.staged
    if target_generation != journal["target_generation"] or not _snapshot_matches(
            target_snapshot, generation=target_generation,
            database_bytes=journal["target_database_bytes"],
            database_sha256=journal["target_database_sha256"]):
        raise TransactionError("staged generation does not match its journal")
    if interrupt == "before-authority":
        return "interrupted-before-authority"
    store.generations[target_generation] = target_snapshot
    store.staged = None
    store.authority = encode_authority(target_snapshot.database)
    if interrupt == "after-authority":
        return "interrupted-after-authority"
    store.generations = {target_generation: target_snapshot}
    store.journal = None
    return "committed"


def recover(store: Store) -> str:
    """Choose an old-or-new complete state and discard every other transaction byte."""
    if store.journal is None:
        database, snapshot = _authoritative_snapshot(store)
        if verify_snapshot(snapshot):
            raise TransactionError("authoritative generation is incomplete")
        store.generations = {database["generation"]: snapshot}
        store.staged = None
        return "authoritative"
    journal = parse_journal(store.journal)
    old_snapshot = store.generations.get(journal["base_generation"])
    new_snapshot = store.generations.get(journal["target_generation"])
    if new_snapshot is None and store.staged is not None \
            and store.staged[0] == journal["target_generation"]:
        new_snapshot = store.staged[1]
    old_complete = _snapshot_matches(
        old_snapshot, generation=journal["base_generation"],
        database_bytes=journal["base_database_bytes"],
        database_sha256=journal["base_database_sha256"])
    new_complete = _snapshot_matches(
        new_snapshot, generation=journal["target_generation"],
        database_bytes=journal["target_database_bytes"],
        database_sha256=journal["target_database_sha256"])
    try:
        selected = parse_authority(store.authority)["generation"]
    except TransactionError:
        selected = 0
    if selected == journal["target_generation"] and new_complete:
        chosen_generation = journal["target_generation"]
        chosen_snapshot = new_snapshot
        result = "new"
    elif old_complete:
        chosen_generation = journal["base_generation"]
        chosen_snapshot = old_snapshot
        result = "old"
    else:
        raise TransactionError("neither journal generation is complete")
    assert chosen_snapshot is not None
    store.generations = {chosen_generation: chosen_snapshot}
    store.authority = encode_authority(chosen_snapshot.database)
    store.staged = None
    store.journal = None
    return result


def cancel(store: Store) -> None:
    """Cancel only a transaction whose old authority is still selected."""
    if store.journal is None:
        raise TransactionError("no transaction is prepared")
    journal = parse_journal(store.journal)
    authority = parse_authority(store.authority)
    if authority["generation"] != journal["base_generation"]:
        raise TransactionError("transaction passed its commit point; recovery is required")
    store.generations.pop(journal["target_generation"], None)
    store.staged = None
    store.journal = None


def _read_bounded(path: str, maximum: int, field: str) -> bytes:
    try:
        return PACKAGE.read_regular_bounded(Path(path), maximum, field)
    except PACKAGE.PackageError as error:
        raise TransactionError(str(error)) from error


def command_build_database(args: argparse.Namespace) -> None:
    spec = json.loads(_read_bounded(args.spec, 4 * 1024 * 1024,
                                    "installed database specification").decode("utf-8"))
    if not isinstance(spec, dict) or set(spec) != {
            "generation", "architecture", "abi", "packages"}:
        raise TransactionError("database specification has missing or unknown fields")
    database = encode_database(**spec)
    PACKAGE.atomic_write(Path(args.output), database)
    print(json.dumps(parse_database(database), sort_keys=True, indent=2))


def command_inspect_database(args: argparse.Namespace) -> None:
    print(json.dumps(parse_database(_read_bounded(
        args.database, DATABASE_MAX_BYTES, "installed database")),
        sort_keys=True, indent=2))


def command_inspect_authority(args: argparse.Namespace) -> None:
    print(json.dumps(parse_authority(_read_bounded(
        args.authority, AUTHORITY_BYTES, "generation authority")),
        sort_keys=True, indent=2))


def command_inspect_journal(args: argparse.Namespace) -> None:
    print(json.dumps(parse_journal(_read_bounded(
        args.journal, JOURNAL_BYTES, "transaction journal")),
        sort_keys=True, indent=2))


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    builder = commands.add_parser("build-database")
    builder.add_argument("--spec", required=True)
    builder.add_argument("--output", required=True)
    builder.set_defaults(function=command_build_database)
    inspector = commands.add_parser("inspect-database")
    inspector.add_argument("database")
    inspector.set_defaults(function=command_inspect_database)
    authority = commands.add_parser("inspect-authority")
    authority.add_argument("authority")
    authority.set_defaults(function=command_inspect_authority)
    journal = commands.add_parser("inspect-journal")
    journal.add_argument("journal")
    journal.set_defaults(function=command_inspect_journal)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        args.function(args)
    except (TransactionError, OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        print(f"Phipia transaction state refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
