#!/usr/bin/env python3
"""Host tests for signed repository indexes and deterministic resolution."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import os
from pathlib import Path
import sys
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "phipia_repository", ROOT / "tools" / "phipia-repository.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load repository tool")
REPOSITORY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPOSITORY)

GENERATED = 1_800_000_000
EXPIRES = GENERATED + 86_400
NOW = GENERATED + 60
PUBLISHER_KEY_ID = hashlib.sha256(b"test publisher public key").hexdigest()


def expect_refusal(action: Callable[[], Any], contains: str) -> None:
    try:
        action()
    except REPOSITORY.RepositoryError as error:
        assert contains in str(error), str(error)
        return
    raise AssertionError(f"expected refusal containing {contains!r}")


def package(identifier: str, version: str, payload: bytes, *,
            dependencies: list[dict[str, str]] | None = None,
            conflicts: list[dict[str, str]] | None = None,
            provides: list[dict[str, str]] | None = None) -> dict[str, Any]:
    return {
        "identifier": identifier,
        "version": version,
        "download_path": f"packages/{identifier}/{version}.spk",
        "bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "publisher_key_id": PUBLISHER_KEY_ID,
        "dependencies": dependencies or [],
        "conflicts": conflicts or [],
        "provides": provides or [],
    }


def specification(packages: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "format": 1,
        "repository": "org.phipia.main",
        "repository_version": 42,
        "generated_at": GENERATED,
        "expires_at": EXPIRES,
        "architecture": "x86_64",
        "abi_min": 1,
        "abi_max": 2,
        "packages": packages,
    }


def main() -> int:
    crypto_available = REPOSITORY.PACKAGE.ed25519_available()
    if os.environ.get("PHIPIA_REQUIRE_ED25519") == "1" and not crypto_available:
        raise AssertionError(
            "Python cryptography with Ed25519 support is required for verification")

    assert REPOSITORY.version_satisfies("1.9.0", "^1.2.3")
    assert not REPOSITORY.version_satisfies("2.0.0", "^1.2.3")
    assert REPOSITORY.version_satisfies("0.2.9", "^0.2.3")
    assert not REPOSITORY.version_satisfies("0.3.0", "^0.2.3")
    assert REPOSITORY.version_satisfies("1.2.9", "~1.2.3")
    assert REPOSITORY.semver_compare("1.0.0", "1.0.0-rc.1") > 0

    libc_old_payload = b"libc version 0.9.0"
    libc_payload = b"libc version 1.0.0"
    renderer_payload = b"software renderer version 2"
    desktop_payload = b"desktop package version 3"
    packages = [
        package("org.phipia.desktop", "3.0.0", desktop_payload,
                dependencies=[
                    {"identifier": "virtual.renderer",
                     "constraint": ">=1.0.0,<2.0.0"},
                    {"identifier": "org.phipia.libc", "constraint": "^1.0.0"},
                ]),
        package("org.phipia.libc", "0.9.0", libc_old_payload),
        package("org.phipia.render-soft", "2.0.0", renderer_payload,
                provides=[{"identifier": "virtual.renderer", "version": "1.5.0"}]),
        package("org.phipia.libc", "1.0.0", libc_payload),
    ]
    repository_spec = specification(packages)
    private_seed = bytes(range(32))
    if not crypto_available:
        expect_refusal(
            lambda: REPOSITORY.build_repository(repository_spec, private_seed),
            "real Ed25519 support is unavailable")
        print("Phipia repository host tests passed: Ed25519 unavailable and refused closed")
        return 0

    first = REPOSITORY.build_repository(repository_spec, private_seed)
    assert hashlib.sha256(first).hexdigest().upper() == (
        "3B0539202AB65C5A3D21F3BA0C366F38734C87DE0DBAECA99A5EC3BBB2506F7F")
    reordered = copy.deepcopy(repository_spec)
    reordered["packages"].reverse()
    reordered["packages"][0]["dependencies"].reverse()
    second = REPOSITORY.build_repository(reordered, private_seed)
    assert first == second
    public_key = REPOSITORY.PACKAGE._ed25519_public_bytes_from_private(private_seed)
    root_key_id = hashlib.sha256(public_key).hexdigest()
    trusted = {root_key_id: public_key}
    report = REPOSITORY.parse_repository(
        first, trusted_root_keys=trusted, now=NOW, minimum_repository_version=42)
    assert report["repository"] == "org.phipia.main"
    assert report["repository_version"] == 42
    assert report["signature"] == {
        "algorithm": "Ed25519", "verified": True,
        "root_key_id": root_key_id.upper(),
    }
    assert [(item["identifier"], item["version"]) for item in report["packages"]] == [
        ("org.phipia.desktop", "3.0.0"),
        ("org.phipia.libc", "0.9.0"),
        ("org.phipia.libc", "1.0.0"),
        ("org.phipia.render-soft", "2.0.0"),
    ]

    expect_refusal(
        lambda: REPOSITORY.parse_repository(first, trusted_root_keys=None, now=NOW),
        "requires immutable root key")
    other_public = REPOSITORY.PACKAGE._ed25519_public_bytes_from_private(
        bytes(range(1, 33)))
    other_key_id = hashlib.sha256(other_public).hexdigest()
    expect_refusal(
        lambda: REPOSITORY.parse_repository(
            first, trusted_root_keys={other_key_id: other_public}, now=NOW),
        "unknown repository root key")
    expect_refusal(
        lambda: REPOSITORY.parse_repository(first, trusted_root_keys=trusted,
                                            now=EXPIRES),
        "expired")
    expect_refusal(
        lambda: REPOSITORY.parse_repository(first, trusted_root_keys=trusted,
                                            now=GENERATED - 1),
        "not yet valid")
    expect_refusal(
        lambda: REPOSITORY.parse_repository(
            first, trusted_root_keys=trusted, now=NOW,
            minimum_repository_version=43),
        "older than the immutable minimum")
    changed = bytearray(first)
    changed[-1] ^= 1
    expect_refusal(
        lambda: REPOSITORY.parse_repository(bytes(changed),
                                            trusted_root_keys=trusted, now=NOW),
        "content SHA-256 mismatch")
    changed = bytearray(first)
    changed[REPOSITORY.INDEX_SIGNATURE_OFFSET] ^= 1
    expect_refusal(
        lambda: REPOSITORY.parse_repository(bytes(changed),
                                            trusted_root_keys=trusted, now=NOW),
        "signature verification failed")
    changed = bytearray(first)
    changed[300] = 1
    expect_refusal(
        lambda: REPOSITORY.parse_repository(bytes(changed),
                                            trusted_root_keys=trusted, now=NOW),
        "reserved bytes")

    def resign(changed_index: bytearray) -> bytes:
        changed_index[168:200] = hashlib.sha256(
            changed_index[REPOSITORY.INDEX_HEADER_BYTES:]).digest()
        changed_index[REPOSITORY.INDEX_SIGNATURE_OFFSET:
                      REPOSITORY.INDEX_SIGNATURE_OFFSET +
                      REPOSITORY.INDEX_SIGNATURE_BYTES] = bytes(
                          REPOSITORY.INDEX_SIGNATURE_BYTES)
        signature = REPOSITORY.PACKAGE._ed25519_private(private_seed).sign(
            bytes(changed_index))
        changed_index[REPOSITORY.INDEX_SIGNATURE_OFFSET:
                      REPOSITORY.INDEX_SIGNATURE_OFFSET +
                      REPOSITORY.INDEX_SIGNATURE_BYTES] = signature
        return bytes(changed_index)

    changed = bytearray(first)
    changed[REPOSITORY.INDEX_HEADER_BYTES + 352] = 1
    expect_refusal(
        lambda: REPOSITORY.parse_repository(resign(changed),
                                            trusted_root_keys=trusted, now=NOW),
        "reserved bytes")
    changed = bytearray(first)
    relation_offset = int.from_bytes(changed[152:160], "little")
    changed[relation_offset + 120] = 1
    expect_refusal(
        lambda: REPOSITORY.parse_repository(resign(changed),
                                            trusted_root_keys=trusted, now=NOW),
        "reserved bytes")
    duplicate_spec = specification([packages[0], copy.deepcopy(packages[0])])
    expect_refusal(
        lambda: REPOSITORY.build_repository(duplicate_spec, private_seed),
        "duplicate package identity/version")

    plan, lock = REPOSITORY.resolve_repository(
        report, ["org.phipia.desktop@^3.0.0"], architecture="x86_64",
        abi_version=1, max_packages=8)
    assert [(item["identifier"], item["version"]) for item in plan] == [
        ("org.phipia.libc", "1.0.0"),
        ("org.phipia.render-soft", "2.0.0"),
        ("org.phipia.desktop", "3.0.0"),
    ]
    assert lock.endswith(b"\n") and b" " not in lock
    assert hashlib.sha256(lock).hexdigest().upper() == (
        "CC866126192335D8B4798434A4F9E06CF920184AF2F060242FDA0757D9C5616D")
    assert REPOSITORY.resolve_repository(
        report, ["org.phipia.desktop@^3.0.0"], architecture="x86_64",
        abi_version=1, max_packages=8)[1] == lock
    REPOSITORY.verify_download(
        next(item for item in report["packages"]
             if item["identifier"] == "org.phipia.libc" and item["version"] == "1.0.0"),
        libc_payload)
    libc_entry = next(item for item in report["packages"]
                      if item["identifier"] == "org.phipia.libc"
                      and item["version"] == "1.0.0")
    expect_refusal(lambda: REPOSITORY.verify_download(libc_entry, b"wrong"),
                   "download size mismatch")
    wrong_digest_payload = bytearray(libc_payload)
    wrong_digest_payload[-1] ^= 1
    expect_refusal(
        lambda: REPOSITORY.verify_download(libc_entry, bytes(wrong_digest_payload)),
        "download SHA-256 mismatch")
    expect_refusal(
        lambda: REPOSITORY.resolve_repository(
            report, ["org.phipia.desktop"], architecture="aarch64",
            abi_version=1),
        "architecture")
    expect_refusal(
        lambda: REPOSITORY.resolve_repository(
            report, ["org.phipia.desktop"], architecture="x86_64",
            abi_version=3),
        "outside the repository range")

    def parsed_with(extra_packages: list[dict[str, Any]]) -> dict[str, Any]:
        index = REPOSITORY.build_repository(specification(extra_packages), private_seed)
        return REPOSITORY.parse_repository(
            index, trusted_root_keys=trusted, now=NOW)

    unsatisfied_report = parsed_with([
        package("org.phipia.need-missing", "1.0.0", b"missing",
                dependencies=[{"identifier": "org.phipia.absent",
                               "constraint": "*"}]),
    ])
    expect_refusal(
        lambda: REPOSITORY.resolve_repository(
            unsatisfied_report, ["org.phipia.need-missing"],
            architecture="x86_64", abi_version=1),
        "unsatisfied dependency")
    backtracking_report = parsed_with([
        package("org.phipia.app", "1.0.0", b"app one",
                dependencies=[{"identifier": "org.phipia.lib",
                               "constraint": "^1.0.0"}]),
        package("org.phipia.app", "2.0.0", b"app two",
                dependencies=[{"identifier": "org.phipia.lib",
                               "constraint": "^2.0.0"}]),
        package("org.phipia.lib", "1.1.0", b"lib one"),
    ])
    backtracking_plan, _ = REPOSITORY.resolve_repository(
        backtracking_report, ["org.phipia.app"], architecture="x86_64",
        abi_version=1)
    assert [(item["identifier"], item["version"])
            for item in backtracking_plan] == [
                ("org.phipia.lib", "1.1.0"), ("org.phipia.app", "1.0.0")]
    conflict_packages = copy.deepcopy(packages)
    next(item for item in conflict_packages
         if item["identifier"] == "org.phipia.render-soft")["conflicts"] = [
             {"identifier": "org.phipia.desktop", "constraint": "*"}]
    conflict_report = parsed_with(conflict_packages)
    expect_refusal(
        lambda: REPOSITORY.resolve_repository(
            conflict_report, ["org.phipia.desktop"],
            architecture="x86_64", abi_version=1),
        "conflicts with")
    ambiguous_packages = copy.deepcopy(packages)
    ambiguous_packages.append(
        package("org.phipia.render-hw", "1.0.0", b"hardware renderer",
                provides=[{"identifier": "virtual.renderer", "version": "1.4.0"}]))
    ambiguous_report = parsed_with(ambiguous_packages)
    expect_refusal(
        lambda: REPOSITORY.resolve_repository(
            ambiguous_report, ["org.phipia.desktop"],
            architecture="x86_64", abi_version=1),
        "ambiguous providers")
    cycle_report = parsed_with([
        package("org.phipia.a", "1.0.0", b"a",
                dependencies=[{"identifier": "org.phipia.b", "constraint": "*"}]),
        package("org.phipia.b", "1.0.0", b"b",
                dependencies=[{"identifier": "org.phipia.a", "constraint": "*"}]),
    ])
    expect_refusal(
        lambda: REPOSITORY.resolve_repository(
            cycle_report, ["org.phipia.a"], architecture="x86_64", abi_version=1),
        "dependency cycle")
    chain_report = parsed_with([
        package("org.phipia.a", "1.0.0", b"a",
                dependencies=[{"identifier": "org.phipia.b", "constraint": "*"}]),
        package("org.phipia.b", "1.0.0", b"b",
                dependencies=[{"identifier": "org.phipia.c", "constraint": "*"}]),
        package("org.phipia.c", "1.0.0", b"c"),
    ])
    expect_refusal(
        lambda: REPOSITORY.resolve_repository(
            chain_report, ["org.phipia.a"], architecture="x86_64",
            abi_version=1, max_packages=2),
        "graph exceeds its 2-package bound")

    print("Phipia repository host tests passed: canonical signature, freshness, digests, resolver refusals, exact lock")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
