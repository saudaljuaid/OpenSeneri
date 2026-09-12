#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build real signed fixtures and exercise the bounded C package-manager core."""

from __future__ import annotations

import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "phipia_repository_for_guest_test", ROOT / "tools" / "phipia-repository.py"
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load the Phipia repository tool")
REPOSITORY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPOSITORY)
PACKAGE = REPOSITORY.PACKAGE

GENERATED = 1_800_000_000
EXPIRES = GENERATED + 86_400
NOW = GENERATED + 60
ROOT_SEED = bytes(range(32))
PUBLISHER_SEED = bytes(range(32, 64))


def package_spec(identifier: str, name: str, *,
                 version: str = "1.0.0",
                 dependencies: list[dict[str, str]] | None = None) -> dict[str, Any]:
    return {
        "format": 3,
        "architecture": "x86_64",
        "abi_min": 1,
        "abi_max": 1,
        "identifier": identifier,
        "name": name,
        "version": version,
        "publisher": "Phipia Package Test",
        "capabilities": ["console"],
        "dependencies": dependencies or [],
        "conflicts": [],
    }


def repository_package(identifier: str, version: str, payload: bytes,
                       publisher_key_id: str, *,
                       dependencies: list[dict[str, str]] | None = None,
                       conflicts: list[dict[str, str]] | None = None,
                       provides: list[dict[str, str]] | None = None) -> dict[str, Any]:
    return {
        "identifier": identifier,
        "version": version,
        "download_path": f"packages/{identifier}/{version}.spk",
        "bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "publisher_key_id": publisher_key_id,
        "dependencies": dependencies or [],
        "conflicts": conflicts or [],
        "provides": provides or [],
    }


def repository_spec(packages: list[dict[str, Any]], *,
                    repository_version: int = 42) -> dict[str, Any]:
    return {
        "format": 1,
        "repository": "org.phipia.main",
        "repository_version": repository_version,
        "generated_at": GENERATED,
        "expires_at": EXPIRES,
        "architecture": "x86_64",
        "abi_min": 1,
        "abi_max": 1,
        "packages": packages,
    }


def write(path: Path, payload: bytes) -> str:
    path.write_bytes(payload)
    return str(path)


def main() -> int:
    if len(sys.argv) not in (2, 3):
        raise SystemExit(
            "usage: package_manager_host_test.py MANAGER_TEST "
            "[CONTROL_TEST]"
        )
    if not PACKAGE.ed25519_available():
        if os.environ.get("PHIPIA_REQUIRE_ED25519") == "1":
            raise AssertionError("Python Ed25519 support is required")
        print("Phipia guest package-manager tests skipped: Ed25519 unavailable")
        return 0

    root_public = PACKAGE._ed25519_public_bytes_from_private(ROOT_SEED)
    publisher_public = PACKAGE._ed25519_public_bytes_from_private(PUBLISHER_SEED)
    publisher_key_id = hashlib.sha256(publisher_public).hexdigest()
    dependency = [{
        "identifier": "org.phipia.lib",
        "constraint": ">=1.0.0,<2.0.0",
    }]
    library = PACKAGE.build_package_v3(
        package_spec("org.phipia.lib", "Proof Library"),
        ({"path": "lib/libproof.so.1", "kind": "library",
          "soname": "libproof.so.1", "payload": b"\x7fELFproof-library"},),
        PUBLISHER_SEED,
    )
    application = PACKAGE.build_package_v3(
        package_spec("org.phipia.app", "Proof Application", dependencies=dependency),
        ({"path": "bin/proof-app", "kind": "executable",
          "payload": b"\x7fELFproof-application"},),
        PUBLISHER_SEED,
    )
    main_packages = [
        repository_package("org.phipia.app", "1.0.0", application,
                           publisher_key_id, dependencies=dependency),
        repository_package("org.phipia.lib", "1.0.0", library,
                           publisher_key_id, provides=[{
                               "identifier": "virtual.proof",
                               "version": "1.0.0",
                           }]),
    ]
    main_index = REPOSITORY.build_repository(repository_spec(main_packages), ROOT_SEED)
    replacement_dependency = [{
        "identifier": "org.phipia.newlib",
        "constraint": "^2.0.0",
    }]
    replacement_library = PACKAGE.build_package_v3(
        package_spec("org.phipia.newlib", "Replacement Library", version="2.0.0"),
        ({"path": "lib/libnew.so.2", "kind": "library",
          "soname": "libnew.so.2", "payload": b"\x7fELFreplacement-library"},),
        PUBLISHER_SEED,
    )
    replacement_application = PACKAGE.build_package_v3(
        package_spec("org.phipia.app", "Proof Application", version="2.0.0",
                     dependencies=replacement_dependency),
        ({"path": "bin/proof-app", "kind": "executable",
          "payload": b"\x7fELFupdated-application"},),
        PUBLISHER_SEED,
    )
    update_index = REPOSITORY.build_repository(repository_spec([
        repository_package("org.phipia.app", "2.0.0", replacement_application,
                           publisher_key_id,
                           dependencies=replacement_dependency),
        repository_package("org.phipia.newlib", "2.0.0", replacement_library,
                           publisher_key_id),
    ], repository_version=43), ROOT_SEED)
    trusted_root = {hashlib.sha256(root_public).hexdigest(): root_public}
    trusted_publisher = {publisher_key_id: publisher_public}
    REPOSITORY.parse_repository(
        main_index, trusted_root_keys=trusted_root, now=NOW,
        minimum_repository_version=42,
    )
    PACKAGE.parse_package(application, trusted_keys=trusted_publisher)
    PACKAGE.parse_package(library, trusted_keys=trusted_publisher)

    dummy = lambda name, version=b"payload": repository_package(
        name, "1.0.0", version, publisher_key_id
    )
    cycle = REPOSITORY.build_repository(repository_spec([
        {**dummy("org.phipia.a"), "dependencies": [
            {"identifier": "org.phipia.b", "constraint": "*"}]},
        {**dummy("org.phipia.b"), "dependencies": [
            {"identifier": "org.phipia.a", "constraint": "*"}]},
    ]), ROOT_SEED)
    conflict = REPOSITORY.build_repository(repository_spec([
        {**dummy("org.phipia.conflict-app"), "dependencies": [
            {"identifier": "org.phipia.conflict-lib", "constraint": "*"}]},
        {**dummy("org.phipia.conflict-lib"), "conflicts": [
            {"identifier": "org.phipia.conflict-app", "constraint": "*"}]},
    ]), ROOT_SEED)
    ambiguous = REPOSITORY.build_repository(repository_spec([
        {**dummy("org.phipia.ambiguous-app"), "dependencies": [
            {"identifier": "virtual.renderer", "constraint": "*"}]},
        {**dummy("org.phipia.renderer-a"), "provides": [
            {"identifier": "virtual.renderer", "version": "1.0.0"}]},
        {**dummy("org.phipia.renderer-b"), "provides": [
            {"identifier": "virtual.renderer", "version": "1.0.0"}]},
    ]), ROOT_SEED)
    unsatisfied = REPOSITORY.build_repository(repository_spec([
        {**dummy("org.phipia.unsatisfied"), "dependencies": [
            {"identifier": "org.phipia.missing", "constraint": "*"}]},
    ]), ROOT_SEED)
    backtrack = REPOSITORY.build_repository(repository_spec([
        repository_package("org.phipia.backtrack", "1.0.0", b"old-app",
                           publisher_key_id, dependencies=[
                               {"identifier": "org.phipia.old-lib",
                                "constraint": "^1.0.0"}]),
        repository_package("org.phipia.backtrack", "2.0.0", b"new-app",
                           publisher_key_id, dependencies=[
                               {"identifier": "org.phipia.new-lib",
                                "constraint": "^2.0.0"}]),
        dummy("org.phipia.old-lib"),
    ]), ROOT_SEED)
    chain_packages = []
    for index in range(66):
        identifier = f"org.phipia.chain{index:02d}"
        dependencies = [] if index == 65 else [{
            "identifier": f"org.phipia.chain{index + 1:02d}",
            "constraint": "*",
        }]
        chain_packages.append({**dummy(identifier), "dependencies": dependencies})
    deep_chain = REPOSITORY.build_repository(
        repository_spec(chain_packages), ROOT_SEED
    )

    output_root = ROOT / "build" / "tests"
    output_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="package-manager-", dir=output_root) as temp:
        directory = Path(temp)
        paths = [
            write(directory / "main.sri", main_index),
            write(directory / "root.pub", root_public),
            write(directory / "publisher.pub", publisher_public),
            write(directory / "app.spk", application),
            write(directory / "lib.spk", library),
            write(directory / "cycle.sri", cycle),
            write(directory / "conflict.sri", conflict),
            write(directory / "ambiguous.sri", ambiguous),
            write(directory / "unsatisfied.sri", unsatisfied),
            write(directory / "backtrack.sri", backtrack),
            write(directory / "deep-chain.sri", deep_chain),
            write(directory / "update.sri", update_index),
            write(directory / "app-v2.spk", replacement_application),
            write(directory / "newlib.spk", replacement_library),
        ]
        subprocess.run([sys.argv[1], *paths], check=True)
        if len(sys.argv) == 3:
            subprocess.run(
                [sys.argv[2], *paths[:5], *paths[11:14]], check=True
            )
    print(
        "Phipia guest package-manager host tests passed: real signed bytes, "
        "bounded parser/planner/builder, update pruning, trust refusals"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
