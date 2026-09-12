#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run one deterministic Phipia networking scenario under QEMU."""

from __future__ import annotations

import argparse
import json
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


NO_NIC = {
    "network-nic-absent",
    "network-missing-linux-echo",
    "network-missing-linux-uname",
    "network-missing-linux-cat",
}

STORAGE = {
    "network-http-length",
    "network-http-chunked",
    "network-http-redirect",
    "network-http-malformed",
    "network-http-nested",
    "network-http-replace",
    "network-http-disk-full",
    "network-system-immutable",
    "network-missing-linux-echo",
    "network-missing-linux-uname",
    "network-missing-linux-cat",
    "network-files",
    "network-notes",
    "network-media-editor",
    "network-persistence",
    "network-native",
    "native-https",
    "native-phip",
}

FIXTURE_MODE = {
    "native-https": "https",
    "native-phip": "packages-lifecycle",
    "network-dhcp-timeout": "dhcp-timeout",
    "network-icmp-timeout": "silent",
    "network-dns-cname": "dns-cname",
    "network-dns-malformed": "dns-truncated",
    "network-tcp-retransmit": "tcp-retransmit",
    "network-tcp-reset": "tcp-reset",
    "network-tcp-listen": "tcp-listen",
    "network-tcp-refused": "tcp-refused",
    "network-http-chunked": "http-chunked",
    "network-http-redirect": "http-redirect",
    "network-http-malformed": "http-malformed",
}


def free_udp_ports() -> tuple[int, int]:
    sockets: list[socket.socket] = []
    ports: list[int] = []
    try:
        for _ in range(2):
            current = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            current.bind(("127.0.0.1", 0))
            sockets.append(current)
            ports.append(current.getsockname()[1])
    finally:
        for current in sockets:
            current.close()
    return ports[0], ports[1]


def free_tcp_port() -> int:
    current = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        current.bind(("127.0.0.1", 0))
        return int(current.getsockname()[1])
    finally:
        current.close()


def wait_ready(path: Path, process: subprocess.Popen[bytes]) -> None:
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            raise RuntimeError("network fixture exited before becoming ready")
        time.sleep(0.02)
    raise RuntimeError("network fixture readiness timed out")


def qmp_command(endpoint: Path | tuple[str, int], execute: str,
                arguments: dict[str, object]) -> None:
    deadline = time.monotonic() + 8.0
    family = socket.AF_UNIX if isinstance(endpoint, Path) else socket.AF_INET
    address: str | tuple[str, int] = (
        str(endpoint) if isinstance(endpoint, Path) else endpoint
    )
    while True:
        client = socket.socket(family, socket.SOCK_STREAM)
        try:
            client.connect(address)
            break
        except (FileNotFoundError, ConnectionRefusedError) as error:
            client.close()
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    "QMP socket did not become ready"
                ) from error
            time.sleep(0.02)
    stream = client.makefile("rwb", buffering=0)

    def receive_return() -> None:
        while True:
            line = stream.readline()
            if not line:
                raise RuntimeError("QMP closed before returning a result")
            message = json.loads(line)
            if "error" in message:
                raise RuntimeError(f"QMP command failed: {message['error']}")
            if "return" in message:
                return

    greeting = json.loads(stream.readline())
    if "QMP" not in greeting:
        raise RuntimeError("QMP greeting was malformed")
    stream.write(b'{"execute":"qmp_capabilities"}\n')
    receive_return()
    payload = json.dumps({"execute": execute, "arguments": arguments},
                         separators=(",", ":")).encode("ascii") + b"\n"
    stream.write(payload)
    receive_return()
    stream.close()
    client.close()


def storage_arguments(args: argparse.Namespace, output: Path) -> list[str]:
    if args.scenario not in STORAGE:
        return []
    source = args.full if args.scenario == "network-http-disk-full" else args.data
    if args.system is None or source is None:
        raise RuntimeError("storage scenario requires System and Data images")
    data = output / "data.raw"
    shutil.copyfile(source, data)
    data_block_size = 4096 if args.data_filesystem == "ext4" else 512
    return [
        "-boot", "order=d",
        "-blockdev", f"driver=file,filename={args.system},node-name=system-file,read-only=on,auto-read-only=off",
        "-blockdev", "driver=raw,file=system-file,node-name=system-raw,read-only=on",
        "-device", "nvme,serial=phipia-system-fat32,drive=system-raw,logical_block_size=512,physical_block_size=512,max_ioqpairs=1,msix_qsize=1",
        "-blockdev", f"driver=file,filename={data},node-name=data-file,read-only=off,auto-read-only=off",
        "-blockdev", "driver=raw,file=data-file,node-name=data-raw,read-only=off",
        "-device", f"nvme,serial=phipia-data-{args.data_filesystem},drive=data-raw,logical_block_size={data_block_size},physical_block_size={data_block_size},max_ioqpairs=1,msix_qsize=1",
    ]


def terminate(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3.0)


def boot_arguments(args: argparse.Namespace, output: Path) -> list[str]:
    if args.efi_root is None:
        if args.iso is None:
            raise RuntimeError("an ISO or --efi-root is required")
        return ["-cdrom", str(args.iso.resolve())]
    if args.kernel is None or args.uefi_code is None or args.uefi_vars is None:
        raise RuntimeError(
            "--efi-root requires --kernel, --uefi-code, and --uefi-vars"
        )

    root = output / "efi"
    variables = output / "efi-vars.fd"
    if root.exists():
        shutil.rmtree(root)
    shutil.copytree(args.efi_root, root)
    shutil.copyfile(args.kernel, root / "boot" / "phipia.elf")
    configuration = (
        "set default=0\n"
        "set timeout=0\n\n"
        'menuentry "Phipia test" {\n'
        f"    multiboot2 /boot/phipia.elf phipia.test={args.scenario}\n"
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
        "-drive", f"if=pflash,format=raw,readonly=on,file={args.uefi_code.resolve().as_posix()}",
        "-drive", f"if=pflash,format=raw,file={variables.resolve().as_posix()}",
        "-drive", f"format=raw,file=fat:rw:{root.resolve().as_posix()}",
    ]


def run(args: argparse.Namespace) -> int:
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    serial = output / "serial.log"
    capture = output / "network.pcap"
    audit = output / "packet-audit.json"
    ready = output / "fixture.ready"
    fixture_log = output / "fixture.log"
    qmp = output / "qmp.sock"
    qmp_endpoint: Path | tuple[str, int] = qmp
    for path in (serial, capture, audit, ready, fixture_log, qmp):
        path.unlink(missing_ok=True)

    fixture: subprocess.Popen[bytes] | None = None
    machine: subprocess.Popen[bytes] | None = None
    fixture_stream = None
    peer_port, guest_port = free_udp_ports()
    qemu = [
        args.qemu, "-machine", f"q35,accel={args.accel}", "-m", "128M",
        "-smp", "1", "-display", "none",
        "-monitor", "none", "-serial", "stdio", "-device",
        "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]
    if args.scenario in ("native-https", "native-phip"):
        qemu.extend([
            "-cpu", "max",
            "-rtc", "base=" + (
                "2027-01-15T08:01:00" if args.scenario == "native-phip"
                else "2026-08-31T00:00:00"
            ) + ",clock=vm",
        ])
    qemu.extend(boot_arguments(args, output))
    qemu.extend(storage_arguments(args, output))
    if args.scenario not in NO_NIC:
        fixture_stream = fixture_log.open("wb")
        mode = FIXTURE_MODE.get(args.scenario, "normal")
        fixture_command = [
            args.python, str(args.fixture.resolve()), "--unicast",
            "--port", str(peer_port), "--peer-port", str(guest_port),
            "--mode", mode, "--capture", str(capture), "--ready", str(ready),
        ]
        if args.content_root is not None:
            fixture_command.extend([
                "--content-root", str(args.content_root.resolve())
            ])
        fixture = subprocess.Popen(
            fixture_command, stdout=fixture_stream, stderr=subprocess.STDOUT
        )
        wait_ready(ready, fixture)
        qemu.extend([
            "-netdev", "dgram,id=phipnet,local.type=inet,local.host=127.0.0.1,local.port="
            f"{guest_port},remote.type=inet,remote.host=127.0.0.1,remote.port={peer_port}",
            "-device", "virtio-net-pci,id=virtio-net0,netdev=phipnet,mac=52:54:00:12:34:56,disable-legacy=on,mrg_rxbuf=off",
        ])
    if args.scenario == "network-link-down":
        if hasattr(socket, "AF_UNIX"):
            qemu.extend(["-qmp", f"unix:{qmp},server=on,wait=off"])
        else:
            qmp_endpoint = ("127.0.0.1", free_tcp_port())
            qemu.extend([
                "-qmp", f"tcp:{qmp_endpoint[0]}:{qmp_endpoint[1]},server=on,wait=off"
            ])
    if args.scenario not in ("network-persistence", "native-phip"):
        qemu.append("-no-reboot")

    expected_begins = 3 if args.scenario == "native-phip" else (
        2 if args.scenario == "network-persistence" else 1
    )
    try:
        with serial.open("wb") as serial_stream:
            machine = subprocess.Popen(qemu, stdin=subprocess.DEVNULL,
                                       stdout=serial_stream,
                                       stderr=subprocess.STDOUT)
            if args.scenario == "network-link-down":
                qmp_command(qmp_endpoint, "set_link",
                            {"name": "virtio-net0", "up": False})
            try:
                result = machine.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                machine.kill()
                machine.wait(timeout=5.0)
                result = 124
    finally:
        terminate(machine)
        terminate(fixture)
        if fixture_stream is not None:
            fixture_stream.close()

    transcript = serial.read_text(encoding="utf-8", errors="replace")
    begin = transcript.count(f"ST BEGIN {args.scenario}\n")
    passed = transcript.count(f"ST PASS {args.scenario}\n")
    healthy = (result == args.expected and begin == expected_begins and
               passed == 1 and "ST FAIL" not in transcript and
               "Phipia PANIC" not in transcript and
               "ST NETWORK production path bounded and recoverable" in transcript)
    if args.scenario == "network-native" and healthy:
        healthy = (
            transcript.count(
                "PHIPIA NETAPP PASS dns=10.0.2.20 http=30 udp=echo "
                "timeout reset cancel malformed-dns\n"
            ) == 1
            and transcript.count(
                "Phipia: native DNS, TCP, UDP, timeout, reset and "
                "cancellation passed\n"
            ) == 1
        )
    if args.scenario == "native-https" and healthy:
        required = (
            "PHIPIA HTTPSAPP PHASE start\n",
            "PHIPIA HTTPSAPP PHASE authenticated-download PASS\n",
            "PHIPIA HTTPSAPP PHASE durable-output PASS\n",
            "PHIPIA HTTPSAPP PHASE kernel-upload PASS\n",
            "PHIPIA HTTPSAPP PASS hostname time trust length close upload\n",
            "Phipia: HTTPS strong hardware entropy passed\n",
            "Phipia: HTTPS TLS 1.2 hostname time trust framing close and "
            "teardown passed\n",
        )
        healthy = all(transcript.count(marker) == 1 for marker in required)
        if healthy:
            audited = subprocess.run([
                args.python, str(args.audit.resolve()), str(capture),
                "--https", "--json", str(audit),
            ], check=False)
            healthy = audited.returncode == 0
    if args.scenario == "native-phip" and healthy:
        required = (
            "PHIPIA PHIP PHASE signed-plan-refused PASS\n",
            "PHIPIA PHIP PHASE committed generation=1 PASS\n",
            "PHIPIA PHIP PHASE committed generation=2 PASS\n",
            "PHIPIA PHIP PHASE repair-plan PASS\n",
            "PHIPIA PHIP PHASE repaired generation=3 PASS\n",
            "PHIPIA PHIP REPAIR PASS trust payload transaction cleanup\n",
            "Phipia: signed HTTPS package install synchronized reboot phase\n",
            "Phipia: signed HTTPS package update synchronized reboot phase\n",
            "Phipia: damaged package generation quarantined before repair "
            "passed\n",
            "PHIPIA SDL CHESS PASS upstream=release-2.32.10 "
            "frames=8 persistent=yes\n",
            "Phipia: damaged SDL package repaired authenticated and launched "
            "from writable ext4 passed\n",
        )
        healthy = all(transcript.count(marker) == 1 for marker in required)
        healthy = healthy and all(
            transcript.count(marker) == count for marker, count in (
                ("PHIPIA PHIP PHASE start\n", 4),
                ("PHIPIA PHIP PHASE signed-plan PASS\n", 2),
                ("PHIPIA PHIP PHASE payloads-authenticated PASS\n", 3),
                ("PHIPIA PHIP PASS https trust plan payload transaction "
                 "cleanup\n", 2),
            )
        )
        if healthy:
            audited = subprocess.run([
                args.python, str(args.audit.resolve()), str(capture),
                "--https", "--json", str(audit),
            ], check=False)
            healthy = audited.returncode == 0
        if healthy:
            fsck_log = output / "data-fsck.log"
            fsck = subprocess.run(
                ["e2fsck", "-f", "-n", str(output / "data.raw")],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
            )
            fsck_log.write_bytes(fsck.stdout)
            healthy = fsck.returncode == 0
    if args.scenario == "network-http-length" and healthy:
        audited = subprocess.run([
            args.python, str(args.audit.resolve()), str(capture),
            "--json", str(audit),
        ], check=False)
        healthy = audited.returncode == 0
    if not healthy:
        print(f"QEMU scenario {args.scenario} failed: status={result} "
              f"expected={args.expected} begin={begin}/{expected_begins} pass={passed}",
              file=sys.stderr)
        print(transcript, file=sys.stderr)
        return 1
    print(f"QEMU scenario {args.scenario} passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--expected", required=True, type=int)
    parser.add_argument("--iso", type=Path)
    parser.add_argument("--efi-root", type=Path)
    parser.add_argument("--kernel", type=Path)
    parser.add_argument("--uefi-code", type=Path)
    parser.add_argument("--uefi-vars", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--fixture", type=Path,
                        default=Path("tools/network_fixture.py"))
    parser.add_argument("--audit", type=Path,
                        default=Path("tools/network_packet_audit.py"))
    parser.add_argument("--system", type=Path)
    parser.add_argument("--data", type=Path)
    parser.add_argument(
        "--data-filesystem", choices=("fat32", "ext4"), default="fat32"
    )
    parser.add_argument("--full", type=Path)
    parser.add_argument("--content-root", type=Path)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--accel", default="tcg")
    parser.add_argument("--timeout", type=float, default=45.0)
    args = parser.parse_args()
    try:
        return run(args)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"network scenario runner: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
