#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Boot the existing malformed Linux fixtures and prove refusal without writes."""

import argparse
import json
from pathlib import Path
import subprocess

import ext4_image
import ext4_kernel_read
import ext4_powercut_test as recovery
import ext4_unlink_powercut_test as cuts


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    tools = ext4_image.require_tools()
    kernel, fixture = args.kernel.resolve(), args.fixture.resolve()
    before = ext4_image.inspect_image(fixture, tools=tools)
    if before["needs_recovery"]:
        raise RuntimeError("malformed admission test requires a clean Linux source")
    (output / "head.txt").write_text(subprocess.check_output(["git", "rev-parse", "HEAD"], text=True))
    for name, executable in {**tools, "dumpe2fs": "dumpe2fs"}.items():
        version = subprocess.run([executable, "-V"], capture_output=True, text=True)
        (output / f"{name}-version.txt").write_text(version.stdout + version.stderr)
    iso = output / "refusal.iso"
    recovery._build_iso(kernel, iso, args.grub_mkrescue, args.grub_module_dir,
        None, scenario="ext4-admission-refusal")
    reports = []
    for kind in ext4_image.MUTATIONS:
        target = output / kind
        target.mkdir()
        image = target / "refused.raw"
        ext4_image.malform_image(kind, fixture, image)
        digest = ext4_kernel_read.digest(image)
        try:
            ext4_image.inspect_image(image, tools=tools)
        except ext4_image.Ext4ImageError as error:
            (target / "verifier-refusal.txt").write_text(str(error) + "\n")
        else:
            raise RuntimeError("malformed fixture was admitted by the independent verifier")
        for boot in (1, 2):
            status, trace = recovery._run_qemu(args.qemu, args.accel, iso, image,
                target / f"boot-{boot}.log", args.timeout)
            cuts.verify_exit(status, trace,
                "ST EXT4 malformed admission refused VFS unavailable census exact")
            if ext4_kernel_read.digest(image) != digest:
                raise RuntimeError(f"{kind} refusal changed the disk image")
        inspections = {}
        for executable, options, name in ((tools["e2fsck"], ["-f", "-n"], "e2fsck.txt"),
                ("dumpe2fs", ["-h"], "dumpe2fs.txt"),
                (tools["debugfs"], ["-R", "ls -p /data/user"], "namespace.txt")):
            result = subprocess.run([executable, *options, str(image)], capture_output=True, text=True, timeout=60)
            (target / name).write_text(result.stdout + result.stderr)
            inspections[name] = result.returncode
        # These deliberately malformed images are refused, not clean-fsck
        # results. Record diagnostic exits; never repair or suppress corruption.
        if ext4_kernel_read.digest(image) != digest:
            raise RuntimeError("read-only malformed inspection changed the image")
        report = {"mutation": kind, "image_sha256": digest, "boots": 2,
            "vfs_admitted": False, "unchanged": True, "inspection_exit_codes": inspections}
        (target / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        reports.append(report)
    (output / "report.json").write_text(json.dumps({"before": before, "reports": reports,
        "kernel_sha256": ext4_kernel_read.digest(kernel), "fixture_sha256": ext4_kernel_read.digest(fixture),
        "scope": "existing malformed fixtures refused twice before VFS exposure, byte-identical disks and empty census; no corruption repair claim"},
        indent=2, sort_keys=True) + "\n")
    print(f"ext4 malformed admission: {len(reports)} fixtures, two boots each, unchanged disks, VFS refusal and census PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--grub-mkrescue", default="grub-mkrescue")
    parser.add_argument("--grub-module-dir", type=Path)
    parser.add_argument("--accel", default="tcg")
    parser.add_argument("--timeout", type=int, default=120)
    run(parser.parse_args())


if __name__ == "__main__":
    main()
