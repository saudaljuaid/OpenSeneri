#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Power-cut ordinary held-file unlink, fsync, final close and orphan reclaim."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

import ext4_image
import ext4_powercut_test as recovery


PASS = "ST EXT4 VFS held-unlink old-or-new cleanup census exact"


def verify_exit(status, transcript):
    if status != recovery.PASS_EXIT_STATUS or transcript.count(PASS) != 1 or \
            transcript.count(recovery.PASS_MARKER) != 1 or "ST FAIL" in transcript or "Phipia PANIC" in transcript:
        raise RuntimeError(f"held-unlink reboot failed ({status}):\n" + recovery._transcript_tail(transcript))


def inspect(image, tools, output, expected_blocks, expected_inodes):
    report = ext4_image.inspect_image(image, tools=tools)
    if report["needs_recovery"] or report["free_blocks"] != expected_blocks or report["free_inodes"] != expected_inodes:
        raise RuntimeError("held-unlink recovery leaked allocations or retained recovery state")
    namespace = ext4_image._debugfs(tools, image, "ls -p /data/user")
    if "/owned-cut/" in namespace:
        raise RuntimeError("held-unlink retained its removed name")
    output.mkdir()
    (output / "namespace.txt").write_text(namespace)
    for executable, arguments, name in (
        (tools["e2fsck"], ["-f", "-n"], "e2fsck.txt"),
        ("dumpe2fs", ["-h"], "dumpe2fs.txt"),
        (tools["debugfs"], ["-R", "stat /data/user"], "debugfs.txt"),
    ):
        result = subprocess.run([executable, *arguments, str(image)], capture_output=True, text=True)
        (output / name).write_text(result.stdout + result.stderr)
        if result.returncode != 0:
            raise RuntimeError(f"held-unlink {name} refused the clean image")
    report["image_sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
    (output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return report


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = ext4_image.require_tools()
    initial = output / "initial.raw"
    shutil.copyfile(args.fixture, initial)
    (output / "head.txt").write_text(subprocess.check_output(["git", "rev-parse", "HEAD"], text=True))
    for name, executable in {**tools, "dumpe2fs": "dumpe2fs"}.items():
        version = subprocess.run([executable, "-V"], capture_output=True, text=True)
        (output / f"{name}-version.txt").write_text(version.stdout + version.stderr)
    with tempfile.TemporaryDirectory(prefix="unlink-input-", dir=output) as raw:
        work = Path(raw)
        source = work / "content"
        source.write_bytes(b"c" * 4500)
        empty = work / "empty"
        empty.write_bytes(b"")
        for file, name in ((source, "owned-cut"), (empty, "CUTUNLINK.TST")):
            ext4_image._run([tools["debugfs"], "-w", "-R",
                f'write "{file.as_posix()}" /data/user/{name}', initial])
    before = ext4_image.inspect_image(initial, tools=tools)
    # The non-sparse 4500-byte Linux file owns exactly two 4 KiB data blocks.
    target = ext4_image._parse_stat(ext4_image._debugfs(tools, initial,
        "stat /data/user/owned-cut"), "/data/user/owned-cut")
    if target["size"] != 4500 or target["block_count_512"] != 16:
        raise RuntimeError("held-unlink input has unexpected allocation geometry")
    expected_blocks, expected_inodes = before["free_blocks"] + 2, before["free_inodes"] + 1
    iso = output / "verify.iso"
    recovery._build_iso(args.kernel.resolve(), iso, args.grub_mkrescue, args.grub_module_dir, None)
    baseline = output / "complete.raw"
    shutil.copyfile(initial, baseline)
    status, transcript = recovery._run_qemu(args.qemu, args.accel, iso, baseline,
        output / "complete.log", args.timeout)
    verify_exit(status, transcript)
    inspect(baseline, tools, output / "complete", expected_blocks, expected_inodes)
    boundaries = re.findall(r"^ST EXT4 DURABLE (\d+) ([a-z-]+)$", transcript, re.MULTILINE)
    if not 1 <= len(boundaries) <= 64 or [int(number) for number, _ in boundaries] != list(range(1, len(boundaries) + 1)):
        raise RuntimeError("held-unlink trace is not a bounded contiguous boundary sequence")
    first_commit = next(int(number) for number, name in boundaries if name == "commit")
    reports = []
    for number, boundary in boundaries:
        cut = int(number)
        image = output / f"cut-{cut:02d}.raw"
        cut_iso = output / f"cut-{cut:02d}.iso"
        shutil.copyfile(initial, image)
        recovery._build_iso(args.kernel.resolve(), cut_iso, args.grub_mkrescue, args.grub_module_dir, cut)
        status, transcript = recovery._run_qemu(args.qemu, args.accel, cut_iso, image,
            output / f"cut-{cut:02d}.log", args.timeout)
        marker = f"ST EXT4 POWER CUT {cut} {boundary}"
        if status != recovery.POWER_CUT_EXIT_STATUS or transcript.count(marker) != 1 or \
                "ST FAIL" in transcript or PASS in transcript or "Phipia PANIC" in transcript:
            raise RuntimeError(f"held-unlink boundary {cut} did not cut exactly:\n" + recovery._transcript_tail(transcript))
        status, transcript = recovery._run_qemu(args.qemu, args.accel, iso, image,
            output / f"cut-{cut:02d}-reboot.log", args.timeout)
        verify_exit(status, transcript)
        expected_state = "new" if cut >= first_commit else "old"
        if transcript.count(f"ST EXT4 HELD UNLINK initial {expected_state}\n") != 1:
            raise RuntimeError(f"held-unlink boundary {cut} violated durable {expected_state} namespace")
        report = inspect(image, tools, output / f"cut-{cut:02d}", expected_blocks, expected_inodes)
        reports.append({"cut": cut, "boundary": boundary, "recovered_state": expected_state, "result": report})
    (output / "report.json").write_text(json.dumps({"before": before, "reports": reports,
        "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256(args.fixture.read_bytes()).hexdigest()}, indent=2, sort_keys=True) + "\n")
    print(f"ordinary VFS held unlink: {len(boundaries)} power cuts, reboot, orphan reclamation, census and read-only fsck PASS")


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
    run(parser.parse_args())


if __name__ == "__main__":
    main()
