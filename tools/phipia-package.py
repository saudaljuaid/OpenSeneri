#!/usr/bin/env python3
"""Build, inspect, and install deterministic Phipia application packages."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
from typing import Any

import fat32_image


MANIFEST_BYTES = 1024
PACKAGE_HEADER_BYTES = 64
RESOURCE_HEADER_BYTES = 32
MAX_PACKAGE_RESOURCES = 13
PACKAGE_MAGIC = b"PHIPPKG1"
MANIFEST_MAGIC = b"PHIPIAA1"
V3_HEADER_BYTES = 512
V3_FILE_RECORD_BYTES = 256
V3_RELATION_RECORD_BYTES = 128
V3_MAX_PACKAGE_BYTES = 256 * 1024 * 1024
V3_MAX_FILE_BYTES = 64 * 1024 * 1024
V3_MAX_FILES = 256
V3_MAX_RELATIONS = 64
V3_SIGNATURE_OFFSET = 440
V3_SIGNATURE_BYTES = 64
V3_SIGNATURE_ALGORITHM_ED25519 = 1
V3_ARCHITECTURES = {"x86_64"}
V3_FILE_KINDS = {
    "executable": 1,
    "library": 2,
    "resource": 3,
    "icon": 4,
}
V3_FILE_KIND_NAMES = {value: key for key, value in V3_FILE_KINDS.items()}
SEMVER_RE = re.compile(
    r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
    r"(?:-(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?")
CAPABILITIES = {
    "console": 1 << 0,
    "system-read": 1 << 1,
    "data-read": 1 << 2,
    "data-write": 1 << 3,
    "time": 1 << 4,
    "entropy": 1 << 5,
    "window": 1 << 6,
    "input": 1 << 7,
    "network": 1 << 8,
    "threads": 1 << 9,
    "audio": 1 << 10,
    "packages": 1 << 11,
}


class PackageError(ValueError):
    """A named package-format or installation refusal."""


def read_regular(path: Path) -> bytes:
    if not path.is_file() or path.is_symlink():
        raise PackageError(f"not an ordinary input file: {path}")
    return path.read_bytes()


def read_regular_bounded(path: Path, maximum: int, field: str) -> bytes:
    if not path.is_file() or path.is_symlink():
        raise PackageError(f"not an ordinary input file: {path}")
    size = path.stat().st_size
    if not 0 < size <= maximum:
        raise PackageError(f"{field} is empty or exceeds {maximum} bytes: {path}")
    data = path.read_bytes()
    if len(data) != size or len(data) > maximum:
        raise PackageError(f"{field} changed while it was read: {path}")
    return data


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.unlink(missing_ok=True)
    try:
        with temporary.open("xb") as stream:
            stream.write(data)
            stream.flush()
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def text_field(value: Any, width: int, field: str, *, required: bool) -> bytes:
    if not isinstance(value, str) or (required and not value):
        raise PackageError(f"{field} must be a{' non-empty' if required else ''} string")
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise PackageError(f"{field} must be ASCII") from error
    if len(encoded) >= width or b"\0" in encoded:
        raise PackageError(f"{field} exceeds its {width - 1}-byte bound")
    return encoded + bytes(width - len(encoded))


def argument_field(value: Any, field: str) -> bytes:
    encoded = text_field(value, 32, field, required=True)
    end = encoded.index(0)
    if any(byte < 0x20 or byte > 0x7e for byte in encoded[:end]):
        raise PackageError(f"{field} must contain printable ASCII")
    return encoded


def identifier(value: Any, field: str) -> str:
    if not isinstance(value, str) or not 1 <= len(value) <= 8:
        raise PackageError(f"{field} must contain 1-8 characters")
    upper = value.upper()
    if any(character not in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for character in upper):
        raise PackageError(f"{field} is outside the ASCII 8.3 identifier subset")
    return upper


def short_path(value: Any, field: str, *, required: bool) -> str:
    if value in (None, "") and not required:
        return ""
    if not isinstance(value, str):
        raise PackageError(f"{field} must be a string")
    fat32_image.short_name_bytes(value)
    return value.upper()


def canonical_printable(value: Any, width: int, field: str) -> str:
    encoded = text_field(value, width, field, required=True)
    end = encoded.index(0)
    if any(byte < 0x20 or byte > 0x7e for byte in encoded[:end]):
        raise PackageError(f"{field} must contain printable ASCII")
    return encoded[:end].decode("ascii")


def package_identifier(value: Any, field: str) -> str:
    value = canonical_printable(value, 64, field)
    if value != value.lower() or not re.fullmatch(
            r"[a-z0-9]+(?:[.-][a-z0-9]+)*", value):
        raise PackageError(
            f"{field} must be a canonical lowercase dotted identifier")
    return value


def semantic_version(value: Any, field: str) -> str:
    value = canonical_printable(value, 64, field)
    if SEMVER_RE.fullmatch(value) is None:
        raise PackageError(f"{field} must be a canonical SemVer 2.0.0 version")
    return value


def version_constraint(value: Any, field: str) -> str:
    value = canonical_printable(value, 56, field)
    if value == "*":
        return value
    canonical: list[str] = []
    for index, clause in enumerate(value.split(",")):
        clause = clause.strip()
        match = re.fullmatch(r"(=|>=|>|<=|<|\^|~)?(.+)", clause)
        if match is None:
            raise PackageError(f"{field}[{index}] is invalid")
        operator = match.group(1) or "="
        canonical.append(operator + semantic_version(
            match.group(2), f"{field}[{index}].version"))
    return ",".join(canonical)


def package_path(value: Any, field: str) -> str:
    value = canonical_printable(value, 128, field)
    if value.startswith("/") or value.endswith("/") or "\\" in value:
        raise PackageError(f"{field} must be a package-root-relative POSIX path")
    pieces = value.split("/")
    if any(piece in ("", ".", "..") for piece in pieces) or re.fullmatch(
            r"[0-9A-Za-z._+/-]+", value) is None:
        raise PackageError(f"{field} is not a canonical package path")
    return value


def soname(value: Any, field: str) -> str:
    value = canonical_printable(value, 64, field)
    if "/" in value or "\\" in value or re.fullmatch(
            r"[0-9A-Za-z._+-]+", value) is None:
        raise PackageError(f"{field} must be a canonical library basename")
    return value


def _capability_bits(value: Any) -> int:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise PackageError("capabilities must be a list of names")
    if len(value) != len(set(value)):
        raise PackageError("capabilities must not contain duplicates")
    unknown = sorted(set(value) - CAPABILITIES.keys())
    if unknown:
        raise PackageError(f"unknown capabilities: {', '.join(unknown)}")
    result = 0
    for name in value:
        result |= CAPABILITIES[name]
    return result


def _ed25519_modules() -> tuple[Any, Any, Any]:
    try:
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PrivateKey, Ed25519PublicKey)
    except (ImportError, OSError) as error:
        raise PackageError(
            "real Ed25519 support is unavailable; install Python cryptography") from error
    return serialization, Ed25519PrivateKey, Ed25519PublicKey


def ed25519_available() -> bool:
    try:
        _ed25519_modules()
    except PackageError:
        return False
    return True


def _ed25519_private(value: bytes) -> Any:
    serialization, private_type, _ = _ed25519_modules()
    try:
        key = (private_type.from_private_bytes(value) if len(value) == 32 else
               serialization.load_pem_private_key(value, password=None))
    except (TypeError, ValueError) as error:
        raise PackageError("Ed25519 private key must be a raw 32-byte seed or PEM") from error
    if not isinstance(key, private_type):
        raise PackageError("private key is not Ed25519")
    return key


def _ed25519_public_bytes_from_private(value: bytes) -> bytes:
    serialization, _, _ = _ed25519_modules()
    return _ed25519_private(value).public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw)


def _ed25519_public(value: bytes) -> tuple[Any, bytes]:
    serialization, _, public_type = _ed25519_modules()
    try:
        key = (public_type.from_public_bytes(value) if len(value) == 32 else
               serialization.load_pem_public_key(value))
    except (TypeError, ValueError) as error:
        raise PackageError("Ed25519 public key must be 32 raw bytes or PEM") from error
    if not isinstance(key, public_type):
        raise PackageError("trusted key is not Ed25519")
    raw = key.public_bytes(encoding=serialization.Encoding.Raw,
                           format=serialization.PublicFormat.Raw)
    return key, raw


def trusted_ed25519_keys(paths: list[str] | None) -> dict[str, bytes]:
    result: dict[str, bytes] = {}
    for path_value in paths or []:
        material = read_regular_bounded(Path(path_value), 64 * 1024,
                                        "Ed25519 public key")
        _, raw = _ed25519_public(material)
        key_id = hashlib.sha256(raw).hexdigest()
        if key_id in result:
            raise PackageError(f"duplicate trusted Ed25519 key: {key_id}")
        result[key_id] = material
    return result


def encode_manifest(spec: dict[str, Any], executable: bytes,
                    resources: tuple[tuple[str, bytes], ...] = ()) -> bytes:
    abi_version = spec.get("abi_version", 1)
    if abi_version != 1:
        raise PackageError("abi_version must be 1")
    name = text_field(spec.get("name"), 32, "name", required=True)
    app_id = identifier(spec.get("identifier"), "identifier")
    executable_path = short_path(spec.get("executable"), "executable", required=True)
    data_namespace = identifier(spec.get("data_namespace"), "data_namespace")
    resource_directory = spec.get("resource_directory", "")
    if resource_directory:
        resource_directory = identifier(resource_directory, "resource_directory")
    dynamic_catalog = spec.get("dynamic_catalog", "")
    dynamic_catalog_digest = bytes(32)
    if dynamic_catalog:
        if not resource_directory:
            raise PackageError("dynamic_catalog requires resource_directory")
        if not isinstance(dynamic_catalog, str) or dynamic_catalog.count("/") != 1:
            raise PackageError("dynamic_catalog must be DIRECTORY/NAME")
        catalog_directory, catalog_name = dynamic_catalog.split("/", 1)
        if identifier(catalog_directory, "dynamic_catalog directory") != resource_directory:
            raise PackageError("dynamic_catalog must be inside resource_directory")
        catalog_name = short_path(catalog_name, "dynamic_catalog name", required=True)
        dynamic_catalog = resource_directory + "/" + catalog_name
        matches = [payload for path, payload in resources
                   if path.upper() == catalog_name]
        if len(matches) != 1:
            raise PackageError("dynamic_catalog must name exactly one packaged resource")
        dynamic_catalog_digest = hashlib.sha256(matches[0]).digest()
    icon = short_path(spec.get("icon", ""), "icon", required=False)
    arguments = spec.get("arguments", [])
    if not isinstance(arguments, list) or len(arguments) > 8:
        raise PackageError("arguments must be a list of at most eight strings")
    memory_limit = spec.get("memory_limit", 16 * 1024 * 1024)
    max_handles = spec.get("max_handles", 64)
    max_threads = spec.get("max_threads", 4)
    if not isinstance(memory_limit, int) or memory_limit < 64 * 1024 \
            or memory_limit > 256 * 1024 * 1024 or memory_limit % 4096:
        raise PackageError("memory_limit must be a page multiple from 64 KiB through 256 MiB")
    if not isinstance(max_handles, int) or not 1 <= max_handles <= 128:
        raise PackageError("max_handles must be from 1 through 128")
    if not isinstance(max_threads, int) or not 1 <= max_threads <= 8:
        raise PackageError("max_threads must be from 1 through 8")
    requested = spec.get("capabilities", [])
    if not isinstance(requested, list) or any(not isinstance(item, str) for item in requested):
        raise PackageError("capabilities must be a list of names")
    unknown = sorted(set(requested) - CAPABILITIES.keys())
    if unknown:
        raise PackageError(f"unknown capabilities: {', '.join(unknown)}")
    capability_bits = 0
    for capability in requested:
        capability_bits |= CAPABILITIES[capability]

    manifest = bytearray(MANIFEST_BYTES)
    manifest[:8] = MANIFEST_MAGIC
    struct.pack_into("<HHIIHHHHQQ", manifest, 8, 1, MANIFEST_BYTES,
                     abi_version, 0,
                     len(arguments), max_handles, max_threads, 0,
                     capability_bits, memory_limit)
    manifest[64:96] = name
    manifest[96:112] = text_field(app_id, 16, "identifier", required=True)
    manifest[112:128] = text_field(executable_path, 16, "executable", required=True)
    manifest[128:160] = hashlib.sha256(executable).digest()
    manifest[160:176] = text_field(resource_directory, 16, "resource_directory", required=False)
    manifest[176:192] = text_field(data_namespace, 16, "data_namespace", required=True)
    manifest[192:208] = text_field(icon, 16, "icon", required=False)
    for index, argument in enumerate(arguments):
        manifest[208 + index * 32:240 + index * 32] = argument_field(
            argument, f"arguments[{index}]")
    manifest[464:480] = text_field(dynamic_catalog, 16, "dynamic_catalog",
                                   required=False)
    manifest[480:512] = dynamic_catalog_digest
    return bytes(manifest)


def decode_text(data: bytes, field: str) -> str:
    try:
        end = data.index(0)
    except ValueError as error:
        raise PackageError(f"{field} is not terminated") from error
    if any(data[end:]):
        raise PackageError(f"{field} has nonzero tail bytes")
    try:
        return data[:end].decode("ascii")
    except UnicodeDecodeError as error:
        raise PackageError(f"{field} is not ASCII") from error


def decode_argument(data: bytes, field: str) -> str:
    value = decode_text(data, field)
    if any(ord(character) < 0x20 or ord(character) > 0x7e
           for character in value):
        raise PackageError(f"{field} contains non-printable ASCII")
    return value


def inspect_manifest(manifest: bytes, executable: bytes) -> dict[str, Any]:
    if len(manifest) != MANIFEST_BYTES or manifest[:8] != MANIFEST_MAGIC:
        raise PackageError("manifest length or magic is invalid")
    (version, size, abi, flags, argument_count, max_handles, max_threads,
     reserved, capabilities, memory_limit) = struct.unpack_from("<HHIIHHHHQQ", manifest, 8)
    if (version, size, abi, flags, reserved) != (1, MANIFEST_BYTES, 1, 0, 0):
        raise PackageError("manifest version, size, flags, or reserved field is invalid")
    if any(manifest[44:64]) or any(manifest[512:]):
        raise PackageError("manifest reserved bytes are nonzero")
    if argument_count > 8 or capabilities & ~sum(CAPABILITIES.values()):
        raise PackageError("manifest argument or capability bits are invalid")
    digest = hashlib.sha256(executable).digest()
    if manifest[128:160] != digest:
        raise PackageError("manifest executable SHA-256 mismatch")
    args = [decode_argument(manifest[208 + index * 32:240 + index * 32],
                            f"arguments[{index}]")
            for index in range(argument_count)]
    if any(any(manifest[208 + index * 32:240 + index * 32])
           for index in range(argument_count, 8)):
        raise PackageError("unused argument records are nonzero")
    names = [name for name, bit in CAPABILITIES.items() if capabilities & bit]
    dynamic_catalog = decode_text(manifest[464:480], "dynamic_catalog")
    dynamic_catalog_sha256 = manifest[480:512]
    if bool(dynamic_catalog) != any(dynamic_catalog_sha256):
        raise PackageError("dynamic catalog path and digest must be paired")
    return {
        "format": version,
        "abi_version": abi,
        "name": decode_text(manifest[64:96], "name"),
        "identifier": decode_text(manifest[96:112], "identifier"),
        "executable": decode_text(manifest[112:128], "executable"),
        "executable_bytes": len(executable),
        "executable_sha256": digest.hex().upper(),
        "memory_limit": memory_limit,
        "max_handles": max_handles,
        "max_threads": max_threads,
        "capabilities": names,
        "resource_directory": decode_text(manifest[160:176], "resource_directory"),
        "data_namespace": decode_text(manifest[176:192], "data_namespace"),
        "icon": decode_text(manifest[192:208], "icon"),
        "arguments": args,
        "dynamic_catalog": dynamic_catalog,
        "dynamic_catalog_sha256": (dynamic_catalog_sha256.hex().upper()
                                    if dynamic_catalog else ""),
    }


def encode_resources(resources: tuple[tuple[str, bytes], ...]) -> bytes:
    if len(resources) > MAX_PACKAGE_RESOURCES:
        raise PackageError("resource count exceeds the one-cluster directory bound")
    encoded = bytearray()
    occupied: set[str] = set()
    for path, payload in sorted(resources, key=lambda item: item[0].upper()):
        normalized = short_path(path, "resource path", required=True)
        if "/" in normalized or normalized in occupied:
            raise PackageError("resource paths must be unique 8.3 filenames")
        if not payload or len(payload) > 16 * 1024 * 1024:
            raise PackageError("resource is empty or exceeds the FAT32 file bound")
        occupied.add(normalized)
        header = bytearray(RESOURCE_HEADER_BYTES)
        header[:16] = text_field(normalized, 16, "resource path", required=True)
        struct.pack_into("<Q", header, 16, len(payload))
        encoded.extend(header)
        encoded.extend(payload)
    return bytes(encoded)


def inspect_dynamic_catalog(report: dict[str, Any],
                            resources: tuple[tuple[str, bytes], ...]) -> None:
    """Bind every authenticated SONAME to one exact package resource."""
    catalog_path = str(report["dynamic_catalog"])
    if not catalog_path:
        return
    directory = str(report["resource_directory"])
    prefix = directory + "/"
    if not catalog_path.startswith(prefix) or catalog_path.count("/") != 1:
        raise PackageError("dynamic catalog is outside its resource directory")
    resource_map = {path: payload for path, payload in resources}
    catalog_name = catalog_path[len(prefix):]
    catalog = resource_map.get(catalog_name)
    if catalog is None or hashlib.sha256(catalog).hexdigest().upper() != \
            report["dynamic_catalog_sha256"]:
        raise PackageError("dynamic catalog resource or digest does not match")
    if len(catalog) != 2048 or catalog[:8] != b"PHIPDYN1":
        raise PackageError("dynamic catalog length or magic is invalid")
    version, header_bytes, total_bytes, count, entry_bytes = struct.unpack_from(
        "<HHIHH", catalog, 8)
    if (version, header_bytes, total_bytes, entry_bytes) != (1, 64, 2048, 96) \
            or not 1 <= count <= 16 or any(catalog[20:64]):
        raise PackageError("dynamic catalog header is invalid")
    used_end = 64 + count * entry_bytes
    if any(catalog[used_end:]):
        raise PackageError("dynamic catalog reserved tail is nonzero")
    previous = ""
    for index in range(count):
        offset = 64 + index * entry_bytes
        name = decode_text(catalog[offset:offset + 64],
                           f"dynamic_catalog[{index}].soname")
        short_path(name, f"dynamic_catalog[{index}].soname", required=True)
        if name != name.upper() or name <= previous:
            raise PackageError("dynamic catalog SONAMEs are not canonical and sorted")
        previous = name
        payload = resource_map.get(name)
        digest = catalog[offset + 64:offset + 96]
        if payload is None or digest != hashlib.sha256(payload).digest():
            raise PackageError(
                f"dynamic catalog resource digest does not match {name}")


def build_package(spec: dict[str, Any], executable: bytes,
                  resources: tuple[tuple[str, bytes], ...] = ()) -> bytes:
    if not executable or len(executable) > 16 * 1024 * 1024:
        raise PackageError("executable is empty or exceeds the FAT32 file bound")
    manifest = encode_manifest(spec, executable, resources)
    resource_body = encode_resources(resources)
    body = manifest + executable + resource_body
    header = bytearray(PACKAGE_HEADER_BYTES)
    header[:8] = PACKAGE_MAGIC
    version = 2 if resources else 1
    struct.pack_into("<HHIQ", header, 8, version, PACKAGE_HEADER_BYTES,
                     MANIFEST_BYTES, len(executable))
    if resources:
        struct.pack_into("<HHI", header, 24, len(resources), 0,
                         len(resource_body))
    header[32:64] = hashlib.sha256(body).digest()
    return bytes(header) + body


def _v3_relation_records(value: Any, field: str) -> tuple[bytes, list[dict[str, str]]]:
    if not isinstance(value, list) or len(value) > V3_MAX_RELATIONS:
        raise PackageError(f"{field} must be a list of at most {V3_MAX_RELATIONS} entries")
    relations: list[tuple[str, str]] = []
    for index, relation in enumerate(value):
        if not isinstance(relation, dict) or set(relation) != {"identifier", "constraint"}:
            raise PackageError(
                f"{field}[{index}] must contain only identifier and constraint")
        relations.append((
            package_identifier(relation["identifier"], f"{field}[{index}].identifier"),
            version_constraint(relation["constraint"], f"{field}[{index}].constraint")))
    relations.sort()
    if len({identifier_value for identifier_value, _ in relations}) != len(relations):
        raise PackageError(f"{field} contains a duplicate package identifier")
    encoded = bytearray()
    report: list[dict[str, str]] = []
    for identifier_value, constraint in relations:
        record = bytearray(V3_RELATION_RECORD_BYTES)
        record[:64] = text_field(identifier_value, 64, field, required=True)
        record[64:120] = text_field(constraint, 56, field, required=True)
        encoded.extend(record)
        report.append({"identifier": identifier_value, "constraint": constraint})
    return bytes(encoded), report


def _v3_file_records(files: tuple[dict[str, Any], ...],
                     payload_offset: int) -> tuple[bytes, bytes, list[dict[str, Any]]]:
    if not files or len(files) > V3_MAX_FILES:
        raise PackageError(f"files must contain 1-{V3_MAX_FILES} entries")
    normalized: list[tuple[str, str, int, str, bytes]] = []
    for index, file_value in enumerate(files):
        if not isinstance(file_value, dict):
            raise PackageError(f"files[{index}] must be an object")
        allowed = {"path", "kind", "mode", "soname", "payload"}
        if set(file_value) - allowed or not {"path", "kind", "payload"} <= set(file_value):
            raise PackageError(
                f"files[{index}] has missing or unknown file metadata")
        path = package_path(file_value["path"], f"files[{index}].path")
        kind = file_value["kind"]
        if not isinstance(kind, str) or kind not in V3_FILE_KINDS:
            raise PackageError(f"files[{index}].kind is invalid")
        payload = file_value["payload"]
        if not isinstance(payload, bytes) or not payload or len(payload) > V3_MAX_FILE_BYTES:
            raise PackageError(
                f"files[{index}].payload is empty or exceeds {V3_MAX_FILE_BYTES} bytes")
        default_mode = 0o555 if kind == "executable" else 0o444
        mode = file_value.get("mode", default_mode)
        if mode not in (0o444, 0o555) or (kind == "executable" and mode != 0o555):
            raise PackageError(f"files[{index}].mode is not a supported canonical mode")
        soname_value = file_value.get("soname", "")
        if kind == "library":
            soname_value = soname(soname_value, f"files[{index}].soname")
        elif soname_value:
            raise PackageError(f"files[{index}].soname is only valid for libraries")
        normalized.append((path, kind, mode, soname_value, payload))
    normalized.sort(key=lambda item: item[0].encode("ascii"))
    if len({item[0] for item in normalized}) != len(normalized):
        raise PackageError("files contains a duplicate path")
    if any(current[0].startswith(previous[0] + "/")
           for previous, current in zip(normalized, normalized[1:])):
        raise PackageError("a packaged file cannot be another file's ancestor")

    records = bytearray()
    payloads = bytearray()
    report: list[dict[str, Any]] = []
    cursor = payload_offset
    for path, kind, mode, soname_value, payload in normalized:
        digest = hashlib.sha256(payload).digest()
        record = bytearray(V3_FILE_RECORD_BYTES)
        record[:128] = text_field(path, 128, "file path", required=True)
        struct.pack_into("<HHIQQ", record, 128, V3_FILE_KINDS[kind], 0, mode,
                         cursor, len(payload))
        record[152:184] = digest
        record[184:248] = text_field(soname_value, 64, "soname", required=False)
        records.extend(record)
        payloads.extend(payload)
        report.append({"path": path, "kind": kind, "mode": mode,
                       "soname": soname_value, "bytes": len(payload),
                       "sha256": digest.hex().upper()})
        cursor += len(payload)
    return bytes(records), bytes(payloads), report


def build_package_v3(spec: dict[str, Any], files: tuple[dict[str, Any], ...],
                     signing_key: bytes) -> bytes:
    """Build a canonical format-v3 package signed with a real Ed25519 key."""
    if spec.get("format", 3) != 3:
        raise PackageError("format-v3 specification has a conflicting format value")
    architecture = canonical_printable(
        spec.get("architecture"), 16, "architecture")
    if architecture not in V3_ARCHITECTURES:
        raise PackageError("architecture must be x86_64")
    abi_min = spec.get("abi_min")
    abi_max = spec.get("abi_max")
    if type(abi_min) is not int or type(abi_max) is not int \
            or not 1 <= abi_min <= abi_max <= 0xffffffff:
        raise PackageError("abi_min and abi_max must be an ordered positive uint32 range")
    identifier_value = package_identifier(spec.get("identifier"), "identifier")
    name = canonical_printable(spec.get("name"), 64, "name")
    version = semantic_version(spec.get("version"), "version")
    publisher = canonical_printable(spec.get("publisher"), 64, "publisher")
    capability_bits = _capability_bits(spec.get("capabilities", []))
    dependency_records, dependencies = _v3_relation_records(
        spec.get("dependencies", []), "dependencies")
    conflict_records, conflicts = _v3_relation_records(
        spec.get("conflicts", []), "conflicts")
    if ({item["identifier"] for item in dependencies} &
            {item["identifier"] for item in conflicts}):
        raise PackageError("a package cannot be both a dependency and a conflict")

    file_table_offset = V3_HEADER_BYTES
    dependency_table_offset = file_table_offset + len(files) * V3_FILE_RECORD_BYTES
    conflict_table_offset = dependency_table_offset + len(dependency_records)
    payload_offset = conflict_table_offset + len(conflict_records)
    file_records, payloads, _ = _v3_file_records(files, payload_offset)
    total_bytes = payload_offset + len(payloads)
    if total_bytes > V3_MAX_PACKAGE_BYTES:
        raise PackageError("format-v3 package exceeds its 256 MiB bound")

    public_key = _ed25519_public_bytes_from_private(signing_key)
    key_id = hashlib.sha256(public_key).digest()
    declared_key_id = spec.get("publisher_key_id")
    if declared_key_id is not None:
        if not isinstance(declared_key_id, str) or declared_key_id.lower() != key_id.hex():
            raise PackageError("publisher_key_id does not match the Ed25519 public key")

    tables_and_payload = file_records + dependency_records + conflict_records + payloads
    header = bytearray(V3_HEADER_BYTES)
    header[:8] = PACKAGE_MAGIC
    struct.pack_into("<HHI", header, 8, 3, V3_HEADER_BYTES, 0)
    struct.pack_into("<QQII", header, 16, total_bytes, file_table_offset,
                     len(files), V3_FILE_RECORD_BYTES)
    struct.pack_into("<QII", header, 40, dependency_table_offset,
                     len(dependencies), V3_RELATION_RECORD_BYTES)
    struct.pack_into("<QII", header, 56, conflict_table_offset,
                     len(conflicts), V3_RELATION_RECORD_BYTES)
    struct.pack_into("<QQII", header, 72, payload_offset, len(payloads),
                     abi_min, abi_max)
    header[96:112] = text_field(architecture, 16, "architecture", required=True)
    header[112:176] = text_field(identifier_value, 64, "identifier", required=True)
    header[176:240] = text_field(name, 64, "name", required=True)
    header[240:304] = text_field(version, 64, "version", required=True)
    header[304:368] = text_field(publisher, 64, "publisher", required=True)
    struct.pack_into("<Q", header, 368, capability_bits)
    header[376:408] = hashlib.sha256(tables_and_payload).digest()
    header[408:440] = key_id
    struct.pack_into("<HH", header, 504, V3_SIGNATURE_ALGORITHM_ED25519,
                     V3_SIGNATURE_BYTES)
    unsigned = bytes(header) + tables_and_payload
    signature = _ed25519_private(signing_key).sign(unsigned)
    if len(signature) != V3_SIGNATURE_BYTES:
        raise PackageError("Ed25519 backend returned an invalid signature length")
    header[V3_SIGNATURE_OFFSET:V3_SIGNATURE_OFFSET + V3_SIGNATURE_BYTES] = signature
    return bytes(header) + tables_and_payload


def _parse_legacy_package(package: bytes) -> tuple[
        bytes, bytes, tuple[tuple[str, bytes], ...], dict[str, Any]]:
    if len(package) < PACKAGE_HEADER_BYTES + MANIFEST_BYTES or package[:8] != PACKAGE_MAGIC:
        raise PackageError("package is truncated or has invalid magic")
    version, header_bytes, manifest_bytes, executable_bytes = struct.unpack_from(
        "<HHIQ", package, 8)
    if version not in (1, 2) or header_bytes != PACKAGE_HEADER_BYTES \
            or manifest_bytes != MANIFEST_BYTES:
        raise PackageError("package header version, size, or reserved bytes are invalid")
    resource_count, resource_reserved, resource_bytes = struct.unpack_from(
        "<HHI", package, 24)
    if version == 1 and (resource_count or resource_reserved or resource_bytes):
        raise PackageError("version 1 package resource fields are nonzero")
    if version == 2 and (resource_count == 0 or resource_reserved != 0):
        raise PackageError("version 2 resource count or reserved field is invalid")
    if resource_count > MAX_PACKAGE_RESOURCES:
        raise PackageError("package resource count exceeds the directory bound")
    expected = (PACKAGE_HEADER_BYTES + manifest_bytes + executable_bytes +
                resource_bytes)
    if expected != len(package):
        raise PackageError("package length does not match its header")
    body = package[PACKAGE_HEADER_BYTES:]
    if hashlib.sha256(body).digest() != package[32:64]:
        raise PackageError("package body SHA-256 mismatch")
    manifest = body[:manifest_bytes]
    executable_end = manifest_bytes + executable_bytes
    executable = body[manifest_bytes:executable_end]
    cursor = executable_end
    resources: list[tuple[str, bytes]] = []
    occupied: set[str] = set()
    for index in range(resource_count):
        if cursor + RESOURCE_HEADER_BYTES > len(body):
            raise PackageError(f"resource {index} header is truncated")
        header = body[cursor:cursor + RESOURCE_HEADER_BYTES]
        cursor += RESOURCE_HEADER_BYTES
        path = decode_text(header[:16], f"resources[{index}].path")
        short_path(path, f"resources[{index}].path", required=True)
        length = struct.unpack_from("<Q", header, 16)[0]
        if any(header[24:]) or length == 0 or length > 16 * 1024 * 1024 \
                or path in occupied or cursor + length > len(body):
            raise PackageError(f"resource {index} record is invalid")
        occupied.add(path)
        resources.append((path, body[cursor:cursor + length]))
        cursor += length
    if cursor != len(body):
        raise PackageError("resource records do not consume the package body")
    report = inspect_manifest(manifest, executable)
    if resources and not report["resource_directory"]:
        raise PackageError("packaged resources require resource_directory")
    inspect_dynamic_catalog(report, tuple(resources))
    report["package_format"] = version
    report["resources"] = [
        {"path": path, "bytes": len(payload),
         "sha256": hashlib.sha256(payload).hexdigest().upper()}
        for path, payload in resources
    ]
    return manifest, executable, tuple(resources), report


def _v3_decode_relation_records(package: bytes, offset: int, count: int,
                                field: str) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    previous: tuple[str, str] | None = None
    identifiers: set[str] = set()
    for index in range(count):
        start = offset + index * V3_RELATION_RECORD_BYTES
        record = package[start:start + V3_RELATION_RECORD_BYTES]
        identifier_value = decode_text(record[:64], f"{field}[{index}].identifier")
        constraint = decode_text(record[64:120], f"{field}[{index}].constraint")
        if package_identifier(identifier_value, f"{field}[{index}].identifier") \
                != identifier_value or version_constraint(
                    constraint, f"{field}[{index}].constraint") != constraint:
            raise PackageError(f"{field}[{index}] is not canonical")
        if any(record[120:]) or identifier_value in identifiers:
            raise PackageError(f"{field}[{index}] has nonzero reserved bytes or duplicates")
        key = (identifier_value, constraint)
        if previous is not None and key <= previous:
            raise PackageError(f"{field} records are not in canonical order")
        previous = key
        identifiers.add(identifier_value)
        result.append({"identifier": identifier_value, "constraint": constraint})
    return result


def _v3_trusted_key(key_id: bytes, trusted_keys: dict[str, bytes] | None) -> Any:
    if not trusted_keys:
        raise PackageError(
            f"format-v3 package requires trusted Ed25519 key {key_id.hex().upper()}")
    normalized: dict[bytes, Any] = {}
    for supplied_id, material in trusted_keys.items():
        if not isinstance(supplied_id, str) or not isinstance(material, bytes):
            raise PackageError("trusted keys must map hexadecimal key IDs to key bytes")
        try:
            supplied = bytes.fromhex(supplied_id)
        except ValueError as error:
            raise PackageError("trusted Ed25519 key ID is not hexadecimal") from error
        if len(supplied) != 32:
            raise PackageError("trusted Ed25519 key ID must be 32 bytes")
        key, raw = _ed25519_public(material)
        derived = hashlib.sha256(raw).digest()
        if supplied != derived:
            raise PackageError("trusted Ed25519 key ID does not match its public key")
        if supplied in normalized:
            raise PackageError("duplicate trusted Ed25519 key ID")
        normalized[supplied] = key
    if key_id not in normalized:
        raise PackageError(f"untrusted Ed25519 key ID: {key_id.hex().upper()}")
    return normalized[key_id]


def _parse_package_v3(package: bytes, trusted_keys: dict[str, bytes] | None) -> tuple[
        bytes, bytes, tuple[tuple[str, bytes], ...], dict[str, Any]]:
    if len(package) < V3_HEADER_BYTES or len(package) > V3_MAX_PACKAGE_BYTES:
        raise PackageError("format-v3 package is truncated or exceeds 256 MiB")
    version, header_bytes, flags = struct.unpack_from("<HHI", package, 8)
    total_bytes, file_table_offset, file_count, file_record_bytes = struct.unpack_from(
        "<QQII", package, 16)
    dependency_table_offset, dependency_count, dependency_record_bytes = struct.unpack_from(
        "<QII", package, 40)
    conflict_table_offset, conflict_count, conflict_record_bytes = struct.unpack_from(
        "<QII", package, 56)
    payload_offset, payload_bytes, abi_min, abi_max = struct.unpack_from(
        "<QQII", package, 72)
    if version != 3 or header_bytes != V3_HEADER_BYTES or flags != 0 \
            or total_bytes != len(package):
        raise PackageError("format-v3 header version, size, flags, or total length is invalid")
    if not 1 <= file_count <= V3_MAX_FILES \
            or dependency_count > V3_MAX_RELATIONS \
            or conflict_count > V3_MAX_RELATIONS:
        raise PackageError("format-v3 table count exceeds its bound")
    if file_record_bytes != V3_FILE_RECORD_BYTES \
            or dependency_record_bytes != V3_RELATION_RECORD_BYTES \
            or conflict_record_bytes != V3_RELATION_RECORD_BYTES:
        raise PackageError("format-v3 record size is invalid")
    expected_dependency = V3_HEADER_BYTES + file_count * V3_FILE_RECORD_BYTES
    expected_conflict = expected_dependency + dependency_count * V3_RELATION_RECORD_BYTES
    expected_payload = expected_conflict + conflict_count * V3_RELATION_RECORD_BYTES
    if (file_table_offset, dependency_table_offset, conflict_table_offset,
            payload_offset) != (V3_HEADER_BYTES, expected_dependency,
                                expected_conflict, expected_payload):
        raise PackageError("format-v3 tables are not canonically contiguous")
    if payload_bytes != len(package) - payload_offset:
        raise PackageError("format-v3 payload length is invalid")
    if not 1 <= abi_min <= abi_max <= 0xffffffff:
        raise PackageError("format-v3 ABI range is invalid")
    signature_algorithm, signature_bytes = struct.unpack_from("<HH", package, 504)
    if signature_algorithm != V3_SIGNATURE_ALGORITHM_ED25519 \
            or signature_bytes != V3_SIGNATURE_BYTES or any(package[508:512]):
        raise PackageError(
            "format-v3 signature algorithm, length, or reserved bytes are invalid")

    architecture = decode_text(package[96:112], "architecture")
    if architecture not in V3_ARCHITECTURES:
        raise PackageError("format-v3 architecture is invalid")
    identifier_value = decode_text(package[112:176], "identifier")
    if package_identifier(identifier_value, "identifier") != identifier_value:
        raise PackageError("format-v3 identifier is not canonical")
    name = decode_argument(package[176:240], "name")
    canonical_printable(name, 64, "name")
    version_value = decode_text(package[240:304], "version")
    if semantic_version(version_value, "version") != version_value:
        raise PackageError("format-v3 version is not canonical")
    publisher = decode_argument(package[304:368], "publisher")
    canonical_printable(publisher, 64, "publisher")
    capabilities = struct.unpack_from("<Q", package, 368)[0]
    if capabilities & ~sum(CAPABILITIES.values()):
        raise PackageError("format-v3 capability bits are invalid")
    if package[376:408] != hashlib.sha256(package[V3_HEADER_BYTES:]).digest():
        raise PackageError("format-v3 content SHA-256 mismatch")

    dependencies = _v3_decode_relation_records(
        package, dependency_table_offset, dependency_count, "dependencies")
    conflicts = _v3_decode_relation_records(
        package, conflict_table_offset, conflict_count, "conflicts")
    if ({item["identifier"] for item in dependencies} &
            {item["identifier"] for item in conflicts}):
        raise PackageError("format-v3 dependency and conflict sets overlap")

    files: list[dict[str, Any]] = []
    previous_path: bytes | None = None
    paths: set[str] = set()
    expected_file_payload = payload_offset
    for index in range(file_count):
        start = file_table_offset + index * V3_FILE_RECORD_BYTES
        record = package[start:start + V3_FILE_RECORD_BYTES]
        path = decode_text(record[:128], f"files[{index}].path")
        if package_path(path, f"files[{index}].path") != path:
            raise PackageError(f"files[{index}].path is not canonical")
        path_bytes = path.encode("ascii")
        if (previous_path is not None and
                (path_bytes <= previous_path or
                 path_bytes.startswith(previous_path + b"/"))) or path in paths:
            raise PackageError("format-v3 file records are not uniquely sorted")
        previous_path = path_bytes
        paths.add(path)
        kind_value, file_flags, mode, offset, length = struct.unpack_from(
            "<HHIQQ", record, 128)
        if kind_value not in V3_FILE_KIND_NAMES or file_flags != 0 \
                or mode not in (0o444, 0o555) or length == 0 \
                or length > V3_MAX_FILE_BYTES or offset != expected_file_payload \
                or offset + length > len(package):
            raise PackageError(f"files[{index}] metadata or payload range is invalid")
        kind = V3_FILE_KIND_NAMES[kind_value]
        if kind == "executable" and mode != 0o555:
            raise PackageError(f"files[{index}] executable mode is invalid")
        digest = record[152:184]
        file_payload = package[offset:offset + length]
        if digest != hashlib.sha256(file_payload).digest():
            raise PackageError(f"files[{index}] SHA-256 mismatch")
        soname_value = decode_text(record[184:248], f"files[{index}].soname")
        if kind == "library":
            if soname(soname_value, f"files[{index}].soname") != soname_value:
                raise PackageError(f"files[{index}].soname is not canonical")
        elif soname_value:
            raise PackageError(f"files[{index}] non-library SONAME is nonempty")
        if any(record[248:]):
            raise PackageError(f"files[{index}] reserved bytes are nonzero")
        files.append({"path": path, "kind": kind, "mode": mode,
                      "soname": soname_value, "bytes": length,
                      "sha256": digest.hex().upper()})
        expected_file_payload += length
    if expected_file_payload != len(package):
        raise PackageError("format-v3 file payloads do not consume the package")

    signature = package[V3_SIGNATURE_OFFSET:V3_SIGNATURE_OFFSET + V3_SIGNATURE_BYTES]
    if not any(signature):
        raise PackageError("format-v3 Ed25519 signature is missing")
    key_id = package[408:440]
    trusted_key = _v3_trusted_key(key_id, trusted_keys)
    signed = bytearray(package)
    signed[V3_SIGNATURE_OFFSET:V3_SIGNATURE_OFFSET + V3_SIGNATURE_BYTES] = bytes(
        V3_SIGNATURE_BYTES)
    try:
        trusted_key.verify(signature, bytes(signed))
    except Exception as error:
        raise PackageError("format-v3 Ed25519 signature verification failed") from error

    names = [capability for capability, bit in CAPABILITIES.items()
             if capabilities & bit]
    report = {
        "package_format": 3,
        "architecture": architecture,
        "abi_min": abi_min,
        "abi_max": abi_max,
        "identifier": identifier_value,
        "name": name,
        "version": version_value,
        "publisher": publisher,
        "capabilities": names,
        "dependencies": dependencies,
        "conflicts": conflicts,
        "files": files,
        "content_sha256": package[376:408].hex().upper(),
        "signature": {"algorithm": "Ed25519", "verified": True,
                      "key_id": key_id.hex().upper()},
    }
    return b"", b"", (), report


def parse_package(package: bytes, *, trusted_keys: dict[str, bytes] | None = None) -> tuple[
        bytes, bytes, tuple[tuple[str, bytes], ...], dict[str, Any]]:
    if len(package) < 12 or package[:8] != PACKAGE_MAGIC:
        raise PackageError("package is truncated or has invalid magic")
    version = struct.unpack_from("<H", package, 8)[0]
    if version == 3:
        return _parse_package_v3(package, trusted_keys)
    return _parse_legacy_package(package)


def command_build(args: argparse.Namespace) -> None:
    spec_path = Path(args.spec)
    spec_value = json.loads(read_regular(spec_path).decode("utf-8"))
    if not isinstance(spec_value, dict):
        raise PackageError("package specification must be one JSON object")
    spec_format = spec_value.get("format")
    if args.format is not None and spec_format is not None \
            and args.format != spec_format:
        raise PackageError("--format conflicts with the package specification")
    requested_format = args.format if args.format is not None else spec_format
    if requested_format == 3:
        if not args.signing_key:
            raise PackageError("format-v3 build requires --signing-key")
        if args.executable:
            raise PackageError("format-v3 files come from spec.files, not --executable")
        if "resources" in spec_value:
            raise PackageError("format-v3 resources must be entries in spec.files")
        file_specs = spec_value.get("files")
        if not isinstance(file_specs, list):
            raise PackageError("format-v3 files must be a list")
        files: list[dict[str, Any]] = []
        for index, file_value in enumerate(file_specs):
            required = {"path", "kind", "source"}
            allowed = required | {"mode", "soname"}
            if not isinstance(file_value, dict) or not required <= set(file_value) \
                    or set(file_value) - allowed:
                raise PackageError(
                    f"files[{index}] must contain path, kind, source, and optional mode/soname")
            source = file_value["source"]
            if not isinstance(source, str) or not source:
                raise PackageError(f"files[{index}].source must be a path")
            converted = {key: value for key, value in file_value.items()
                         if key != "source"}
            converted["payload"] = read_regular_bounded(
                spec_path.parent / source, V3_MAX_FILE_BYTES,
                f"files[{index}].source")
            files.append(converted)
        signing_key = read_regular_bounded(
            Path(args.signing_key), 64 * 1024, "Ed25519 private key")
        package = build_package_v3(spec_value, tuple(files), signing_key)
        public_key = _ed25519_public_bytes_from_private(signing_key)
        _, _, _, report = parse_package(package, trusted_keys={
            hashlib.sha256(public_key).hexdigest(): public_key})
    else:
        if requested_format not in (None, 1, 2):
            raise PackageError("package format must be 1, 2, or 3")
        if args.signing_key:
            raise PackageError("--signing-key is only valid for format 3")
        if not args.executable:
            raise PackageError("legacy package build requires --executable")
        executable = read_regular_bounded(
            Path(args.executable), 16 * 1024 * 1024, "executable")
        resource_specs = spec_value.get("resources", [])
        if not isinstance(resource_specs, list):
            raise PackageError("resources must be a list")
        resources: list[tuple[str, bytes]] = []
        for index, resource in enumerate(resource_specs):
            if not isinstance(resource, dict) or set(resource) != {"path", "source"}:
                raise PackageError(f"resources[{index}] must contain path and source")
            source = resource["source"]
            if not isinstance(source, str) or not source:
                raise PackageError(f"resources[{index}].source must be a path")
            resources.append((resource["path"], read_regular_bounded(
                spec_path.parent / source, 16 * 1024 * 1024,
                f"resources[{index}].source")))
        if requested_format == 1 and resources:
            raise PackageError("format 1 cannot contain resources")
        if requested_format == 2 and not resources:
            raise PackageError("format 2 requires at least one resource")
        package = build_package(spec_value, executable, tuple(resources))
        _, _, _, report = parse_package(package)
    atomic_write(Path(args.output), package)
    print(json.dumps({"output": str(Path(args.output)),
                      "package_sha256": hashlib.sha256(package).hexdigest().upper(),
                      **report}, sort_keys=True))


def command_inspect(args: argparse.Namespace) -> None:
    package = read_regular_bounded(
        Path(args.package), V3_MAX_PACKAGE_BYTES, "package")
    _, _, _, report = parse_package(
        package, trusted_keys=trusted_ed25519_keys(args.trusted_key))
    print(json.dumps({"package": str(Path(args.package)),
                      "package_sha256": hashlib.sha256(package).hexdigest().upper(),
                      **report}, indent=2, sort_keys=True))


def command_install(args: argparse.Namespace) -> None:
    legacy_paths = (args.echo, args.uname, args.cat)
    if any(legacy_paths) and not all(legacy_paths):
        raise PackageError("legacy echo, uname, and cat inputs are all-or-none")
    busyboxes = tuple(read_regular(Path(path)) for path in legacy_paths if path)
    extras: list[tuple[str, bytes]] = []
    identifiers: set[str] = set()
    trusted_keys = trusted_ed25519_keys(args.trusted_key)
    for path in args.packages:
        manifest, executable, resources, report = parse_package(
            read_regular_bounded(Path(path), V3_MAX_PACKAGE_BYTES, "package"),
            trusted_keys=trusted_keys)
        if report["package_format"] == 3:
            raise PackageError(
                "format-v3 packages cannot be installed into the legacy FAT32 System image")
        app_id = str(report["identifier"])
        if app_id in identifiers:
            raise PackageError(f"duplicate package identifier: {app_id}")
        identifiers.add(app_id)
        extras.append((app_id + ".MAN", manifest))
        extras.append((str(report["executable"]), executable))
        resource_directory = str(report["resource_directory"])
        for resource_path, payload in resources:
            extras.append((resource_directory + "/" + resource_path, payload))
    image = fat32_image.build_image("system", busyboxes, tuple(extras))
    fat32_image.verify_system(image, busyboxes, tuple(extras))
    atomic_write(Path(args.output), image)
    print(json.dumps({"output": str(Path(args.output)),
                      "packages": sorted(identifiers),
                      "sha256": hashlib.sha256(image).hexdigest().upper()}, sort_keys=True))


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    builder = commands.add_parser("build")
    builder.add_argument("--spec", required=True)
    builder.add_argument("--executable")
    builder.add_argument("--format", type=int, choices=(1, 2, 3))
    builder.add_argument("--signing-key")
    builder.add_argument("--output", required=True)
    builder.set_defaults(function=command_build)
    inspector = commands.add_parser("inspect")
    inspector.add_argument("--trusted-key", action="append", default=[])
    inspector.add_argument("package")
    inspector.set_defaults(function=command_inspect)
    installer = commands.add_parser("install-system")
    installer.add_argument("--echo")
    installer.add_argument("--uname")
    installer.add_argument("--cat")
    installer.add_argument("--trusted-key", action="append", default=[])
    installer.add_argument("--output", required=True)
    installer.add_argument("packages", nargs="+")
    installer.set_defaults(function=command_install)
    return result


def main() -> int:
    try:
        args = parser().parse_args()
        args.function(args)
    except (PackageError, fat32_image.Fat32Error, OSError, UnicodeError,
            json.JSONDecodeError, struct.error) as error:
        print(f"Phipia package refused: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
