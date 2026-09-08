#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Refuse a Paint save on block/inode exhaustion, reclaim in Phip, and retry."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

import ext4_application_test as applications
import ext4_image
import ext4_kernel_read

capture = applications.capture
PAINT_BYTES = 54 + 320 * 180 * 3


def click(pointer, x, y):
    pointer.move_to(x, y)
    pointer.click()


def dump_paint(image, tools, destination):
    ext4_image._debugfs(tools, image, f'dump /PAINT.BMP "{destination.as_posix()}"')
    return destination.read_bytes()


def boot(args, image, output, work, number, original, before):
    serial = output / f"boot-{number}.log"
    port = capture.free_port()
    command = [args.qemu, "-machine", "accel=tcg", "-m", "128M", "-smp", "1",
        "-boot", "order=d", "-cdrom", str(args.iso.resolve()), "-display", "none",
        *capture.storage_arguments(args.system, image, "ext4"),
        "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-serial", f"file:{serial}", "-no-reboot"]
    with (output / f"qemu-{number}.log").open("w") as errors:
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=errors)
        qmp = None
        try:
            qmp = capture.Qmp(port)
            capture.wait_serial(serial, (capture.PROOF_LINE, capture.PROMPT))
            pointer = capture.Pointer(qmp)
            pointer.prime_terminal()
            if number == 1:
                offset = serial.stat().st_size
                pointer.rehome()
                click(pointer, capture.dock_item_center(capture.DOCK_CANVAS), capture.DOCK_POINTER_Y)
                capture.wait_serial_after(serial, offset, b"Phipia: Paint opened")
                pointer.settle_guest(0.5)
                # First application window is the home frame (82,40 860x602).
                click(pointer, 873, 56)
                pointer.settle_guest(0.4)
                click(pointer, 220, 96)
                click(pointer, 370, 407)
                click(pointer, 500, 328)
                for _ in range(4):
                    qmp.hmp("sendkey backspace")
                pointer.settle_guest(0.2)
                capture.send_text(qmp, "320", 0.060)
                click(pointer, 500, 366)
                for _ in range(3):
                    qmp.hmp("sendkey backspace")
                pointer.settle_guest(0.2)
                capture.send_text(qmp, "180", 0.060)
                click(pointer, 550, 445)
                pointer.settle_guest(0.4)
                pointer.drag_to(60, 200, 250, 290)
                offset = serial.stat().st_size
                click(pointer, 42, 16)
                capture.wait_serial_after(serial, offset, b"Phipia: Save failed: ", timeout=90.0)
                if b"runtime disabled" in serial.read_bytes() or b"Phipia PANIC" in serial.read_bytes():
                    raise RuntimeError("storage refusal disabled the desktop")
                capture.capture_png(qmp, work, output, "paint-full-refused")
                tools = ext4_image.require_tools()
                if dump_paint(image, tools, output / "paint-after-refusal.bmp") != original:
                    raise RuntimeError("failed Paint publication changed the original image")
                refused = ext4_image.parse_superblock(image.read_bytes())
                if any(refused[field] != before[field] for field in ("free_blocks", "free_inodes")):
                    raise RuntimeError("failed Paint save leaked blocks or an inode")
                qmp.hmp("sendkey esc")
                pointer.settle_guest(0.4)
                applications.terminal(qmp, pointer)
                applications.command(qmp, serial, "rm system/release")
                applications.command(qmp, serial, "sync", b"data synchronized")
                reclaimed = ext4_image.parse_superblock(image.read_bytes())
                if reclaimed["free_blocks"] != before["free_blocks"] + 64 or reclaimed["free_inodes"] != before["free_inodes"] + 1:
                    raise RuntimeError("Phip did not reclaim the exact repair file")
                pointer.rehome()
                click(pointer, capture.dock_item_center(capture.DOCK_CANVAS), capture.DOCK_POINTER_Y)
                pointer.settle_guest(0.5)
                offset = serial.stat().st_size
                click(pointer, 42, 16)
                capture.wait_serial_after(serial, offset, b"Phipia: Paint saved PAINT.BMP", timeout=90.0)
                capture.wait_for_named_file(image, "PAINT.BMP", PAINT_BYTES, filesystem="ext4")
                capture.capture_png(qmp, work, output, "paint-space-reclaimed-retry")
                applications.terminal(qmp, pointer)
            else:
                applications.terminal(qmp, pointer)
                applications.command(qmp, serial, "stat PAINT.BMP", b"172854 bytes")
                capture.capture_png(qmp, work, output, "paint-low-space-cold-boot")
            applications.command(qmp, serial, "sync", b"data synchronized")
            applications.command(qmp, serial, "reboot", applications.REBOOT)
            if applications.CENSUS not in serial.read_bytes() or process.wait(timeout=30) != 0:
                raise RuntimeError("Paint low-space reboot omitted clean shutdown and resource census")
        except Exception:
            if qmp is not None and process.poll() is None:
                try:
                    capture.capture_png(qmp, work, output, f"boot-{number}-failed")
                except (OSError, RuntimeError):
                    pass
            if serial.exists():
                print(serial.read_bytes()[-4096:].decode(errors="replace"), flush=True)
            raise
        finally:
            if qmp is not None:
                if process.poll() is None:
                    try:
                        qmp.execute("quit")
                    except (OSError, RuntimeError):
                        pass
                qmp.close()
            if process.poll() is None:
                process.kill()
            process.wait()


def inspect(image, output, label, before, released_blocks, original, expected=None):
    tools = ext4_image.require_tools()
    target = output / label
    target.mkdir()
    report = ext4_image.inspect_image(image, tools=tools)
    if report["needs_recovery"] or report["free_blocks"] != before["free_blocks"] + released_blocks or \
            report["free_inodes"] != before["free_inodes"] + 1:
        raise RuntimeError("Paint retry or reclamation changed allocation accounting")
    content = dump_paint(image, tools, target / "PAINT.BMP")
    if len(content) != PAINT_BYTES or content == original or (expected is not None and content != expected):
        raise RuntimeError("Paint retry did not preserve its edited bitmap across reboot")
    names = ext4_image._debugfs(tools, image, "ls -p /")
    (target / "namespace.txt").write_text(names)
    if any("/PNTMP" in line for line in names.splitlines()):
        raise RuntimeError("Paint retry leaked an owned scratch file")
    manifest = {"PAINT.BMP": {"bytes": len(content), "sha256": hashlib.sha256(content).hexdigest()},
        "system/release": None}
    if "inode_fill_count" in before:
        inode_names = ext4_image._debugfs(tools, image, "ls -p /inode-full")
        if sorted(inode_names.splitlines()) != sorted((output / "inode-namespace-before.txt").read_text().splitlines()):
            raise RuntimeError("Paint inode exhaustion changed unrelated inode identities or names")
        (target / "inode-namespace.txt").write_text(inode_names)
        manifest["inode-full"] = {"entries": sorted(f"entry-{index}" for index in range(before["inode_fill_count"]))}
    report["linux_kernel_read"] = ext4_kernel_read.verify_files(image, target / "linux-kernel", manifest)
    report["image_sha256"] = ext4_kernel_read.digest(image)
    for executable, arguments, name in ((tools["e2fsck"], ["-f", "-n"], "e2fsck.txt"),
            ("dumpe2fs", ["-h"], "dumpe2fs.txt")):
        result = subprocess.run([executable, *arguments, str(image)], capture_output=True, text=True)
        (target / name).write_text(result.stdout + result.stderr)
        if result.returncode != 0:
            raise RuntimeError(f"{name} refused the Paint retry result")
    (target / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return content


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--system", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--inodes", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    image = output / "ext4-data.raw"
    ext4_image.build_image(image)
    tools = ext4_image.require_tools()
    (output / "head.txt").write_text(subprocess.check_output(["git", "rev-parse", "HEAD"], text=True))
    with tempfile.TemporaryDirectory(prefix="paint-space-", dir=output) as raw:
        work = Path(raw)
        bitmap = work / "PAINT.BMP"
        subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i",
            str(Path(__file__).resolve().parent.parent / "assets/phipia/wallpaper.png"),
            "-vf", "scale=320:180", "-pix_fmt", "bgr24", "-c:v", "bmp", str(bitmap)], check=True)
        original = bitmap.read_bytes()
        ext4_image._run([tools["debugfs"], "-w", "-R", f'write "{bitmap.as_posix()}" /PAINT.BMP', image])
        available = ext4_image.parse_superblock(image.read_bytes())["free_blocks"]
        if available <= 128:
            raise RuntimeError("Paint fixture has insufficient initial free space")
        if not args.inodes:
            filler = work / "filler"
            with filler.open("wb") as stream:
                for _ in range(available - 80):
                    stream.write(b"Z" * 4096)
            ext4_image._run([tools["debugfs"], "-w", "-R", f'write "{filler.as_posix()}" /system/filler', image])
        release = work / "release"
        release.write_bytes(b"R" * (64 * 4096))
        ext4_image._run([tools["debugfs"], "-w", "-R", f'write "{release.as_posix()}" /system/release', image])
        released = ext4_image._parse_stat(ext4_image._debugfs(tools, image,
            "stat /system/release"), "/system/release")["block_count_512"] // 8
        if args.inodes:
            available_inodes = ext4_image.parse_superblock(image.read_bytes())["free_inodes"]
            if not 2 < available_inodes < 8192:
                raise RuntimeError("Paint inode fixture exceeds the admitted census bound")
            empty = work / "empty"
            empty.write_bytes(b"")
            commands = work / "fill-inodes.commands"
            commands.write_text("mkdir /inode-full\n" + "".join(
                f'write "{empty.as_posix()}" /inode-full/entry-{index}\n'
                for index in range(available_inodes - 1)))
            result = ext4_image._run([tools["debugfs"], "-w", "-f", commands, image])
            (output / "fixture-debugfs.txt").write_text(result.stdout)
            (output / "inode-namespace-before.txt").write_text(ext4_image._debugfs(tools, image, "ls -p /inode-full"))
        before = ext4_image.inspect_image(image, tools=tools)
        if args.inodes:
            if before["free_inodes"] != 0 or before["free_blocks"] <= 128 or released != 64:
                raise RuntimeError("Paint fixture does not isolate inode exhaustion with sufficient free blocks")
            before["inode_fill_count"] = available_inodes - 1
        elif not 1 <= before["free_blocks"] < 43 or released != 64:
            raise RuntimeError("Paint fixture does not force the required low-space refusal")
        (output / "before.json").write_text(json.dumps(before, indent=2, sort_keys=True) + "\n")
        boot(args, image, output, work, 1, original, before)
        saved = inspect(image, output, "after-retry", before, released, original)
        boot(args, image, output, work, 2, original, before)
        inspect(image, output, "after-cold-boot", before, released, original, saved)
    exhaustion = "inode exhaustion" if args.inodes else "ENOSPC"
    print(f"ext4 Paint {exhaustion} original preservation, in-memory retry, Phip reclamation, cold boot, Linux readback and fsck: PASS")


if __name__ == "__main__":
    main()
