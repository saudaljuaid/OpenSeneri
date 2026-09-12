<!-- SPDX-License-Identifier: GPL-3.0-only -->

# High Definition Audio

Phipia has a bounded PCM path for the QEMU ICH9 controller and
`hda-duplex` codec. The boot proof remains a one-shot kernel-owned stream. The
native ABI now reuses the same controller route through a persistent bounded
owner; applications never see its DMA pages or registers.

The implementation follows the Intel High Definition Audio 1.0a controller
contract and derives its codec profile from QEMU's published `hda-codec.c` and
`hda-codec-common.h` descriptions. It does not select node numbers by identity.
It walks the audio function group's bounded widget range, finds a stereo audio
output converter that advertises 48 kHz and 16-bit PCM, finds an output-capable
pin, and proves that the pin's direct connection list names that converter. The
current QEMU codec exposes function group 1, DAC 2, and output pin 3; those
numbers are evidence recorded after discovery, not configuration constants.

## Fixed PCM profile

The only accepted profile is:

- signed little-endian 16-bit PCM;
- two interleaved channels;
- 48,000 frames per second;
- stream format `0x0011`;
- stream tag 1, starting at channel 0;
- 1,024 frames / 4,096 bytes of payload followed by one zero-filled 4,096-byte
  drain-guard period;
- two 4,096-byte BDL periods; the proof marks both for status and native
  playback marks the payload period so the guard absorbs bounded stop latency
  without replaying the payload;
- a deterministic 750 Hz square wave at amplitude +/-8192, identical on both
  channels.

The conservative format is inside QEMU's advertised 16-96 kHz signed-16-bit
profile. The waveform is generated with integer operations; kernel code does
not use floating point, MMX, or SIMD.

The codec route is explicitly put in D0, its pin is set to output, and the DAC
is assigned the same tag and format as the controller stream. Read-back verbs
must return the selected pin control, converter tag, and converter format before
DMA starts.

## DMA ownership and playback evidence

Four typed below-4-GiB DMA allocations exist during the proof:

1. CORB command ring;
2. RIRB response ring;
3. BDL page;
4. immutable two-page PCM payload-and-guard allocation.

All four are initialized while CPU-owned and named in one bus-master request.
The first enable attempt, before ownership transfer, must fail with
`DMA_NOT_PREPARED`. Only after all four allocations are device-owned may PCI bus
mastering be enabled. The BDL and PCM allocation are never modified while the
device owns them.

The first output stream descriptor is located after all input descriptors, as
required by GCAP. Phipia stops and resets it, programs BDL base, cyclic buffer
length, LVI 1, format and tag, then sets RUN. The proof accepts playback only
after both forms of independent controller evidence appear within one second:

- LPIB changes to another in-range byte position;
- a BDL buffer-completion status arrives and is acknowledged.

The existing QEMU scenario uses `-audiodev none`. Therefore this proves that the
emulated codec consumed the programmed PCM stream, not that a host speaker or
WAV backend emitted audible sound. The deterministic PCM hash proves what bytes
were made available to the device; it is not an acoustic-capture claim.

## Native ABI, queueing and mixing

An admitted application must request the distinct `audio` capability. It can
open at most two generation-protected `PHIPIA_HANDLE_AUDIO_OUTPUT` objects. One
process owns the controller at a time; another process receives `EBUSY`, not an
implicitly shared global device.

The public format is exactly the fixed profile above. `phipia_audio_submit()`
accepts one complete 4,096-byte chunk. Lengths, request versions, flags, handle
types, generations, and every input page are validated before the kernel copies
the chunk into its own bounded queue. Submitting while that handle already owns
a queued or active chunk returns `EBUSY`. There is no per-sample syscall and no
userspace DMA.

With two handles open, the scheduler gives the first queued chunk one guaranteed
return to its owner plus a ten-millisecond coalescing window. The service grace
is independent of host pauses, so two consecutive public submit calls cannot be
split merely because slow TCG advanced the monotonic deadline while the first
syscall returned through the scheduler. If both handles queue during that
bounded opportunity, an integer-only two-input mixer applies an independent
unsigned Q15 left/right gain to each stream, adds in signed 32-bit space, and
saturates to signed 16-bit output. Unity is 32768 and zero is silent. A lone
queued handle starts after the grace/window expires; a single open handle starts
without the two-handle coalescing delay. At most two 4,096-byte source chunks and
one 8,192-byte DMA payload-and-guard mix exist.

`PHIPIA_WAIT_WRITABLE` means the handle can accept another chunk;
`PHIPIA_WAIT_CLOSED` reports cancellation or a stream error. Drain blocks only
the calling native thread and has an absolute monotonic deadline. Cancel removes
a chunk that has not entered DMA. Once a mixed chunk is device-owned it is
atomic: cancel marks that handle canceled when the current 21.3 ms chunk
finishes rather than rewriting memory under DMA. Close follows the same rule.
Process death is stronger: because both streams must belong to that one process,
it synchronously stops/resets the descriptor, clears both queues, and performs
the full controller teardown.

## Bounded service and underrun recovery

The boot proof polls synchronously and is bounded by both one second and
1,000,000 service steps. Native playback is scheduler-polled at bounded
deadlines of at most 100 microseconds while a chunk is active. No stream or
global HDA interrupt is enabled.

A buffer completion is acknowledged and counted. A FIFO error is acknowledged,
then the descriptor is stopped, reset, completely reprogrammed and restarted.
At most three such recoveries are allowed. Native recovery replays the current
whole chunk from its beginning; it never exposes a partially consumed source
queue. A descriptor error, an out-of-range LPIB in the boot proof, a fourth
FIFO error, or failure of any stop/reset/start handshake fails the operation.
Pure controls independently exercise completion/underrun/fatal status
classification; QEMU is not required to manufacture an underrun in a healthy
run.

## Teardown and lock model

`audio_active` is the one-controller lock. `audio_prove()` is synchronous and
one-shot. Native playback holds that same exclusive claim between first open
and final close, and the scheduler services it only in kernel address space
with maskable interrupts disabled. Source queues are kernel BSS, the mixed PCM
page changes ownership only while the output descriptor is stopped/reset, and
no callback or user mapping can touch device-owned memory.

Teardown has one allowed order:

```text
clear output-stream RUN and observe it clear
reset the output stream and observe reset set, then clear
stop RIRB and CORB
put the whole controller in reset and observe it
withdraw PCI bus mastering
transfer PCM, BDL, RIRB and CORB ownership back to the CPU
release all four DMA allocations
unmap BAR and release the PCI claim
```

Any partial bring-up follows the same release function. The scenario requires
the frame, paging, DMA, PCI, vector, MSI-X and interrupt-state census to equal
the pre-proof census, with no claim, allocation, mapping or bus master left.

## Supported and refused

Supported now:

- one QEMU ICH9 HDA controller and one discovered fixed direct output route;
- one hardware 48 kHz / S16LE / stereo playback stream;
- two process-local logical output handles for one owning process, one queued
  chunk per handle, Q15 stereo volume, bounded saturated software mixing;
- native submit, wait readiness, drain, cancel, close, and process-death cleanup;
- topology/format/read-back validation, polled completion service, bounded FIFO
  recovery, and complete resource teardown.

Still explicitly refused:

- more than two logical streams, more than one queued chunk per handle, a
  system-wide mixer, or cross-process sharing;
- resampling, format conversion, master volume, input capture, hotplug or power
  management;
- codec connection ranges, selectable multi-input pins, unsolicited responses,
  and physical-codec compatibility claims;
- interrupt-driven persistent playback, host WAV/acoustic validation, and
  production daily-driver audio.

The existing `-audiodev none` scenario still proves DMA consumption only. It
does not prove loudness, speaker selection, or an audible host artifact.

## Public-ABI and WAV evidence

`apps/native-audio` is the Ring 3 proof for the public surface. Its admitted
manifest requests only `console`, `time`, and `audio`; a second manifest for
the same executable omits `audio` and must observe `EACCES`
without changing the PCI or DMA census. The admitted process uses only SDK
headers and wrappers to prove the two-handle limit, immediate and completion
readiness, malformed-length refusal, per-channel volume, deterministic
two-stream mixing, drain, queued cancellation, terminal readiness, explicit
close, stale-handle refusal, and process-exit cleanup.

`make native-audio-proof` builds both packages, their isolated System/Data
images, and the host WAV controls. `make qemu-test-native-audio` attaches the
same QEMU ICH9/`hda-duplex` model as the kernel proof. If that QEMU build
advertises its `wav` audio driver, the scenario writes
`build/tests/native-audio/native-audio.wav` with explicit 48 kHz, stereo,
signed-16 host output settings and independently verifies:

- 48,000 Hz, signed 16-bit, two-channel uncompressed PCM;
- at least 256 persisted frames, a bounded frame count, and reported duration;
- either the exact 1,024-frame Q15 mixed waveform or an exact, frame-aligned
  prefix of it, anchored to the complete fixture SHA-256
  `5864c13557496ba86294adbbfe8078e9f2c0b5e808e4d0c4f49738fd465d1261`;
- non-silence and absence of the chunk canceled before DMA ownership.

QEMU's HDA codec stages guest DMA in front of the WAV backend. The guest stops
the controller only after both complete 1,024-frame drain calls, but the host
WAV timer can persist a shorter prefix before stream deactivation discards its
private staging tail. The verifier therefore authenticates either the whole
chunk or a bounded exact prefix, reports the matched-frame count and digest,
and has negative controls for truncation below 256 frames and prefix
corruption. This does not replace or relax either guest drain assertion.

When the runner does not expose the WAV backend, the scenario uses the null
backend and prints an explicit WAV skip while retaining all serial, refusal,
DMA-consumption, and resource-census assertions. A matching WAV is digital
emulator-backend evidence, not microphone, loudspeaker, or perceptual acoustic
proof.
