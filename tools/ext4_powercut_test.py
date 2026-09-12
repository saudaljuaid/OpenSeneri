#!/usr/bin/env python3
"""Cut Phipia at every ext4 durability boundary and verify reboot recovery."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

import ext4_image


POWER_CUTS = (
    "checkpoint",
    "journal-state",
    "filesystem-state",
    "filesystem-state",
    "ordered-data",
    "journal-payload",
    "commit",
    "checkpoint",
    "journal-state",
    "filesystem-state",
)
POWER_CUT_EXIT_STATUS = 221
PASS_EXIT_STATUS = 13
PASS_MARKER = "ST PASS ext4-recovery"


class PowerCutError(RuntimeError):
    """A named power-cut evidence failure."""


def _transcript_tail(transcript: str, line_count: int = 24) -> str:
    """Return a bounded serial tail suitable for a failed CI annotation."""
    return "\n".join(transcript.splitlines()[-line_count:])


def _build_iso(
    kernel: Path,
    output: Path,
    grub_mkrescue: str,
    grub_module_dir: Path | None,
    cut: int | None,
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="phipia-ext4-cut-", dir=output.parent) as raw:
        root = Path(raw)
        boot = root / "boot"
        grub = boot / "grub"
        grub.mkdir(parents=True)
        shutil.copyfile(kernel, boot / "phipia.elf")
        command_line = "phipia.test=ext4-recovery"
        if cut is not None:
            command_line += f" phipia.ext4-cut={cut}"
        (grub / "grub.cfg").write_text(
            "\n".join(
                (
                    "set default=0",
                    "set timeout=0",
                    "",
                    'menuentry "Phipia ext4 durability test" {',
                    f"    multiboot2 /boot/phipia.elf {command_line}",
                    "    boot",
                    "}",
                    "",
                )
            ),
            encoding="ascii",
            newline="\n",
        )
        command = [grub_mkrescue]
        if grub_module_dir is not None:
            command.extend(("-d", str(grub_module_dir)))
        command.extend(("-o", str(output), str(root)))
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise PowerCutError(
                f"grub-mkrescue failed for boundary {cut}:\n{result.stdout.strip()}"
            )


def _run_qemu(
    qemu: str,
    accel: str,
    iso: Path,
    image: Path,
    log: Path,
    timeout: int,
) -> tuple[int, str]:
    command = (
        qemu,
        "-machine",
        f"accel={accel}",
        "-m",
        "128M",
        "-smp",
        "1",
        "-boot",
        "order=d",
        "-blockdev",
        f"driver=file,filename={image},node-name=ext4-file,read-only=off,auto-read-only=off",
        "-blockdev",
        "driver=raw,file=ext4-file,node-name=ext4-raw,read-only=off",
        "-device",
        "nvme,serial=phipia-ext4-powercut,drive=ext4-raw,logical_block_size=4096,physical_block_size=4096,max_ioqpairs=1,msix_qsize=1",
        "-cdrom",
        str(iso),
        "-display",
        "none",
        "-monitor",
        "none",
        "-serial",
        "stdio",
        "-device",
        "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-no-reboot",
    )
    try:
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        transcript = error.stdout or ""
        if isinstance(transcript, bytes):
            transcript = transcript.decode("utf-8", errors="replace")
        log.write_text(transcript, encoding="utf-8", newline="\n")
        raise PowerCutError(f"QEMU timed out; transcript: {log}") from error
    log.write_text(result.stdout, encoding="utf-8", newline="\n")
    return result.returncode, result.stdout


def _verify_guest_result(image: Path, tools: dict[str, str], temporary: Path) -> None:
    destination = temporary / "README.TXT"
    result = subprocess.run(
        (
            tools["debugfs"],
            "-R",
            f"dump -p /system/README.TXT {destination}",
            str(image),
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if result.returncode != 0 or not destination.is_file():
        raise PowerCutError("debugfs could not dump the recovered README:\n" + result.stdout)
    data = destination.read_bytes()
    prefix = b"Phipia deterministic ext4 fixture\n"
    if (
        len(data) != 4097
        or data[: len(prefix)] != prefix
        or any(data[len(prefix) : 4096])
        or data[4096:] != b"X"
    ):
        raise PowerCutError("recovered README namespace or sparse extension is wrong")


def run(args: argparse.Namespace) -> dict[str, object]:
    kernel = args.kernel.resolve()
    fixture = args.fixture.resolve()
    output = args.output.resolve()
    if not kernel.is_file() or not fixture.is_file():
        raise PowerCutError("kernel or clean ext4 fixture is missing")
    output.mkdir(parents=True, exist_ok=True)
    tools = ext4_image.require_tools()
    verify_iso = output / "verify.iso"
    _build_iso(kernel, verify_iso, args.grub_mkrescue, args.grub_module_dir, None)

    reports: list[dict[str, object]] = []
    for cut, boundary in enumerate(POWER_CUTS, start=1):
        image = output / f"cut-{cut:02d}.raw"
        cut_iso = output / f"cut-{cut:02d}.iso"
        cut_log = output / f"cut-{cut:02d}.log"
        reboot_log = output / f"cut-{cut:02d}-reboot.log"
        before_report = ext4_image.prepare_recovery_marker_image(fixture, image)
        _build_iso(
            kernel,
            cut_iso,
            args.grub_mkrescue,
            args.grub_module_dir,
            cut,
        )
        status, transcript = _run_qemu(
            args.qemu, args.accel, cut_iso, image, cut_log, args.timeout
        )
        marker = f"ST EXT4 POWER CUT {cut} {boundary}"
        if status != POWER_CUT_EXIT_STATUS or marker not in transcript:
            raise PowerCutError(
                f"boundary {cut} did not cut exactly: status={status}, log={cut_log}\n"
                + _transcript_tail(transcript)
            )
        if PASS_MARKER in transcript or "ST FAIL" in transcript or "Phipia PANIC" in transcript:
            raise PowerCutError(f"boundary {cut} reached an invalid terminal marker")

        status, transcript = _run_qemu(
            args.qemu, args.accel, verify_iso, image, reboot_log, args.timeout
        )
        if (
            status != PASS_EXIT_STATUS
            or transcript.count("ST BEGIN ext4-recovery") != 1
            or transcript.count(PASS_MARKER) != 1
            or "ST FAIL" in transcript
            or "Phipia PANIC" in transcript
        ):
            raise PowerCutError(
                f"boundary {cut} reboot failed: status={status}, log={reboot_log}\n"
                + _transcript_tail(transcript)
            )
        after_report = ext4_image.inspect_image(image, tools=tools)
        with tempfile.TemporaryDirectory(prefix="phipia-ext4-result-", dir=output) as raw:
            _verify_guest_result(image, tools, Path(raw))
        image_sha256 = hashlib.sha256(image.read_bytes()).hexdigest()
        report = {
            "boundary": boundary,
            "cut": cut,
            "cut_log": cut_log.name,
            "reboot_log": reboot_log.name,
            "before": before_report,
            "after": after_report,
            "image_sha256": image_sha256,
        }
        (output / f"cut-{cut:02d}.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        reports.append(report)
        cut_iso.unlink()
        if not args.keep_images:
            image.unlink()

    aggregate = {
        "boundary_count": len(POWER_CUTS),
        "kernel_sha256": hashlib.sha256(kernel.read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
        "power_cut_exit_status": POWER_CUT_EXIT_STATUS,
        "reboot_exit_status": PASS_EXIT_STATUS,
        "reports": reports,
    }
    (output / "report.json").write_text(
        json.dumps(aggregate, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return aggregate


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--grub-mkrescue", default="grub-mkrescue")
    parser.add_argument("--grub-module-dir", type=Path)
    parser.add_argument("--accel", default="tcg")
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--keep-images", action="store_true")
    return parser


def main() -> int:
    args = _parser().parse_args()
    try:
        report = run(args)
    except (OSError, PowerCutError, ext4_image.Ext4ImageError) as error:
        print(f"ext4_powercut_test.py: {error}")
        return 1
    print(f"verified {report['boundary_count']} ext4 durability power cuts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
