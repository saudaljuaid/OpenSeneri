#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Repository hygiene checks for Media Editor.

The rules Media Editor writes down have to be checkable, and that includes the
rules about the documents themselves. This runs over the tracked tree and
refuses trailing whitespace, stray tabs, missing final newlines, a document
without its licence header, an over-long prose line, and an internal link that
points at nothing. It repairs nothing: every finding is printed with its file,
line, and the rule it broke, and the exit status is non-zero.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent

BINARY_SUFFIXES = {".png", ".jpg", ".jpeg", ".mp4", ".ico", ".gif", ".pdf"}
TAB_EXEMPT = {"Makefile", ".gitmodules"}
SPDX_REQUIRED_SUFFIXES = {".md", ".py", ".sh", ".rs", ".c", ".h", ".ld", ".yml"}
SPDX_EXEMPT = {"LICENSE", ".github/CODEOWNERS"}
PROSE_LIMIT = 80

LINK = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
FENCE = re.compile(r"^\s*(```|~~~)")


def tracked_files() -> list[Path]:
    output = subprocess.run(
        ["git", "-C", str(REPOSITORY), "ls-files"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [REPOSITORY / line for line in output.splitlines() if line]


def is_text(path: Path) -> bool:
    return path.suffix.lower() not in BINARY_SUFFIXES


def prose_line_ok(line: str) -> bool:
    """Tables, code, and unsplittable links are exempt from the prose limit."""
    stripped = line.strip()
    if len(line) <= PROSE_LIMIT:
        return True
    if stripped.startswith("|") or stripped.startswith("<!--"):
        return True
    if stripped.startswith("[") and "]:" in stripped:
        return True
    if " " not in stripped:
        return True
    return False


def check_file(path: Path, findings: list[str]) -> None:
    relative = path.relative_to(REPOSITORY).as_posix()
    if not path.exists():
        findings.append(f"{relative}: tracked file is missing from the tree")
        return
    if not is_text(path):
        return

    raw = path.read_bytes()
    if raw and not raw.endswith(b"\n"):
        findings.append(f"{relative}: file does not end with a newline")
    if b"\r\n" in raw:
        findings.append(f"{relative}: file contains CRLF line endings")

    text = raw.decode("utf-8")
    lines = text.splitlines()

    if (
        path.suffix in SPDX_REQUIRED_SUFFIXES
        and relative not in SPDX_EXEMPT
        and "SPDX-License-Identifier: GPL-3.0-only" not in "\n".join(lines[:6])
    ):
        findings.append(f"{relative}:1: missing GPL-3.0-only SPDX header")

    in_fence = False
    for number, line in enumerate(lines, start=1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if line != line.rstrip():
            findings.append(f"{relative}:{number}: trailing whitespace")
        if "\t" in line and path.name not in TAB_EXEMPT:
            findings.append(f"{relative}:{number}: tab character")
        if path.suffix == ".md" and not in_fence and not prose_line_ok(line):
            findings.append(
                f"{relative}:{number}: prose line is "
                f"{len(line)} characters, limit is {PROSE_LIMIT}"
            )
        if path.suffix == ".md" and not in_fence:
            check_links(path, relative, number, line, findings)
    if in_fence:
        findings.append(f"{relative}: an unterminated code fence")


def check_links(
    path: Path,
    relative: str,
    number: int,
    line: str,
    findings: list[str],
) -> None:
    for target in LINK.findall(line):
        target = target.split(" ")[0]
        if target.startswith(("http://", "https://", "#", "mailto:")):
            continue
        resolved = (path.parent / target.split("#")[0]).resolve()
        if not resolved.exists():
            findings.append(f"{relative}:{number}: broken link {target}")


def main() -> int:
    findings: list[str] = []
    for path in tracked_files():
        check_file(path, findings)
    for finding in findings:
        print(finding)
    if findings:
        print(f"\n{len(findings)} hygiene findings", file=sys.stderr)
        return 1
    print("lint: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
