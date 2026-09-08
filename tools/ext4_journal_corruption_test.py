#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Refuse corrupted committed journal records before recovery writes or VFS exposure."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

import ext4_image
import ext4_kernel_read
import ext4_powercut_test as recovery
import ext4_unlink_powercut_test as cuts

BLOCK = 4096
MAGIC = 0xC03B3998


def transaction_layout(data, mapping):
    """Read the actual CSUM_V3 tags; never assume physically contiguous journal blocks.

    Layout reference: https://docs.kernel.org/filesystems/ext4/journal.html
    """
    superblock = data[mapping[0] * BLOCK:(mapping[0] + 1) * BLOCK]
    sequence, start = struct.unpack_from(">II", superblock, 0x18)
    if not 1 <= start < len(mapping) or struct.unpack_from(">I", superblock, 0x28)[0] != 0x13:
        raise RuntimeError("committed seed has no admitted active journal transaction")

    def slot(offset):
        return 1 + (start - 1 + offset) % (len(mapping) - 1)

    descriptor = mapping[start] * BLOCK
    if struct.unpack_from(">III", data, descriptor) != (MAGIC, 1, sequence):
        raise RuntimeError("committed seed does not start with the expected descriptor")
    tags, cursor = [], descriptor + 12
    while cursor + 16 <= descriptor + BLOCK - 4:
        _, flags, _, checksum = struct.unpack_from(">IIII", data, cursor)
        if flags & ~0xA:
            raise RuntimeError("corruption seed has unexpected escaped or deleted tags")
        tags.append(checksum)
        cursor += 16 + (0 if flags & 2 else 16)
        if flags & 8:
            break
    else:
        raise RuntimeError("committed seed descriptor lacks its final tag")
    if not 1 <= len(tags) <= 64:
        raise RuntimeError("committed seed exceeds the admitted metadata image bound")
    payloads = [mapping[slot(index + 1)] * BLOCK for index in range(len(tags))]
    commit = mapping[slot(len(tags) + 1)] * BLOCK
    if struct.unpack_from(">III", data, commit) != (MAGIC, 2, sequence):
        raise RuntimeError("corruption seed must contain one committed transaction without revokes")
    uuid = superblock[0x30:0x40]
    for offset, expected in zip(payloads, tags):
        if ext4_image._crc32c_raw(uuid + struct.pack(">I", sequence) + data[offset:offset + BLOCK]) != expected:
            raise RuntimeError("intact seed payload checksum does not match its journal tag")
    return {"sequence": sequence, "start_slot": start, "descriptor_offset": descriptor,
        "payload_offsets": payloads, "commit_offset": commit}


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    kernel, fixture = args.kernel.resolve(), args.fixture.resolve()
    tools = ext4_image.require_tools()
    (output / "head.txt").write_text(subprocess.check_output(["git", "rev-parse", "HEAD"], text=True))
    for name, executable in {**tools, "dumpe2fs": "dumpe2fs"}.items():
        version = subprocess.run([executable, "-V"], capture_output=True, text=True)
        (output / f"{name}-version.txt").write_text(version.stdout + version.stderr)
    seed = output / "committed.raw"
    ext4_image.prepare_recovery_marker_image(fixture, seed)
    cut_iso, intact_iso, refusal_iso = (output / name for name in ("commit.iso", "intact.iso", "refusal.iso"))
    recovery._build_iso(kernel, cut_iso, args.grub_mkrescue, args.grub_module_dir, 7)
    status, trace = recovery._run_qemu(args.qemu, args.accel, cut_iso, seed, output / "commit.log", args.timeout)
    if status != recovery.POWER_CUT_EXIT_STATUS or trace.count("ST EXT4 POWER CUT 7 commit\n") != 1 or \
            "ST FAIL" in trace or recovery.PASS_MARKER in trace or "Phipia PANIC" in trace:
        raise RuntimeError("journal seed did not stop exactly after its commit flush")
    data = seed.read_bytes()
    if not ext4_image.parse_superblock(data)["needs_recovery"]:
        raise RuntimeError("committed seed lost its ext4 recovery marker")
    mapping = ext4_image.journal_inode_map(seed, tools, output, "committed")
    layout = transaction_layout(data, mapping)
    (output / "transaction-layout.json").write_text(json.dumps(layout, indent=2) + "\n")
    recovery._build_iso(kernel, intact_iso, args.grub_mkrescue, args.grub_module_dir, None)
    recovery._build_iso(kernel, refusal_iso, args.grub_mkrescue, args.grub_module_dir,
        None, scenario="ext4-admission-refusal")
    intact = output / "intact-replayed.raw"
    shutil.copyfile(seed, intact)
    status, trace = recovery._run_qemu(args.qemu, args.accel, intact_iso, intact,
        output / "intact-replay.log", args.timeout)
    cuts.verify_exit(status, trace, "ST EXT4 RECOVERY marker cleared transaction committed")
    recovery._verify_guest_result(intact, tools, output)
    report = ext4_image.inspect_image(intact, tools=tools)
    expected = (b"Phipia deterministic ext4 fixture\n").ljust(4096, b"\0") + b"X"
    report["linux_kernel_read"] = ext4_kernel_read.verify_files(intact, output / "intact-linux",
        {"system/README.TXT": {"bytes": len(expected), "sha256": hashlib.sha256(expected).hexdigest()}})
    report["image_sha256"] = ext4_kernel_read.digest(intact)
    for executable, options, name in ((tools["e2fsck"], ["-f", "-n"], "intact-e2fsck.txt"),
            ("dumpe2fs", ["-h"], "intact-dumpe2fs.txt")):
        result = subprocess.run([executable, *options, str(intact)], capture_output=True, text=True, timeout=60)
        (output / name).write_text(result.stdout + result.stderr)
        if result.returncode != 0:
            raise RuntimeError("intact journal replay failed read-only filesystem inspection")
    (output / "intact-report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    descriptor, commit = layout["descriptor_offset"], layout["commit_offset"]
    mutations = [("descriptor-checksum", descriptor + BLOCK - 4, None),
        ("descriptor-tag", descriptor + 12, None), ("commit-checksum", commit + 16, None),
        ("descriptor-prefix-512", descriptor + 512, descriptor + BLOCK),
        ("commit-prefix-16", commit + 16, commit + BLOCK)]
    mutations.extend((f"payload-{index}-checksum", offset + 512, None)
        for index, offset in enumerate(layout["payload_offsets"]))
    reports = []
    for name, offset, end in mutations:
        target = output / name
        target.mkdir()
        image = target / "refused.raw"
        changed = bytearray(data)
        if end is None:
            changed[offset] ^= 0x80
        else:
            changed[offset:end] = bytes(end - offset)
        if changed == data:
            raise RuntimeError("corruption mutation did not change the committed record")
        image.write_bytes(changed)
        digest = ext4_kernel_read.digest(image)
        for boot in (1, 2):
            status, trace = recovery._run_qemu(args.qemu, args.accel, refusal_iso, image,
                target / f"boot-{boot}.log", args.timeout)
            cuts.verify_exit(status, trace, "ST EXT4 malformed admission refused VFS unavailable census exact")
            if ext4_kernel_read.digest(image) != digest:
                raise RuntimeError("corrupt journal refusal wrote recovery or home metadata")
        inspections = {}
        for executable, options, label in ((tools["e2fsck"], ["-f", "-n"], "e2fsck.txt"),
                ("dumpe2fs", ["-h"], "dumpe2fs.txt"),
                (tools["debugfs"], ["-R", "stat /system/README.TXT"], "namespace.txt")):
            result = subprocess.run([executable, *options, str(image)], capture_output=True, text=True, timeout=60)
            (target / label).write_text(result.stdout + result.stderr)
            inspections[label] = result.returncode
        if ext4_kernel_read.digest(image) != digest:
            raise RuntimeError("read-only journal inspection changed the refused image")
        item = {"mutation": name, "byte_offset": offset, "zeroed_until": end, "image_sha256": digest,
            "boots": 2, "unchanged": True, "vfs_admitted": False, "inspection_exit_codes": inspections}
        (target / "report.json").write_text(json.dumps(item, indent=2, sort_keys=True) + "\n")
        reports.append(item)
    (output / "report.json").write_text(json.dumps({"reports": reports,
        "seed_sha256": ext4_kernel_read.digest(seed), "kernel_sha256": ext4_kernel_read.digest(kernel),
        "fixture_sha256": ext4_kernel_read.digest(fixture), "layout": layout,
        "scope": "byte corruption and partial record models refused without writes; intact replay Linux/fsck control; no physical torn-sector tolerance or corruption repair claim"},
        indent=2, sort_keys=True) + "\n")
    print(f"ext4 corrupt journal: {len(reports)} records, repeated refusal, unchanged images, VFS/census and intact Linux recovery control PASS")


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
