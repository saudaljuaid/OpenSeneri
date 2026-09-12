<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Dependency policy

[`DEPENDENCIES.md`](DEPENDENCIES.md) records evaluated components. This policy
covers importing, maintaining, upgrading, and removing one.

## The import gate

A dependency enters through one reviewable pull request that completes all nine
steps.

**1. State the need.** Explain the required capability and the local
alternative. Convenience alone is not enough.

**2. State the alternative.** Estimate the shortest in-tree implementation. If
core semantics take less than a week to implement locally, R-12.7 applies.

**3. Verify the licence from the source.** Read the licence file in the
vendored tree. Check every subdirectory: media libraries routinely carry a
different licence for a test corpus, an assembly file, or one bundled
dependency. Record the SPDX expression and the file paths that establish it. A
registry field is not evidence.

**4. Verify the shape.** The component must build for the Media Editor target
with
no `std`, no libc, no build script that generates code from an uncommitted
source, and no network access. Record the exact build command.

**5. Count the `unsafe`.** Record the number of `unsafe` blocks and their
purpose in one line each. This is the dependency's budget, and it is compared
at every upgrade.

**6. Vendor it.** `vendor/<name>-<version>/`, the complete source, no
submodule, no fetch at build time. Record the upstream URL, the exact revision,
and the SHA-256 of the archive. Two clean builds must produce identical
artefacts (R-13.2).

**7. Wrap it.** No dependency's types appear in Media Editor's own code. Each
one
gets a wrapper module that translates its errors into Media Editor's typed
status
enum, bounds its inputs, and refuses what R-1.1 requires bounding. For a C or
C++ dependency, that wrapper lives in `media-editor-abi` and nowhere else.

**8. Test it.** A test that proves the wrapper's contract, and a negative
control that proves the wrapper refuses what it claims to refuse. For any
dependency that parses bytes, a fuzz target as well (R-11.3).

**9. Record it.** An entry in `deps/manifest.toml` with every field below, and
a row updated in [`DEPENDENCIES.md`](DEPENDENCIES.md).

## The manifest

`deps/manifest.toml` is the authoritative record. CI checks that the manifest,
the vendored tree, and the lockfile agree.

```toml
[[dependency]]
name          = "tiny-skia"
version       = "0.11.4"
upstream      = "https://github.com/RazrFalcon/tiny-skia"
revision      = "<exact git revision>"
archive_sha256 = "<uppercase hex>"
licence       = "BSD-3-Clause"
licence_files = ["LICENSE", "src/pipeline/LICENSE-skia"]
tier          = "T1"
purpose       = "CPU path rasterisation for the compositor and interface."
alternative   = "Write a rasteriser; roughly two months for lower quality."
owner         = "<person accountable for this dependency>"
unsafe_blocks = 0
std_required  = false
build_script  = false
wrapper       = "crates/media-editor-render/src/raster/mod.rs"
fuzz_target   = "fuzz/targets/raster_path.rs"
exit_plan     = "Swap to `zeno` behind the same wrapper trait; two weeks."
imported      = "<date>"
reviewed      = "<date>"
```

Every field is required. `exit_plan` must describe a practical replacement or
removal path.

## Keeping it

**Review annually.** CI checks each dependency's `reviewed` date. Renew the
record by checking the license, `unsafe` count, maintenance status, and need.

**Upgrades are deliberate.** One dependency per commit, with the upstream
changelog summarised, the `unsafe` count re-taken, the licence re-verified, and
the full evidence run. Automated dependency updates are forbidden (R-12.6).

**Pinning is absolute.** Exact versions, a committed lockfile, `--locked
--offline` in every build. A build that can resolve a different version than
the last one is not reproducible and therefore not acceptable (R-13.2).

**Avoid duplicates.** `cargo-deny` runs duplicate checks with `deny`.

## Extra rules for C and C++ dependencies

Native dependencies follow these additional rules:

- It is wrapped in `media-editor-abi` and never appears elsewhere (R-3.2.1).
- Its build is reproduced by Media Editor's own build rules. Its upstream build
  system is not run, because it will look for a libc, a configure script, and a
  host it does not have. Where that is impractical, the port is the work item,
  not a shortcut around it.
- It is compiled with Media Editor's flags, including warnings-as-errors and the
  SIMD prohibition of R-13.6.
- Every buffer it writes into is validated over its complete range by Rust
  before the call (R-3.2.5).
- Its assembly is audited for instructions the platform does not support, using
  the same disassembly scan Phipia runs on BusyBox.
- It is presumed hostile: R-11 applies to it exactly as it applies to a file.

## Removal

Removal includes replacement code when needed, deletion of the manifest and
vendored source, an updated `DEPENDENCIES.md` verdict, and regression evidence.

A dependency is removed on sight when it: changes to an incompatible licence,
gains a network or telemetry path, gains an unbounded allocation in a path
Media Editor uses, is unmaintained with an open memory-safety issue, or turns
out
to duplicate something the application must own.

## What CI enforces

| Check | Tool |
| --- | --- |
| Licence allowlist, advisories, duplicates, unknown sources | `cargo-deny` |
| Lockfile is current and the build is offline | `cargo --locked --offline` |
| Vendored tree matches the manifest digests | `tools/check-vendor.py` |
| Manifest is complete and every review date is current | `tools/check-manifest.py` |
| `unsafe` count per dependency matches the recorded budget | `cargo-geiger` |
| Every file carries an SPDX identifier | `reuse` |
| Two clean builds are byte-identical | the build itself |

An import cannot proceed until every required check has an implementation.
