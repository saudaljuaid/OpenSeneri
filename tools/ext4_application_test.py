#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the existing desktop journey on ext4, cleanly reboot, and compare data.

This is bounded application evidence, not the complete release/power-cut matrix.
Only disposable regular-file images are attached to QEMU.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace

import ext4_image


SPEC = importlib.util.spec_from_file_location("phipia_capture", Path(__file__).with_name("capture-phipia.py"))
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)
REBOOT = b"restarting after clean synchronization"
CENSUS = b"Phipia: reboot VFS ext4 handles mounts reservations snapshots zero NVMe released heap paging valid"
EXPECTED = {
    "PHIPIA.BMP": 54 + 320 * 180 * 3,
    "PHIPIA - Copy.BMP": 54 + 320 * 180 * 3,
    "PAINT.BMP": 54 + 320 * 180 * 3,
    "MEDIAEDT.PHI": 424,
    "PHIPMED.PHI": 688,
    "EXPORT.BMP": 54 + 320 * 180 * 3,
    "SETTINGS.PHI": 16,
}


def inspect_results(image, output, label):
    """Inspect only a stopped, cleanly unmounted guest image."""
    tools = ext4_image.require_tools()
    report = ext4_image.inspect_image(image, tools=tools)
    if report["needs_recovery"]:
        raise RuntimeError("application journey did not cleanly unmount")
    files = {}
    target = output / label
    target.mkdir()
    namespace = ext4_image._debugfs(tools, image, "ls -p /")
    (target / "namespace.txt").write_text(namespace)
    for line in namespace.splitlines():
        fields = line.split("/")
        if len(fields) > 5:
            name = fields[5]
            if name == "journey" or name.startswith(("SNTMP", "PNTMP", "MSTMP", "METMP", "MEXTP", "STTMP", "CPTMP")):
                raise RuntimeError(f"application cleanup leaked {name}")
    for name in (*EXPECTED, "NOTES.TXT", "JOURNEY.TXT"):
        destination = target / name
        ext4_image._debugfs(tools, image, f'dump "/{name}" "{destination.as_posix()}"')
        data = destination.read_bytes()
        if name in EXPECTED and len(data) != EXPECTED[name]:
            raise RuntimeError(f"wrong application output length: {name}: {len(data)}")
        if name == "NOTES.TXT" and b"Massive Phipia update." not in data:
            raise RuntimeError("Notes content was not persisted")
        if name == "JOURNEY.TXT" and data != b"ext4-persistent\n":
            raise RuntimeError(f"terminal contents changed: {data!r}")
        if name == "SETTINGS.PHI" and data != b"PHIPCFG\x01\x03\x00\x00\x01\x01\x00\x00\x00":
            raise RuntimeError(f"Settings control was not saved: {data.hex()}")
        files[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    if files["PHIPIA.BMP"] != files["PHIPIA - Copy.BMP"]:
        raise RuntimeError("Files copy changed the source bitmap bytes")
    report["application_files"] = files
    report["image_sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
    (target / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    for command, name in (([tools["e2fsck"], "-f", "-n", str(image)], "e2fsck.txt"),
                          (["dumpe2fs", "-h", str(image)], "dumpe2fs.txt")):
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        (target / name).write_text(result.stdout + result.stderr)
        if result.returncode != 0:
            raise RuntimeError(f"{name} rejected application results")
    return files


def terminal(qmp, pointer):
    pointer.rehome()
    pointer.move_to(capture.dock_item_center(capture.DOCK_TERMINAL), capture.DOCK_POINTER_Y)
    pointer.click()
    pointer.settle_guest(0.5)


def command(qmp, serial, text, marker=capture.PROMPT):
    offset = serial.stat().st_size
    capture.send_text(qmp, text)
    qmp.hmp("sendkey ret")
    capture.wait_serial_after(serial, offset, marker, timeout=30.0)


def boot(args, image, output, work, number):
    serial = output / f"boot-{number}.log"
    port = capture.free_port()
    qemu_command = [args.qemu, "-machine", "accel=tcg", "-m", "128M", "-smp", "1",
        "-boot", "order=d", "-cdrom", str(args.iso.resolve()), "-display", "none",
        *capture.storage_arguments(args.system, image, "ext4"),
        "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-serial", f"file:{serial}", "-no-reboot"]
    stderr = output / f"qemu-{number}.log"
    with stderr.open("w") as error_stream:
        process = subprocess.Popen(qemu_command, stdout=subprocess.DEVNULL, stderr=error_stream)
        qmp = None
        try:
            qmp = capture.Qmp(port)
            capture.wait_serial(serial, (capture.PROOF_LINE, capture.PROMPT))
            pointer = capture.Pointer(qmp)
            if number == 1:
                session = SimpleNamespace(data_filesystem="ext4")
                events, frames, timestamps = capture.capture_phipia_session(
                    session, qmp, pointer, work, output, image, serial)
                if not (capture.PHIPIA_REQUIRED_EVENTS | {"files_copied"}).issubset(events):
                    raise RuntimeError("desktop application actions were omitted")
                capture.encode(args.ffmpeg, frames, timestamps, 24,
                    timestamps[-1] - timestamps[0] + 2.0, output / "ext4-applications.mp4")
                # Settings is the fifth window in the existing scripted session,
                # at (138,84), before maximizing. Coordinates then follow the
                # production settings.c caption/grid/row geometry at 1024x768.
                pointer.rehome()
                pointer.move_to(capture.dock_item_center(capture.DOCK_SETTINGS), capture.DOCK_POINTER_Y)
                pointer.click()
                pointer.settle_guest(0.5)
                pointer.move_to(929, 100)
                pointer.click()
                pointer.settle_guest(0.4)
                pointer.move_to(180, 265)
                pointer.click()
                pointer.settle_guest(0.4)
                pointer.move_to(820, 250)
                pointer.click()
                capture.wait_for_named_file(image, "SETTINGS.PHI", 16, filesystem="ext4")
                capture.capture_png(qmp, work, output, "ext4-settings-saved")
                terminal(qmp, pointer)
                command(qmp, serial, "mkdir journey")
                command(qmp, serial, "write journey/state.txt ext4-persistent")
                command(qmp, serial, "mv journey/state.txt JOURNEY.TXT")
                command(qmp, serial, "rm journey")
            else:
                capture.wait_serial(serial, (b"Phipia: settings restored from ext4 Data",))
                capture.capture_png(qmp, work, output, "ext4-cold-boot-restored")
                pointer.prime_terminal()
                terminal(qmp, pointer)
                command(qmp, serial, "read JOURNEY.TXT", b"ext4-persistent")
                command(qmp, serial, "read NOTES.TXT", b"Massive Phipia update.")
            command(qmp, serial, "sync", b"data synchronized")
            command(qmp, serial, "reboot", REBOOT)
            if CENSUS not in serial.read_bytes():
                raise RuntimeError("clean reboot omitted ext4 release census")
            if process.wait(timeout=30) != 0:
                raise RuntimeError("QEMU did not stop at clean guest reboot")
        except Exception:
            # Preserve the guest failure context before shutting down QEMU. A
            # missing output file alone cannot distinguish a UI mistake from a
            # filesystem refusal or kernel panic.
            if qmp is not None and process.poll() is None:
                try:
                    capture.capture_png(qmp, work, output, f"ext4-boot-{number}-failed")
                except (OSError, RuntimeError):
                    pass
            if serial.exists():
                print(serial.read_bytes()[-8192:].decode("utf-8", errors="replace"), flush=True)
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument("--system", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    image = output / "ext4-data.raw"
    ext4_image.build_image(image)
    (output / "head.txt").write_text(subprocess.check_output(["git", "rev-parse", "HEAD"], text=True))
    tools = ext4_image.require_tools()
    for name, executable in {**tools, "dumpe2fs": "dumpe2fs"}.items():
        version = subprocess.run([executable, "-V"], capture_output=True, text=True, check=False)
        (output / f"{name}-version.txt").write_text(version.stdout + version.stderr)
    with tempfile.TemporaryDirectory(prefix="application-assets-", dir=output) as raw:
        work = Path(raw)
        bitmap = work / "PHIPIA.BMP"
        subprocess.run([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i",
            str(Path(__file__).resolve().parent.parent / "assets/phipia/wallpaper.png"),
            "-vf", "scale=320:180", "-pix_fmt", "bgr24", "-c:v", "bmp", str(bitmap)], check=True)
        note = work / "NOTES.TXT"
        note.write_bytes(b"Ready for a Phipia note.")
        for source in (bitmap, note):
            ext4_image._run([tools["debugfs"], "-w", "-R", f'write "{source.as_posix()}" /{source.name}', image])
        ext4_image.inspect_image(image)
        boot(args, image, output, work, 1)
        before = inspect_results(image, output, "after-save")
        boot(args, image, output, work, 2)
        after = inspect_results(image, output, "after-cold-boot")
        if before != after:
            raise RuntimeError("application namespace/content hashes changed across cold boot")
    print("ext4 application save, clean reboot, remount, hashes and read-only fsck: PASS")


if __name__ == "__main__":
    main()
