#!/usr/bin/env python3
"""Build, verify, and resolve deterministic signed Phipia repository indexes."""

from __future__ import annotations

import argparse
from functools import cmp_to_key
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import time
from typing import Any


PACKAGE_TOOL_PATH = Path(__file__).with_name("phipia-package.py")
PACKAGE_TOOL_SPEC = importlib.util.spec_from_file_location(
    "phipia_repository_package_tool", PACKAGE_TOOL_PATH)
if PACKAGE_TOOL_SPEC is None or PACKAGE_TOOL_SPEC.loader is None:
    raise RuntimeError("could not load Phipia package tool")
PACKAGE = importlib.util.module_from_spec(PACKAGE_TOOL_SPEC)
PACKAGE_TOOL_SPEC.loader.exec_module(PACKAGE)

RepositoryError = PACKAGE.PackageError

INDEX_MAGIC = b"PHIPIDX1"
INDEX_HEADER_BYTES = 512
INDEX_PACKAGE_RECORD_BYTES = 512
INDEX_RELATION_RECORD_BYTES = 128
INDEX_SIGNATURE_OFFSET = 232
INDEX_SIGNATURE_BYTES = 64
INDEX_SIGNATURE_ALGORITHM_ED25519 = 1
INDEX_MAX_BYTES = 32 * 1024 * 1024
INDEX_MAX_PACKAGES = 1024
INDEX_MAX_RELATIONS_PER_PACKAGE = 64
INDEX_MAX_GRAPH_PACKAGES = 256


def _uint(value: Any, maximum: int, field: str, *, positive: bool = False) -> int:
    minimum = 1 if positive else 0
    if type(value) is not int or not minimum <= value <= maximum:
        raise RepositoryError(f"{field} must be an integer from {minimum} through {maximum}")
    return value


def _digest(value: Any, field: str) -> bytes:
    if not isinstance(value, str) or len(value) != 64:
        raise RepositoryError(f"{field} must be a 64-digit SHA-256 value")
    try:
        result = bytes.fromhex(value)
    except ValueError as error:
        raise RepositoryError(f"{field} must be hexadecimal") from error
    if len(result) != 32:
        raise RepositoryError(f"{field} must contain exactly 32 bytes")
    return result


def _semver_parts(value: str) -> tuple[tuple[int, int, int], tuple[str, ...] | None]:
    PACKAGE.semantic_version(value, "version")
    without_build = value.split("+", 1)[0]
    core, separator, prerelease = without_build.partition("-")
    numbers = tuple(int(item) for item in core.split("."))
    return (numbers[0], numbers[1], numbers[2]), (
        tuple(prerelease.split(".")) if separator else None)


def semver_compare(left: str, right: str) -> int:
    left_core, left_pre = _semver_parts(left)
    right_core, right_pre = _semver_parts(right)
    if left_core != right_core:
        return -1 if left_core < right_core else 1
    if left_pre is None or right_pre is None:
        if left_pre is right_pre:
            return 0
        return 1 if left_pre is None else -1
    for left_item, right_item in zip(left_pre, right_pre):
        if left_item == right_item:
            continue
        left_numeric = left_item.isdigit()
        right_numeric = right_item.isdigit()
        if left_numeric and right_numeric:
            return -1 if int(left_item) < int(right_item) else 1
        if left_numeric != right_numeric:
            return -1 if left_numeric else 1
        return -1 if left_item < right_item else 1
    if len(left_pre) == len(right_pre):
        return 0
    return -1 if len(left_pre) < len(right_pre) else 1


def _upper_bound(version: str, operator: str) -> str:
    (major, minor, patch), _ = _semver_parts(version)
    if operator == "~":
        return f"{major}.{minor + 1}.0"
    if major:
        return f"{major + 1}.0.0"
    if minor:
        return f"0.{minor + 1}.0"
    return f"0.0.{patch + 1}"


def version_satisfies(version: str, constraint: str) -> bool:
    canonical = PACKAGE.version_constraint(constraint, "constraint")
    if canonical == "*":
        return True
    for clause in canonical.split(","):
        for candidate in (">=", "<=", "=", ">", "<", "^", "~"):
            if clause.startswith(candidate):
                operator = candidate
                required = clause[len(candidate):]
                break
        else:
            raise RepositoryError("constraint operator is invalid")
        comparison = semver_compare(version, required)
        if operator == "=" and comparison != 0:
            return False
        if operator == ">" and comparison <= 0:
            return False
        if operator == ">=" and comparison < 0:
            return False
        if operator == "<" and comparison >= 0:
            return False
        if operator == "<=" and comparison > 0:
            return False
        if operator in ("^", "~") and not (
                comparison >= 0 and semver_compare(version, _upper_bound(required, operator)) < 0):
            return False
    return True


def _relation_records(value: Any, field: str) -> tuple[bytes, list[dict[str, str]]]:
    if not isinstance(value, list) or len(value) > INDEX_MAX_RELATIONS_PER_PACKAGE:
        raise RepositoryError(
            f"{field} must be a list of at most {INDEX_MAX_RELATIONS_PER_PACKAGE} entries")
    return PACKAGE._v3_relation_records(value, field)


def _provide_records(value: Any, field: str) -> tuple[bytes, list[dict[str, str]]]:
    if not isinstance(value, list) or len(value) > INDEX_MAX_RELATIONS_PER_PACKAGE:
        raise RepositoryError(
            f"{field} must be a list of at most {INDEX_MAX_RELATIONS_PER_PACKAGE} entries")
    normalized: list[tuple[str, str]] = []
    for index, provide in enumerate(value):
        if not isinstance(provide, dict) or set(provide) != {"identifier", "version"}:
            raise RepositoryError(
                f"{field}[{index}] must contain only identifier and version")
        normalized.append((
            PACKAGE.package_identifier(provide["identifier"],
                                       f"{field}[{index}].identifier"),
            PACKAGE.semantic_version(provide["version"],
                                     f"{field}[{index}].version")))
    normalized.sort()
    if len({identifier for identifier, _ in normalized}) != len(normalized):
        raise RepositoryError(f"{field} contains a duplicate provider identifier")
    encoded = bytearray()
    report: list[dict[str, str]] = []
    for identifier, version in normalized:
        record = bytearray(INDEX_RELATION_RECORD_BYTES)
        record[:64] = PACKAGE.text_field(identifier, 64, field, required=True)
        record[64:120] = PACKAGE.text_field(version, 56, field, required=True)
        encoded.extend(record)
        report.append({"identifier": identifier, "version": version})
    return bytes(encoded), report


def _normalize_packages(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list) or not value or len(value) > INDEX_MAX_PACKAGES:
        raise RepositoryError(f"packages must contain 1-{INDEX_MAX_PACKAGES} entries")
    result: list[dict[str, Any]] = []
    allowed = {
        "identifier", "version", "download_path", "bytes", "sha256",
        "publisher_key_id", "dependencies", "conflicts", "provides",
    }
    required = {
        "identifier", "version", "download_path", "bytes", "sha256",
        "publisher_key_id",
    }
    for index, entry in enumerate(value):
        if not isinstance(entry, dict) or not required <= set(entry) \
                or set(entry) - allowed:
            raise RepositoryError(
                f"packages[{index}] has missing or unknown package metadata")
        identifier = PACKAGE.package_identifier(
            entry["identifier"], f"packages[{index}].identifier")
        version = PACKAGE.semantic_version(
            entry["version"], f"packages[{index}].version")
        download_path = PACKAGE.package_path(
            entry["download_path"], f"packages[{index}].download_path")
        package_bytes = _uint(entry["bytes"], PACKAGE.V3_MAX_PACKAGE_BYTES,
                              f"packages[{index}].bytes", positive=True)
        digest = _digest(entry["sha256"], f"packages[{index}].sha256")
        publisher_key_id = _digest(
            entry["publisher_key_id"], f"packages[{index}].publisher_key_id")
        dependency_bytes, dependencies = _relation_records(
            entry.get("dependencies", []), f"packages[{index}].dependencies")
        conflict_bytes, conflicts = _relation_records(
            entry.get("conflicts", []), f"packages[{index}].conflicts")
        provide_bytes, provides = _provide_records(
            entry.get("provides", []), f"packages[{index}].provides")
        if ({item["identifier"] for item in dependencies} &
                {item["identifier"] for item in conflicts}):
            raise RepositoryError(
                f"packages[{index}] dependency and conflict sets overlap")
        if identifier in {item["identifier"] for item in provides}:
            raise RepositoryError(f"packages[{index}] redundantly provides its own identifier")
        result.append({
            "identifier": identifier,
            "version": version,
            "download_path": download_path,
            "bytes": package_bytes,
            "digest": digest,
            "publisher_key_id_bytes": publisher_key_id,
            "dependencies": dependencies,
            "conflicts": conflicts,
            "provides": provides,
            "dependency_bytes": dependency_bytes,
            "conflict_bytes": conflict_bytes,
            "provide_bytes": provide_bytes,
        })
    result.sort(key=lambda item: (item["identifier"].encode("ascii"),
                                  item["version"].encode("ascii")))
    identities: set[tuple[str, str]] = set()
    paths: set[str] = set()
    precedence: dict[str, list[str]] = {}
    for entry in result:
        identity = (entry["identifier"], entry["version"])
        if identity in identities:
            raise RepositoryError("repository contains a duplicate package identity/version")
        if entry["download_path"] in paths:
            raise RepositoryError("repository contains a duplicate download path")
        identities.add(identity)
        paths.add(entry["download_path"])
        versions = precedence.setdefault(entry["identifier"], [])
        if any(semver_compare(entry["version"], other) == 0 for other in versions):
            raise RepositoryError(
                "repository contains ambiguous build-only package versions")
        versions.append(entry["version"])
    return result


def build_repository(spec: dict[str, Any], signing_key: bytes) -> bytes:
    """Encode and sign one canonical repository-index-v1 snapshot."""
    allowed = {
        "format", "repository", "repository_version", "generated_at",
        "expires_at", "architecture", "abi_min", "abi_max", "root_key_id",
        "packages",
    }
    required = allowed - {"root_key_id"}
    if not required <= set(spec) or set(spec) - allowed or spec.get("format") != 1:
        raise RepositoryError("repository specification has missing or unknown fields")
    repository = PACKAGE.package_identifier(spec["repository"], "repository")
    repository_version = _uint(
        spec["repository_version"], 0xffffffffffffffff,
        "repository_version", positive=True)
    generated_at = _uint(spec["generated_at"], 0xffffffffffffffff,
                         "generated_at", positive=True)
    expires_at = _uint(spec["expires_at"], 0xffffffffffffffff,
                       "expires_at", positive=True)
    if generated_at >= expires_at:
        raise RepositoryError("repository freshness interval is empty or reversed")
    architecture = PACKAGE.canonical_printable(
        spec["architecture"], 16, "architecture")
    if architecture not in PACKAGE.V3_ARCHITECTURES:
        raise RepositoryError("repository architecture must be x86_64")
    abi_min = _uint(spec["abi_min"], 0xffffffff, "abi_min", positive=True)
    abi_max = _uint(spec["abi_max"], 0xffffffff, "abi_max", positive=True)
    if abi_min > abi_max:
        raise RepositoryError("repository ABI range is reversed")
    packages = _normalize_packages(spec["packages"])

    package_records = bytearray()
    relation_records = bytearray()
    relation_cursor = 0
    for entry in packages:
        dependency_start = relation_cursor
        dependency_count = len(entry["dependencies"])
        relation_records.extend(entry["dependency_bytes"])
        relation_cursor += dependency_count
        conflict_start = relation_cursor
        conflict_count = len(entry["conflicts"])
        relation_records.extend(entry["conflict_bytes"])
        relation_cursor += conflict_count
        provide_start = relation_cursor
        provide_count = len(entry["provides"])
        relation_records.extend(entry["provide_bytes"])
        relation_cursor += provide_count

        record = bytearray(INDEX_PACKAGE_RECORD_BYTES)
        record[:64] = PACKAGE.text_field(
            entry["identifier"], 64, "identifier", required=True)
        record[64:128] = PACKAGE.text_field(
            entry["version"], 64, "version", required=True)
        record[128:256] = PACKAGE.text_field(
            entry["download_path"], 128, "download_path", required=True)
        struct.pack_into("<Q", record, 256, entry["bytes"])
        record[264:296] = entry["digest"]
        record[296:328] = entry["publisher_key_id_bytes"]
        struct.pack_into("<IIIIII", record, 328,
                         dependency_start, dependency_count,
                         conflict_start, conflict_count,
                         provide_start, provide_count)
        package_records.extend(record)

    package_table_offset = INDEX_HEADER_BYTES
    relation_table_offset = package_table_offset + len(package_records)
    body = bytes(package_records + relation_records)
    total_bytes = INDEX_HEADER_BYTES + len(body)
    if total_bytes > INDEX_MAX_BYTES:
        raise RepositoryError("repository index exceeds its 32 MiB bound")
    public_key = PACKAGE._ed25519_public_bytes_from_private(signing_key)
    root_key_id = hashlib.sha256(public_key).digest()
    declared_key_id = spec.get("root_key_id")
    if declared_key_id is not None and _digest(declared_key_id, "root_key_id") != root_key_id:
        raise RepositoryError("root_key_id does not match the Ed25519 public key")

    header = bytearray(INDEX_HEADER_BYTES)
    header[:8] = INDEX_MAGIC
    struct.pack_into("<HHI", header, 8, 1, INDEX_HEADER_BYTES, 0)
    struct.pack_into("<QQQQII", header, 16, total_bytes, repository_version,
                     generated_at, expires_at, abi_min, abi_max)
    header[56:72] = PACKAGE.text_field(
        architecture, 16, "architecture", required=True)
    header[72:136] = PACKAGE.text_field(
        repository, 64, "repository", required=True)
    struct.pack_into("<QIIQII", header, 136,
                     package_table_offset, len(packages), INDEX_PACKAGE_RECORD_BYTES,
                     relation_table_offset, relation_cursor, INDEX_RELATION_RECORD_BYTES)
    header[168:200] = hashlib.sha256(body).digest()
    header[200:232] = root_key_id
    struct.pack_into("<HH", header, 296, INDEX_SIGNATURE_ALGORITHM_ED25519,
                     INDEX_SIGNATURE_BYTES)
    unsigned = bytes(header) + body
    signature = PACKAGE._ed25519_private(signing_key).sign(unsigned)
    if len(signature) != INDEX_SIGNATURE_BYTES:
        raise RepositoryError("Ed25519 backend returned an invalid signature length")
    header[INDEX_SIGNATURE_OFFSET:INDEX_SIGNATURE_OFFSET + INDEX_SIGNATURE_BYTES] = signature
    return bytes(header) + body


def _parse_provides(index: bytes, offset: int, count: int,
                    field: str) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    previous: tuple[str, str] | None = None
    identifiers: set[str] = set()
    for relation_index in range(count):
        start = offset + relation_index * INDEX_RELATION_RECORD_BYTES
        record = index[start:start + INDEX_RELATION_RECORD_BYTES]
        identifier = PACKAGE.decode_text(record[:64], f"{field}[{relation_index}].identifier")
        version = PACKAGE.decode_text(record[64:120], f"{field}[{relation_index}].version")
        if PACKAGE.package_identifier(identifier, field) != identifier \
                or PACKAGE.semantic_version(version, field) != version:
            raise RepositoryError(f"{field}[{relation_index}] is not canonical")
        key = (identifier, version)
        if identifier in identifiers or previous is not None and key <= previous:
            raise RepositoryError(f"{field} is duplicated or not canonically sorted")
        if any(record[120:]):
            raise RepositoryError(f"{field}[{relation_index}] reserved bytes are nonzero")
        identifiers.add(identifier)
        previous = key
        result.append({"identifier": identifier, "version": version})
    return result


def _trusted_root(root_key_id: bytes, trusted_root_keys: dict[str, bytes] | None) -> Any:
    if not trusted_root_keys:
        raise RepositoryError(
            f"repository requires immutable root key {root_key_id.hex().upper()}")
    matched = None
    for supplied_id, material in trusted_root_keys.items():
        if not isinstance(supplied_id, str) or not isinstance(material, bytes):
            raise RepositoryError("trusted roots must map hexadecimal key IDs to key bytes")
        supplied = _digest(supplied_id, "trusted root key ID")
        key, raw = PACKAGE._ed25519_public(material)
        derived = hashlib.sha256(raw).digest()
        if supplied != derived:
            raise RepositoryError("trusted root key ID does not match its public key")
        if supplied == root_key_id:
            if matched is not None:
                raise RepositoryError("duplicate trusted repository root key")
            matched = key
    if matched is None:
        raise RepositoryError(f"unknown repository root key: {root_key_id.hex().upper()}")
    return matched


def parse_repository(index: bytes, *, trusted_root_keys: dict[str, bytes] | None,
                     now: int | None = None,
                     minimum_repository_version: int = 0) -> dict[str, Any]:
    if len(index) < INDEX_HEADER_BYTES or len(index) > INDEX_MAX_BYTES \
            or index[:8] != INDEX_MAGIC:
        raise RepositoryError("repository index is truncated, oversized, or has invalid magic")
    version, header_bytes, flags = struct.unpack_from("<HHI", index, 8)
    (total_bytes, repository_version, generated_at, expires_at,
     abi_min, abi_max) = struct.unpack_from("<QQQQII", index, 16)
    package_offset, package_count, package_record_bytes = struct.unpack_from(
        "<QII", index, 136)
    relation_offset, relation_count, relation_record_bytes = struct.unpack_from(
        "<QII", index, 152)
    if version != 1 or header_bytes != INDEX_HEADER_BYTES or flags != 0 \
            or total_bytes != len(index):
        raise RepositoryError("repository header version, size, flags, or length is invalid")
    if not 1 <= package_count <= INDEX_MAX_PACKAGES:
        raise RepositoryError("repository package count exceeds its bound")
    if package_record_bytes != INDEX_PACKAGE_RECORD_BYTES \
            or relation_record_bytes != INDEX_RELATION_RECORD_BYTES:
        raise RepositoryError("repository record size is invalid")
    expected_relation_offset = INDEX_HEADER_BYTES + package_count * INDEX_PACKAGE_RECORD_BYTES
    if package_offset != INDEX_HEADER_BYTES or relation_offset != expected_relation_offset \
            or relation_count > package_count * 3 * INDEX_MAX_RELATIONS_PER_PACKAGE \
            or relation_offset + relation_count * INDEX_RELATION_RECORD_BYTES != len(index):
        raise RepositoryError("repository tables are not canonically contiguous")
    signature_algorithm, signature_bytes = struct.unpack_from("<HH", index, 296)
    if signature_algorithm != INDEX_SIGNATURE_ALGORITHM_ED25519 \
            or signature_bytes != INDEX_SIGNATURE_BYTES or any(index[300:512]):
        raise RepositoryError(
            "repository signature algorithm, length, or reserved bytes are invalid")
    minimum_repository_version = _uint(
        minimum_repository_version, 0xffffffffffffffff,
        "minimum_repository_version")
    if repository_version == 0 or generated_at == 0 or expires_at == 0:
        raise RepositoryError("repository version and freshness values must be positive")
    if not 1 <= abi_min <= abi_max <= 0xffffffff:
        raise RepositoryError("repository ABI range is invalid")
    architecture = PACKAGE.decode_text(index[56:72], "architecture")
    if architecture not in PACKAGE.V3_ARCHITECTURES:
        raise RepositoryError("repository architecture is invalid")
    repository = PACKAGE.decode_text(index[72:136], "repository")
    if PACKAGE.package_identifier(repository, "repository") != repository:
        raise RepositoryError("repository identifier is not canonical")
    if repository_version < minimum_repository_version:
        raise RepositoryError("repository version is older than the immutable minimum")
    current_time = int(time.time()) if now is None else _uint(
        now, 0xffffffffffffffff, "current time")
    if generated_at >= expires_at:
        raise RepositoryError("repository freshness interval is invalid")
    if current_time < generated_at:
        raise RepositoryError("repository index is not yet valid")
    if current_time >= expires_at:
        raise RepositoryError("repository index is expired")
    if index[168:200] != hashlib.sha256(index[INDEX_HEADER_BYTES:]).digest():
        raise RepositoryError("repository content SHA-256 mismatch")

    packages: list[dict[str, Any]] = []
    previous_identity: tuple[bytes, bytes] | None = None
    identities: set[tuple[str, str]] = set()
    paths: set[str] = set()
    precedence: dict[str, list[str]] = {}
    relation_cursor = 0
    for package_index in range(package_count):
        start = package_offset + package_index * INDEX_PACKAGE_RECORD_BYTES
        record = index[start:start + INDEX_PACKAGE_RECORD_BYTES]
        identifier = PACKAGE.decode_text(record[:64], f"packages[{package_index}].identifier")
        version_value = PACKAGE.decode_text(record[64:128],
                                            f"packages[{package_index}].version")
        download_path = PACKAGE.decode_text(record[128:256],
                                            f"packages[{package_index}].download_path")
        if PACKAGE.package_identifier(identifier, "package identifier") != identifier \
                or PACKAGE.semantic_version(version_value, "package version") != version_value \
                or PACKAGE.package_path(download_path, "download path") != download_path:
            raise RepositoryError(f"packages[{package_index}] text metadata is not canonical")
        identity_bytes = (identifier.encode("ascii"), version_value.encode("ascii"))
        if previous_identity is not None and identity_bytes <= previous_identity:
            raise RepositoryError("repository packages are not in canonical order")
        previous_identity = identity_bytes
        identity = (identifier, version_value)
        if identity in identities or download_path in paths:
            raise RepositoryError("repository contains duplicate identity or download path")
        identities.add(identity)
        paths.add(download_path)
        versions = precedence.setdefault(identifier, [])
        if any(semver_compare(version_value, other) == 0 for other in versions):
            raise RepositoryError("repository contains ambiguous build-only package versions")
        versions.append(version_value)
        package_bytes = struct.unpack_from("<Q", record, 256)[0]
        if not 1 <= package_bytes <= PACKAGE.V3_MAX_PACKAGE_BYTES:
            raise RepositoryError(f"packages[{package_index}] size is invalid")
        digest = record[264:296]
        publisher_key_id = record[296:328]
        (dependency_start, dependency_count, conflict_start, conflict_count,
         provide_start, provide_count) = struct.unpack_from("<IIIIII", record, 328)
        if dependency_count > INDEX_MAX_RELATIONS_PER_PACKAGE \
                or conflict_count > INDEX_MAX_RELATIONS_PER_PACKAGE \
                or provide_count > INDEX_MAX_RELATIONS_PER_PACKAGE \
                or (dependency_start, conflict_start, provide_start) != (
                    relation_cursor, relation_cursor + dependency_count,
                    relation_cursor + dependency_count + conflict_count):
            raise RepositoryError(f"packages[{package_index}] relation ranges are invalid")
        dependency_offset = relation_offset + dependency_start * INDEX_RELATION_RECORD_BYTES
        conflict_offset = relation_offset + conflict_start * INDEX_RELATION_RECORD_BYTES
        provide_offset = relation_offset + provide_start * INDEX_RELATION_RECORD_BYTES
        dependencies = PACKAGE._v3_decode_relation_records(
            index, dependency_offset, dependency_count,
            f"packages[{package_index}].dependencies")
        conflicts = PACKAGE._v3_decode_relation_records(
            index, conflict_offset, conflict_count,
            f"packages[{package_index}].conflicts")
        provides = _parse_provides(
            index, provide_offset, provide_count,
            f"packages[{package_index}].provides")
        relation_cursor += dependency_count + conflict_count + provide_count
        if relation_cursor > relation_count:
            raise RepositoryError(f"packages[{package_index}] relation range exceeds the table")
        if ({item["identifier"] for item in dependencies} &
                {item["identifier"] for item in conflicts}):
            raise RepositoryError(f"packages[{package_index}] dependency/conflict overlap")
        if identifier in {item["identifier"] for item in provides}:
            raise RepositoryError(f"packages[{package_index}] redundantly provides itself")
        if any(record[352:]):
            raise RepositoryError(f"packages[{package_index}] reserved bytes are nonzero")
        packages.append({
            "identifier": identifier,
            "version": version_value,
            "download_path": download_path,
            "bytes": package_bytes,
            "sha256": digest.hex().upper(),
            "publisher_key_id": publisher_key_id.hex().upper(),
            "dependencies": dependencies,
            "conflicts": conflicts,
            "provides": provides,
        })
    if relation_cursor != relation_count:
        raise RepositoryError("repository relation records are unowned")

    signature = index[INDEX_SIGNATURE_OFFSET:INDEX_SIGNATURE_OFFSET + INDEX_SIGNATURE_BYTES]
    if not any(signature):
        raise RepositoryError("repository Ed25519 signature is missing")
    root_key_id = index[200:232]
    trusted_root = _trusted_root(root_key_id, trusted_root_keys)
    unsigned = bytearray(index)
    unsigned[INDEX_SIGNATURE_OFFSET:INDEX_SIGNATURE_OFFSET + INDEX_SIGNATURE_BYTES] = bytes(
        INDEX_SIGNATURE_BYTES)
    try:
        trusted_root.verify(signature, bytes(unsigned))
    except Exception as error:
        raise RepositoryError("repository Ed25519 signature verification failed") from error

    return {
        "format": 1,
        "repository": repository,
        "repository_version": repository_version,
        "generated_at": generated_at,
        "expires_at": expires_at,
        "architecture": architecture,
        "abi_min": abi_min,
        "abi_max": abi_max,
        "packages": packages,
        "content_sha256": index[168:200].hex().upper(),
        "index_sha256": hashlib.sha256(index).hexdigest().upper(),
        "signature": {"algorithm": "Ed25519", "verified": True,
                      "root_key_id": root_key_id.hex().upper()},
    }


class _ResolutionFailure(Exception):
    def __init__(self, kind: str, message: str) -> None:
        super().__init__(message)
        self.kind = kind


def _requirement(value: str, field: str) -> dict[str, str]:
    if not isinstance(value, str) or not value:
        raise RepositoryError(f"{field} must be a nonempty package requirement")
    identifier, separator, constraint = value.partition("@")
    identifier = PACKAGE.package_identifier(identifier, field)
    constraint = PACKAGE.version_constraint(constraint if separator else "*", field)
    return {"identifier": identifier, "constraint": constraint}


def _candidate_entries(packages: list[dict[str, Any]], identifier: str,
                       constraint: str) -> dict[str, list[dict[str, Any]]]:
    providers: dict[str, list[dict[str, Any]]] = {}
    for package in packages:
        provided_versions: list[str] = []
        if package["identifier"] == identifier:
            provided_versions.append(package["version"])
        provided_versions.extend(
            item["version"] for item in package["provides"]
            if item["identifier"] == identifier)
        if any(version_satisfies(version, constraint) for version in provided_versions):
            providers.setdefault(package["identifier"], []).append(package)
    for entries in providers.values():
        entries.sort(key=cmp_to_key(
            lambda left, right: -semver_compare(left["version"], right["version"])))
    return providers


def _provided_version(package: dict[str, Any], identifier: str) -> str | None:
    if package["identifier"] == identifier:
        return package["version"]
    for provide in package["provides"]:
        if provide["identifier"] == identifier:
            return provide["version"]
    return None


def _selected_conflict(selected: dict[str, dict[str, Any]]) -> str | None:
    for source_id in sorted(selected):
        source = selected[source_id]
        for conflict in source["conflicts"]:
            for target_id in sorted(selected):
                target = selected[target_id]
                provided = _provided_version(target, conflict["identifier"])
                if provided is not None and version_satisfies(
                        provided, conflict["constraint"]):
                    return (f"{source['identifier']}@{source['version']} conflicts with "
                            f"{target['identifier']}@{target['version']} through "
                            f"{conflict['identifier']}@{conflict['constraint']}")
    return None


def _cycle_or_order(selected: dict[str, dict[str, Any]],
                    bindings: dict[str, str]) -> list[str]:
    edges: dict[str, set[str]] = {identifier: set() for identifier in selected}
    for identifier, package in selected.items():
        for dependency in package["dependencies"]:
            target = bindings.get(dependency["identifier"])
            if target is None or target not in selected:
                raise _ResolutionFailure(
                    "unsatisfied", f"unbound dependency {dependency['identifier']}")
            edges[identifier].add(target)
    visiting: set[str] = set()
    visited: set[str] = set()
    order: list[str] = []

    def visit(identifier: str, stack: list[str]) -> None:
        if identifier in visiting:
            cycle_start = stack.index(identifier)
            cycle = stack[cycle_start:] + [identifier]
            raise _ResolutionFailure("cycle", "dependency cycle: " + " -> ".join(cycle))
        if identifier in visited:
            return
        visiting.add(identifier)
        stack.append(identifier)
        for dependency in sorted(edges[identifier]):
            visit(dependency, stack)
        stack.pop()
        visiting.remove(identifier)
        visited.add(identifier)
        order.append(identifier)

    for identifier in sorted(selected):
        visit(identifier, [])
    return order


def _solve(packages: list[dict[str, Any]], pending: list[tuple[str, str, str | None]],
           selected: dict[str, dict[str, Any]], bindings: dict[str, str],
           max_packages: int) -> tuple[dict[str, dict[str, Any]], dict[str, str], list[str]]:
    if not pending:
        conflict = _selected_conflict(selected)
        if conflict is not None:
            raise _ResolutionFailure("conflict", conflict)
        return selected, bindings, _cycle_or_order(selected, bindings)
    pending = sorted(pending, key=lambda item: (
        item[0].encode("ascii"), item[1].encode("ascii"),
        b"" if item[2] is None else item[2].encode("ascii")))
    identifier, constraint, _parent = pending[0]
    remaining = pending[1:]
    providers = _candidate_entries(packages, identifier, constraint)
    if not providers:
        raise _ResolutionFailure(
            "unsatisfied", f"unsatisfied dependency {identifier}@{constraint}")
    if len(providers) != 1:
        raise _ResolutionFailure(
            "ambiguous", f"ambiguous providers for {identifier}@{constraint}: " +
            ", ".join(sorted(providers)))
    provider_id = next(iter(providers))
    existing_binding = bindings.get(identifier)
    if existing_binding is not None and existing_binding != provider_id:
        raise _ResolutionFailure(
            "ambiguous", f"provider binding changed for {identifier}")
    existing = selected.get(provider_id)
    candidates = providers[provider_id]
    if existing is not None:
        if existing not in candidates:
            raise _ResolutionFailure(
                "unsatisfied", f"selected {provider_id}@{existing['version']} does not satisfy "
                f"{identifier}@{constraint}")
        next_bindings = dict(bindings)
        next_bindings[identifier] = provider_id
        return _solve(packages, remaining, selected, next_bindings, max_packages)

    failures: list[_ResolutionFailure] = []
    for candidate in candidates:
        next_selected = dict(selected)
        next_selected[provider_id] = candidate
        if len(next_selected) > max_packages:
            raise _ResolutionFailure(
                "bound", f"dependency graph exceeds its {max_packages}-package bound")
        next_bindings = dict(bindings)
        next_bindings[identifier] = provider_id
        next_bindings[provider_id] = provider_id
        next_pending = list(remaining)
        next_pending.extend((dependency["identifier"], dependency["constraint"], provider_id)
                            for dependency in candidate["dependencies"])
        try:
            return _solve(packages, next_pending, next_selected,
                          next_bindings, max_packages)
        except _ResolutionFailure as error:
            failures.append(error)
    priority = {"bound": 5, "ambiguous": 4, "cycle": 3, "conflict": 2,
                "unsatisfied": 1}
    raise max(failures, key=lambda failure: priority[failure.kind])


def resolve_repository(repository: dict[str, Any], requirements: list[str], *,
                       architecture: str, abi_version: int,
                       max_packages: int = 128) -> tuple[list[dict[str, Any]], bytes]:
    architecture = PACKAGE.canonical_printable(architecture, 16, "architecture")
    abi_version = _uint(abi_version, 0xffffffff, "abi_version", positive=True)
    max_packages = _uint(max_packages, INDEX_MAX_GRAPH_PACKAGES,
                         "max_packages", positive=True)
    if architecture != repository["architecture"]:
        raise RepositoryError(
            f"repository architecture {repository['architecture']} does not match {architecture}")
    if not repository["abi_min"] <= abi_version <= repository["abi_max"]:
        raise RepositoryError(f"ABI {abi_version} is outside the repository range")
    if not isinstance(requirements, list) or not requirements:
        raise RepositoryError("at least one package requirement is required")
    requested = [_requirement(value, f"requirements[{index}]")
                 for index, value in enumerate(requirements)]
    requested.sort(key=lambda item: (item["identifier"], item["constraint"]))
    if len({item["identifier"] for item in requested}) != len(requested):
        raise RepositoryError("top-level requirements contain a duplicate identifier")
    pending = [(item["identifier"], item["constraint"], None) for item in requested]
    try:
        selected, _bindings, order = _solve(
            repository["packages"], pending, {}, {}, max_packages)
    except _ResolutionFailure as error:
        raise RepositoryError(str(error)) from error
    plan = [{
        "order": index,
        "identifier": selected[identifier]["identifier"],
        "version": selected[identifier]["version"],
        "download_path": selected[identifier]["download_path"],
        "bytes": selected[identifier]["bytes"],
        "sha256": selected[identifier]["sha256"],
        "publisher_key_id": selected[identifier]["publisher_key_id"],
    } for index, identifier in enumerate(order)]
    lock = {
        "lock_format": 1,
        "repository": {
            "identifier": repository["repository"],
            "version": repository["repository_version"],
            "index_sha256": repository["index_sha256"],
            "root_key_id": repository["signature"]["root_key_id"],
            "architecture": repository["architecture"],
            "abi": abi_version,
        },
        "requested": requested,
        "install": plan,
    }
    lock_bytes = (json.dumps(lock, sort_keys=True, separators=(",", ":")) + "\n").encode(
        "ascii")
    return plan, lock_bytes


def verify_download(package: dict[str, Any], payload: bytes) -> None:
    if len(payload) != package["bytes"]:
        raise RepositoryError(
            f"download size mismatch for {package['identifier']}@{package['version']}")
    if hashlib.sha256(payload).hexdigest().upper() != package["sha256"]:
        raise RepositoryError(
            f"download SHA-256 mismatch for {package['identifier']}@{package['version']}")


def _trusted_roots(paths: list[str]) -> dict[str, bytes]:
    return PACKAGE.trusted_ed25519_keys(paths)


def _read_index(path: str) -> bytes:
    return PACKAGE.read_regular_bounded(Path(path), INDEX_MAX_BYTES, "repository index")


def command_build(args: argparse.Namespace) -> None:
    spec_path = Path(args.spec)
    spec = json.loads(PACKAGE.read_regular_bounded(
        spec_path, 4 * 1024 * 1024, "repository specification").decode("utf-8"))
    if not isinstance(spec, dict):
        raise RepositoryError("repository specification must be one JSON object")
    signing_key = PACKAGE.read_regular_bounded(
        Path(args.signing_key), 64 * 1024, "repository root private key")
    index = build_repository(spec, signing_key)
    public_key = PACKAGE._ed25519_public_bytes_from_private(signing_key)
    key_id = hashlib.sha256(public_key).hexdigest()
    report = parse_repository(index, trusted_root_keys={key_id: public_key},
                              now=spec["generated_at"])
    PACKAGE.atomic_write(Path(args.output), index)
    print(json.dumps({"output": str(Path(args.output)), **report}, sort_keys=True))


def command_inspect(args: argparse.Namespace) -> None:
    report = parse_repository(
        _read_index(args.index), trusted_root_keys=_trusted_roots(args.trusted_root),
        now=args.now, minimum_repository_version=args.minimum_version)
    print(json.dumps({"index": str(Path(args.index)), **report}, indent=2,
                     sort_keys=True))


def command_resolve(args: argparse.Namespace) -> None:
    report = parse_repository(
        _read_index(args.index), trusted_root_keys=_trusted_roots(args.trusted_root),
        now=args.now, minimum_repository_version=args.minimum_version)
    plan, lock = resolve_repository(
        report, args.requirements, architecture=args.architecture,
        abi_version=args.abi, max_packages=args.max_packages)
    if args.lock_output:
        PACKAGE.atomic_write(Path(args.lock_output), lock)
    print(json.dumps({"plan": plan, "lock": json.loads(lock),
                      "lock_sha256": hashlib.sha256(lock).hexdigest().upper()},
                     indent=2, sort_keys=True))


def command_verify_download(args: argparse.Namespace) -> None:
    report = parse_repository(
        _read_index(args.index), trusted_root_keys=_trusted_roots(args.trusted_root),
        now=args.now, minimum_repository_version=args.minimum_version)
    matches = [entry for entry in report["packages"]
               if entry["identifier"] == args.identifier and entry["version"] == args.version]
    if len(matches) != 1:
        raise RepositoryError("download package identity/version is absent or ambiguous")
    payload = PACKAGE.read_regular_bounded(
        Path(args.file), PACKAGE.V3_MAX_PACKAGE_BYTES, "package download")
    verify_download(matches[0], payload)
    print(json.dumps({"identifier": args.identifier, "version": args.version,
                      "sha256": matches[0]["sha256"], "verified": True},
                     sort_keys=True))


def _verification_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--trusted-root", action="append", required=True)
    parser.add_argument("--now", type=int)
    parser.add_argument("--minimum-version", type=int, default=0)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    builder = commands.add_parser("build")
    builder.add_argument("--spec", required=True)
    builder.add_argument("--signing-key", required=True)
    builder.add_argument("--output", required=True)
    builder.set_defaults(function=command_build)
    inspector = commands.add_parser("inspect")
    _verification_arguments(inspector)
    inspector.add_argument("index")
    inspector.set_defaults(function=command_inspect)
    resolver = commands.add_parser("resolve")
    _verification_arguments(resolver)
    resolver.add_argument("--architecture", default="x86_64")
    resolver.add_argument("--abi", type=int, required=True)
    resolver.add_argument("--max-packages", type=int, default=128)
    resolver.add_argument("--lock-output")
    resolver.add_argument("index")
    resolver.add_argument("requirements", nargs="+")
    resolver.set_defaults(function=command_resolve)
    verifier = commands.add_parser("verify-download")
    _verification_arguments(verifier)
    verifier.add_argument("--identifier", required=True)
    verifier.add_argument("--version", required=True)
    verifier.add_argument("--file", required=True)
    verifier.add_argument("index")
    verifier.set_defaults(function=command_verify_download)
    return result


def main() -> int:
    try:
        args = parser().parse_args()
        args.function(args)
    except (RepositoryError, OSError, UnicodeError, json.JSONDecodeError,
            struct.error) as error:
        print(f"Phipia repository refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
