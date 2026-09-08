#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fill the admitted 64 MiB file bound, cold-read it, and reclaim it through VFS."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

import ext4_image
import ext4_kernel_read
import ext4_powercut_test as recovery
import ext4_unlink_powercut_test as cuts

MAXIMUM = 64 * 1024 * 1024
PASS = "ST EXT4 VFS dense maximum contents shared EOF reclaim census exact"


def pattern_digest():
    # Keep host memory bounded too. The guest computes the formula independently.
    period = bytes((index * 17 + 3) % 251 for index in range(251))
    repeated = period * (1024 * 1024 // len(period) + 2)
    digest = hashlib.sha256()
    for offset in range(0, MAXIMUM, 1024 * 1024):
        start = offset % len(period)
        digest.update(repeated[start:start + 1024 * 1024])
    return digest.hexdigest()


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    image = output / "dense.raw"
    shutil.copyfile(args.fixture, image)
    tools = ext4_image.require_tools()
    (output / "head.txt").write_text(subprocess.check_output(["git", "rev-parse", "HEAD"], text=True))
    for name, executable in {**tools, "dumpe2fs": "dumpe2fs"}.items():
        version = subprocess.run([executable, "-V"], capture_output=True, text=True)
        (output / f"{name}-version.txt").write_text(version.stdout + version.stderr)
    empty = output / "empty"
    empty.write_bytes(b"")
    for name in ("dense-target", "DENSE.TST"):
        ext4_image._run([tools["debugfs"], "-w", "-R",
            f'write "{empty.as_posix()}" /data/user/{name}', image])
    before = ext4_image.inspect_image(image, tools=tools)
    original_text = ext4_image._debugfs(tools, image, "stat /data/user/dense-target")
    (output / "original-stat.txt").write_text(original_text)
    original = ext4_image._parse_stat(original_text, "/data/user/dense-target")
    if original["size"] != 0 or original["links"] != 1 or original["block_count_512"] != 0 or \
            int(original["mode"], 8) != 0o644 or before["free_blocks"] <= MAXIMUM // 4096 + 1024:
        raise RuntimeError("dense file fixture lacks its empty inode or allocation capacity")
    shutil.copyfile(image, output / "initial.raw")
    iso = output / "dense.iso"
    recovery._build_iso(args.kernel.resolve(), iso, args.grub_mkrescue, args.grub_module_dir, None)
    expected_digest = pattern_digest()
    reports = []
    for boot in (1, 2, 3):
        started = time.perf_counter()
        status, trace = recovery._run_qemu(args.qemu, args.accel, iso, image,
            output / f"boot-{boot}.log", args.timeout)
        elapsed = time.perf_counter() - started
        cuts.verify_exit(status, trace, PASS)
        required = {1: ("ST EXT4 DENSE written 67108864",),
            2: ("ST EXT4 DENSE cold read 67108864", "ST EXT4 DENSE reclaimed"),
            3: ("ST EXT4 DENSE cleanup retained",)}[boot]
        if any(trace.count(marker + "\n") != 1 for marker in required):
            raise RuntimeError("dense file omitted its expected write, cold read or reclamation receipt")
        target = output / f"boot-{boot}"
        target.mkdir()
        after = ext4_image.inspect_image(image, tools=tools)
        if after["needs_recovery"]:
            raise RuntimeError("dense file clean unmount retained recovery state")
        namespace = ext4_image._debugfs(tools, image, "ls -p /data/user")
        (target / "namespace.txt").write_text(namespace)
        expected = {"data/user/dense-target": None}
        if boot == 1:
            text = ext4_image._debugfs(tools, image, "stat /data/user/dense-target")
            (target / "dense-stat.txt").write_text(text)
            metadata = ext4_image._parse_stat(text, "/data/user/dense-target")
            blocks = metadata["block_count_512"] // 8
            if metadata["size"] != MAXIMUM or metadata["inode"] != original["inode"] or \
                    metadata["links"] != 1 or int(metadata["mode"], 8) != 0o644 or \
                    metadata["block_count_512"] % 8 or blocks < MAXIMUM // 4096 or \
                    before["free_blocks"] - after["free_blocks"] != blocks or \
                    before["free_inodes"] != after["free_inodes"] or \
                    after["journal"]["sequence"] < before["journal"]["sequence"] + 512:
                raise RuntimeError("dense allocation, extent metadata, counters or transaction splitting changed")
            after["dense_inode"] = metadata
            expected["data/user/dense-target"] = {"bytes": MAXIMUM,
                "sha256": expected_digest, "mode": 0o644}
            shutil.copyfile(image, output / "allocated.raw")
        elif "/dense-target/" in namespace or after["free_blocks"] != before["free_blocks"] or \
                after["free_inodes"] != before["free_inodes"] + 1:
            raise RuntimeError("dense truncate/unlink leaked blocks, extent nodes or its inode")
        for executable, options, name in ((tools["e2fsck"], ["-f", "-n"], "e2fsck.txt"),
                ("dumpe2fs", ["-h"], "dumpe2fs.txt")):
            result = subprocess.run([executable, *options, str(image)], capture_output=True, text=True)
            (target / name).write_text(result.stdout + result.stderr)
            if result.returncode != 0:
                raise RuntimeError(f"{name} refused dense file result")
        after["linux_kernel_read"] = ext4_kernel_read.verify_files(image, target / "linux-kernel", expected)
        after["image_sha256"] = ext4_kernel_read.digest(image)
        after["qemu_seconds_including_boot_and_guest_verification"] = elapsed
        (target / "report.json").write_text(json.dumps(after, indent=2, sort_keys=True) + "\n")
        reports.append(after)
    (output / "report.json").write_text(json.dumps({"before": before, "reports": reports,
        "bytes": MAXIMUM, "content_sha256": expected_digest,
        "scope": "fully allocated VFS file bound, split writes, cold read, truncate and unlink; no power-cut claim",
        "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256(args.fixture.read_bytes()).hexdigest()}, indent=2, sort_keys=True) + "\n")
    print("ordinary VFS dense 64 MiB: complete contents, shared EOF, cold read, allocation reclamation, Linux readback, fsck and census PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--grub-mkrescue", default="grub-mkrescue")
    parser.add_argument("--grub-module-dir", type=Path)
    parser.add_argument("--accel", default="tcg")
    parser.add_argument("--timeout", type=int, default=1800)
    run(parser.parse_args())


if __name__ == "__main__":
    main()
