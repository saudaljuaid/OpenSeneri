#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise physical journal-slot wrap through 513 ordinary VFS chmod commits."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import ext4_image
import ext4_kernel_read
import ext4_powercut_test as recovery
import ext4_unlink_powercut_test as cuts

PASS = "ST EXT4 VFS journal wrap held metadata contents cursor allocation census exact"


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    image = output / "wrap.raw"
    shutil.copyfile(args.fixture, image)
    tools = ext4_image.require_tools()
    (output / "head.txt").write_text(subprocess.check_output(["git", "rev-parse", "HEAD"], text=True))
    for name, executable in {**tools, "dumpe2fs": "dumpe2fs"}.items():
        version = subprocess.run([executable, "-V"], capture_output=True, text=True)
        (output / f"{name}-version.txt").write_text(version.stdout + version.stderr)
    source, sentinel = output / "original.bin", output / "sentinel"
    source.write_bytes(b"t" * 1700)
    sentinel.write_bytes(b"")
    for file, name in ((source, "wrap-target"), (sentinel, "WRAP.TST")):
        ext4_image._run([tools["debugfs"], "-w", "-R",
            f'write "{file.as_posix()}" /data/user/{name}', image])
    before = ext4_image.inspect_image(image, tools=tools)
    original = ext4_image._parse_stat(ext4_image._debugfs(tools, image,
        "stat /data/user/wrap-target"), "/data/user/wrap-target")
    physical = ext4_image.journal_inode_map(image, tools, output, "before")
    logical = {block: index for index, block in enumerate(physical)}
    iso = output / "wrap.iso"
    recovery._build_iso(args.kernel.resolve(), iso, args.grub_mkrescue,
        args.grub_module_dir, None, storage_cut=0)
    reports = []
    journal_records = []
    for boot in (1, 2):
        status, trace = recovery._run_qemu(args.qemu, args.accel, iso, image,
            output / f"boot-{boot}.log", args.timeout)
        cuts.verify_exit(status, trace, PASS)
        marker = "ST EXT4 WRAP transactions 513" if boot == 1 else "ST EXT4 WRAP cold boot retained"
        if trace.count(marker + "\n") != 1:
            raise RuntimeError("journal wrap omitted its exact transaction or cold-boot receipt")
        commands = re.findall(r"^ST EXT4 STORAGE (\d+) (write|flush) (\d+)$", trace, re.MULTILINE)
        if len(commands) > 10000 or [int(item[0]) for item in commands] != list(range(1, len(commands) + 1)):
            raise RuntimeError("journal wrap command trace is not bounded and contiguous")
        records = [logical[int(detail)] for _, kind, detail in commands
            if kind == "write" and int(detail) in logical and logical[int(detail)] != 0]
        if boot == 1:
            commits = re.findall(r"^ST EXT4 DURABLE \d+ commit$", trace, re.MULTILINE)
            if len(commits) != 513 or len(records) <= 1023 or records != [1 + index % 1023 for index in range(len(records))]:
                raise RuntimeError("ordinary VFS commits did not wrap and reuse the mapped journal slots in order")
            journal_records = records
        elif records:
            raise RuntimeError("clean cold boot unexpectedly wrote new journal records")
        after = ext4_image.inspect_image(image, tools=tools)
        if after["needs_recovery"] or any(after[field] != before[field] for field in ("free_blocks", "free_inodes")) or \
                after["journal"]["sequence"] != before["journal"]["sequence"] + 513:
            raise RuntimeError("journal wrap changed allocations or lost transaction sequence progress")
        metadata = ext4_image._parse_stat(ext4_image._debugfs(tools, image,
            "stat /data/user/wrap-target"), "/data/user/wrap-target")
        if any(metadata[field] != original[field] for field in ("inode", "size", "links", "block_count_512")) or int(metadata["mode"], 8) != 0o600:
            raise RuntimeError("journal wrap changed file identity, content length, links or mode")
        target = output / f"boot-{boot}"
        target.mkdir()
        for executable, options, name in ((tools["e2fsck"], ["-f", "-n"], "e2fsck.txt"),
                ("dumpe2fs", ["-h"], "dumpe2fs.txt"), (tools["debugfs"], ["-R", "ls -p /data/user"], "namespace.txt")):
            result = subprocess.run([executable, *options, str(image)], capture_output=True, text=True)
            (target / name).write_text(result.stdout + result.stderr)
            if result.returncode != 0:
                raise RuntimeError(f"{name} refused journal wrap result")
        after["linux_kernel_read"] = ext4_kernel_read.verify_files(image, target / "linux-kernel", {
            "data/user/wrap-target": {"bytes": 1700, "sha256": hashlib.sha256(source.read_bytes()).hexdigest(), "mode": 0o600}})
        after["image_sha256"] = ext4_kernel_read.digest(image)
        (target / "report.json").write_text(json.dumps(after, indent=2, sort_keys=True) + "\n")
        reports.append(after)
    if ext4_image.journal_inode_map(image, tools, output, "after") != physical:
        raise RuntimeError("journal wrap changed its physical inode map")
    (output / "report.json").write_text(json.dumps({"before": before, "reports": reports,
        "physical_journal_map": physical, "journal_record_slots": journal_records,
        "transactions": 513, "scope": "ordinary VFS wrap and clean cold boot; no crash-at-wrap claim",
        "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256(args.fixture.read_bytes()).hexdigest()}, indent=2, sort_keys=True) + "\n")
    print("ordinary VFS journal wrap: 513 commits, physical slot reuse, held metadata, cold boot, Linux readback, fsck and census PASS")


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
