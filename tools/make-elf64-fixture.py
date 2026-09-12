#!/usr/bin/env python3
"""Emit Phipia's exact ELF64 proof payload as an ordinary build file."""

from __future__ import annotations

import os
from pathlib import Path
import stat
import sys

from elf64_fixture import FILE_BYTES, PAYLOAD_SHA256, build_payload, verify_payload


def checked_output(argument: str) -> Path:
    """Confine the output to an ordinary file below the repository build tree."""
    repository = Path(__file__).resolve().parents[1]
    build = (repository / "build").resolve()
    output = Path(argument)
    if not output.is_absolute():
        output = repository / output
    resolved = output.resolve(strict=False)
    if build != resolved and build not in resolved.parents:
        raise ValueError("ELF output must remain under the repository build directory")
    if output.exists():
        mode = output.lstat().st_mode
        if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
            raise ValueError("ELF output must be an ordinary non-symlink file")
    output.parent.mkdir(parents=True, exist_ok=True)
    return output


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} OUTPUT", file=sys.stderr)
        return 2
    output = None
    try:
        output = checked_output(sys.argv[1])
        payload = build_payload()
        with output.open("wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        with output.open("rb") as stream:
            reopened = stream.read()
        verify_payload(reopened)
    except (OSError, ValueError) as error:
        if output is not None:
            try:
                output.unlink(missing_ok=True)
            except OSError:
                pass
        print(f"ELF64 fixture refused: {error}", file=sys.stderr)
        return 1
    print(f"{output}: ELF64 ET_EXEC {FILE_BYTES} bytes, SHA-256 {PAYLOAD_SHA256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

