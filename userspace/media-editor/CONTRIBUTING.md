<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Contributing to Media Editor

Media Editor is a Phipia application. Changes stay narrow, preserve the trust
boundaries, and arrive with evidence that matches their risk.

Read [`docs/ENGINEERING_RULES.md`](docs/ENGINEERING_RULES.md) before your first
change. It is normative, and review will cite it by rule number.

## Set up

```sh
rustup target add x86_64-unknown-none
make hooks     # enable the pre-commit check for this clone
make verify    # run the complete local verification gate
```

`make hooks` sets the clone-local `core.hooksPath`; run it once for each clone.

Ubuntu 24.04 or a compatible Debian system is the reference host, as it is for
Phipia, and the same GNU binutils build both. The pinned compiler is in
`rust-toolchain.toml`.

## Work on a branch

Start from the current remote default branch and use a descriptive name:

```sh
git fetch origin
git switch -c timeline-ripple-trim origin/main
```

Do not push directly to `main`, bypass hooks, or force-push shared history.

## Required evidence

| Change | Minimum evidence |
| --- | --- |
| Documentation only | `make lint` |
| Rules, policy, or platform contract | `make lint`, plus the affected documents updated in the same change |
| Ordinary code | `make lint`, `make check`, and `make test` |
| A parser, a format, or anything reading bytes | Above, plus a fuzz target and a committed corpus |
| Render, colour, or audio path | Above, plus golden hashes and a determinism check |
| Platform boundary, allocator, or ABI | Above, plus `make verify` and, once they exist, the relevant QEMU scenarios |
| A new dependency | The complete import gate in [`docs/DEPENDENCY_POLICY.md`](docs/DEPENDENCY_POLICY.md) |

For a new invariant, include the negative control described in
[`docs/VERIFICATION.md`](docs/VERIFICATION.md) and report its failure result.

**A change that adds or removes a test updates the counts in the same commit.**
`docs/ARCHITECTURE.md` states a count per crate and `README.md` states a total
and a count of negative controls. `make lint` checks both against the tree.

## Commits

One logical change per commit. Imperative subject, at most 72 characters,
prefixed by area:

```text
timeline: refuse an edit whose out point precedes its in point
abi: assert the frame descriptor layout on both sides
docs: record the large code model's section renaming
```

## Pull requests

State what changed and why it belongs in Media Editor; the exact verification
run;
the negative control and its refusal; and the most credible failure the checks
do not cover. "None" is not an acceptable risk statement for anything that
touches the model, the platform, or a user's file.

## Review boundaries

Do not widen bounded contracts in place, bypass the dependency gate, add
`unsafe` outside its permitted crates, introduce POSIX assumptions, make
renders nondeterministic, or weaken project-data safety. The complete rules are
in [`docs/ENGINEERING_RULES.md`](docs/ENGINEERING_RULES.md).
