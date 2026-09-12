# Phipia SDL 2 port boundary

The `include/` and `src/` trees originate from the pinned SDL 2.32.10 commit
recorded in `UPSTREAM-COMMIT.txt`. Phipia adds a platform configuration and
small backends below the upstream platform registries. Those additions remain
under SDL's zlib license and are visibly marked as Phipia changes.
The source also carries a semantics-preserving `sizeof` cast in `SDL_guid.c`
and scans `strtoull` values through an upstream-width temporary before
assigning SDL's fixed-width `Uint64`. These keep the pinned release valid under
Phipia's LP64 freestanding headers and `-Werror` policy without changing the
public ABI or parsing behavior.
The vendored upstream translation units retain `-Wall -Wextra -Werror` with
only sign-comparison and platform-unused-parameter warnings disabled; Phipia
applications add the repository's full pedantic warning profile.

The supported first profile is deliberately narrow:

- x86_64, statically linked, freestanding SDL 2.0 public ABI;
- Phipia software windows with direct xRGB8888 framebuffer surfaces and
  bounded partial-damage submission;
- Phipia keyboard, pointer, focus, close, wait, and wake events;
- native PCM output at 48 kHz, stereo, signed 16-bit samples;
- Phipia pthread/futex threads, mutexes, condition variables, semaphores, and
  compiler TLS;
- monotonic timers and `Data:` preference storage;
- SDL's software renderer and public event/audio APIs.

Every platform operation goes through installed SDK headers. The port does not
include or reach into kernel-private headers. Dynamic loading, HIDAPI, haptics,
joysticks, sensors, capture audio, OpenGL/Vulkan, and hardware render drivers
are disabled. SDL thread detach is implemented as a conservative join because
the current native ABI has no detached-thread resource-reclamation contract.

Vendoring and compiling SDL do not by themselves prove an application works.
That claim requires a Ring 3 QEMU scenario which creates a window, presents
pixels, receives injected input, submits non-silent PCM, writes persistent
state, exits, and leaves a clean handle/allocation/process census.
The retained WAV proof identifies every PCM frame in the non-repeating fixture,
requires ordered minimum coverage for all four callbacks across both launches,
and tolerates only silent backend gaps that fragment an otherwise forward-moving
callback stream.
