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
LONG_SYMLINK_TARGET = "link-source-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz-abcdefghijklmnopqrstuvwxyz"


def verify_exit(status, transcript, pass_marker=PASS):
    if status != recovery.PASS_EXIT_STATUS or transcript.count(pass_marker) != 1 or \
            transcript.count(recovery.PASS_MARKER) != 1 or "ST FAIL" in transcript or "Phipia PANIC" in transcript:
        raise RuntimeError(f"held-unlink reboot failed ({status}):\n" + recovery._transcript_tail(transcript))


def inspect(image, tools, output, expected_blocks, expected_inodes, replacement_inode=None, operation="unlink", expected_parent_links=None, expected_collision_inode=None):
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
    if operation in ("rename", "rename-cross", "rename-wrap"):
        removed = "rename-source"
    if operation not in ("truncate", "grow", "append", "overwrite", "chmod", "times", "xattr", "xattr-remove", "create", "mkdir", "link", "symlink", "symlink-long") and f"/{removed}/" in namespace:
        raise RuntimeError("held-unlink retained its removed name")
    output.mkdir()
    (output / "namespace.txt").write_text(namespace)
    expected_files = {} if operation in ("truncate", "grow", "append", "overwrite", "chmod", "times", "xattr", "xattr-remove", "create", "mkdir", "link", "symlink", "symlink-long") else {f"data/user/{removed}": None}
    if operation in ("symlink", "symlink-long"):
        target_name = LONG_SYMLINK_TARGET if operation == "symlink-long" else "link-source"
        literal = ext4_image._debugfs(tools, image, "stat /data/user/symbolic-alias")
        symbolic = ext4_image._parse_stat(literal, "/data/user/symbolic-alias")
        original = ext4_image._parse_stat(ext4_image._debugfs(tools, image,
            f"stat /data/user/{target_name}"), f"/data/user/{target_name}")
        if not re.search(r"Type:\s+symlink\b", literal) or symbolic["inode"] != replacement_inode or \
                symbolic["size"] != len(target_name) or symbolic["links"] != 1 or \
                symbolic["block_count_512"] != (8 if operation == "symlink-long" else 0):
            raise RuntimeError("symlink inode type target length or allocation changed")
        if original["inode"] == symbolic["inode"] or original["links"] != 1 or original["size"] != 4500 or original["block_count_512"] != 16:
            raise RuntimeError("symlink changed its source inode accounting")
        expected_content = {"bytes": 4500, "sha256": hashlib.sha256(b"s" * 4500).hexdigest()}
        expected_files[f"data/user/{target_name}"] = expected_content
        expected_files["data/user/symbolic-alias"] = {**expected_content, "symlink": target_name}
    elif replacement_inode is not None:
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
        if operation == "append":
            target_name, target_size = "append-target", 9000
        if operation == "overwrite":
            target_name, target_size = "overwrite-target", 4500
        if operation in ("chmod", "times"):
            target_name, target_size = "metadata-target", 1700
        if operation in ("rename", "rename-cross", "rename-wrap"):
            target_name = "rename-destination/moved-file" if operation == "rename-cross" else "rename-target"
        expected_content = (b"t" if operation in ("truncate", "chmod", "times", "xattr", "xattr-remove") else b"s") * target_size
        if operation == "grow":
            expected_content = b"t" * 1700 + bytes(target_size - 1700)
        if operation == "append":
            expected_content = b"t" * 4500 + b"s" * 4500
        if operation == "overwrite":
            expected_content = b"t" * 123 + b"s" * 4097 + b"t" * 280
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
        if operation == "append" and target["block_count_512"] != 24:
            raise RuntimeError("append allocated the wrong physical blocks")
        if operation == "overwrite" and (target["block_count_512"] != 16 or target["mode"] != "0644"):
            raise RuntimeError("in-place overwrite changed physical allocation or mode")
        if operation in ("rename", "rename-cross", "rename-wrap"):
            collision_name = "rename-destination/occupied" if operation == "rename-cross" else "rename-existing"
            collision = ext4_image._parse_stat(ext4_image._debugfs(tools, image,
                f"stat /data/user/{collision_name}"), f"/data/user/{collision_name}")
            if target["block_count_512"] != 16 or target["mode"] != "0644" or \
                    collision["inode"] != expected_collision_inode or collision["size"] != 1000 or \
                    collision["links"] != 1 or collision["mode"] != "0644" or collision["block_count_512"] != 8:
                raise RuntimeError("rename changed target allocation or the no-replace collision inode")
            expected_files[f"data/user/{collision_name}"] = {"bytes": 1000, "sha256": hashlib.sha256(b"c" * 1000).hexdigest()}
            if operation == "rename-cross":
                directory = ext4_image._parse_stat(ext4_image._debugfs(tools, image,
                    "stat /data/user/rename-destination"), "/data/user/rename-destination")
                if directory["links"] != 2 or directory["size"] != 4096 or directory["block_count_512"] != 8:
                    raise RuntimeError("cross-directory rename changed destination directory accounting")
                expected_files["data/user/rename-destination"] = {"entries": ["moved-file", "occupied"]}
        if operation == "xattr" and target["block_count_512"] != 16:
            raise RuntimeError("external xattr physical allocation changed")
        if operation == "xattr-remove" and target["block_count_512"] != 8:
            raise RuntimeError("removed external xattr block was not reclaimed")
        if operation == "mkdir":
            if target["mode"] != "0750" or target["block_count_512"] != 8 or not re.search(r"Type:\s+directory\b", target_output):
                raise RuntimeError("created directory mode type or allocation changed")
            expected_files[f"data/user/{target_name}"] = {"entries": []}
        else:
            content = output / f"{Path(target_name).name}.bin"
            ext4_image._debugfs(tools, image, f'dump -p /data/user/{target_name} "{content.as_posix()}"')
            if content.read_bytes() != expected_content:
                raise RuntimeError("held-replace changed the published file contents")
            expected_files[f"data/user/{target_name}"] = {
                "bytes": target_size, "sha256": hashlib.sha256(expected_content).hexdigest()}
            if operation in ("chmod", "times"):
                mode = 0o640 if operation == "chmod" else 0o644
                if int(target["mode"], 8) != mode or target["block_count_512"] != 8:
                    raise RuntimeError("metadata mutation changed mode or block allocation")
                offset = ext4_image._locate_inode(tools, image, f"/data/user/{target_name}")
                with image.open("rb") as disk:
                    disk.seek(offset)
                    inode = disk.read(256)
                if len(inode) != 256:
                    raise RuntimeError("metadata inode evidence is truncated")
                (output / "metadata-inode.bin").write_bytes(inode)
                (output / "metadata-debugfs.txt").write_text(target_output)
                expected_metadata = {"mode": mode}
                for field, low_offset, extra_offset, seconds, nanos in (
                        ("atime_ns", 8, 0x8c, 2200000000, 123456789),
                        ("mtime_ns", 16, 0x88, 2300000000, 987654321)):
                    if operation == "chmod":
                        seconds, nanos = ext4_image.FIXED_EPOCH, 0
                    low = int.from_bytes(inode[low_offset:low_offset + 4], "little", signed=True)
                    extra = int.from_bytes(inode[extra_offset:extra_offset + 4], "little")
                    if (low + ((extra & 3) << 32), extra >> 2) != (seconds, nanos):
                        raise RuntimeError(f"metadata mutation changed exact {field} encoding")
                    expected_metadata[field] = seconds * 1000000000 + nanos
                expected_files[f"data/user/{target_name}"].update(expected_metadata)
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
    renaming = args.operation in ("rename", "rename-cross", "rename-wrap")
    wrapped_rename = args.operation == "rename-wrap"
    cross_rename = args.operation == "rename-cross"
    collision_name = "rename-destination/occupied" if cross_rename else "rename-existing"
    truncating = args.operation in ("truncate", "grow")
    growing = args.operation == "grow"
    appending = args.operation == "append"
    overwriting = args.operation == "overwrite"
    control_name = {"append": "APPFAIL.BIN", "overwrite": "OVERFAIL.BIN",
        "truncate": "TRUNCFAIL.BIN", "grow": "GROWFAIL.BIN",
        "rename": "RENFAIL.BIN", "rename-cross": "RENFAIL.BIN"}.get(args.operation)
    storage_marker = f"ST EXT4 {'RENAME' if renaming else args.operation.upper()} storage"
    metadata_change = args.operation in ("chmod", "times")
    creating = args.operation in ("create", "mkdir")
    directory = args.operation == "mkdir"
    removing_directory = args.operation == "rmdir"
    attributing = args.operation in ("xattr", "xattr-remove")
    removing_attribute = args.operation == "xattr-remove"
    linking = args.operation == "link"
    symlinking = args.operation in ("symlink", "symlink-long")
    external_symlink = args.operation == "symlink-long"
    symlink_target = LONG_SYMLINK_TARGET if external_symlink else "link-source"
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
    if symlinking:
        pass_marker = "ST EXT4 VFS symlink target followed inode allocation census exact"
        state_marker = "ST EXT4 SYMLINK initial"
    if appending:
        pass_marker = "ST EXT4 VFS append old-or-new tail shared EOF allocation census exact"
        state_marker = "ST EXT4 APPEND initial"
    if overwriting:
        pass_marker = "ST EXT4 VFS overwrite flush-boundary contents held inode allocation census exact"
        state_marker = "ST EXT4 OVERWRITE initial"
    if metadata_change:
        pass_marker = "ST EXT4 VFS metadata old-or-new held inode fields contents allocation census exact"
        state_marker = "ST EXT4 METADATA initial"
    if renaming:
        pass_marker = "ST EXT4 VFS rename no-replace old-or-new held contents allocation census exact"
        state_marker = "ST EXT4 RENAME initial"
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
        if renaming:
            source.write_bytes(b"s" * 4500)
            collision = work / "collision"
            collision.write_bytes(b"c" * 1000)
            if cross_rename:
                ext4_image._run([tools["debugfs"], "-w", "-R", "mkdir /data/user/rename-destination", initial])
            files = [(source, "rename-source"), (collision, collision_name),
                (empty, "CUTRENWRAP.TST" if wrapped_rename else ("CUTRENCROSS.TST" if cross_rename else "CUTRENAME.TST"))]
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
        if symlinking:
            source.write_bytes(b"s" * 4500)
            files = [(source, symlink_target), (empty, "CUTSYMLONG.TST" if external_symlink else "CUTSYM.TST")]
        if appending:
            source.write_bytes(b"t" * 4500)
            files = [(source, "append-target"), (empty, "CUTAPPEND.TST")]
        if overwriting:
            source.write_bytes(b"t" * 4500)
            files = [(source, "overwrite-target"), (empty, "CUTOVER.TST")]
        if metadata_change:
            source.write_bytes(b"t" * 1700)
            files = [(source, "metadata-target"), (empty, "CUTMODE.TST" if args.operation == "chmod" else "CUTTIMES.TST")]
        if args.storage_failures:
            control = work / "storage-control"
            control.write_bytes((0x4F570000).to_bytes(4, "little"))
            files.append((control, control_name))
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
    if symlinking:
        removed = symlink_target
    if appending:
        removed = "append-target"
    if overwriting:
        removed = "overwrite-target"
    if metadata_change:
        removed = "metadata-target"
    if renaming:
        removed = "rename-source"
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
    if symlinking:
        reclaimed = -1 if external_symlink else 0
    if appending:
        reclaimed = -1
    if metadata_change:
        reclaimed, initial_blocks, initial_size = 0, 1, 1700
    if renaming or overwriting:
        reclaimed = 0
    if not creating and (target["size"] != initial_size or target["block_count_512"] != initial_blocks * 8):
        raise RuntimeError("held-unlink input has unexpected allocation geometry")
    replacement_inode = target["inode"] if truncating or attributing or linking or appending or overwriting or metadata_change or renaming else None
    expected_collision_inode = None
    if renaming:
        expected_collision_inode = ext4_image._parse_stat(ext4_image._debugfs(tools, initial,
            f"stat /data/user/{collision_name}"), f"/data/user/{collision_name}")["inode"]
    if replacement:
        source_stat = ext4_image._parse_stat(ext4_image._debugfs(tools, initial,
            "stat /data/user/replace-source"), "/data/user/replace-source")
        if source_stat["size"] != 4500 or source_stat["block_count_512"] != 16:
            raise RuntimeError("held-replace source allocation geometry changed")
        replacement_inode = source_stat["inode"]
    expected_blocks, expected_inodes = before["free_blocks"] + reclaimed, before["free_inodes"] + (0 if truncating or attributing or linking or appending or overwriting or metadata_change or renaming else 1)
    if creating:
        expected_blocks, expected_inodes = before["free_blocks"] - (1 if directory else 0), before["free_inodes"] - 1
    if symlinking:
        expected_inodes = before["free_inodes"] - 1
    iso = output / "verify.iso"
    recovery._build_iso(args.kernel.resolve(), iso, args.grub_mkrescue, args.grub_module_dir, None,
        storage_cut=0 if args.physical_cuts else None)
    baseline = output / "complete.raw"
    shutil.copyfile(initial, baseline)
    status, transcript = recovery._run_qemu(args.qemu, args.accel, iso, baseline,
        output / "complete.log", args.timeout)
    verify_exit(status, transcript, pass_marker)
    if creating or symlinking:
        name = "symbolic-alias" if symlinking else ("created-directory" if directory else "created-target")
        created = ext4_image._parse_stat(ext4_image._debugfs(tools, baseline,
            f"stat /data/user/{name}"), f"/data/user/{name}")
        replacement_inode = created["inode"]
    inspect(baseline, tools, output / "complete", expected_blocks, expected_inodes, replacement_inode, args.operation, expected_parent_links, expected_collision_inode)
    if args.physical_cuts:
        boundaries = re.findall(r"^ST EXT4 STORAGE (\d+) (write|flush) (\d+)$", transcript, re.MULTILINE)
        if not 1 <= len(boundaries) <= 128 or [int(item[0]) for item in boundaries] != list(range(1, len(boundaries) + 1)):
            raise RuntimeError("device-command trace is not a bounded contiguous sequence")
        if {item[1] for item in boundaries} != {"write", "flush"}:
            raise RuntimeError("device-command matrix omitted writes or flushes")
        wrapped_slots = []
        if wrapped_rename:
            physical = ext4_image.journal_inode_map(baseline, tools, output, "wrapped")
            logical = {block: index for index, block in enumerate(physical)}
            wrapped_slots = [logical[int(detail)] for _, kind, detail in boundaries
                if kind == "write" and int(detail) in logical and logical[int(detail)] != 0]
            if transcript.count("ST EXT4 RENAME WRAP prepared 340\n") != 1 or not wrapped_slots or \
                    wrapped_slots[0] != 1021 or len(wrapped_slots) <= 3 or wrapped_slots != [
                        1 + (1020 + index) % 1023 for index in range(len(wrapped_slots))]:
                raise RuntimeError("rename did not cross the verified physical journal end after its VFS preparation")
            if re.search(r"^ST EXT4 (?:STORAGE|DURABLE) ",
                    transcript.split("ST EXT4 RENAME WRAP prepared 340\n", 1)[0], re.MULTILINE):
                raise RuntimeError("wrapped rename baseline included commands from uncut preparation")
        reports, repeated_recovery = [], []

        def inspect_recovered(image, prefix):
            return inspect(image, tools, output / prefix, expected_blocks, expected_inodes,
                replacement_inode, args.operation, expected_parent_links, expected_collision_inode)

        def require_cut(status, trace, ordinal):
            if status != recovery.POWER_CUT_EXIT_STATUS or trace.count(f"ST EXT4 STORAGE CUT {ordinal}\n") != 1 or \
                    pass_marker in trace or "ST FAIL" in trace or "Phipia PANIC" in trace:
                raise RuntimeError(f"device-command cut {ordinal} did not terminate exactly:\n" + recovery._transcript_tail(trace))

        for number, kind, detail in boundaries:
            ordinal = int(number)
            prefix = f"device-cut-{ordinal:03d}"
            image, cut_iso = output / f"{prefix}.raw", output / f"{prefix}.iso"
            shutil.copyfile(initial, image)
            recovery._build_iso(args.kernel.resolve(), cut_iso, args.grub_mkrescue,
                args.grub_module_dir, None, storage_cut=ordinal)
            status, trace = recovery._run_qemu(args.qemu, args.accel, cut_iso, image,
                output / f"{prefix}.log", args.timeout)
            require_cut(status, trace, ordinal)
            if wrapped_rename and trace.count("ST EXT4 RENAME WRAP prepared 340\n") != 1:
                raise RuntimeError("wrapped rename cut escaped its prepared transaction window")
            if trace.count(f"ST EXT4 STORAGE {ordinal} {kind} {detail}\n") != 1:
                raise RuntimeError("device-command cut changed its baseline command identity")
            committed = bool(re.search(r"^ST EXT4 DURABLE \d+ commit$", trace, re.MULTILINE))
            crashed = output / f"{prefix}-crashed.raw"
            shutil.copyfile(image, crashed)
            status, reboot = recovery._run_qemu(args.qemu, args.accel, iso, image,
                output / f"{prefix}-reboot.log", args.timeout)
            verify_exit(status, reboot, pass_marker)
            states = re.findall(rf"^{re.escape(state_marker)} (old|new)$", reboot, re.MULTILINE)
            if len(states) != 1 or (committed and states[0] != "new"):
                raise RuntimeError("device-command cut violated old-or-new state or lost a durable commit")
            reports.append({"cut": ordinal, "kind": kind, "detail": int(detail), "commit_durable": committed,
                "recovered_state": states[0], "result": inspect_recovered(image, prefix),
                "crashed_sha256": hashlib.sha256(crashed.read_bytes()).hexdigest()})
            # At the first durable commit, replay still has physical home writes
            # and cleanup barriers. Cut each command before public VFS admission.
            if committed and not repeated_recovery:
                mount_trace = reboot.split(f"{state_marker} new\n", 1)[0]
                recovery_commands = re.findall(r"^ST EXT4 STORAGE (\d+) (write|flush) (\d+)$", mount_trace, re.MULTILINE)
                if not recovery_commands or not any(item[1] == "write" for item in recovery_commands):
                    raise RuntimeError("durable commit omitted physical recovery checkpoint writes")
                for recovery_number, recovery_kind, recovery_detail in recovery_commands:
                    second_cut = int(recovery_number)
                    second_prefix = f"device-recovery-{second_cut:03d}"
                    repeated_image = output / f"{second_prefix}.raw"
                    repeated_iso = output / f"{second_prefix}.iso"
                    shutil.copyfile(crashed, repeated_image)
                    recovery._build_iso(args.kernel.resolve(), repeated_iso, args.grub_mkrescue,
                        args.grub_module_dir, None, storage_cut=second_cut)
                    second_status, second_trace = recovery._run_qemu(args.qemu, args.accel, repeated_iso,
                        repeated_image, output / f"{second_prefix}.log", args.timeout)
                    require_cut(second_status, second_trace, second_cut)
                    if state_marker in second_trace or second_trace.count(
                            f"ST EXT4 STORAGE {second_cut} {recovery_kind} {recovery_detail}\n") != 1:
                        raise RuntimeError("repeated device cut escaped mount recovery or changed command identity")
                    second_hash = hashlib.sha256(repeated_image.read_bytes()).hexdigest()
                    shutil.copyfile(repeated_image, output / f"{second_prefix}-crashed.raw")
                    final_status, final_trace = recovery._run_qemu(args.qemu, args.accel, iso,
                        repeated_image, output / f"{second_prefix}-reboot.log", args.timeout)
                    verify_exit(final_status, final_trace, pass_marker)
                    if final_trace.count(f"{state_marker} new\n") != 1:
                        raise RuntimeError("repeated device cut lost committed state")
                    repeated_recovery.append({"cut": second_cut, "kind": recovery_kind,
                        "detail": int(recovery_detail), "crashed_sha256": second_hash,
                        "result": inspect_recovered(repeated_image, second_prefix)})
        if not repeated_recovery:
            raise RuntimeError("device-command matrix omitted repeated recovery")
        (output / "report.json").write_text(json.dumps({"operation": f"{args.operation}-device-cuts",
            "before": before, "reports": reports, "repeated_recovery": repeated_recovery,
            "wrapped_journal_slots": wrapped_slots,
            "preparation_scope": "340 uncut VFS chmod commits before each wrapped rename" if wrapped_rename else "none",
            "cut_scope": "after each completed NVMe block write and flush; no torn-sector or volatile-cache-loss model",
            "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
            "fixture_sha256": hashlib.sha256(args.fixture.read_bytes()).hexdigest()}, indent=2, sort_keys=True) + "\n")
        print(f"ordinary VFS {args.operation}: {len(reports)} device-command cuts, {len(repeated_recovery)} repeated recovery cuts, Linux readback, fsck and census PASS")
        return
    if args.storage_failures:
        totals = re.findall(rf"^{re.escape(storage_marker)} attempts (\d+)$", transcript, re.MULTILINE)
        if len(totals) != 1 or not 1 <= int(totals[0]) <= 128:
            raise RuntimeError("storage refusal baseline omitted its bounded attempt count")
        reports = []
        kinds = set()
        for ordinal in range(1, int(totals[0]) + 1):
            image = output / f"refusal-{ordinal:03d}.raw"
            control = output / f"refusal-{ordinal:03d}.bin"
            shutil.copyfile(initial, image)
            control.write_bytes((0x4F570000 | ordinal).to_bytes(4, "little"))
            ext4_image._run([tools["debugfs"], "-w", "-R", f"rm /data/user/{control_name}", image])
            ext4_image._run([tools["debugfs"], "-w", "-R",
                f'write "{control.as_posix()}" /data/user/{control_name}', image])
            prepared = ext4_image.inspect_image(image, tools=tools)
            if prepared["free_blocks"] != before["free_blocks"] or prepared["free_inodes"] != before["free_inodes"]:
                raise RuntimeError("storage refusal control changed fixture accounting")
            status, trace = recovery._run_qemu(args.qemu, args.accel, iso, image,
                output / f"refusal-{ordinal:03d}.log", args.timeout)
            verify_exit(status, trace, pass_marker)
            refused = re.findall(rf"^{re.escape(storage_marker)} refused (\d+) (write|flush)$", trace, re.MULTILINE)
            if len(refused) != 1 or int(refused[0][0]) != ordinal or trace.count(f"{state_marker} old\n") != 1:
                raise RuntimeError("VFS mutation did not exercise the exact storage refusal and identical retry")
            kinds.add(refused[0][1])
            after_retry = inspect(image, tools, output / f"refusal-{ordinal:03d}", expected_blocks,
                expected_inodes, replacement_inode, args.operation, expected_parent_links, expected_collision_inode)
            status, reboot = recovery._run_qemu(args.qemu, args.accel, iso, image,
                output / f"refusal-{ordinal:03d}-reboot.log", args.timeout)
            verify_exit(status, reboot, pass_marker)
            if reboot.count(f"{state_marker} new\n") != 1 or f"{storage_marker} refused" in reboot:
                raise RuntimeError("storage refusal retry did not survive cold reboot")
            after_reboot = inspect(image, tools, output / f"refusal-{ordinal:03d}-reboot", expected_blocks,
                expected_inodes, replacement_inode, args.operation, expected_parent_links, expected_collision_inode)
            reports.append({"ordinal": ordinal, "kind": refused[0][1], "after_retry": after_retry,
                "after_reboot": after_reboot, "prepared": prepared})
        if kinds != {"write", "flush"}:
            raise RuntimeError("storage refusal matrix did not exercise writes and flushes")
        (output / "report.json").write_text(json.dumps({"operation": f"{args.operation}-storage-refusals",
            "before": before, "attempts": int(totals[0]), "reports": reports,
            "scope": "one refusal before each platform callback, then identical retry; not torn physical writes",
            "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
            "fixture_sha256": hashlib.sha256(args.fixture.read_bytes()).hexdigest()}, indent=2, sort_keys=True) + "\n")
        print(f"ordinary VFS {args.operation}: {len(reports)} exact storage refusals, identical retries, cold reboots, Linux readback, fsck and census PASS")
        return
    boundaries = re.findall(r"^ST EXT4 DURABLE (\d+) ([a-z-]+)$", transcript, re.MULTILINE)
    if not 1 <= len(boundaries) <= 64 or [int(number) for number, _ in boundaries] != list(range(1, len(boundaries) + 1)):
        raise RuntimeError("held-unlink trace is not a bounded contiguous boundary sequence")
    first_commit = next(int(number) for number, name in boundaries if name == "commit")
    # Existing allocated data is written home before metadata commit. This
    # harness cuts after flushes, not between individual writes: it cannot
    # establish whole-write atomicity or exclude mixed bytes at other cuts.
    first_visible = first_commit
    if overwriting:
        first_visible = next(int(number) for number, name in boundaries if name == "ordered-data")
        if first_visible >= first_commit:
            raise RuntimeError("overwrite omitted ordered-data durability before metadata commit")
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
        expected_state = "new" if cut >= first_visible else "old"
        if transcript.count(f"{state_marker} {expected_state}\n") != 1:
            raise RuntimeError(f"held-unlink boundary {cut} violated durable {expected_state} namespace")
        report = inspect(image, tools, output / f"cut-{cut:02d}", expected_blocks, expected_inodes, replacement_inode, args.operation, expected_parent_links, expected_collision_inode)
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
                result = inspect(repeated_image, tools, output / prefix, expected_blocks, expected_inodes, replacement_inode, args.operation, expected_parent_links, expected_collision_inode)
                repeated_recovery.append({"cut": second_cut, "boundary": recovery_boundary, "result": result,
                    "crashed_sha256": hashlib.sha256(second_crashed.read_bytes()).hexdigest()})
    (output / "report.json").write_text(json.dumps({"operation": args.operation, "before": before, "reports": reports,
        "cut_scope": "after durability flushes only; no whole-write atomicity claim",
        "repeated_recovery": repeated_recovery,
        "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256(args.fixture.read_bytes()).hexdigest()}, indent=2, sort_keys=True) + "\n")
    print(f"ordinary VFS held {args.operation}: {len(boundaries)} operation cuts, {len(repeated_recovery)} repeated recovery cuts, allocation accounting, census and read-only fsck PASS")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operation", choices=("unlink", "replace", "rename", "rename-cross", "rename-wrap", "truncate", "grow", "append", "overwrite", "chmod", "times", "create", "mkdir", "rmdir", "xattr", "xattr-remove", "link", "symlink", "symlink-long"), default="unlink")
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--grub-mkrescue", default="grub-mkrescue")
    parser.add_argument("--grub-module-dir", type=Path)
    parser.add_argument("--accel", default="tcg")
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--storage-failures", action="store_true")
    parser.add_argument("--physical-cuts", action="store_true")
    args = parser.parse_args()
    if args.storage_failures and args.operation not in ("overwrite", "append", "truncate", "grow", "rename", "rename-cross"):
        parser.error("--storage-failures requires --operation overwrite, append, truncate, grow, rename or rename-cross")
    if args.physical_cuts and (args.storage_failures or args.operation not in ("rename", "rename-cross", "rename-wrap", "append", "truncate", "grow")):
        parser.error("--physical-cuts requires rename, rename-cross, rename-wrap, append, truncate or grow and excludes --storage-failures")
    if args.operation == "rename-wrap" and not args.physical_cuts:
        parser.error("--operation rename-wrap requires --physical-cuts")
    run(args)


if __name__ == "__main__":
    main()
