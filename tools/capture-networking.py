#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Capture an authentic Phipia networking session and its packet evidence."""

from __future__ import annotations

import argparse
import importlib.util
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import fat32_image


def load_capture_support():
    path = Path(__file__).with_name("capture-phipia.py")
    specification = importlib.util.spec_from_file_location(
        "phipia_capture_support", path
    )
    if specification is None or specification.loader is None:
        raise RuntimeError("Phipia capture support is unavailable")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def free_udp_ports() -> tuple[int, int]:
    sockets: list[socket.socket] = []
    ports: list[int] = []
    try:
        for _ in range(2):
            current = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            current.bind(("127.0.0.1", 0))
            sockets.append(current)
            ports.append(int(current.getsockname()[1]))
    finally:
        for current in sockets:
            current.close()
    return ports[0], ports[1]


def wait_ready(path: Path, process: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            raise RuntimeError("network fixture exited before readiness")
        time.sleep(0.02)
    raise RuntimeError("network fixture readiness timed out")


def terminate(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3.0)


def send_command(support, qmp, command: str) -> None:
    names = {
        " ": "spc", "-": "minus", ".": "dot", "/": "slash",
        ":": "shift-semicolon",
    }
    for character in command:
        if "A" <= character <= "Z":
            key = f"shift-{character.lower()}"
        else:
            key = names.get(character, character)
        qmp.hmp(f"sendkey {key} 15")
        time.sleep(0.020)
    qmp.hmp("sendkey ret")


class Pointer:
    def __init__(self, qmp) -> None:
        self.qmp = qmp
        self.x = 1024 - 1024 // 4
        self.y = 768 // 3

    def move_to(self, x: int, y: int) -> None:
        while self.x != x or self.y != y:
            dx = max(-20, min(20, x - self.x))
            dy = max(-20, min(20, y - self.y))
            events = []
            if dx != 0:
                events.append({
                    "type": "rel", "data": {"axis": "x", "value": dx}
                })
            if dy != 0:
                events.append({
                    "type": "rel", "data": {"axis": "y", "value": dy}
                })
            self.qmp.execute("input-send-event", {"events": events})
            self.x += dx
            self.y += dy
            time.sleep(0.02)
        time.sleep(0.10)

    def rehome(self) -> None:
        """Clamp guest and script coordinates to the same top-left origin."""
        for _ in range(14):
            self.qmp.execute("input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -80}},
                {"type": "rel", "data": {"axis": "y", "value": -80}},
            ]})
            time.sleep(0.02)
        self.x = 0
        self.y = 0
        time.sleep(0.15)

    def click(self) -> None:
        self.qmp.execute("input-send-event", {"events": [{
            "type": "btn", "data": {"button": "left", "down": True}
        }]})
        time.sleep(0.05)
        self.qmp.execute("input-send-event", {"events": [{
            "type": "btn", "data": {"button": "left", "down": False}
        }]})
        time.sleep(0.10)


def storage_arguments(system: Path, data: Path) -> list[str]:
    return [
        "-blockdev",
        f"driver=file,filename={system.resolve()},node-name=system-file,read-only=on,auto-read-only=off",
        "-blockdev", "driver=raw,file=system-file,node-name=system-raw,read-only=on",
        "-device", "nvme,serial=phipia-system-fat32,drive=system-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1",
        "-blockdev",
        f"driver=file,filename={data.resolve()},node-name=data-file,read-only=off,auto-read-only=off",
        "-blockdev", "driver=raw,file=data-file,node-name=data-raw,read-only=off",
        "-device", "nvme,serial=phipia-data-fat32,drive=data-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1",
    ]


def boot_arguments(args: argparse.Namespace, work: Path) -> list[str]:
    if args.iso is not None:
        return ["-boot", "order=d", "-cdrom", str(args.iso.resolve())]
    if (args.efi_root is None or args.kernel is None or
            args.uefi_code is None or args.uefi_vars is None):
        raise RuntimeError(
            "an ISO or --efi-root/--kernel/--uefi-code/--uefi-vars is required"
        )
    root = work / "efi"
    variables = work / "efi-vars.fd"
    shutil.copytree(args.efi_root, root)
    shutil.copyfile(args.kernel, root / "boot" / "phipia.elf")
    configuration = (
        "set default=0\n"
        "set timeout=0\n\n"
        'menuentry "Phipia" {\n'
        "    multiboot2 /boot/phipia.elf\n"
        "    boot\n"
        "}\n"
    )
    for relative in (Path("boot/grub/grub.cfg"),
                     Path("EFI/ubuntu/grub.cfg")):
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(configuration, encoding="ascii", newline="\n")
    shutil.copyfile(args.uefi_vars, variables)
    return [
        "-drive",
        f"if=pflash,format=raw,readonly=on,file={args.uefi_code.resolve().as_posix()}",
        "-drive", f"if=pflash,format=raw,file={variables.resolve().as_posix()}",
        "-drive", f"format=raw,file=fat:rw:{root.resolve().as_posix()}",
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", type=Path)
    parser.add_argument("--efi-root", type=Path)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--uefi-code", type=Path)
    parser.add_argument("--uefi-vars", type=Path)
    parser.add_argument("--system", required=True, type=Path)
    parser.add_argument("--data", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--accel", default="tcg")
    parser.add_argument("--icount")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--fixture", type=Path,
                        default=Path("tools/network_fixture.py"))
    parser.add_argument("--audit", type=Path,
                        default=Path("tools/network_packet_audit.py"))
    parser.add_argument("--seconds", type=float, default=22.0)
    parser.add_argument("--fps", type=int, default=8)
    args = parser.parse_args()
    if args.seconds < 20.0 or args.seconds > 25.0:
        parser.error("networking evidence must be between 20 and 25 seconds")
    if args.fps <= 0:
        parser.error("--fps must be positive")

    support = load_capture_support()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    data = output / "phipia-v2.1.0-network-data.raw"
    serial = output / "phipia-v2.1.0-networking-serial.log"
    capture = output / "phipia-v2.1.0-networking.pcap"
    audit = output / "phipia-v2.1.0-network-packet-audit.json"
    fixture_log = output / "phipia-v2.1.0-network-fixture.log"
    screenshot = output / "Phipia-v2.1.0-networking.png"
    video = output / "Phipia-v2.1.0-networking-22s.mp4"
    report_path = output / "phipia-v2.1.0-network-fat32-report.json"
    ready = output / ".fixture-ready"
    for path in (serial, capture, audit, fixture_log, screenshot, video,
                 report_path, ready):
        path.unlink(missing_ok=True)
    shutil.copyfile(args.data.resolve(), data)

    peer_port, guest_port = free_udp_ports()
    qmp_port = support.free_port()
    boot_work = tempfile.TemporaryDirectory(prefix="phipia-network-boot-")
    boot = boot_arguments(args, Path(boot_work.name))
    fixture_stream = fixture_log.open("wb")
    fixture = subprocess.Popen([
        args.python, str(args.fixture.resolve()), "--unicast",
        "--port", str(peer_port), "--peer-port", str(guest_port),
        "--mode", "normal", "--capture", str(capture),
        "--ready", str(ready),
    ], stdout=fixture_stream, stderr=subprocess.STDOUT)
    machine: subprocess.Popen[bytes] | None = None
    qmp = None
    try:
        wait_ready(ready, fixture)
        command = [
            args.qemu, "-machine", f"q35,accel={args.accel}", "-m", "128M",
            "-smp", "1", *boot, "-display", "none", "-monitor", "none",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
            *(["-icount", args.icount] if args.icount else []),
            *storage_arguments(args.system, data),
            "-netdev",
            "dgram,id=phipnet,local.type=inet,local.host=127.0.0.1,"
            f"local.port={guest_port},remote.type=inet,remote.host=127.0.0.1,"
            f"remote.port={peer_port}",
            "-device",
            "virtio-net-pci,id=virtio-net0,netdev=phipnet,"
            "mac=52:54:00:12:34:56,disable-legacy=on,mrg_rxbuf=off",
            "-qmp", f"tcp:127.0.0.1:{qmp_port},server=on,wait=off",
            "-serial", f"file:{serial}", "-no-reboot",
        ]
        machine = subprocess.Popen(
            command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        qmp = support.Qmp(qmp_port)
        support.wait_serial(serial, (support.PROOF_LINE, support.PROMPT))
        pointer = Pointer(qmp)
        pointer.rehome()
        events: set[str] = set()
        frames: list[Path] = []
        capture_times: list[float] = []
        with tempfile.TemporaryDirectory(prefix="phipia-network-capture-") as raw:
            work = Path(raw)
            started = time.monotonic()
            next_capture = started
            index = 0
            while time.monotonic() - started < args.seconds:
                elapsed = time.monotonic() - started
                if elapsed >= 0.50 and "terminal_hover" not in events:
                    # Dock artwork magnifies and eases, but input uses the
                    # fixed resting lane shared with the Phipia capture.
                    pointer.move_to(
                        support.dock_item_center(support.DOCK_TERMINAL),
                        support.DOCK_POINTER_Y,
                    )
                    events.add("terminal_hover")
                elif elapsed >= 1.50 and "terminal_open" not in events:
                    # Keyboard activation is deterministic even while the
                    # magnified Dock is still settling after the hover.  The
                    # installed focus starts on Files, so Tab selects Terminal
                    # and Enter opens it through the ordinary PS/2 path.
                    qmp.hmp("sendkey tab 15")
                    time.sleep(0.08)
                    qmp.hmp("sendkey ret 15")
                    support.wait_serial(
                        serial, (b"Phipia: Phip terminal opened",),
                        timeout=5.0,
                    )
                    support.capture_png(
                        qmp, work, output,
                        "Phipia-v2.1.0-networking-terminal-open",
                    )
                    events.add("terminal_open")
                elif elapsed >= 2.30 and "network" not in events:
                    send_command(support, qmp, "network")
                    events.add("network")
                elif elapsed >= 3.60 and "dhcp" not in events:
                    send_command(support, qmp, "dhcp")
                    events.add("dhcp")
                elif elapsed >= 6.50 and "ping" not in events:
                    send_command(support, qmp, "ping 10.0.2.2 1")
                    events.add("ping")
                elif elapsed >= 9.50 and "resolve" not in events:
                    send_command(support, qmp, "resolve phipia.test")
                    events.add("resolve")
                elif elapsed >= 12.50 and "http" not in events:
                    send_command(
                        support, qmp,
                        "http http://phipia.test/welcome.txt NETCAP.TXT",
                    )
                    events.add("http")
                elif elapsed >= 17.50 and "netstat" not in events:
                    send_command(support, qmp, "netstat")
                    events.add("netstat")

                now = time.monotonic()
                if now - started >= args.seconds:
                    break
                if next_capture > now:
                    time.sleep(next_capture - now)
                captured_at = time.monotonic()
                if captured_at - started >= args.seconds:
                    break
                frame = work / f"frame-{index:04d}.ppm"
                support.capture_ppm(qmp, frame)
                frames.append(frame)
                capture_times.append(time.monotonic())
                index += 1
                next_capture = capture_times[-1] + 1.0 / args.fps

            required_events = {
                "terminal_hover", "terminal_open", "network", "dhcp",
                "ping", "resolve", "http", "netstat",
            }
            if events != required_events:
                raise RuntimeError(
                    f"networking video omitted interactions: {required_events - events}"
                )
            support.wait_serial(serial, (
                b"virtio-net0  link up", b"source       dhcp",
                b"1 sent, 1 received", b"10.0.2.20",
                b"200 HTTP response", b"saved NETCAP.TXT",
                b"30 bytes synchronized", b"ipv4-checksum-fail 0",
            ), timeout=20.0)
            ppm = work / "final.ppm"
            support.capture_ppm(qmp, ppm)
            support.ppm_to_png(ppm, screenshot)
            support.encode(args.ffmpeg, frames, capture_times, args.fps,
                           args.seconds, video)
    finally:
        if qmp is not None:
            try:
                qmp.execute("quit")
            except (OSError, RuntimeError):
                pass
            qmp.close()
        terminate(machine)
        terminate(fixture)
        fixture_stream.close()
        ready.unlink(missing_ok=True)
        boot_work.cleanup()

    audited = subprocess.run([
        args.python, str(args.audit.resolve()), str(capture),
        "--json", str(audit),
    ], check=False)
    if audited.returncode != 0:
        raise RuntimeError("captured networking packets failed reconstruction")
    report = fat32_image.inspect_image(data.read_bytes())
    files = {
        str(record["path"]): record for record in report["files"]
        if not bool(record["directory"])
    }
    if "NETCAP.TXT" not in files or int(files["NETCAP.TXT"]["size"]) != 30:
        raise RuntimeError("captured HTTP response was not synchronized to FAT32")
    if (not bool(report["fat_copies_match"]) or int(report["cycles"]) != 0 or
            int(report["cross_links"]) != 0 or
            int(report["leaked_clusters"]) != 0):
        raise RuntimeError("networking capture left inconsistent FAT32 state")
    report_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="ascii"
    )
    print(screenshot)
    print(video)
    print(capture)
    return 0


if __name__ == "__main__":
    sys.exit(main())
