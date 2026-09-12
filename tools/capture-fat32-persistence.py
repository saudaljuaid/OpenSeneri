#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Capture a real Phipia FAT32 create/sync/reboot/read interaction.

The clean reboot boundary tears down the first emulator after the guest's
synchronization proof, then starts a second QEMU process on the same data image.
"""

import argparse
import json
import socket
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path


PROOF = b"Phipia: Boot Ledger installed proof passed"


class Qmp:
    def __init__(self, port):
        deadline = time.monotonic() + 15.0
        while True:
            try:
                self.socket = socket.create_connection(
                    ("127.0.0.1", port), 0.5
                )
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
        return self.execute(
            "human-monitor-command", {"command-line": command}
        )

    def close(self):
        self.file.close()
        self.socket.close()


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_count(path, marker, count, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and path.read_bytes().count(marker) >= count:
            return
        time.sleep(0.05)
    transcript = path.read_bytes() if path.exists() else b""
    tail = transcript[-8192:].decode("utf-8", errors="replace")
    raise RuntimeError(
        f"serial transcript omitted occurrence {count} of "
        f"{marker.decode('ascii')}\n{tail}"
    )


def wait_after(path, start, marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and marker in path.read_bytes()[start:]:
            return
        time.sleep(0.05)
    transcript = path.read_bytes()[start:] if path.exists() else b""
    tail = transcript[-8192:].decode("utf-8", errors="replace")
    raise RuntimeError(
        f"serial transcript did not echo {marker.decode('ascii')!r}\n{tail}"
    )


def key_name(character):
    names = {" ": "spc", ".": "dot", "/": "slash", "-": "minus"}
    if character not in names and not character.isalnum():
        raise ValueError(f"unsupported evidence command key: {character!r}")
    return names.get(character, character)


def send_line(qmp, serial, text):
    start = len(serial.read_bytes()) if serial.exists() else 0
    echoed = b""
    for character in text:
        qmp.hmp(f"sendkey {key_name(character)}")
        echoed += character.encode("ascii")
        wait_after(serial, start, echoed, 2.0)
    qmp.hmp("sendkey ret")


def open_terminal(qmp):
    """Open Terminal through ordinary Phipia keyboard focus."""
    qmp.hmp("sendkey tab")
    time.sleep(0.10)
    qmp.hmp("sendkey ret")


def screendump(qmp, destination):
    qmp.execute("screendump", {
        "filename": destination.resolve().as_posix(), "format": "ppm"
    })


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
    png += png_chunk(
        b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    )
    png += png_chunk(b"IDAT", zlib.compress(rows, 9))
    png += png_chunk(b"IEND", b"")
    Path(destination).write_bytes(png)


def encode(ffmpeg, frames, fps, seconds, output):
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
        "-framerate", str(fps), "-i", str(frames),
        "-vf",
        "scale=1024:768:force_original_aspect_ratio=decrease:flags=neighbor,"
        "pad=1024:768:(ow-iw)/2:(oh-ih)/2:black,format=yuv420p",
        "-c:v", "libx264", "-preset", "medium", "-crf", "18",
        "-movflags", "+faststart", "-t", f"{seconds:.3f}", str(output)
    ], check=True)


def storage_arguments(system, data):
    return [
        "-blockdev",
        f"driver=file,filename={system},node-name=system-file,"
        "read-only=on,auto-read-only=off",
        "-blockdev", "driver=raw,file=system-file,node-name=system-raw,"
        "read-only=on",
        "-device", "nvme,serial=phipia-system-fat32,drive=system-raw,"
        "logical_block_size=512,physical_block_size=512,max_ioqpairs=1,"
        "msix_qsize=1",
        "-blockdev",
        f"driver=file,filename={data},node-name=data-file,"
        "read-only=off,auto-read-only=off",
        "-blockdev", "driver=raw,file=data-file,node-name=data-raw,"
        "read-only=off",
        "-device", "nvme,serial=phipia-data-fat32,drive=data-raw,"
        "logical_block_size=512,physical_block_size=512,max_ioqpairs=1,"
        "msix_qsize=1",
    ]


def guest_command(args, system, data, serial, port):
    return [
        args.qemu, "-machine", "accel=tcg", "-m", "128M", "-smp", "1",
        "-boot", "order=d", "-cdrom", str(Path(args.iso).resolve()),
        *storage_arguments(system, data),
        "-display", "none",
        "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-serial", f"file:{serial}"
    ]


def stop_guest(qmp, process):
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--iso", required=True)
    parser.add_argument("--system", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--screenshot", required=True)
    parser.add_argument("--video", required=True)
    parser.add_argument("--transcript", required=True)
    parser.add_argument("--seconds", type=float, default=22.0)
    parser.add_argument("--fps", type=int, default=8)
    args = parser.parse_args()
    if args.seconds < 18.0 or args.fps <= 0:
        parser.error("capture needs at least 18 seconds and a positive FPS")

    system = Path(args.system).resolve()
    data = Path(args.data).resolve()
    screenshot = Path(args.screenshot).resolve()
    video = Path(args.video).resolve()
    transcript = Path(args.transcript).resolve()
    for destination in (screenshot, video, transcript):
        destination.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="phipia-fat32-video-") as temp:
        temp = Path(temp)
        serial = temp / "serial.log"
        second_serial = temp / "serial-second.log"
        port = free_port()
        process = subprocess.Popen(
            guest_command(args, system, data, serial, port),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        qmp = None
        try:
            qmp = Qmp(port)
            wait_count(serial, PROOF, 1, 60.0)
            prompt_count = serial.read_bytes().count(b"phip> ")
            open_terminal(qmp)
            wait_count(serial, b"phip> ", prompt_count + 1, 15.0)
            started = time.monotonic()
            actions = [
                (0.5, "drives"),
                (1.7, "mkdir projects"),
                (3.1, "write projects/notes.txt first cut"),
                (4.6, "append projects/notes.txt second line"),
                (6.1, "sync"),
                (7.3, "reboot"),
            ]
            action_index = 0
            frames = round(args.seconds * args.fps)
            for index in range(frames):
                deadline = started + index / args.fps
                remaining = deadline - time.monotonic()
                if remaining > 0.0:
                    time.sleep(remaining)
                elapsed = time.monotonic() - started
                if action_index < len(actions) and elapsed >= actions[action_index][0]:
                    text = actions[action_index][1]
                    prompt_count = serial.read_bytes().count(b"phip> ")
                    send_line(qmp, serial, text)
                    if text == "reboot":
                        wait_count(serial,
                            b"restarting after clean synchronization", 1, 15.0)
                        stop_guest(qmp, process)
                        qmp = None
                        port = free_port()
                        process = subprocess.Popen(
                            guest_command(args, system, data, second_serial,
                                port),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL
                        )
                        qmp = Qmp(port)
                        wait_count(second_serial, PROOF, 1, 60.0)
                        prompt_count = second_serial.read_bytes().count(
                            b"phip> ")
                        open_terminal(qmp)
                        wait_count(second_serial, b"phip> ",
                            prompt_count + 1, 15.0)
                        prompt_count = second_serial.read_bytes().count(
                            b"phip> ")
                        send_line(qmp, second_serial,
                            "read projects/notes.txt")
                        wait_count(second_serial, b"first cut", 1, 15.0)
                        wait_count(second_serial, b"second line", 1, 15.0)
                        wait_count(second_serial, b"phip> ",
                            prompt_count + 1, 15.0)
                    else:
                        wait_count(serial, b"phip> ", prompt_count + 1, 15.0)
                    action_index += 1
                frame = temp / f"frame-{index:04d}.ppm"
                screendump(qmp, frame)
            time.sleep(0.4)
            final_ppm = temp / "persistence.ppm"
            screendump(qmp, final_ppm)
            ppm_to_png(final_ppm, screenshot)
        finally:
            stop_guest(qmp, process)

        serial_bytes = serial.read_bytes() if serial.exists() else b""
        if second_serial.exists():
            serial_bytes += second_serial.read_bytes()
        transcript.write_bytes(serial_bytes)
        if serial_bytes.count(PROOF) < 2:
            raise RuntimeError("capture omitted the clean second boot")
        after_second_boot = serial_bytes.split(PROOF, 2)[2]
        required = (
            b"read projects/notes.txt", b"first cut", b"second line"
        )
        if any(marker not in after_second_boot for marker in required):
            tail = after_second_boot[-8192:].decode("utf-8", errors="replace")
            raise RuntimeError("persisted read was not visible after reboot\n" + tail)
        if (b"data synchronized" not in serial_bytes or
                b"restarting after clean synchronization" not in serial_bytes):
            raise RuntimeError("capture omitted clean synchronization evidence")
        encode(
            args.ffmpeg, temp / "frame-%04d.ppm", args.fps,
            args.seconds, video
        )
    print(screenshot)
    print(video)
    print(transcript)


if __name__ == "__main__":
    main()
