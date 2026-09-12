#!/usr/bin/env python3
"""Compare two SDK trees by relative names and exact content."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def inventory(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*")) if path.is_file()
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("first", type=Path)
    parser.add_argument("second", type=Path)
    arguments = parser.parse_args()
    first = inventory(arguments.first)
    second = inventory(arguments.second)
    if first != second:
        names = sorted(set(first) | set(second))
        differences = [name for name in names if first.get(name) != second.get(name)]
        raise SystemExit("SDK trees differ: " + ", ".join(differences))
    print(f"Phipia SDK reproducibility passed: {len(first)} byte-identical files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
