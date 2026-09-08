#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise ordinary VFS short writes and ENOSPC rollback on a Linux fixture."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

import ext4_image
import ext4_powercut_test as recovery


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = ext4_image.require_tools()
    image = output / ("inode-full.raw" if args.inodes else "low-space.raw")
    shutil.copyfile(args.fixture, image)
    (output / "head.txt").write_text(subprocess.check_output(["git", "rev-parse", "HEAD"], text=True))
    with tempfile.TemporaryDirectory(prefix="low-space-input-", dir=output) as raw:
        work = Path(raw)
        empty = work / "empty"
        empty.write_bytes(b"")
        sentinel = "INOFULL.TST" if args.inodes else "LOWSPACE.TST"
        ext4_image._run([tools["debugfs"], "-w", "-R",
            f'write "{empty.as_posix()}" /data/user/{sentinel}', image])
        superblock = ext4_image.parse_superblock(image.read_bytes())
        if args.inodes:
            available = superblock["free_inodes"]
            if not 2 < available < 8192:
                raise RuntimeError("inode fixture exceeds the bounded fill profile")
            commands = work / "fill.commands"
            commands.write_text("mkdir /inode-full\n" + "".join(
                f'write "{empty.as_posix()}" /inode-full/entry-{index}\n'
                for index in range(available - 1)))
            result = ext4_image._run([tools["debugfs"], "-w", "-f", commands, image])
            (output / "fixture-debugfs.txt").write_text(result.stdout)
        else:
            available = superblock["free_blocks"]
            filler = work / "filler"
            # Match the established real ENOSPC coordinator fixture: leave room for
            # one 32-block transaction but not the second, including extent nodes.
            if available <= 64:
                raise RuntimeError("fixture is already too full")
            with filler.open("wb") as stream:
                for _ in range(available - 48):
                    stream.write(b"Z" * 4096)
            ext4_image._run([tools["debugfs"], "-w", "-R",
                f'write "{filler.as_posix()}" /system/filler', image])
    before = ext4_image.inspect_image(image, tools=tools)
    if args.inodes and before["free_inodes"] != 0:
        raise RuntimeError("debugfs did not exhaust all inodes")
    if not args.inodes and not 32 < before["free_blocks"] < 64:
        raise RuntimeError("debugfs did not leave the required bounded reserve")
    namespace = ext4_image._debugfs(tools, image, "ls -p /data/user")
    recycled = ext4_image._debugfs(tools, image, "ls -p /inode-full") if args.inodes else ""
    iso = output / "low-space.iso"
    recovery._build_iso(args.kernel.resolve(), iso, args.grub_mkrescue, args.grub_module_dir, None)
    status, transcript = recovery._run_qemu(args.qemu, args.accel, iso, image,
        output / "serial.log", args.timeout)
    marker = ("ST EXT4 VFS inode exhaustion create mkdir symlink rollback reuse cleanup census exact"
        if args.inodes else "ST EXT4 VFS ENOSPC durable short prefix rollback cursor reuse cleanup census exact")
    if status != recovery.PASS_EXIT_STATUS or transcript.count(marker) != 1 or \
            transcript.count(recovery.PASS_MARKER) != 1 or "ST FAIL" in transcript or "Phipia PANIC" in transcript:
        raise RuntimeError(f"ordinary VFS low-space test failed ({status}):\n" + recovery._transcript_tail(transcript))
    after = ext4_image.inspect_image(image, tools=tools)
    if after["needs_recovery"] or any(before[field] != after[field] for field in ("free_blocks", "free_inodes")):
        raise RuntimeError("low-space cleanup changed counters or retained the recovery marker")
    after_namespace = ext4_image._debugfs(tools, image, "ls -p /data/user")
    if namespace != after_namespace:
        raise RuntimeError("low-space cleanup changed the fixture namespace")
    if args.inodes:
        restored = ext4_image._debugfs(tools, image, "ls -p /inode-full")
        if sorted(recycled.splitlines()) != sorted(restored.splitlines()):
            raise RuntimeError("inode recycling changed names, identities or file metadata")
        (output / "inode-namespace.txt").write_text(restored)
    (output / "namespace.txt").write_text(after_namespace)
    for executable, arguments, name in (
        (tools["e2fsck"], ["-f", "-n"], "e2fsck.txt"),
        ("dumpe2fs", ["-h"], "dumpe2fs.txt"),
        (tools["debugfs"], ["-R", "stat /inode-full" if args.inodes else "stat /system/filler"], "debugfs.txt"),
    ):
        result = subprocess.run([executable, *arguments, str(image)], capture_output=True, text=True)
        (output / name).write_text(result.stdout + result.stderr)
        if result.returncode != 0:
            raise RuntimeError(f"{name} refused the cleanly unmounted result")
    report = {"before": before, "after": after,
        "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256(args.fixture.read_bytes()).hexdigest(),
        "image_sha256": hashlib.sha256(image.read_bytes()).hexdigest()}
    (output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(f"ordinary ext4 VFS {'inode' if args.inodes else 'block'} exhaustion, rollback, reuse, census and read-only fsck: PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--grub-mkrescue", default="grub-mkrescue")
    parser.add_argument("--grub-module-dir", type=Path)
    parser.add_argument("--accel", default="tcg")
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--inodes", action="store_true")
    run(parser.parse_args())


if __name__ == "__main__":
    main()
