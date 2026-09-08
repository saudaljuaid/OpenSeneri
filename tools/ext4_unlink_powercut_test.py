#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Power-cut ordinary held-file unlink/replace/truncate and recovery."""

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
import ext4_kernel_read
import ext4_powercut_test as recovery


PASS = "ST EXT4 VFS held-unlink old-or-new cleanup census exact"


def verify_exit(status, transcript, pass_marker=PASS):
    if status != recovery.PASS_EXIT_STATUS or transcript.count(pass_marker) != 1 or \
            transcript.count(recovery.PASS_MARKER) != 1 or "ST FAIL" in transcript or "Phipia PANIC" in transcript:
        raise RuntimeError(f"held-unlink reboot failed ({status}):\n" + recovery._transcript_tail(transcript))


def inspect(image, tools, output, expected_blocks, expected_inodes, replacement_inode=None, operation="unlink", expected_parent_links=None):
    report = ext4_image.inspect_image(image, tools=tools)
    if report["needs_recovery"] or report["free_blocks"] != expected_blocks or report["free_inodes"] != expected_inodes:
        raise RuntimeError("held-unlink recovery leaked allocations or retained recovery state")
    namespace = ext4_image._debugfs(tools, image, "ls -p /data/user")
    if expected_parent_links is not None:
        parent = ext4_image._parse_stat(ext4_image._debugfs(tools, image,
            "stat /data/user"), "/data/user")
        if parent["links"] != expected_parent_links:
            raise RuntimeError("recovered directory parent link count changed")
    removed = "owned-cut" if replacement_inode is None else "replace-source"
    if operation == "rmdir":
        removed = "removed-directory"
    if operation not in ("truncate", "grow", "xattr", "xattr-remove", "create", "mkdir", "link") and f"/{removed}/" in namespace:
        raise RuntimeError("held-unlink retained its removed name")
    output.mkdir()
    (output / "namespace.txt").write_text(namespace)
    expected_files = {} if operation in ("truncate", "grow", "xattr", "xattr-remove", "create", "mkdir", "link") else {f"data/user/{removed}": None}
    if replacement_inode is not None:
        target_name = "truncate-target" if operation in ("truncate", "grow") else "replace-target"
        target_size = 1700 if operation == "truncate" else 4500
        if operation == "grow":
            target_size = 12345
        if operation in ("xattr", "xattr-remove"):
            target_name, target_size = "attribute-target", 1700
        if operation == "create":
            target_name, target_size = "created-target", 0
        if operation == "mkdir":
            target_name, target_size = "created-directory", 4096
        if operation == "link":
            target_name, target_size = "link-alias", 4500
        expected_content = (b"t" if operation in ("truncate", "xattr", "xattr-remove") else b"s") * target_size
        if operation == "grow":
            expected_content = b"t" * 1700 + bytes(target_size - 1700)
        target_output = ext4_image._debugfs(tools, image, f"stat /data/user/{target_name}")
        target = ext4_image._parse_stat(target_output, f"/data/user/{target_name}")
        if target["inode"] != replacement_inode or target["size"] != target_size or target["links"] != (2 if operation in ("mkdir", "link") else 1):
            raise RuntimeError("held-replace published the wrong inode or link count")
        if operation == "link":
            original = ext4_image._parse_stat(ext4_image._debugfs(tools, image,
                "stat /data/user/link-source"), "/data/user/link-source")
            if any(original[field] != target[field] for field in ("inode", "links", "mode", "size", "block_count_512")) or target["block_count_512"] != 16:
                raise RuntimeError("hard link source and alias do not share the exact inode accounting")
            expected_files["data/user/link-source"] = {
                "bytes": target_size, "sha256": hashlib.sha256(expected_content).hexdigest()}
        if operation == "create" and (target["mode"] != "0640" or target["block_count_512"] != 0):
            raise RuntimeError("created inode mode or empty allocation changed")
        if operation == "grow" and target["block_count_512"] != 8:
            raise RuntimeError("sparse growth allocated physical hole blocks")
        if operation == "xattr" and target["block_count_512"] != 16:
            raise RuntimeError("external xattr physical allocation changed")
        if operation == "xattr-remove" and target["block_count_512"] != 8:
            raise RuntimeError("removed external xattr block was not reclaimed")
        if operation == "mkdir":
            if target["mode"] != "0750" or target["block_count_512"] != 8 or not re.search(r"Type:\s+directory\b", target_output):
                raise RuntimeError("created directory mode type or allocation changed")
            expected_files[f"data/user/{target_name}"] = {"entries": []}
        else:
            content = output / f"{target_name}.bin"
            ext4_image._debugfs(tools, image, f'dump -p /data/user/{target_name} "{content.as_posix()}"')
            if content.read_bytes() != expected_content:
                raise RuntimeError("held-replace changed the published file contents")
            expected_files[f"data/user/{target_name}"] = {
                "bytes": target_size, "sha256": hashlib.sha256(expected_content).hexdigest()}
            if operation in ("xattr", "xattr-remove"):
                expected_files[f"data/user/{target_name}"]["xattrs"] = {
                    "user.cut": None if operation == "xattr-remove" else bytes(index % 251 for index in range(300)).hex()}
        if operation in ("truncate", "grow"):
            mapping = ext4_image._debugfs(tools, image, "bmap /data/user/truncate-target 0")
            blocks = re.findall(r"^([0-9]+)$", mapping, re.MULTILINE)
            if len(blocks) != 1 or int(blocks[0]) == 0:
                raise RuntimeError("truncate retained block mapping is missing or ambiguous")
            physical = int(blocks[0])
            with image.open("rb") as disk:
                disk.seek(physical * 4096 + 1700)
                if disk.read(4096 - 1700) != bytes(4096 - 1700):
                    raise RuntimeError("truncate recovery failed to zero the retained block tail")
    for executable, arguments, name in (
        (tools["e2fsck"], ["-f", "-n"], "e2fsck.txt"),
        ("dumpe2fs", ["-h"], "dumpe2fs.txt"),
        (tools["debugfs"], ["-R", "stat /data/user"], "debugfs.txt"),
    ):
        result = subprocess.run([executable, *arguments, str(image)], capture_output=True, text=True)
        (output / name).write_text(result.stdout + result.stderr)
        if result.returncode != 0:
            raise RuntimeError(f"held-unlink {name} refused the clean image")
    report["linux_kernel_read"] = ext4_kernel_read.verify_files(
        image, output / "linux-kernel", expected_files)
    report["image_sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
    (output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return report


def run(args):
    replacement = args.operation == "replace"
    truncating = args.operation in ("truncate", "grow")
    growing = args.operation == "grow"
    creating = args.operation in ("create", "mkdir")
    directory = args.operation == "mkdir"
    removing_directory = args.operation == "rmdir"
    attributing = args.operation in ("xattr", "xattr-remove")
    removing_attribute = args.operation == "xattr-remove"
    linking = args.operation == "link"
    pass_marker = PASS if not replacement else "ST EXT4 VFS held-replace old-or-new cleanup census exact"
    state_marker = "ST EXT4 HELD REPLACE initial" if replacement else "ST EXT4 HELD UNLINK initial"
    if truncating:
        pass_marker = "ST EXT4 VFS truncate old-or-new tail shared EOF census exact"
        state_marker = "ST EXT4 TRUNCATE initial"
        if growing:
            pass_marker = "ST EXT4 VFS grow old-or-new holes shared EOF census exact"
    if creating:
        pass_marker = "ST EXT4 VFS create exclusive mode empty contents census exact"
        state_marker = "ST EXT4 CREATE initial"
    if directory:
        pass_marker = "ST EXT4 VFS mkdir mode parent links empty snapshot census exact"
        state_marker = "ST EXT4 MKDIR initial"
    if removing_directory:
        pass_marker = "ST EXT4 VFS rmdir parent links held snapshot census exact"
        state_marker = "ST EXT4 RMDIR initial"
    if attributing:
        pass_marker = "ST EXT4 VFS external xattr bytes inode allocation census exact"
        state_marker = "ST EXT4 XATTR initial"
        if removing_attribute:
            pass_marker = "ST EXT4 VFS external xattr remove inode reclamation census exact"
    if linking:
        pass_marker = "ST EXT4 VFS hard link shared inode contents accounting census exact"
        state_marker = "ST EXT4 LINK initial"
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
        source.write_bytes((b"s" if replacement else b"c") * 4500)
        empty = work / "empty"
        empty.write_bytes(b"")
        files = [(source, "owned-cut"), (empty, "CUTUNLINK.TST")]
        if replacement:
            destination = work / "destination"
            destination.write_bytes(b"t" * 3000)
            files = [(source, "replace-source"), (destination, "replace-target"), (empty, "CUTREPLACE.TST")]
        if truncating:
            source.write_bytes(b"t" * (1700 if growing else 4500))
            files = [(source, "truncate-target"), (empty, "CUTGROW.TST" if growing else "CUTTRUNC.TST")]
        if creating:
            files = [(empty, "CUTMKDIR.TST" if directory else "CUTCREATE.TST")]
        if removing_directory:
            ext4_image._run([tools["debugfs"], "-w", "-R", "mkdir /data/user/removed-directory", initial])
            files = [(empty, "CUTRMDIR.TST")]
        if attributing:
            source.write_bytes(b"t" * 1700)
            files = [(source, "attribute-target"), (empty, "CUTXREM.TST" if removing_attribute else "CUTXATTR.TST")]
        if linking:
            source.write_bytes(b"s" * 4500)
            files = [(source, "link-source"), (empty, "CUTLINK.TST")]
        for file, name in files:
            ext4_image._run([tools["debugfs"], "-w", "-R",
                f'write "{file.as_posix()}" /data/user/{name}', initial])
        if removing_attribute:
            attribute = work / "attribute"
            attribute.write_bytes(bytes(index % 251 for index in range(300)))
            ext4_image._run([tools["debugfs"], "-w", "-R",
                f'ea_set -f "{attribute.as_posix()}" /data/user/attribute-target user.cut', initial])
    before = ext4_image.inspect_image(initial, tools=tools)
    parent = ext4_image._parse_stat(ext4_image._debugfs(tools, initial, "stat /data/user"), "/data/user")
    expected_parent_links = parent["links"] + (1 if directory else (-1 if removing_directory else 0))
    # Unlink/replace final close reclaims the removed inode. Shrink frees only
    # the second data block and retains the inode and partial first block.
    removed = "truncate-target" if truncating else ("replace-target" if replacement else "owned-cut")
    if removing_directory:
        removed = "removed-directory"
    if attributing:
        removed = "attribute-target"
    if linking:
        removed = "link-source"
    target = None if creating else ext4_image._parse_stat(ext4_image._debugfs(tools, initial,
        f"stat /data/user/{removed}"), f"/data/user/{removed}")
    reclaimed = 0 if growing else (1 if replacement or truncating else 2)
    initial_blocks = 1 if replacement or growing else 2
    initial_size = 1700 if growing else (3000 if replacement else 4500)
    if removing_directory:
        reclaimed, initial_blocks, initial_size = 1, 1, 4096
    if attributing:
        reclaimed, initial_blocks, initial_size = -1, 1, 1700
        if removing_attribute:
            reclaimed, initial_blocks = 1, 2
    if linking:
        reclaimed = 0
    if not creating and (target["size"] != initial_size or target["block_count_512"] != initial_blocks * 8):
        raise RuntimeError("held-unlink input has unexpected allocation geometry")
    replacement_inode = target["inode"] if truncating or attributing or linking else None
    if replacement:
        source_stat = ext4_image._parse_stat(ext4_image._debugfs(tools, initial,
            "stat /data/user/replace-source"), "/data/user/replace-source")
        if source_stat["size"] != 4500 or source_stat["block_count_512"] != 16:
            raise RuntimeError("held-replace source allocation geometry changed")
        replacement_inode = source_stat["inode"]
    expected_blocks, expected_inodes = before["free_blocks"] + reclaimed, before["free_inodes"] + (0 if truncating or attributing or linking else 1)
    if creating:
        expected_blocks, expected_inodes = before["free_blocks"] - (1 if directory else 0), before["free_inodes"] - 1
    iso = output / "verify.iso"
    recovery._build_iso(args.kernel.resolve(), iso, args.grub_mkrescue, args.grub_module_dir, None)
    baseline = output / "complete.raw"
    shutil.copyfile(initial, baseline)
    status, transcript = recovery._run_qemu(args.qemu, args.accel, iso, baseline,
        output / "complete.log", args.timeout)
    verify_exit(status, transcript, pass_marker)
    if creating:
        name = "created-directory" if directory else "created-target"
        created = ext4_image._parse_stat(ext4_image._debugfs(tools, baseline,
            f"stat /data/user/{name}"), f"/data/user/{name}")
        replacement_inode = created["inode"]
    inspect(baseline, tools, output / "complete", expected_blocks, expected_inodes, replacement_inode, args.operation, expected_parent_links)
    boundaries = re.findall(r"^ST EXT4 DURABLE (\d+) ([a-z-]+)$", transcript, re.MULTILINE)
    if not 1 <= len(boundaries) <= 64 or [int(number) for number, _ in boundaries] != list(range(1, len(boundaries) + 1)):
        raise RuntimeError("held-unlink trace is not a bounded contiguous boundary sequence")
    first_commit = next(int(number) for number, name in boundaries if name == "commit")
    reports = []
    repeated_recovery = []
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
                "ST FAIL" in transcript or pass_marker in transcript or "Phipia PANIC" in transcript:
            raise RuntimeError(f"held-unlink boundary {cut} did not cut exactly:\n" + recovery._transcript_tail(transcript))
        crashed = output / f"cut-{cut:02d}-crashed.raw"
        shutil.copyfile(image, crashed)
        status, transcript = recovery._run_qemu(args.qemu, args.accel, iso, image,
            output / f"cut-{cut:02d}-reboot.log", args.timeout)
        verify_exit(status, transcript, pass_marker)
        expected_state = "new" if cut >= first_commit else "old"
        if transcript.count(f"{state_marker} {expected_state}\n") != 1:
            raise RuntimeError(f"held-unlink boundary {cut} violated durable {expected_state} namespace")
        report = inspect(image, tools, output / f"cut-{cut:02d}", expected_blocks, expected_inodes, replacement_inode, args.operation, expected_parent_links)
        reports.append({"cut": cut, "boundary": boundary, "recovered_state": expected_state, "result": report,
            "crashed_sha256": hashlib.sha256(crashed.read_bytes()).hexdigest()})
        if cut == first_commit:
            # Use only the flushes performed during mount recovery, before the
            # VFS test can start another operation. Cut each, then recover again.
            mount_trace = transcript.split(f"{state_marker} new\n", 1)[0]
            recovery_boundaries = re.findall(r"^ST EXT4 DURABLE (\d+) ([a-z-]+)$", mount_trace, re.MULTILINE)
            if not recovery_boundaries or recovery_boundaries[0][1] != "checkpoint":
                raise RuntimeError("committed held unlink omitted checkpoint replay during mount")
            for recovery_number, recovery_boundary in recovery_boundaries:
                second_cut = int(recovery_number)
                if not 1 <= second_cut <= 64:
                    raise RuntimeError("held-unlink recovery trace exceeds the cut bound")
                prefix = f"recovery-cut-{second_cut:02d}"
                repeated_image = output / f"{prefix}.raw"
                repeated_iso = output / f"{prefix}.iso"
                shutil.copyfile(crashed, repeated_image)
                recovery._build_iso(args.kernel.resolve(), repeated_iso, args.grub_mkrescue,
                    args.grub_module_dir, second_cut)
                second_status, second_trace = recovery._run_qemu(args.qemu, args.accel,
                    repeated_iso, repeated_image, output / f"{prefix}.log", args.timeout)
                second_marker = f"ST EXT4 POWER CUT {second_cut} {recovery_boundary}"
                if second_status != recovery.POWER_CUT_EXIT_STATUS or second_trace.count(second_marker) != 1 or \
                        state_marker in second_trace or "ST FAIL" in second_trace or "Phipia PANIC" in second_trace:
                    raise RuntimeError(f"held-unlink recovery boundary {second_cut} did not cut during mount")
                second_crashed = output / f"{prefix}-crashed.raw"
                shutil.copyfile(repeated_image, second_crashed)
                final_status, final_trace = recovery._run_qemu(args.qemu, args.accel, iso,
                    repeated_image, output / f"{prefix}-reboot.log", args.timeout)
                verify_exit(final_status, final_trace, pass_marker)
                if final_trace.count(f"{state_marker} new\n") != 1:
                    raise RuntimeError("repeated recovery resurrected the committed removed name")
                result = inspect(repeated_image, tools, output / prefix, expected_blocks, expected_inodes, replacement_inode, args.operation, expected_parent_links)
                repeated_recovery.append({"cut": second_cut, "boundary": recovery_boundary, "result": result,
                    "crashed_sha256": hashlib.sha256(second_crashed.read_bytes()).hexdigest()})
    (output / "report.json").write_text(json.dumps({"operation": args.operation, "before": before, "reports": reports,
        "repeated_recovery": repeated_recovery,
        "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256(args.fixture.read_bytes()).hexdigest()}, indent=2, sort_keys=True) + "\n")
    print(f"ordinary VFS held {args.operation}: {len(boundaries)} operation cuts, {len(repeated_recovery)} repeated recovery cuts, allocation accounting, census and read-only fsck PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operation", choices=("unlink", "replace", "truncate", "grow", "create", "mkdir", "rmdir", "xattr", "xattr-remove", "link"), default="unlink")
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
