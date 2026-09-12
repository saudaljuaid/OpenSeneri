# Contributing to Phipia

Phipia is a small freestanding kernel. Changes should stay narrow, preserve the
existing trust boundaries, and arrive with evidence that matches their risk.

## Set up a development host

Ubuntu 24.04 or a compatible Debian system is the reference environment:

```sh
sudo apt-get install binutils gcc grub-common grub-pc-bin make mtools \
    qemu-system-x86 xorriso
rustup target add x86_64-unknown-none
make hooks
```

`make hooks` enables the repository's pre-commit and pre-push checks for the
current clone.

## Work on a branch

Start from the current remote default branch and use a descriptive name:

```sh
git fetch origin
git switch -c paging-checks origin/main
```

Do not push directly to `main`, bypass hooks, or force-push protected history.

## Required checks

| Change | Minimum local evidence |
| --- | --- |
| Documentation only | `make lint` and a link check |
| Ordinary code | `make verify` and `make smoke` |
| Boot, CPU, interrupt, memory, device, process, or ABI code | `make verify` and `make qemu-tests` |
| Measured BusyBox profile | Relevant contract workflow plus `make qemu-tests` |

The pull request's `build-and-boot` check must pass on its latest commit. See
[`docs/VERIFICATION.md`](docs/VERIFICATION.md) for what each gate covers.

## Engineering rules

- C11 and x86_64 assembly own machine-facing work. Rust validates selected
  untrusted byte streams; [`docs/RUST.md`](docs/RUST.md) defines that boundary.
- The kernel is freestanding. Do not introduce a host libc, hidden runtime,
  dynamic linking, floating-point/SIMD kernel state, or a red zone.
- Warnings are errors. Keep arithmetic bounded, waits timed, mappings W^X, and
  device ownership explicit.
- PCI drivers must claim resources before enabling them. DMA teardown disables
  bus mastering before memory is reclaimed. Phipia has no IOMMU.
- Preserve supervisor-only kernel mappings and validate every user pointer over
  its complete range before copying.
- Keep fixtures ordinary local files attached read-only to emulated QEMU
  devices. Never use host-device passthrough for project evidence.
- A new invariant needs a failure test capable of disproving it.
- Generated kernels, ISOs, fixtures, toolchains, and editor state are not
  committed.

The current architecture and bounded feature set are summarized in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). The measured Linux compatibility
surface is specified in
[`docs/LINUX_SYSCALL_ABI.md`](docs/LINUX_SYSCALL_ABI.md); widening it requires a
new measured profile, not an edit to an existing allowlist.

## Commits and pull requests

Use an imperative subject of at most 72 characters:

```text
mm: reject overlapping physical ranges
docs: clarify the syscall boundary
```

Keep one logical change per commit. A pull request should explain:

- what changed and why;
- the exact verification run;
- the most credible failure the checks do not cover.

Screenshots show presentation, not correctness. For kernel behavior, attach the
relevant serial transcript or CI result.
