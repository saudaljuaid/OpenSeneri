#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Record a fixed-duration Phipia boot from QEMU's emulated display.

Frames come directly from QMP ``screendump`` calls. The guest is started in a
paused state, continued on the first frame, and required to emit the installed
Boot Ledger proof before the recording is accepted. At ten seconds the script
opens Terminal; at thirteen seconds it enters the selected measured command.
The default cat interaction then supplies ``pebble`` and Ctrl-D.
"""

import argparse
import json
import socket
import subprocess
import tempfile
import time
from pathlib import Path


PROOF_LINE = b"Phipia: Boot Ledger installed proof passed"
USERLAND_LINES = {
    "uname": b"RW USERLAND launch completed successfully uname ordinal 1",
    "cat": b"RW USERLAND launch completed successfully cat ordinal 1",
}
CAT_FOREGROUND_LINE = (
    b"RW USERLAND cat foreground launch yielded to Phipia"
)
CAT_STDOUT_LINE = b"RW CAT userspace stdout accepted"
COMMAND_SECONDS = 13.0
CAT_INPUT_SECONDS = 15.0
CAT_EOF_SECONDS = 17.0


class Qmp:
    def __init__(self, port):
        deadline = time.monotonic() + 10.0
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


def capture(qmp, destination):
    qmp.execute("screendump", {
        "filename": destination.resolve().as_posix(), "format": "ppm"
    })


def transcript_contains(path, marker):
    return path.exists() and marker in path.read_bytes()


def encode(ffmpeg, pattern, fps, seconds, output):
    command = [
        ffmpeg, "-hide_banner", "-loglevel", "warning", "-y",
        "-framerate", str(fps), "-i", str(pattern),
        "-vf",
        "scale=1024:768:force_original_aspect_ratio=decrease:flags=neighbor,"
        "pad=1024:768:(ow-iw)/2:(oh-ih)/2:black,format=yuv420p",
        "-c:v", "libx264", "-preset", "medium", "-crf", "18",
        "-movflags", "+faststart", "-t", f"{seconds:.3f}", str(output)
    ]
    subprocess.run(command, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--iso", required=True)
    parser.add_argument("--userspace", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--fps", type=int, default=10)
    parser.add_argument(
        "--interaction", choices=sorted(USERLAND_LINES), default="cat"
    )
    args = parser.parse_args()
    if args.fps <= 0:
        parser.error("--fps must be positive")
    if args.seconds < COMMAND_SECONDS + 1.0 / args.fps:
        parser.error("--seconds is too short to capture the scheduled command")
    if (args.interaction == "cat" and
            args.seconds < CAT_EOF_SECONDS + 1.0 / args.fps):
        parser.error("--seconds is too short to capture cat input and EOF")

    if args.seconds <= 0.0 or args.fps <= 0:
        raise ValueError("seconds and fps must be positive")
    frame_count = round(args.seconds * args.fps)
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="phipia-boot-video-") as work:
        work = Path(work)
        serial = work / "boot-serial.log"
        port = free_port()
        command = [
            args.qemu, "-S", "-machine", "accel=tcg", "-m", "128M",
            "-smp", "1", "-boot", "order=d", "-cdrom",
            str(Path(args.iso).resolve()),
            "-blockdev",
            f"driver=file,filename={Path(args.userspace).resolve()},node-name=userland-file,read-only=on,auto-read-only=off",
            "-blockdev",
            "driver=raw,file=userland-file,node-name=userland-raw,read-only=on",
            "-device",
            "nvme,serial=phipia-userland,drive=userland-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1",
            "-display", "none",
            "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
            "-serial", f"file:{serial}", "-no-reboot"
        ]
        process = subprocess.Popen(
            command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        qmp = None
        try:
            qmp = Qmp(port)
            started = time.monotonic()
            qmp.execute("cont")
            pointer_parked = False
            terminal_opened = False
            command_entered = False
            cat_input_entered = False
            cat_eof_entered = False
            for index in range(frame_count):
                deadline = started + index / args.fps
                remaining = deadline - time.monotonic()
                if remaining > 0.0:
                    time.sleep(remaining)
                elapsed = index / args.fps
                if elapsed >= 9.5 and not pointer_parked:
                    qmp.hmp("mouse_move -260 320")
                    pointer_parked = True
                if elapsed >= 10.0 and not terminal_opened:
                    qmp.hmp("sendkey ret")
                    terminal_opened = True
                if elapsed >= COMMAND_SECONDS and not command_entered:
                    for key in f"linux {args.interaction}":
                        qmp.hmp(f"sendkey {'spc' if key == ' ' else key}")
                    qmp.hmp("sendkey ret")
                    command_entered = True
                if (args.interaction == "cat" and
                        elapsed >= CAT_INPUT_SECONDS and
                        not cat_input_entered and
                        transcript_contains(serial, CAT_FOREGROUND_LINE)):
                    for key in "pebble":
                        qmp.hmp(f"sendkey {key}")
                    qmp.hmp("sendkey ret")
                    cat_input_entered = True
                if (args.interaction == "cat" and
                        elapsed >= CAT_EOF_SECONDS and
                        cat_input_entered and not cat_eof_entered and
                        transcript_contains(serial, CAT_STDOUT_LINE)):
                    qmp.hmp("sendkey ctrl-d")
                    cat_eof_entered = True
                capture(qmp, work / f"frame-{index:04d}.ppm")
            if (args.interaction == "cat" and
                    (not cat_input_entered or not cat_eof_entered)):
                transcript = serial.read_bytes() if serial.exists() else b""
                tail = transcript[-4096:].decode("utf-8", errors="replace")
                raise RuntimeError(
                    "recording timed out waiting for the cat foreground "
                    f"or stdout handoff (input={cat_input_entered}, "
                    f"eof={cat_eof_entered})\n{tail}"
                )
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
        if (PROOF_LINE not in transcript or
                USERLAND_LINES[args.interaction] not in transcript):
            tail = transcript[-4096:].decode("utf-8", errors="replace")
            raise RuntimeError(
                "recording omitted the installed proof or userspace launch\n" +
                tail
            )
        encode(
            args.ffmpeg, work / "frame-%04d.ppm", args.fps,
            args.seconds, output
        )
    print(output)


if __name__ == "__main__":
    main()
