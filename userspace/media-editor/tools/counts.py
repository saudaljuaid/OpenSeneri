#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check documented test totals against the tracked Rust sources.

`docs/ARCHITECTURE.md` gives a test count for every crate and `README.md` gives
the total. This script keeps those two compact summaries aligned with the tree.

It counts *tracked* files, like the hygiene check does, so a test in a file
that has not been staged yet is invisible to it. That fails safe — the count
comes up short and the check refuses — but it refuses with a confusing reason,
so: stage the file, then read the finding.

The test count is static — `#[test]` attributes in each crate's `src` and
`tests` — rather than taken from a run. `counts_agree_with_a_run` in this
file's own commit message records that the two matched exactly for all nine
crates when it was written, and `make verify` runs the suite anyway, so a
divergence between the two would show up there as a different total.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent
ARCHITECTURE = REPOSITORY / "docs" / "ARCHITECTURE.md"
README = REPOSITORY / "README.md"

#: A crate row in the architecture table, and the count it claims.
ROW = re.compile(r"^\| `(media-editor-[a-z]+)` \|.*?\b(\d+) tests\b", re.MULTILINE)

#: A crate row at all, whether or not it states a count.
ANY_ROW = re.compile(r"^\| `(media-editor-[a-z]+)` \|", re.MULTILINE)

#: The total, in the README's status paragraph.
TOTAL = re.compile(r"^(\d+) tests, no third-party dependencies", re.MULTILINE)

def tracked(pattern: str) -> list[Path]:
    output = subprocess.run(
        ["git", "-C", str(REPOSITORY), "ls-files", pattern],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [REPOSITORY / line for line in output.splitlines() if line]


def tests_per_crate() -> dict[str, int]:
    """How many `#[test]` attributes each crate carries."""
    found: dict[str, int] = {}
    for path in tracked("crates/**/*.rs"):
        crate = path.relative_to(REPOSITORY).parts[1]
        text = path.read_text(encoding="utf-8")
        count = sum(1 for line in text.splitlines() if line.strip() == "#[test]")
        found[crate] = found.get(crate, 0) + count
    return found


def main() -> int:
    findings: list[str] = []
    counted = tests_per_crate()
    architecture = ARCHITECTURE.read_text(encoding="utf-8")
    claimed = {crate: int(number) for crate, number in ROW.findall(architecture)}
    listed = set(ANY_ROW.findall(architecture))

    for crate, count in sorted(counted.items()):
        if count == 0:
            continue
        if crate not in listed:
            findings.append(f"{crate} has {count} tests and no row in the architecture table")
        elif crate not in claimed:
            findings.append(f"{crate} has {count} tests and its row states no count")
        elif claimed[crate] != count:
            findings.append(
                f"{crate}: the architecture says {claimed[crate]} tests, the tree has {count}"
            )

    for crate in sorted(claimed):
        if counted.get(crate, 0) == 0:
            findings.append(f"{crate} claims {claimed[crate]} tests and the tree has none")

    total = sum(counted.values())
    readme = README.read_text(encoding="utf-8")
    stated = TOTAL.search(readme)
    if stated is None:
        findings.append("the README states no test total")
    elif int(stated.group(1)) != total:
        findings.append(f"the README says {stated.group(1)} tests, the tree has {total}")

    for finding in findings:
        print(finding)
    if findings:
        print(f"\n{len(findings)} counting findings", file=sys.stderr)
        return 1
    print(f"counts: clean, {total} tests, as documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
