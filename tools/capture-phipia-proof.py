#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Capture three deterministic Phipia frames from a live QEMU guest.

QMP supplies both the screenshot command and narrowly scoped HMP input. The
images therefore come from the emulated display device; no UI is recreated on
the host. Only Python's standard library is required.
"""

import argparse
import json
import os
import socket
import struct
import subprocess
import time
import zlib
from pathlib import Path


PROOF_LINE = b"Phipia: Boot Ledger installed proof passed"
USERLAND_LINES = {
    "uname": b"RW USERLAND launch completed successfully uname ordinal 1",
    "cat": b"RW USERLAND launch completed successfully cat ordinal 1",
}
CAT_WAIT_LINE = b"RW USERLAND cat foreground launch yielded to Phipia"


def png_chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(
        ">I", zlib.crc32(kind + body) & 0xFFFFFFFF
    )


def ppm_to_png(source, destination):
    data = Path(source).read_bytes()
    tokens = []
    position = 0
    while len(tokens) < 4:
        while position < len(data) and data[position] in b" \t\r\n":
            position += 1
        if position < len(data) and data[position] == ord("#"):
            position = data.find(b"\n", position) + 1
            continue
        end = position
        while end < len(data) and data[end] not in b" \t\r\n":
            end += 1
        tokens.append(data[position:end])
        position = end
    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise RuntimeError("QEMU screendump is not an 8-bit binary PPM")
    width, height = int(tokens[1]), int(tokens[2])
    while position < len(data) and data[position] in b" \t\r\n":
        position += 1
    pixels = data[position:]
    if len(pixels) != width * height * 3:
        raise RuntimeError("QEMU screendump pixel body is truncated")
    rows = b"".join(
        b"\x00" + pixels[y * width * 3:(y + 1) * width * 3]
        for y in range(height)
    )
    png = b"\x89PNG\r\n\x1a\n"
    png += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += png_chunk(b"IDAT", zlib.compress(rows, 9))
    png += png_chunk(b"IEND", b"")
    Path(destination).write_bytes(png)


class Qmp:
    def __init__(self, port):
        deadline = time.monotonic() + 10.0
        while True:
            try:
                self.socket = socket.create_connection(("127.0.0.1", port), 0.5)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise RuntimeError("QMP did not accept a connection")
                time.sleep(0.05)
        self.file = self.socket.makefile("rwb", buffering=0)
        self._read_message()
        self.execute("qmp_capabilities")

    def _read_message(self):
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError("QMP disconnected")
            message = json.loads(line)
            if "event" not in message:
                return message

    def execute(self, command, arguments=None):
        request = {"execute": command}
        if arguments is not None:
            request["arguments"] = arguments
        self.file.write(json.dumps(request).encode("ascii") + b"\r\n")
        response = self._read_message()
        if "error" in response:
            raise RuntimeError(f"QMP {command} failed: {response['error']}")
        return response.get("return")

    def hmp(self, command):
        return self.execute("human-monitor-command", {"command-line": command})

    def close(self):
        self.file.close()
        self.socket.close()


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_serial(path, marker, timeout=35.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and marker in path.read_bytes():
            return
        time.sleep(0.05)
    transcript = path.read_bytes() if path.exists() else b""
    tail = transcript[-8192:].decode("utf-8", errors="replace")
    raise RuntimeError(
        f"serial transcript omitted {marker.decode('ascii')}\n"
        f"--- serial transcript tail ({len(transcript)} bytes total) ---\n"
        f"{tail}\n--- end serial transcript tail ---"
    )


def wait_serial_after(path, anchor, marker, timeout=35.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            transcript = path.read_bytes()
            position = transcript.find(anchor)
            if position >= 0 and marker in transcript[position + len(anchor):]:
                return
        time.sleep(0.05)
    transcript = path.read_bytes() if path.exists() else b""
    tail = transcript[-8192:].decode("utf-8", errors="replace")
    raise RuntimeError(
        f"serial transcript omitted {marker.decode('ascii')} after "
        f"{anchor.decode('ascii')}\n"
        f"--- serial transcript tail ({len(transcript)} bytes total) ---\n"
        f"{tail}\n--- end serial transcript tail ---"
    )


def capture(qmp, directory, stem):
    ppm = directory / f"{stem}.ppm"
    png = directory / f"{stem}.png"
    qmp.execute("screendump", {
        "filename": ppm.resolve().as_posix(), "format": "ppm"
    })
    ppm_to_png(ppm, png)
    ppm.unlink()
    return png


def send_text(qmp, text, delay=0.04):
    for key in text:
        qmp.hmp(f"sendkey {'spc' if key == ' ' else key}")
        time.sleep(delay)


def storage_arguments(userspace, system, data):
    if userspace is not None:
        return [
            "-blockdev",
            f"driver=file,filename={Path(userspace).resolve()},node-name=userland-file,read-only=on,auto-read-only=off",
            "-blockdev",
            "driver=raw,file=userland-file,node-name=userland-raw,read-only=on",
            "-device",
            "nvme,serial=phipia-userland,drive=userland-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1",
        ]
    return [
        "-blockdev",
        f"driver=file,filename={Path(system).resolve()},node-name=system-file,read-only=on,auto-read-only=off",
        "-blockdev",
        "driver=raw,file=system-file,node-name=system-raw,read-only=on",
        "-device",
        "nvme,serial=phipia-system-fat32,drive=system-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1",
        "-blockdev",
        f"driver=file,filename={Path(data).resolve()},node-name=data-file,read-only=off,auto-read-only=off",
        "-blockdev",
        "driver=raw,file=data-file,node-name=data-raw,read-only=off",
        "-device",
        "nvme,serial=phipia-data-fat32,drive=data-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1",
    ]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--iso", required=True)
    parser.add_argument("--userspace")
    parser.add_argument("--system")
    parser.add_argument("--data")
    parser.add_argument("--output", required=True)
    parser.add_argument(
        "--interaction", choices=sorted(USERLAND_LINES), default="cat"
    )
    args = parser.parse_args()
    if (args.userspace is None) == (args.system is None):
        parser.error("provide --userspace or the --system/--data pair")
    if args.userspace is None and args.data is None:
        parser.error("--system requires --data")
    if args.userspace is not None and args.data is not None:
        parser.error("--userspace cannot be combined with --data")

    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    serial = output / "capture-serial.log"
    if serial.exists():
        serial.unlink()
    port = free_port()
    command = [
        args.qemu, "-machine", "accel=tcg", "-m", "128M", "-smp", "1",
        "-boot", "order=d", "-cdrom", str(Path(args.iso).resolve()),
        "-display", "none",
        *storage_arguments(args.userspace, args.system, args.data),
        "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-serial", f"file:{serial}", "-no-reboot"
    ]
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    qmp = None
    try:
        qmp = Qmp(port)
        wait_serial(serial, PROOF_LINE)
        wait_serial_after(serial, PROOF_LINE, b"phip> ")
        time.sleep(0.25)
        # Park the real PS/2 cursor on empty desktop space so the clean frame
        # keeps every status label unobscured.
        qmp.hmp("mouse_move -260 320")
        time.sleep(0.12)
        clean = capture(qmp, output, "phipia-proof")

        # Focus remains on Terminal while the real PS/2 cursor hovers the
        # second tile in the vertical Workspace dock.
        for dx, dy in ((230, -226), (235, -227)):
            qmp.hmp(f"mouse_move {dx} {dy}")
            time.sleep(0.08)
        focus = capture(qmp, output, "phipia-proof-focus")

        qmp.hmp("sendkey ret")
        time.sleep(0.20)
        send_text(qmp, f"linux {args.interaction}")
        qmp.hmp("sendkey ret")
        if args.interaction == "cat":
            wait_serial(serial, CAT_WAIT_LINE)
            send_text(qmp, "pebble")
            qmp.hmp("sendkey ret")
            wait_serial(serial, b"RW CAT userspace stdout accepted")
            qmp.hmp("sendkey ctrl-d")
        wait_serial(serial, USERLAND_LINES[args.interaction])
        time.sleep(0.20)
        terminal = capture(qmp, output, "phipia-proof-terminal")
        print(clean)
        print(focus)
        print(terminal)
    finally:
        if qmp is not None:
            try:
                qmp.execute("quit")
            except (OSError, RuntimeError):
                pass
            qmp.close()
        try:
            process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    main()
