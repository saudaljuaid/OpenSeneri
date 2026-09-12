#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the deterministic signed repository used by the guest phip proof."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


PACKAGE = load("phipia_package_lifecycle", ROOT / "tools" / "phipia-package.py")
REPOSITORY = load(
    "phipia_repository_lifecycle", ROOT / "tools" / "phipia-repository.py"
)

ROOT_SEED = bytes(range(32))
PUBLISHER_SEED = bytes(range(32, 64))
GENERATED = 1_800_000_000
EXPIRES = GENERATED + 86_400


def build(output: Path, executable: Path, manifest_spec: Path) -> dict[str, object]:
    payload = executable.read_bytes()
    manifest_value = json.loads(manifest_spec.read_text(encoding="utf-8"))
    manifest = PACKAGE.encode_manifest(manifest_value, payload)
    executable_name = manifest_value.get("executable")
    if not isinstance(executable_name, str) or not executable_name.endswith(
        ".APP"
    ):
        raise ValueError("lifecycle manifest executable must end in .APP")
    manifest_name = executable_name[:-4] + ".MAN"
    identifier = "org.libsdl.chess"
    capabilities = manifest_value.get("capabilities")
    if not isinstance(capabilities, list):
        raise ValueError("lifecycle manifest capabilities must be a list")
    package_spec = {
        "format": 3,
        "architecture": "x86_64",
        "abi_min": 1,
        "abi_max": 1,
        "identifier": identifier,
        "name": "SDL Chess Board",
        "publisher": "Phipia Development Publisher",
        "capabilities": capabilities,
        "dependencies": [],
        "conflicts": [],
    }
    files = ({
        "path": f"bin/{executable_name}",
        "kind": "executable",
        "mode": 0o555,
        "payload": payload,
    }, {
        "path": f"bin/{manifest_name}",
        "kind": "resource",
        "mode": 0o444,
        "payload": manifest,
    })
    publisher_public = PACKAGE._ed25519_public_bytes_from_private(
        PUBLISHER_SEED
    )
    publisher_key_id = hashlib.sha256(publisher_public).hexdigest()
    root_public = PACKAGE._ed25519_public_bytes_from_private(ROOT_SEED)
    trusted_root = {hashlib.sha256(root_public).hexdigest(): root_public}
    trusted_publisher = {publisher_key_id: publisher_public}

    def release(version: str, repository_version: int) -> tuple[bytes, bytes, str]:
        package = PACKAGE.build_package_v3(
            {**package_spec, "version": version}, files, PUBLISHER_SEED
        )
        download_path = f"packages/{identifier}/{version}.spk"
        repository = REPOSITORY.build_repository({
            "format": 1,
            "repository": "org.phipia.main",
            "repository_version": repository_version,
            "generated_at": GENERATED,
            "expires_at": EXPIRES,
            "architecture": "x86_64",
            "abi_min": 1,
            "abi_max": 1,
            "packages": [{
                "identifier": identifier,
                "version": version,
                "download_path": download_path,
                "bytes": len(package),
                "sha256": hashlib.sha256(package).hexdigest(),
                "publisher_key_id": publisher_key_id,
                "dependencies": [],
                "conflicts": [],
                "provides": [],
            }],
        }, ROOT_SEED)
        REPOSITORY.parse_repository(
            repository, trusted_root_keys=trusted_root, now=GENERATED + 60,
            minimum_repository_version=repository_version,
        )
        PACKAGE.parse_package(package, trusted_keys=trusted_publisher)
        package_path = output / download_path
        package_path.parent.mkdir(parents=True, exist_ok=True)
        PACKAGE.atomic_write(package_path, package)
        return repository, package, download_path

    install_repository, install_package, _ = release("1.0.0", 42)
    update_repository, update_package, _ = release("2.0.0", 43)
    PACKAGE.atomic_write(output / "repository.sri", install_repository)
    PACKAGE.atomic_write(output / "repository-install.sri", install_repository)
    PACKAGE.atomic_write(output / "repository-update.sri", update_repository)
    PACKAGE.atomic_write(output / "repository-rollback.sri", install_repository)
    PACKAGE.atomic_write(output / "repository-repair.sri", update_repository)
    return {
        "output": str(output),
        "repository_bytes": len(install_repository),
        "repository_sha256": hashlib.sha256(
            install_repository
        ).hexdigest().upper(),
        "package_bytes": len(install_package),
        "package_sha256": hashlib.sha256(install_package).hexdigest().upper(),
        "update_repository_sha256": hashlib.sha256(
            update_repository
        ).hexdigest().upper(),
        "update_package_sha256": hashlib.sha256(
            update_package
        ).hexdigest().upper(),
        "payload_sha256": hashlib.sha256(payload).hexdigest().upper(),
        "manifest_sha256": hashlib.sha256(manifest).hexdigest().upper(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--manifest-spec", type=Path, required=True)
    args = parser.parse_args()
    if not PACKAGE.ed25519_available():
        raise RuntimeError("Python Ed25519 support is required")
    print(json.dumps(
        build(args.output, args.executable, args.manifest_spec), sort_keys=True
    ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
