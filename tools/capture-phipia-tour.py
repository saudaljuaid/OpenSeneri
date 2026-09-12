#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Capture the running Phipia from QEMU.

The recording begins only after the installed Boot Ledger proof and the shell
prompt.  Frames come from QEMU's emulated display through QMP, or from its
visible SDL client in live-window mode.  Pointer clicks and keystrokes travel
through the ordinary PS/2 guest input path, and the attached FAT32 data image
is a durable copy retained beside the evidence.
"""

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import zlib
from pathlib import Path

import fat32_image


PROOF_LINE = b"Phipia: Boot Ledger installed proof passed"
PROMPT = b"phip> "
WIDTH = 1024
HEIGHT = 768
DOCK_FIXED_ONE = 65536
DOCK_ICON_SIZE = 58
DOCK_GAP_FACTOR = 10486
DOCK_ITEM_COUNT = 8
DOCK_POINTER_Y = 700
DOCK_FILES = 0
DOCK_TERMINAL = 1
DOCK_NOTES = 2
DOCK_MEDIA_EDITOR = 3
DOCK_CAMERA = 4
DOCK_CANVAS = 5
DOCK_STORE = 6
DOCK_SETTINGS = 7


def dock_item_center(index):
    """Return the center of a stable Dock hit lane at 1024x768."""
    if index < 0 or index >= DOCK_ITEM_COUNT:
        raise ValueError("Dock item index is outside the installed layout")
    icon = DOCK_ICON_SIZE * DOCK_FIXED_ONE
    gap = icon * DOCK_GAP_FACTOR // DOCK_FIXED_ONE
    cell = icon + gap
    resting_width = cell * DOCK_ITEM_COUNT
    resting_x = WIDTH * DOCK_FIXED_ONE // 2 - resting_width // 2
    center = resting_x + index * cell + cell // 2
    return (center + DOCK_FIXED_ONE // 2) // DOCK_FIXED_ONE


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
            newline = data.find(b"\n", position)
            if newline < 0:
                raise RuntimeError("QEMU PPM comment is unterminated")
            position = newline + 1
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
    png += png_chunk(b"IHDR", struct.pack(
        ">IIBBBBB", width, height, 8, 2, 0, 0, 0
    ))
    png += png_chunk(b"IDAT", zlib.compress(rows, 9))
    png += png_chunk(b"IEND", b"")
    Path(destination).write_bytes(png)


class Qmp:
    def __init__(self, port):
        deadline = time.monotonic() + 10.0
        while True:
            try:
                self.socket = socket.create_connection(
                    ("127.0.0.1", port), 0.5
                )
                self.socket.settimeout(None)
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


def wait_serial(path, markers, timeout=90.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        transcript = path.read_bytes() if path.exists() else b""
        if all(marker in transcript for marker in markers):
            return
        time.sleep(0.05)
    transcript = path.read_bytes() if path.exists() else b""
    tail = transcript[-8192:].decode("utf-8", errors="replace")
    raise RuntimeError(f"guest readiness markers were omitted\n{tail}")


def storage_arguments(system, data):
    return [
        "-blockdev",
        f"driver=file,filename={system.resolve()},node-name=system-file,read-only=on,auto-read-only=off",
        "-blockdev",
        "driver=raw,file=system-file,node-name=system-raw,read-only=on",
        "-device",
        "nvme,serial=phipia-system-fat32,drive=system-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1",
        "-blockdev",
        f"driver=file,filename={data.resolve()},node-name=data-file,read-only=off,auto-read-only=off",
        "-blockdev",
        "driver=raw,file=data-file,node-name=data-raw,read-only=off",
        "-device",
        "nvme,serial=phipia-data-fat32,drive=data-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1",
    ]


def capture_ppm(qmp, destination):
    qmp.execute("screendump", {
        "filename": destination.resolve().as_posix(), "format": "ppm"
    })


def capture_png(qmp, work, output, name):
    ppm = work / f"{name}.ppm"
    destination = output / f"{name}.png"
    capture_ppm(qmp, ppm)
    ppm_to_png(ppm, destination)
    ppm.unlink()


def wait_for_export(data_image, timeout=20.0):
    expected_size = 54 + 320 * 180 * 3
    deadline = time.monotonic() + timeout
    last_error = "EXPORT.BMP was not visible"

    # Let the ordinary guest NVMe path finish without competing with repeated
    # 64 MiB host reads while its FAT and FSInfo updates are in flight.
    time.sleep(8.0)
    while time.monotonic() < deadline:
        try:
            report = fat32_image.inspect_image(data_image.read_bytes())
            files = {
                str(record["path"]): record
                for record in report["files"]
                if not bool(record["directory"])
            }
            exported = files.get("EXPORT.BMP")
            if exported is not None and int(exported["size"]) == expected_size:
                return
            last_error = "EXPORT.BMP was absent or incomplete"
        except (OSError, fat32_image.Fat32Error) as error:
            last_error = str(error)
        time.sleep(5.0)
    raise RuntimeError(f"guest export did not synchronize: {last_error}")


def click_export_and_wait(pointer, data_image):
    last_error = None
    for _ in range(3):
        pointer.click()
        try:
            wait_for_export(data_image)
            return
        except RuntimeError as error:
            last_error = error
    raise last_error


def wait_until(started, seconds):
    remaining = started + seconds - time.monotonic()
    if remaining > 0.0:
        time.sleep(remaining)


def finish_recording(process):
    try:
        _, errors = process.communicate(timeout=10.0)
    except subprocess.TimeoutExpired:
        process.kill()
        _, errors = process.communicate()
        raise RuntimeError("live QEMU recording did not stop at its bound")
    if process.returncode != 0:
        raise RuntimeError("live QEMU recording failed\n" + errors[-4096:])


def prepare_live_window(title):
    import ctypes
    from ctypes import wintypes

    class Rect(ctypes.Structure):
        _fields_ = [
            ("left", ctypes.c_long), ("top", ctypes.c_long),
            ("right", ctypes.c_long), ("bottom", ctypes.c_long),
        ]

    class Point(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

    user = ctypes.windll.user32
    deadline = time.monotonic() + 10.0
    handle = 0
    while not handle and time.monotonic() < deadline:
        handle = user.FindWindowW(None, title)
        if not handle:
            time.sleep(0.05)
    if not handle:
        raise RuntimeError(f"QEMU live window was not found: {title}")

    user.ShowWindow(handle, 9)  # SW_RESTORE
    user.SetForegroundWindow(handle)
    # A decorated 1024x768 SDL client is taller than the usable area on some
    # Windows desktops.  The dedicated evidence window is borderless so the
    # guest framebuffer remains unscaled and no host chrome enters the video.
    popup_style = ctypes.c_long(0x90000000).value  # WS_POPUP | WS_VISIBLE
    user.SetWindowLongW(handle, -16, popup_style)  # GWL_STYLE
    stable = 0
    observed = (0, 0)
    for _ in range(60):
        window = Rect()
        client = Rect()
        if not user.GetWindowRect(handle, ctypes.byref(window)) or not \
                user.GetClientRect(handle, ctypes.byref(client)):
            raise RuntimeError("QEMU live window geometry is unavailable")
        client_width = client.right - client.left
        client_height = client.bottom - client.top
        observed = (client_width, client_height)
        if client_width == WIDTH and client_height == HEIGHT:
            stable += 1
            if stable == 3:
                break
            time.sleep(0.15)
            continue
        stable = 0
        if not user.SetWindowPos(handle, 0, 80, 0, WIDTH, HEIGHT,
                                 0x0004 | 0x0020 | 0x0040):
            raise RuntimeError("QEMU live window could not be sized")
        time.sleep(0.15)
    else:
        raise RuntimeError(
            "QEMU live client did not settle at 1024x768 "
            f"(last {observed[0]}x{observed[1]})"
        )

    window = Rect()
    client = Rect()
    origin = Point(0, 0)
    user.GetWindowRect(handle, ctypes.byref(window))
    user.GetClientRect(handle, ctypes.byref(client))
    user.ClientToScreen(handle, ctypes.byref(origin))
    if client.right - client.left != WIDTH or client.bottom - client.top != HEIGHT:
        raise RuntimeError("QEMU live client changed after sizing")
    return origin.x, origin.y


def record_live_window(args, qmp, pointer, work, output, durable_data, video,
                       crop):
    origin_x, origin_y = crop
    command = [
        args.ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
        "-f", "gdigrab", "-draw_mouse", "0", "-framerate", str(args.fps),
        "-offset_x", str(origin_x), "-offset_y", str(origin_y),
        "-video_size", f"{WIDTH}x{HEIGHT}", "-i", "desktop",
        "-t", f"{args.seconds:.3f}", "-vf", "format=yuv420p",
        "-c:v", "libx264", "-preset", "medium",
        "-crf", "18", "-movflags", "+faststart", str(video),
    ]
    recording = subprocess.Popen(
        command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.75)
    if recording.poll() is not None:
        finish_recording(recording)
    started = time.monotonic()

    try:
        wait_until(started, 0.25)
        pointer.prime_terminal()
        capture_png(qmp, work, output, "phipia-dock")
        wait_until(started, 1.00)
        pointer.move_to(dock_item_center(DOCK_SETTINGS), DOCK_POINTER_Y)
        pointer.click()
        time.sleep(0.08)
        capture_png(qmp, work, output, "phipia-window-opening-spring")
        pointer.settle_guest(0.90)
        wait_until(started, 2.05)
        capture_png(qmp, work, output, "phipia-settings-all")
        pointer.move_to(600, 188)
        pointer.click()
        pointer.settle_guest(0.20)
        capture_png(qmp, work, output, "phipia-settings-dock-controls")
        # Prove the switch is live in the recording, then restore the default.
        pointer.move_to(512, 190)
        pointer.click()
        pointer.click()
        pointer.move_to(140, 99)
        pointer.click()
        pointer.move_to(407, 188)
        pointer.click()
        wait_until(started, 2.75)
        capture_png(qmp, work, output, "phipia-settings-desktop")
        pointer.move_to(858, 180)
        pointer.click()
        pointer.move_to(140, 99)
        pointer.click()
        wait_until(started, 3.45)
        pointer.move_to(213, 188)
        pointer.click()
        wait_until(started, 4.05)
        capture_png(qmp, work, output, "phipia-settings-appearance-light")
        pointer.move_to(654, 240)
        pointer.click()
        wait_until(started, 4.65)
        capture_png(qmp, work, output, "phipia-settings-appearance-dark")

        # Keep Camera out of the public demo: QEMU has no webcam source and
        # the application intentionally refuses to fabricate a live frame.
        wait_until(started, 5.10)
        pointer.move_to(dock_item_center(DOCK_FILES), DOCK_POINTER_Y)
        pointer.click()
        pointer.settle_guest(0.90)
        wait_until(started, 7.65)
        capture_png(qmp, work, output, "phipia-files")
        capture_png(qmp, work, output, "phipia-multitasking")
        pointer.move_to(553, 206)
        pointer.click()
        wait_until(started, 8.70)
        send_text(qmp, " UI redesign nailed.", delay=0.020)
        qmp.hmp("sendkey ctrl-s")
        wait_until(started, 9.80)
        capture_png(qmp, work, output, "phipia-notes")

        wait_until(started, 10.60)
        pointer.rehome()
        pointer.move_to(dock_item_center(DOCK_MEDIA_EDITOR), DOCK_POINTER_Y)
        pointer.click()
        pointer.settle_guest(0.30)
        wait_until(started, 11.50)
        for x, y in ((190, 141), (262, 141), (336, 141), (600, 560),
                     (406, 141)):
            pointer.move_to(x, y)
            pointer.click()
        capture_png(qmp, work, output, "phipia-media-editor")

        wait_until(started, 15.20)
        pointer.move_to(dock_item_center(DOCK_SETTINGS), DOCK_POINTER_Y)
        pointer.click()
        pointer.settle_guest(0.30)
        wait_until(started, 16.00)
        pointer.move_to(213, 188)
        pointer.click()
        pointer.move_to(370, 240)
        pointer.click()
        wait_until(started, 17.00)
        capture_png(qmp, work, output, "phipia-settings-appearance-restored")
        pointer.drag_to(510, 56, 570, 116)
        pointer.settle_guest(0.20)
        capture_png(qmp, work, output, "phipia-window-dragged")
        for index in (*range(DOCK_ITEM_COUNT), DOCK_CANVAS):
            pointer.move_to(dock_item_center(index), DOCK_POINTER_Y)
            time.sleep(0.12)
        capture_png(qmp, work, output, "phipia-ui-redesign-final-dock")
        wait_until(started, args.seconds)
        finish_recording(recording)

        pointer.rehome()
        pointer.move_to(dock_item_center(DOCK_MEDIA_EDITOR), DOCK_POINTER_Y)
        pointer.click()
        pointer.settle_guest(0.50)
        pointer.move_to(476, 141)
        click_export_and_wait(pointer, durable_data)
        capture_png(qmp, work, output, "phipia-media-editor")
    finally:
        if recording.poll() is None:
            recording.kill()
            recording.communicate()


def record_fast_demo(args, qmp, pointer, work, output, video, crop):
    """Record a fluid visual demo without persistence waits or Camera."""
    del crop
    command = [
        args.ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
        "-thread_queue_size", "512", "-f", "gdigrab", "-draw_mouse", "0",
        "-framerate", "60", "-i", "title=QEMU (PhipiaCapture-0)",
        "-t", f"{args.seconds:.3f}",
        "-vf", f"fps={args.fps},format=yuv420p", "-c:v", "libx264",
        "-preset", "ultrafast", "-crf", "16", "-r", str(args.fps),
        "-movflags", "+faststart", str(video),
    ]
    recording = subprocess.Popen(
        command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
    )
    time.sleep(0.45)
    if recording.poll() is not None:
        finish_recording(recording)
    started = time.monotonic()

    try:
        # Establish an absolute guest origin once.  All later motion stays on
        # this coordinate system, avoiding SDL-relative drift.
        pointer.rehome()
        pointer.move_to(dock_item_center(DOCK_FILES), DOCK_POINTER_Y)
        for index in (*range(DOCK_ITEM_COUNT), DOCK_CANVAS, DOCK_CAMERA,
                      DOCK_MEDIA_EDITOR):
            pointer.move_to(dock_item_center(index), DOCK_POINTER_Y)
            time.sleep(0.05)
        capture_png(qmp, work, output, "phipia-fast-dock-hover")

        wait_until(started, 3.20)
        pointer.move_to(dock_item_center(DOCK_SETTINGS), DOCK_POINTER_Y)
        pointer.click()
        pointer.settle_guest(0.70)
        capture_png(qmp, work, output, "phipia-fast-settings")

        pointer.move_to(407, 188)
        pointer.click()
        pointer.settle_guest(0.18)
        capture_png(qmp, work, output, "phipia-fast-wallpapers")
        pointer.move_to(858, 180)
        pointer.click()

        wait_until(started, 8.00)
        pointer.move_to(140, 99)
        pointer.click()
        pointer.move_to(213, 188)
        pointer.click()
        pointer.move_to(714, 330)
        pointer.click()
        time.sleep(0.35)
        pointer.move_to(430, 330)
        pointer.click()

        wait_until(started, 10.00)
        pointer.move_to(dock_item_center(DOCK_FILES), DOCK_POINTER_Y)
        pointer.click()
        pointer.settle_guest(0.70)
        capture_png(qmp, work, output, "phipia-fast-files")

        wait_until(started, 12.80)
        pointer.move_to(dock_item_center(DOCK_NOTES), DOCK_POINTER_Y)
        pointer.click()
        pointer.settle_guest(0.70)
        send_text(qmp, "Fluid Phipia.", delay=0.012)
        qmp.hmp("sendkey ctrl-s")
        capture_png(qmp, work, output, "phipia-fast-notes")

        wait_until(started, 16.00)
        pointer.move_to(dock_item_center(DOCK_MEDIA_EDITOR), DOCK_POINTER_Y)
        pointer.click()
        pointer.settle_guest(0.70)
        for x, y in ((190, 141), (262, 141), (336, 141), (600, 560)):
            pointer.move_to(x, y)
            pointer.click()
        capture_png(qmp, work, output, "phipia-fast-media-editor")

        wait_until(started, 20.20)
        pointer.move_to(dock_item_center(DOCK_SETTINGS), DOCK_POINTER_Y)
        pointer.click()
        pointer.settle_guest(0.30)
        pointer.drag_to(510, 56, 570, 116)
        capture_png(qmp, work, output, "phipia-fast-multitasking")

        wait_until(started, 22.20)
        for index in (*range(DOCK_ITEM_COUNT), DOCK_MEDIA_EDITOR):
            pointer.move_to(dock_item_center(index), DOCK_POINTER_Y)
            time.sleep(0.04)
        wait_until(started, args.seconds)
        finish_recording(recording)
    finally:
        if recording.poll() is None:
            recording.kill()
            recording.communicate()


def send_text(qmp, text, delay=0.040):
    names = {" ": "spc", "-": "minus", ".": "dot", "/": "slash"}
    for character in text:
        if "A" <= character <= "Z":
            key = f"shift-{character.lower()}"
        else:
            key = names.get(character, character)
        qmp.hmp(f"sendkey {key} 15")
        time.sleep(delay)


class Pointer:
    def __init__(self, qmp):
        self.qmp = qmp
        self.x = WIDTH - WIDTH // 4
        self.y = HEIGHT // 3

    def move_to(self, x, y):
        while self.x != x or self.y != y:
            dx = max(-40, min(40, x - self.x))
            dy = max(-40, min(40, y - self.y))
            self.qmp.hmp(f"mouse_move {dx} {dy}")
            self.x += dx
            self.y += dy
            time.sleep(0.025)
        time.sleep(0.10)

    def prime_terminal(self):
        # QEMU's relative PS/2 path needs one ordinary large host motion before
        # it begins emitting the smaller packets used for the scripted path.
        self.qmp.hmp("mouse_move -260 320")
        time.sleep(0.25)
        self.qmp.hmp("mouse_move 4 120")
        self.x = 512
        self.y = 696
        time.sleep(0.45)
        self.move_to(dock_item_center(DOCK_TERMINAL), DOCK_POINTER_Y)

    def rehome(self):
        """Clamp guest and script coordinates back to the same origin."""
        for _ in range(14):
            self.qmp.hmp("mouse_move -80 -80")
            time.sleep(0.025)
        self.x = 0
        self.y = 0
        time.sleep(0.15)

    def click(self):
        self.qmp.hmp("mouse_button 1")
        time.sleep(0.05)
        self.qmp.hmp("mouse_button 0")
        time.sleep(0.08)

    def drag_to(self, start_x, start_y, end_x, end_y):
        self.move_to(start_x, start_y)
        self.qmp.hmp("mouse_button 1")
        time.sleep(0.08)
        self.move_to(end_x, end_y)
        time.sleep(0.08)
        self.qmp.hmp("mouse_button 0")
        time.sleep(0.12)

    def settle_guest(self, delay=0.35):
        """Let the guest drain input, then wake one final paint iteration."""
        time.sleep(delay)
        self.qmp.hmp("mouse_move 1 0")
        self.x += 1
        time.sleep(0.12)
        self.qmp.hmp("mouse_move -1 0")
        self.x -= 1
        time.sleep(0.18)


def encode(ffmpeg, frames, capture_times, fps, seconds, output):
    if not frames or len(frames) != len(capture_times):
        raise RuntimeError("video frame timing evidence is incomplete")
    origin = capture_times[0]
    normalized = [timestamp - origin for timestamp in capture_times]
    manifest = frames[0].parent / "frames.ffconcat"
    lines = ["ffconcat version 1.0"]
    for index, frame in enumerate(frames):
        if index + 1 < len(frames):
            duration = normalized[index + 1] - normalized[index]
        else:
            duration = seconds - normalized[index]
        duration = max(0.001, duration)
        lines.append(f"file '{frame.resolve().as_posix()}'")
        lines.append(f"duration {duration:.9f}")
    lines.append(f"file '{frames[-1].resolve().as_posix()}'")
    manifest.write_text("\n".join(lines) + "\n", encoding="ascii")
    frame_count = int(round(seconds * fps))
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
        "-f", "concat", "-safe", "0", "-i", str(manifest),
        "-vf", "setpts=PTS-STARTPTS,format=yuv420p,"
               "tpad=stop_mode=clone:stop_duration=12",
        "-c:v", "libx264", "-preset", "medium",
        "-crf", "18", "-r", str(fps), "-frames:v", str(frame_count),
        "-movflags", "+faststart", str(output)
    ], check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument(
        "--accel", choices=("tcg", "whpx"), default="tcg",
        help="QEMU accelerator used for the evidence boot",
    )
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--iso", required=True)
    parser.add_argument("--system", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--seconds", type=float, default=25.0)
    parser.add_argument("--fps", type=int, default=8)
    parser.add_argument(
        "--live-window", action="store_true",
        help="record the visible Windows QEMU SDL window with gdigrab",
    )
    parser.add_argument(
        "--fast-demo", action="store_true",
        help="record the fluid UI showcase without Camera or disk proof waits",
    )
    args = parser.parse_args()
    if args.seconds < 25.0:
        parser.error("the UI application proof needs at least 25 seconds")
    if args.fps <= 0:
        parser.error("--fps must be positive")
    if args.fast_demo and not args.live_window:
        parser.error("--fast-demo requires --live-window")

    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    durable_data = output / "phipia-data.raw"
    shutil.copyfile(Path(args.data).resolve(), durable_data)
    staged_media = output / ".phipia-capture-aurora.bmp"
    try:
        subprocess.run([
            args.ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-i", str((Path(__file__).resolve().parent.parent /
                       "assets/phipia/wallpaper.png")),
            "-vf", "scale=320:180", "-pix_fmt", "bgr24", "-c:v", "bmp",
            str(staged_media),
        ], check=True)
        populated = fat32_image.populate_data_files(durable_data.read_bytes(), [
            ("AURORA.BMP", staged_media.read_bytes()),
            ("NEW1.TXT", b"Ready for a Phipia note."),
        ])
        fat32_image.atomic_write(durable_data, populated)
    finally:
        staged_media.unlink(missing_ok=True)
    serial = output / "phipia-serial.log"
    if serial.exists():
        serial.unlink()
    video = output / "phipia-ui-redesign-25s.mp4"
    port = free_port()
    command = [
        args.qemu, "-machine", f"accel={args.accel}", "-m", "128M",
        "-smp", "1",
        "-boot", "order=d", "-cdrom", str(Path(args.iso).resolve()),
        "-display", ("sdl,window-close=off" if args.live_window else "none"),
        *( ["-name", "PhipiaCapture"] if args.live_window else [] ),
        *storage_arguments(Path(args.system).resolve(), durable_data),
        "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
        "-serial", f"file:{serial}", "-no-reboot"
    ]

    with tempfile.TemporaryDirectory(prefix="phipia-") as raw:
        work = Path(raw)
        qemu_environment = os.environ.copy()
        if args.live_window:
            # SDL must use physical pixels so gdigrab, Win32 geometry and the
            # 1024x768 guest surface share one coordinate system on scaled
            # Windows desktops.
            qemu_environment["SDL_WINDOWS_DPI_AWARENESS"] = "permonitorv2"
            qemu_environment["SDL_WINDOWS_DPI_SCALING"] = "0"
        process = subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=qemu_environment,
        )
        qmp = None
        try:
            qmp = Qmp(port)
            live_crop = None
            if args.live_window:
                # Size the SDL client before the guest draws the desktop.  A
                # later host resize leaves regions that Phipia has no reason
                # to repaint and produces misleading partial capture frames.
                live_crop = prepare_live_window("QEMU (PhipiaCapture-0)")
            wait_serial(serial, (PROOF_LINE, PROMPT))
            time.sleep(0.35)
            pointer = Pointer(qmp)
            capture_png(qmp, work, output, "phipia")

            if args.live_window:
                if args.fast_demo:
                    record_fast_demo(args, qmp, pointer, work, output, video,
                                     live_crop)
                    events = set()
                else:
                    record_live_window(args, qmp, pointer, work, output,
                                       durable_data, video, live_crop)
                    events = {
                        "dock_hover", "settings_open", "appearance_open",
                        "wallpaper_open", "wallpaper_selected",
                        "appearance_dark",
                        "files_open", "multitask_proof", "file_open",
                        "note_saved", "media_editor_open", "media_editor_new",
                        "media_editor_import", "media_editor_trim", "media_editor_seek",
                        "media_editor_save", "settings_reopen", "appearance_light",
                        "fluid_hover", "window_drag", "media_editor_export",
                    }
                captured_frames = []
                capture_times = []
            else:
                events = set()
                captured_frames = []
                capture_times = []
                started = time.monotonic()
                next_capture = started
                index = 0
                while time.monotonic() - started < args.seconds + 30.0:
                    elapsed = time.monotonic() - started

                    if elapsed >= 0.10 and "dock_hover" not in events:
                        pointer.prime_terminal()
                        capture_png(qmp, work, output,
                                    "phipia-dock")
                        for dock_index in (*range(DOCK_ITEM_COUNT),
                                           DOCK_CANVAS):
                            pointer.move_to(dock_item_center(dock_index),
                                            DOCK_POINTER_Y)
                            time.sleep(0.08)
                        events.add("dock_hover")
                    elif elapsed >= 0.30 and "settings_open" not in events:
                        pointer.move_to(dock_item_center(DOCK_SETTINGS),
                                        DOCK_POINTER_Y)
                        pointer.click()
                        time.sleep(0.08)
                        capture_png(qmp, work, output,
                                    "phipia-window-opening-spring")
                        pointer.settle_guest(0.90)
                        capture_png(qmp, work, output,
                                    "phipia-settings-all")
                        events.add("settings_open")
                    elif elapsed >= 0.60 and "wallpaper_open" not in events:
                        pointer.move_to(600, 188)
                        pointer.click()
                        pointer.settle_guest(0.20)
                        capture_png(qmp, work, output,
                                    "phipia-settings-dock-controls")
                        pointer.move_to(512, 190)
                        pointer.click()
                        pointer.click()
                        pointer.move_to(140, 99)
                        pointer.click()
                        pointer.move_to(407, 188)
                        pointer.click()
                        pointer.settle_guest()
                        capture_png(qmp, work, output,
                                    "phipia-settings-desktop")
                        events.add("wallpaper_open")
                    elif elapsed >= 0.90 and "wallpaper_selected" not in events:
                        pointer.move_to(858, 180)
                        pointer.click()
                        pointer.move_to(140, 99)
                        pointer.click()
                        pointer.move_to(213, 188)
                        pointer.click()
                        pointer.settle_guest()
                        capture_png(qmp, work, output,
                                    "phipia-settings-appearance-light")
                        events.update({"wallpaper_selected", "appearance_open"})
                    elif elapsed >= 1.20 and "appearance_dark" not in events:
                        pointer.move_to(654, 240)
                        pointer.click()
                        pointer.settle_guest()
                        capture_png(qmp, work, output,
                                    "phipia-settings-appearance-dark")
                        events.add("appearance_dark")
                    elif elapsed >= 1.50 and "files_open" not in events:
                        pointer.move_to(dock_item_center(DOCK_FILES),
                                        DOCK_POINTER_Y)
                        pointer.click()
                        pointer.settle_guest(0.90)
                        capture_png(qmp, work, output,
                                    "phipia-files")
                        events.add("files_open")
                    elif elapsed >= 2.40 and "multitask_proof" not in events:
                        capture_png(qmp, work, output,
                                    "phipia-multitasking")
                        events.add("multitask_proof")
                    elif elapsed >= 2.70 and "file_open" not in events:
                        pointer.move_to(553, 206)
                        pointer.click()
                        pointer.settle_guest()
                        events.add("file_open")
                    elif elapsed >= 3.00 and "note_saved" not in events:
                        send_text(qmp, " UI redesign nailed.", delay=0.020)
                        qmp.hmp("sendkey ctrl-s")
                        pointer.settle_guest(0.40)
                        capture_png(qmp, work, output,
                                    "phipia-notes")
                        events.add("note_saved")
                    elif elapsed >= 3.30 and "media_editor_open" not in events:
                        pointer.rehome()
                        pointer.move_to(dock_item_center(DOCK_MEDIA_EDITOR),
                                        DOCK_POINTER_Y)
                        pointer.click()
                        pointer.settle_guest()
                        events.add("media_editor_open")
                    elif elapsed >= 3.60 and "media_editor_new" not in events:
                        pointer.move_to(190, 141)
                        pointer.click()
                        events.add("media_editor_new")
                    elif elapsed >= 3.90 and "media_editor_import" not in events:
                        pointer.move_to(262, 141)
                        pointer.click()
                        pointer.settle_guest(0.25)
                        events.add("media_editor_import")
                    elif elapsed >= 4.20 and "media_editor_trim" not in events:
                        pointer.move_to(336, 141)
                        pointer.click()
                        events.add("media_editor_trim")
                    elif elapsed >= 4.50 and "media_editor_seek" not in events:
                        pointer.move_to(600, 560)
                        pointer.click()
                        events.add("media_editor_seek")
                    elif elapsed >= 4.80 and "media_editor_save" not in events:
                        pointer.move_to(406, 141)
                        pointer.click()
                        pointer.settle_guest(0.25)
                        capture_png(qmp, work, output,
                                    "phipia-media-editor")
                        events.add("media_editor_save")
                    elif elapsed >= 5.40 and "settings_reopen" not in events:
                        pointer.move_to(dock_item_center(DOCK_SETTINGS),
                                        DOCK_POINTER_Y)
                        pointer.click()
                        pointer.settle_guest()
                        pointer.move_to(213, 188)
                        pointer.click()
                        pointer.settle_guest()
                        pointer.move_to(370, 240)
                        pointer.click()
                        pointer.settle_guest(0.20)
                        capture_png(qmp, work, output,
                                    "phipia-settings-appearance-restored")
                        events.update({
                            "settings_reopen", "appearance_light",
                        })
                    elif elapsed >= 6.00 and "window_drag" not in events:
                        pointer.drag_to(510, 56, 570, 116)
                        pointer.settle_guest(0.20)
                        capture_png(qmp, work, output,
                                    "phipia-window-dragged")
                        events.add("window_drag")
                    elif elapsed >= 5.10 and "fluid_hover" not in events:
                        for dock_index in (*range(DOCK_ITEM_COUNT),
                                           DOCK_CANVAS):
                            pointer.move_to(dock_item_center(dock_index),
                                            DOCK_POINTER_Y)
                            time.sleep(0.12)
                        capture_png(qmp, work, output,
                                    "phipia-ui-redesign-final-dock")
                        events.add("fluid_hover")
                    elif elapsed >= 6.40 and "store_restored" not in events:
                        pointer.rehome()
                        pointer.move_to(dock_item_center(DOCK_STORE),
                                        DOCK_POINTER_Y)
                        pointer.click()
                        capture_png(qmp, work, output,
                                    "phipia-store-opening")
                        pointer.settle_guest(0.60)
                        capture_png(qmp, work, output, "phipia-store")

                        # The fifth newly opened built-in window is cascaded
                        # to (138, 84); purple is maximize, grey is minimize.
                        pointer.move_to(179, 101)
                        pointer.click()
                        pointer.settle_guest(0.25)
                        capture_png(qmp, work, output,
                                    "phipia-store-maximized")
                        pointer.move_to(49, 49)
                        pointer.click()
                        pointer.settle_guest(0.25)
                        pointer.move_to(201, 101)
                        pointer.click()
                        capture_png(qmp, work, output,
                                    "phipia-store-minimizing")
                        pointer.settle_guest(0.45)
                        capture_png(qmp, work, output,
                                    "phipia-store-minimized")
                        pointer.move_to(dock_item_center(DOCK_STORE),
                                        DOCK_POINTER_Y)
                        pointer.click()
                        pointer.settle_guest(0.60)
                        capture_png(qmp, work, output,
                                    "phipia-store-restored")
                        events.add("store_restored")

                    now = time.monotonic()
                    if now - started >= args.seconds:
                        if {"media_editor_save", "settings_reopen",
                                "appearance_light", "window_drag",
                                "fluid_hover", "store_restored"}.issubset(events):
                            break
                        continue
                    remaining = next_capture - now
                    if remaining > 0.0:
                        time.sleep(remaining)
                    captured_at = time.monotonic()
                    if captured_at - started >= args.seconds:
                        break

                    frame = work / f"frame-{index:04d}.ppm"
                    capture_ppm(qmp, frame)
                    captured_at = time.monotonic()
                    captured_frames.append(frame)
                    capture_times.append(captured_at)
                    index += 1
                    next_capture = captured_at + 1.0 / args.fps

                # Complete the slower synchronized Media Editor export after the
                # exact 25-second UI recording while QEMU remains active.
                pointer.rehome()
                pointer.move_to(dock_item_center(DOCK_MEDIA_EDITOR), DOCK_POINTER_Y)
                pointer.click()
                time.sleep(0.75)
                pointer.move_to(476, 141)
                click_export_and_wait(pointer, durable_data)
                capture_png(qmp, work, output,
                            "phipia-media-editor")
                events.add("media_editor_export")

            required = {
                "dock_hover", "settings_open", "appearance_open",
                "wallpaper_open", "wallpaper_selected",
                "appearance_dark",
                "files_open", "multitask_proof", "file_open", "note_saved",
                "media_editor_open", "media_editor_new",
                "media_editor_import", "media_editor_trim", "media_editor_seek", "media_editor_save",
                "settings_reopen", "appearance_light", "fluid_hover",
                "window_drag", "media_editor_export", "store_restored"
            }
            if not args.fast_demo and events != required:
                raise RuntimeError(f"capture omitted interactions: {required - events}")
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

        transcript = serial.read_bytes() if serial.exists() else b""
        if PROOF_LINE not in transcript or PROMPT not in transcript:
            tail = transcript[-4096:].decode("utf-8", errors="replace")
            raise RuntimeError("recording omitted the installed proof or prompt\n" + tail)

        if args.fast_demo:
            print(video)
            return

        report = fat32_image.inspect_image(durable_data.read_bytes())
        files = {
            str(record["path"]): record
            for record in report["files"]
            if not bool(record["directory"])
        }
        expected_sizes = {
            "AURORA.BMP": 54 + 320 * 180 * 3,
            "MEDIAEDT.PHI": 424,
            "EXPORT.BMP": 54 + 320 * 180 * 3,
        }
        for name, size in expected_sizes.items():
            if name not in files or int(files[name]["size"]) != size:
                raise RuntimeError(
                    f"guest evidence omitted {name} with exact size {size}"
                )
        if ("NEW1.TXT" not in files or
                int(files["NEW1.TXT"]["size"]) <=
                len(b"Ready for a Phipia note.")):
            raise RuntimeError("guest evidence omitted the saved Notes document")
        if (not bool(report["fat_copies_match"]) or
                int(report["cycles"]) != 0 or
                int(report["cross_links"]) != 0 or
                int(report["leaked_clusters"]) != 0):
            raise RuntimeError("guest evidence left an inconsistent FAT32 image")
        (output / "report.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        if not args.live_window:
            encode(args.ffmpeg, captured_frames, capture_times, args.fps,
                   args.seconds, video)

    print(video)


if __name__ == "__main__":
    main()
