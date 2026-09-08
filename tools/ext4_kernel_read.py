#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read clean Phipia guest results through the Linux ext4 driver."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import subprocess


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def mounted_read(root, expected):
    result = {}
    for name, wanted in expected.items():
        relative = PurePosixPath(name)
        if relative.is_absolute() or not relative.parts or ".." in relative.parts:
            raise RuntimeError(f"invalid kernel read path: {name!r}")
        if wanted is None:
            parent = root.joinpath(*relative.parts[:-1]).resolve(strict=True)
            if not parent.is_relative_to(root):
                raise RuntimeError(f"kernel absence check escaped mount: {name!r}")
            try:
                (parent / relative.name).lstat()
            except FileNotFoundError:
                result[name] = None
                continue
            raise RuntimeError(f"Linux retained removed name: {name!r}")
        target = root.joinpath(*relative.parts).resolve(strict=True)
        if "entries" in wanted:
            if not target.is_relative_to(root) or not target.is_dir():
                raise RuntimeError(f"kernel directory escaped mount or changed type: {name!r}")
            actual = {"entries": sorted(child.name for child in target.iterdir())}
            if actual != wanted:
                raise RuntimeError(f"Linux directory entries differ for {name}: {actual!r} != {wanted!r}")
            result[name] = actual
            continue
        if not target.is_relative_to(root) or not target.is_file():
            raise RuntimeError(f"kernel read escaped mount or is not a regular file: {name!r}")
        size = target.stat().st_size
        if size != wanted["bytes"]:
            raise RuntimeError(f"Linux file length differs for {name}: {size} != {wanted['bytes']}")
        actual = {"bytes": size, "sha256": digest(target)}
        if actual != wanted:
            raise RuntimeError(f"Linux and e2fsprogs disagree for {name}: {actual!r} != {wanted!r}")
        result[name] = actual
    return result


def verify_files(image, output, expected):
    if os.environ.get("PHIPIA_EXT4_KERNEL_INTEROP") != "1":
        return {"verified": False, "reason": "Linux kernel interoperability not requested; no gate credit"}
    if platform.system() != "Linux":
        raise RuntimeError("requested kernel interoperability requires Linux")
    image, output = image.resolve(), output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    mountpoint = output / "mount"
    mountpoint.mkdir()
    manifest = output / "expected.json"
    manifest.write_text(json.dumps(expected, indent=2, sort_keys=True) + "\n")
    with image.open("rb") as source:
        source.seek(1024 + 0x60)
        incompat = int.from_bytes(source.read(4), "little")
    if incompat & 4:
        raise RuntimeError("kernel read requires a cleanly unmounted image, not suppressed replay")
    before = digest(image)
    commands = []
    mounted = False

    def privileged(arguments):
        nonlocal mounted
        result = subprocess.run(["sudo", "-n", *arguments], capture_output=True, text=True)
        if result.returncode == 0 and arguments[0] in ("mount", "umount"):
            mounted = arguments[0] == "mount"
        commands.append({"arguments": arguments, "status": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr})
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode != 0:
            raise RuntimeError(f"Linux kernel read command failed: {arguments!r}: {result.stderr}")
        return result.stdout

    flags = "loop,ro,noload,nodev,nosuid,noexec"
    try:
        privileged(["mount", "-t", "ext4", "-o", flags, str(image), str(mountpoint)])
        files = json.loads(privileged(["python3", str(Path(__file__).resolve()),
            "--mounted-read", str(mountpoint), str(manifest)]))
    finally:
        if mounted:
            privileged(["umount", str(mountpoint)])
    attached = privileged(["losetup", "--associated", str(image)])
    if attached.strip():
        raise RuntimeError("Linux kernel read retained a loop-device association")
    after = digest(image)
    if after != before:
        raise RuntimeError("read-only Linux mount changed the guest image")
    report = {"verified": True, "kernel": platform.release(), "mount_flags": flags,
        "files": files, "image_sha256": after, "unmounted": True, "loop_devices": 0}
    (output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mounted-read", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("image", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.mounted_read:
        print(json.dumps(mounted_read(args.image.resolve(), json.loads(args.output.read_text()))))
    else:
        prefix = b"Phipia deterministic ext4 fixture\n"
        content = prefix + bytes(4096 - len(prefix)) + b"X"
        report = verify_files(args.image, args.output, {
            "system/README.TXT": {"bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()},
            "indexed": {"entries": [f"entry-{index:04d}-phipia-fixture" for index in range(256)]}})
        print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
