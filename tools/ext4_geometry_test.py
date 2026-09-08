#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Refuse a 512-byte NVMe namespace unchanged, then admit its 4 KiB control."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

import ext4_image
import ext4_kernel_read
import ext4_powercut_test as recovery
import ext4_unlink_powercut_test as cuts


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = ext4_image.require_tools()
    kernel = args.kernel.resolve()
    fixture = args.fixture.resolve()
    (output / "head.txt").write_text(subprocess.check_output(["git", "rev-parse", "HEAD"], text=True))
    for name, executable in {**tools, "dumpe2fs": "dumpe2fs"}.items():
        version = subprocess.run([executable, "-V"], capture_output=True, text=True)
        (output / f"{name}-version.txt").write_text(version.stdout + version.stderr)
    before = ext4_image.inspect_image(fixture, tools=tools)
    if before["needs_recovery"]:
        raise RuntimeError("geometry control requires a clean Linux fixture")
    original_digest = ext4_kernel_read.digest(fixture)
    readme = b"Phipia deterministic ext4 fixture\n"
    reports = []
    for logical_bytes in (512, 4096):
        target = output / str(logical_bytes)
        target.mkdir()
        image = target / "disk.raw"
        shutil.copyfile(fixture, image)
        iso = target / "boot.iso"
        scenario = "ext4-geometry-refusal" if logical_bytes == 512 else "ext4-recovery"
        recovery._build_iso(kernel, iso, args.grub_mkrescue, args.grub_module_dir,
            None, scenario=scenario)
        for boot in range(1, 3 if logical_bytes == 512 else 2):
            status, trace = recovery._run_qemu(args.qemu, args.accel, iso, image,
                target / f"boot-{boot}.log", args.timeout, logical_block_bytes=logical_bytes)
            marker = ("ST EXT4 geometry refused before Rust VFS unavailable census exact"
                if logical_bytes == 512 else recovery.PASS_MARKER)
            cuts.verify_exit(status, trace, marker)
            if logical_bytes == 512 and ext4_kernel_read.digest(image) != original_digest:
                raise RuntimeError("refused logical geometry changed the disk image")
        after = ext4_image.inspect_image(image, tools=tools)
        if after["needs_recovery"]:
            raise RuntimeError("geometry test left recovery pending")
        for executable, options, name in ((tools["e2fsck"], ["-f", "-n"], "e2fsck.txt"),
                ("dumpe2fs", ["-h"], "dumpe2fs.txt")):
            result = subprocess.run([executable, *options, str(image)], capture_output=True, text=True)
            (target / name).write_text(result.stdout + result.stderr)
            if result.returncode != 0:
                raise RuntimeError(f"{name} refused geometry result")
        (target / "namespace.txt").write_text(ext4_image._debugfs(tools, image, "ls -p /data/user"))
        content = readme if logical_bytes == 512 else readme + bytes(4096 - len(readme)) + b"X"
        expected = {"system/README.TXT": {"bytes": len(content),
            "sha256": hashlib.sha256(content).hexdigest()}, "data/user/refused-sector": None}
        after["linux_kernel_read"] = ext4_kernel_read.verify_files(image, target / "linux-kernel", expected)
        after["logical_block_bytes"] = logical_bytes
        after["image_sha256"] = ext4_kernel_read.digest(image)
        (target / "report.json").write_text(json.dumps(after, indent=2, sort_keys=True) + "\n")
        reports.append(after)
    (output / "report.json").write_text(json.dumps({"before": before, "reports": reports,
        "fixture_sha256": original_digest, "kernel_sha256": ext4_kernel_read.digest(kernel),
        "scope": "512-byte admission refusal twice without disk writes; 4096-byte ordinary VFS control"},
        indent=2, sort_keys=True) + "\n")
    print("ext4 NVMe geometry: 512-byte refusal unchanged, 4096-byte VFS control, Linux read/fsck and census PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--grub-mkrescue", default="grub-mkrescue")
    parser.add_argument("--grub-module-dir", type=Path)
    parser.add_argument("--accel", default="tcg")
    parser.add_argument("--timeout", type=int, default=600)
    run(parser.parse_args())


if __name__ == "__main__":
    main()
