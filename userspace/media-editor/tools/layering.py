#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Check that the crates depend on each other the way the architecture says.

A layering that lives only in a diagram is a layering that drifts, and this one
had: `media-editor-io` was drawn below `media-editor-media` while depending on it,
and nothing noticed because nothing looked. So the layers are now declared in a
machine-readable block in the architecture document, and this reads that block
and the manifests and refuses any disagreement between them.

The document is the source of truth. If a crate needs to move, the document
moves first and the check follows it — which is the point, because a
dependency added in a hurry is exactly the one nobody writes down.

Three things are checked:

* every crate in the tree appears in the declared layers, and vice versa;
* every dependency runs strictly downward, never sideways and never up;
* the layers cover every dependency, so nothing is quietly unlisted.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent
ARCHITECTURE = REPOSITORY / "docs" / "ARCHITECTURE.md"

#: The fence that opens the declared layers in the architecture document.
OPENING = "```text layers"

#: A path dependency on another crate of this workspace.
DEPENDENCY = re.compile(r'^(media-editor-[a-z]+)\s*=\s*\{[^}]*path\s*=')

#: A line of the declared layers: a depth, then the crates at it.
LAYER = re.compile(r"^(\d+)\s+(.+)$")


def declared_layers() -> dict[str, int]:
    """The depth of every crate, as the architecture document declares it."""
    text = ARCHITECTURE.read_text(encoding="utf-8")
    if OPENING not in text:
        raise SystemExit(f"{ARCHITECTURE}: no `{OPENING}` block to read")
    body = text.split(OPENING, 1)[1].split("```", 1)[0]

    depths: dict[str, int] = {}
    for line in body.splitlines():
        if not line.strip():
            continue
        match = LAYER.match(line.strip())
        if not match:
            raise SystemExit(f"{ARCHITECTURE}: cannot read layer line {line.strip()!r}")
        depth = int(match.group(1))
        for crate in match.group(2).split():
            if crate in depths:
                raise SystemExit(f"{ARCHITECTURE}: {crate} is declared twice")
            depths[crate] = depth
    return depths


def manifests() -> dict[str, list[str]]:
    """Every crate in the tree, and the workspace crates it depends on."""
    found: dict[str, list[str]] = {}
    for manifest in sorted(REPOSITORY.glob("crates/*/Cargo.toml")) + [
        REPOSITORY / "image" / "Cargo.toml"
    ]:
        name = None
        uses: list[str] = []
        section = ""
        for line in manifest.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if stripped.startswith("["):
                section = stripped
                continue
            # Section-aware, because `[[bin]]` carries a `name` too and the
            # first attempt at this read the binary's name as the package's —
            # which the check then reported as a crate missing from the layers.
            # It was right to.
            if section == "[package]" and stripped.startswith("name = "):
                name = stripped.split("=", 1)[1].strip().strip('"')
            match = DEPENDENCY.match(stripped)
            if match and section.endswith("dependencies]"):
                uses.append(match.group(1))
        if name is None:
            raise SystemExit(f"{manifest}: no package name")
        found[name] = uses
    return found


def main() -> int:
    depths = declared_layers()
    crates = manifests()
    findings: list[str] = []

    for name in sorted(set(crates) - set(depths)):
        findings.append(f"{name} is in the tree but not in the declared layers")
    for name in sorted(set(depths) - set(crates)):
        findings.append(f"{name} is in the declared layers but not in the tree")

    for name, uses in sorted(crates.items()):
        if name not in depths:
            continue
        for used in sorted(uses):
            if used not in depths:
                findings.append(f"{name} depends on {used}, which is not declared")
                continue
            if depths[used] >= depths[name]:
                # Strictly downward. Sideways is as bad as upward: two crates
                # in one layer that depend on each other are one crate that
                # has not admitted it, and the layer stops meaning anything.
                findings.append(
                    f"{name} (layer {depths[name]}) depends on {used} "
                    f"(layer {depths[used]}), which is not below it"
                )

    for finding in findings:
        print(finding)
    if findings:
        print(f"\n{len(findings)} layering findings", file=sys.stderr)
        return 1
    print(f"layering: clean, {len(crates)} crates over {max(depths.values()) + 1} layers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
