<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Native userspace performance diagnostics

Phipia records bounded diagnostics to catch structural regressions. They are
QEMU measurements, not production throughput claims, and the QEMU serial logs
retained by the native-porting workflow are the evidence of record.

| Marker | Measured boundary |
| --- | --- |
| `PHIPIA PERF syscall` | 1,024 Ring 3 ABI-version queries, reported as total and mean monotonic nanoseconds. |
| `PHIPIA PERF file` | 64 KiB sequential Data write and read in 4 KiB requests through native file handles. |
| `PHIPIA PERF context-switch` | Real scheduler transition sections. `without_fpu_cycles` includes saved GPR context, address-space activation/restoration, and FS-base work; `with_fpu_cycles` adds the eager aligned `FXSAVE64` or `FXRSTOR64` section. Both are per-transition TSC means and exclude application work. |
| `PHIPIA PERF canvas` | Bounded xRGB8888 damage submissions from two concurrent native Canvas processes. The app reports brush-damage sample count, largest presented region, total, and mean monotonic nanoseconds. |
| `PHIPIA PERF lua` | Native entry-probe time through upstream Lua state creation and standard-library initialization. A link wrapper observes `luaL_openlibs` without changing upstream sources. |
| `PHIPIA PERF sqlite` | The committed insert transaction in phase one, then database reopen, query, integrity check, and close after a clean reboot. |

The following reference sample came from GitHub's Ubuntu QEMU TCG run at
merge-test commit `53466d34c4480f79d7c9ebff08299dfbfa05e1c2`, which merged
project commit `02806d031aaac10d96b66bb642661746d7c3bf45` into `main` for the
test. These numbers are an observation, not acceptance thresholds:

| Boundary | Recorded value |
| --- | ---: |
| Syscall round-trip | 264,784 ns mean over 1,024 ABI-version queries |
| Small surface present | 2,061,181 ns and 2,023,459 ns mean for the two 70×14 presenters |
| Sequential file write | 204,404,924 ns for 64 KiB in 4 KiB requests |
| Sequential file read | 56,946,321 ns for 64 KiB in 4 KiB requests |
| Context switch without FPU section | 11,582 TSC cycles mean |
| Context switch with FPU section | 15,666 TSC cycles mean |
| Lua startup | 88,488,699 ns |
| SQLite transaction | 106,158,650 ns |
| SQLite reboot/reopen/query | 108,049,716 ns |

Canvas's current interactive brush marker additionally caps and reports the
largest application-submitted rectangle; CI rejects zero samples or any sample
that degenerates into a whole-surface redraw. Exact timing varies substantially
with QEMU version, host load, and accelerator, so the workflow enforces the
shape of the work and boundedness rather than marketing a fixed throughput.

`make qemu-port-tests` requires every marker to be present and nonzero. The
same run rejects missing partial-damage activity, absent guest output files,
resource leaks, or an invalid reboot result. File and syscall diagnostics use
public ABI calls from Ring 3; the scheduler comparison is recorded inside the
security boundary because userspace cannot read kernel transition sections.

The Canvas proof also retains a PNG and short MP4 captured directly with QEMU
`screendump` while both native windows are alive. No host-rendered substitute
or camera path participates in that evidence.
