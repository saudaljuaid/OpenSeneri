<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Multiple processes

Phipia can hold up to four user processes and schedule them cooperatively.

## Process state

Phipia holds up to four user processes at once. Each owns:

- its own four-level hierarchy, built from the installed one and validated the
  same way;
- its own image frame, narrowed to read-execute in both the kernel's tables and
  its own, and its own four writable stack pages behind a guard page;
- its own monotonic generation, which is what every paging operation
  authenticates against;
- its own complete CPL3 register set, saved when it leaves and loaded when it
  comes back.

The scheduler gives the processor to each runnable process in turn. A process
leaves through the same reviewed gate the Ring 3 proof uses; the difference is
that the trap saves all sixteen general registers, the instruction pointer, the
stack pointer and the flags into the process, restores the kernel's own tables,
and returns to the scheduler instead of to CPL3. Resuming is the same journey
backwards, so the program continues at the instruction after the one it left
on, with the registers it left with.

## Scheduling model

Scheduling is cooperative, with interrupts masked while a process runs. A
process retains the CPU until it yields or exits. The kernel creates up to four
processes from one bounded executable; the interface has no process creation,
fork, exec, IPC, signals, priorities, shared memory, or wait API.

## The program

The processes run one exact 256-byte ELF64 executable: a second admitted
profile of the same subset the Ring 3 proof uses — one read-execute load
segment, one page, no section table — with a longer body, because a program
that yields does not fit in the proof executable's eight instruction bytes.

It is a bounded loop. Each pass increments a counter, publishes the counter and
the identity the kernel handed it on its own stack, and leaves through
`INT 0x81` with a yield marker in RAX. After the last round it leaves with a
different marker, which is how the kernel tells a yield from an exit. One
branch exists so a process can be told to store into its unmapped guard page
instead; the tail of the page is `HLT`, which is privileged, so a processor
that ever ran past the end of the program would fault rather than continue.

Three independent records hold the same bytes and `make verify` requires all
three to agree:

- `src/kernel/multiprocess.c` carries the whole file as a table;
- `src/rust/elf64.rs` pins the instruction stream and the structural subset it
  must sit in, and refuses it against the Ring 3 profile and vice versa;
- `tools/multiprocess_image.py` rebuilds the file from its instruction table
  and decodes every field again.

## What the proof establishes

`multiprocess_prove` runs three times over.

**Eight controlled failures.** Each one stops a build at a different point —
after the image frame, after its initialization, after the stack frames, after
the address space, after the alias narrowing, after the image mapping, after
the stack mappings, after the permission walk — always on the last process,
with the others already fully installed. Every one of them must return the
frame, paging, DMA, PCI, vector and MSI-X census to exactly what it was. This
is the part single-process teardown never had to survive: releasing a partly
built process while three complete ones are still live.

**One contained fault.** Four processes start; the second is told to store into
its guard page on its third round. The kernel takes the page fault out of CPL3,
recognises it by vector, error code, faulting address and instruction, and ends
that process. The other three keep running and finish normally. Everything is
released together.

**One clean schedule.** Four processes, six rounds each, twenty-eight switches.
The recorded schedule is compared against the schedule a plain round robin
would have produced, recomputed from nothing but each process's configuration:
the order has to be right, and each process has to leave exactly when its own
program says it should.

Isolation is checked by reading back what each process published. Every process
is handed a distinct identity and writes it to its own stack; after the run,
every stack must carry its own process's identity and its own progress. A
process that could reach another's pages would have left a word naming the
wrong process.

Every resume is authenticated before it happens: the stack pointer, the flags
(outside the status bits an instruction is entitled to change), the identity,
the round count, the round it is on, and the exact instruction it is resuming
at. Every trap is authenticated as it happens: the privilege level, the
selectors, the stack pointer and selector, and the hierarchy the processor is
running on.

## Where it runs

- `include/phipia/multiprocess.h` and `src/kernel/multiprocess.c` hold the
  process table and the scheduler.
- `src/arch/x86_64/process.S` gained one function,
  `process_enter_user_context`, which enters CPL3 from a saved register set. It
  publishes the same two words as the original entry, so an interrupt taken out
  of CPL3 returns through the one resume path in `interrupts.S`. Only one user
  process runs at a time on a single core, so one boundary is still the whole
  boundary; what changed is which saved context is loaded into it.
- `src/kernel/interrupts.c` gained `interrupt_process_gate_rearm`, which
  returns an entered-and-returned gate to armed without rewriting the interrupt
  descriptor. The Ring 3 proof crossed its gate once; a scheduler crosses it on
  every switch.
- `src/kernel/paging.c` holds `PAGING_PROCESS_SPACE_SLOTS` hierarchies instead
  of one. Every operation resolves the caller's token to a slot rather than
  confirming the only one. Nothing new crosses the boundary: the token already
  carried the root address, generation and state.

Two typed Boot Ledger stages own it, and the proof stage is **required** rather
than fixture-gated. Its executable is the kernel's own table and its evidence
is the schedule, so it runs on every boot: running several processes is a
property of the system, not of the machine it was started on.

## One new paging rule

Narrowing a frame's kernel identity alias to read-only can split the 2 MiB page
that contains it. A second frame in the same region then borrows that split
table rather than making its own. Restoring the first narrowing would free a
table the second still has a leaf in, so `paging.c` now refuses to restore any
narrowing that is not the newest one owned, and the multiprocess runtime tears
processes down newest first. `multiprocess-slots` proves both halves: an
out-of-order restore is refused, and the in-order one succeeds.

## Evidence

| Scenario | What it proves |
| --- | --- |
| `multiprocess` | the installed receipt and result: four processes, six rounds, twenty-eight switches, round robin, isolation, contained fault, census equal |
| `multiprocess-slots` | four concurrent private hierarchies with distinct roots and generations, a fifth refused, each image mapping private to its own space, an out-of-order alias restore refused, and no frames or tables leaked |

Every other scenario also boots through the required multiprocess proof stage,
so a regression in it fails all ninety-six.
