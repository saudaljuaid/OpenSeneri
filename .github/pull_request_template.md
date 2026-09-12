## Change

Explain exactly what changed and why it belongs in Phipia.

## Evidence

- [ ] `make verify` passes from a clean tree.
- [ ] `make smoke` reaches `Phipia: day one passed` in QEMU.
- [ ] No warning, failed check, or unexplained binary artifact is present.
- [ ] The commit is atomic, reviewable, and safe to revert.
- [ ] Boot, memory, ABI, or linker invariants changed here are documented.

## Risk

Name the failure mode and the rollback plan. "None" is not acceptable for kernel code.
