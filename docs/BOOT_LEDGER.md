<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Boot Ledger

Phipia models startup as a validated dependency graph instead of a call list in
`kernel_main`. Each descriptor names a stable stage ID, phase, prerequisites,
provided capabilities, optional policy, irreversible class, and execution
function.

`src/kernel/boot_plan.c` installs the descriptors. `src/kernel/boot_ledger.c`
validates and orders them. `src/kernel/kernel.c` executes the resulting plan and
checks the installed receipts before publishing it read-only.

## Purpose

Boot code combines discovery, irreversible machine transitions, optional
hardware, and tests. A visually plausible call order does not prove that:

- every prerequisite exists and has one provider;
- a stage cannot run before its dependencies;
- optional absence is distinguished from failure;
- PAT/CR3, interrupts, timers, framebuffer output, and scheduling happen only
  after their safety conditions;
- the installed state is the same plan that was validated.

The ledger makes those claims explicit and executable.

## Model

The current bounded model supports 57 descriptors and receipts, with fixed
capability sets per descriptor. The planner rejects duplicate IDs, missing or
duplicate providers, dependency cycles, capacity overflow, invalid phases, and
unsafe irreversible ordering before it runs a stage.

Stages fall into six broad phases:

1. foundation: serial, IDT, and pure self-tests;
2. discovery: loader records, ACPI, topology, and device windows;
3. controllers and memory transition: APICs, frames, PAT, CR3, and installed
   paging proofs;
4. runtime: heap, framebuffer, input, shell, layout, and early scenarios;
5. services: timers, PCI, threads, scheduler, DMA, devices, storage, processes,
   and measured Linux profiles;
6. proofs and presentation: closing checks, the optional networking/entropy
   availability decision, desktop activation, and the installed Phipia
   proof.

Within those phases, declared capabilities—not descriptor insertion order—form
the canonical sequence.

## Required irreversible edges

| Transition | Must already be proved |
| --- | --- |
| PAT programming and CR3 replacement | validated device windows and physical frames |
| interrupt enable | installed IDT and configured interrupt controllers |
| framebuffer output | independent framebuffer write-combining proof |
| APIC timer activation | IDT, controllers, interrupts, and discovered clocks |
| scheduler activation | heap, threads, timer calibration, and interrupts |

These checks are duplicated at the semantic boundary: descriptor validation
refuses a bad plan, and subsystem installation refuses a bad live state.

## Optional stages

Optional does not mean silent. A stage produces exactly one of:

- success with its declared capabilities;
- a typed neutral absence result when absence is allowed;
- a failure that prevents dependent execution.

Framebuffer, pointer, emulated-device, process, Linux-profile, networking, and
desktop stages use this distinction so a serial-only boot cannot masquerade as
a fully installed graphical or device-backed boot. Networking distinguishes
working, NIC-absent, and link-down outcomes; malformed hardware or failed
ownership setup is not neutral absence.

## Receipts and installed proof

Every executed stage emits a receipt containing its identity, outcome, provided
capabilities, and bounded proof counters. The validated plan has a deterministic
fingerprint. Before handoff, the kernel requires:

- the receipt sequence matches the canonical plan;
- every required stage succeeded;
- every optional stage has one valid outcome;
- provided capabilities match the descriptor and outcome;
- the plan fingerprint is unchanged;
- subsystem resource censuses and installed-state checks pass.

The `boot-ledger` QEMU scenario exercises planning, ordering, receipts, neutral
absence, mutation detection, and the permanent transcript contract. See
[`VERIFICATION.md`](VERIFICATION.md).

## Operator surface

Phipia exposes a read-only summary of installed stages and capabilities.
It does not control or replay startup. The serial transcript remains the full
diagnostic record, while `boot_report.c` formats discovery data without making
policy decisions.
