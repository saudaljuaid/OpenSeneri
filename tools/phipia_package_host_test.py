#!/usr/bin/env python3
"""Host refusal and reproducibility tests for the Phipia package format."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "phipia_package", ROOT / "tools" / "phipia-package.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load package tool")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


def expect_refusal(package: bytes, *, trusted_keys: dict[str, bytes] | None = None,
                   contains: str | None = None) -> None:
    try:
        PACKAGE.parse_package(package, trusted_keys=trusted_keys)
    except PACKAGE.PackageError as error:
        if contains is not None:
            assert contains in str(error), str(error)
        return
    raise AssertionError("malformed package was accepted")


def main() -> int:
    crypto_available = PACKAGE.ed25519_available()
    if os.environ.get("PHIPIA_REQUIRE_ED25519") == "1" and not crypto_available:
        raise AssertionError(
            "Python cryptography with Ed25519 support is required for verification")

    executable = b"\x7fELF" + bytes(range(64))
    spec = {
        "name": "Package Test",
        "identifier": "PKGTEST",
        "executable": "PKGTEST.APP",
        "data_namespace": "PKGTEST",
        "memory_limit": 1024 * 1024,
        "max_handles": 32,
        "max_threads": 2,
        "capabilities": ["console", "data-read"],
        "arguments": ["PKGTEST.APP", "http://phipia.test/welcome.txt"],
    }
    first = PACKAGE.build_package(spec, executable)
    second = PACKAGE.build_package(copy.deepcopy(spec), executable)
    assert first == second
    assert hashlib.sha256(first).hexdigest().upper() == (
        "9A5B5FA3CF2BD46E19824D797099F1408051AB31B67AD96454829920E4A0A80B")
    _, parsed_executable, resources, report = PACKAGE.parse_package(first)
    assert parsed_executable == executable
    assert resources == ()
    assert report["identifier"] == "PKGTEST"
    assert report["arguments"] == [
        "PKGTEST.APP", "http://phipia.test/welcome.txt"
    ]
    audio_spec = copy.deepcopy(spec)
    audio_spec["capabilities"] = ["audio"]
    audio_package = PACKAGE.build_package(audio_spec, executable)
    _, _, _, audio_report = PACKAGE.parse_package(audio_package)
    assert audio_report["capabilities"] == ["audio"]
    control_spec = copy.deepcopy(spec)
    control_spec["arguments"] = ["line\nbreak"]
    try:
        PACKAGE.build_package(control_spec, executable)
    except PACKAGE.PackageError:
        pass
    else:
        raise AssertionError("control character in argument was accepted")
    changed = bytearray(first)
    changed[-1] ^= 1
    expect_refusal(bytes(changed))
    changed = bytearray(first)
    changed[24] = 1
    expect_refusal(bytes(changed))
    resource_spec = copy.deepcopy(spec)
    resource_spec["resource_directory"] = "PKGRES"
    resource = b"immutable packaged resource\n"
    resource_package = PACKAGE.build_package(
        resource_spec, executable, (("DATA.TXT", resource),))
    assert hashlib.sha256(resource_package).hexdigest().upper() == (
        "97A293EF34F5AC7DAAB1C861295429614F3F4D2B42C8751E43809DEA008A7333")
    _, _, resources, report = PACKAGE.parse_package(resource_package)
    assert resources == (("DATA.TXT", resource),)
    assert report["package_format"] == 2
    assert report["resource_directory"] == "PKGRES"
    image = PACKAGE.fat32_image.build_image(
        "system", (), (("PKGRES/DATA.TXT", resource),))
    image_report = PACKAGE.fat32_image.verify_system(
        image, (), (("PKGRES/DATA.TXT", resource),))
    assert "PKGRES/DATA.TXT" in {
        item["path"] for item in image_report["files"]
    }

    library = b"\x7fELFauthenticated-dynamic-library"
    catalog = bytearray(2048)
    catalog[:8] = b"PHIPDYN1"
    struct.pack_into("<HHIHH", catalog, 8, 1, 64, 2048, 1, 96)
    catalog[64:74] = b"DYNLIB.SO\0"
    catalog[128:160] = hashlib.sha256(library).digest()
    dynamic_spec = copy.deepcopy(spec)
    dynamic_spec["identifier"] = "DYNROOT"
    dynamic_spec["executable"] = "DYNROOT.APP"
    dynamic_spec["resource_directory"] = "DYN"
    dynamic_spec["dynamic_catalog"] = "DYN/DYNROOT.CAT"
    dynamic_resources = (("DYNLIB.SO", library),
                         ("DYNROOT.CAT", bytes(catalog)))
    dynamic_package = PACKAGE.build_package(
        dynamic_spec, executable, dynamic_resources)
    _, _, resources, report = PACKAGE.parse_package(dynamic_package)
    assert resources == dynamic_resources
    assert report["dynamic_catalog"] == "DYN/DYNROOT.CAT"
    assert report["dynamic_catalog_sha256"] == hashlib.sha256(
        catalog).hexdigest().upper()
    changed_library = bytearray(library)
    changed_library[-1] ^= 1
    expect_refusal(PACKAGE.build_package(
        dynamic_spec, executable,
        (("DYNLIB.SO", bytes(changed_library)),
         ("DYNROOT.CAT", bytes(catalog)))),
        contains="dynamic catalog resource digest")
    changed_catalog = bytearray(catalog)
    changed_catalog[128] ^= 1
    expect_refusal(PACKAGE.build_package(
        dynamic_spec, executable,
        (("DYNLIB.SO", library), ("DYNROOT.CAT", bytes(changed_catalog)))),
        contains="dynamic catalog resource digest")

    v3_spec = {
        "format": 3,
        "architecture": "x86_64",
        "abi_min": 1,
        "abi_max": 2,
        "identifier": "org.phipia.pkgtest",
        "name": "Package v3 Test",
        "version": "1.2.3-rc.1+hosttest",
        "publisher": "Phipia Project",
        "capabilities": ["data-read", "console"],
        "dependencies": [
            {"identifier": "org.phipia.zlib", "constraint": ">=1.2.13,<2.0.0"},
            {"identifier": "org.phipia.libc", "constraint": "^1.0.0"},
        ],
        "conflicts": [
            {"identifier": "org.phipia.pkgtest-old", "constraint": "*"},
        ],
    }
    v3_files = (
        {"path": "share/pkgtest/readme.txt", "kind": "resource",
         "payload": b"package v3 resource\n"},
        {"path": "lib/libpkgtest.so.1", "kind": "library",
         "soname": "libpkgtest.so.1", "payload": b"\x7fELFshared-v3"},
        {"path": "bin/pkgtest", "kind": "executable", "payload": executable},
    )
    private_seed = bytes(range(32))
    if crypto_available:
        v3_first = PACKAGE.build_package_v3(v3_spec, v3_files, private_seed)
        reordered_spec = copy.deepcopy(v3_spec)
        reordered_spec["dependencies"].reverse()
        reordered_spec["capabilities"].reverse()
        v3_second = PACKAGE.build_package_v3(
            reordered_spec, tuple(reversed(v3_files)), private_seed)
        assert v3_first == v3_second
        assert v3_first[:8] == PACKAGE.PACKAGE_MAGIC
        assert int.from_bytes(v3_first[8:10], "little") == 3
        public_key = PACKAGE._ed25519_public_bytes_from_private(private_seed)
        key_id = hashlib.sha256(public_key).hexdigest()
        trusted = {key_id: public_key}

        def resign(changed: bytearray) -> bytes:
            changed[376:408] = hashlib.sha256(
                changed[PACKAGE.V3_HEADER_BYTES:]).digest()
            changed[PACKAGE.V3_SIGNATURE_OFFSET:
                    PACKAGE.V3_SIGNATURE_OFFSET + PACKAGE.V3_SIGNATURE_BYTES] = bytes(
                        PACKAGE.V3_SIGNATURE_BYTES)
            signature = PACKAGE._ed25519_private(private_seed).sign(bytes(changed))
            changed[PACKAGE.V3_SIGNATURE_OFFSET:
                    PACKAGE.V3_SIGNATURE_OFFSET + PACKAGE.V3_SIGNATURE_BYTES] = signature
            return bytes(changed)

        _, parsed_executable, resources, report = PACKAGE.parse_package(
            v3_first, trusted_keys=trusted)
        assert parsed_executable == b"" and resources == ()
        assert report["identifier"] == "org.phipia.pkgtest"
        assert report["version"] == "1.2.3-rc.1+hosttest"
        assert report["architecture"] == "x86_64"
        assert report["abi_min"] == 1 and report["abi_max"] == 2
        assert report["signature"] == {
            "algorithm": "Ed25519", "verified": True, "key_id": key_id.upper()}
        assert [item["path"] for item in report["files"]] == [
            "bin/pkgtest", "lib/libpkgtest.so.1", "share/pkgtest/readme.txt"]
        assert [item["identifier"] for item in report["dependencies"]] == [
            "org.phipia.libc", "org.phipia.zlib"]
        expect_refusal(v3_first, contains="requires trusted Ed25519 key")
        changed = bytearray(v3_first)
        changed[PACKAGE.V3_SIGNATURE_OFFSET] ^= 1
        expect_refusal(bytes(changed), trusted_keys=trusted,
                       contains="signature verification failed")
        changed = bytearray(v3_first)
        changed[508] = 1
        expect_refusal(bytes(changed), trusted_keys=trusted,
                       contains="reserved bytes")
        changed = bytearray(v3_first)
        changed[-1] ^= 1
        expect_refusal(bytes(changed), trusted_keys=trusted,
                       contains="content SHA-256 mismatch")
        expect_refusal(resign(changed), trusted_keys=trusted,
                       contains="SHA-256 mismatch")
        changed = bytearray(v3_first)
        changed[PACKAGE.V3_HEADER_BYTES + 248] = 1
        expect_refusal(resign(changed), trusted_keys=trusted,
                       contains="reserved bytes")
        duplicate_files = v3_files + (dict(v3_files[0]),)
        try:
            PACKAGE.build_package_v3(v3_spec, duplicate_files, private_seed)
        except PACKAGE.PackageError as error:
            assert "duplicate path" in str(error)
        else:
            raise AssertionError("duplicate format-v3 path was accepted")
    else:
        try:
            PACKAGE.build_package_v3(v3_spec, v3_files, private_seed)
        except PACKAGE.PackageError as error:
            assert "real Ed25519 support is unavailable" in str(error)
        else:
            raise AssertionError("format-v3 package was built without real Ed25519 support")

    print("Phipia package host tests passed: legacy/dynamic bytes, v3 canonical tables, bounds, digests, trust, Ed25519 verification/refusal")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
