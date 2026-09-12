<!-- SPDX-License-Identifier: GPL-3.0-only -->

# SDL 2 on Phipia

Phipia carries the official SDL 2.32.10 source and a first-class `__PHIPIA__`
platform backend. It is built reproducibly into the SDK as `libSDL2.a`; public
headers are installed under `include/SDL2`. Applications include `SDL.h` with
that directory on their include path and explicitly link the static library.

The backend is not a Linux compatibility shim and does not use SDL's dummy
video or audio drivers. Phipia owns the window surface and receives bounded
damage rectangles through the public native window ABI. Input becomes normal
SDL keyboard, text, pointer, focus, and quit events. Event waits use Phipia's
public multi-handle wait call, while a native timer handle implements SDL's
wakeup hook without polling.

Audio opens Phipia's public PCM service and submits ordinary SDL mixed buffers.
The accepted profile is 48 kHz, stereo S16 with a bounded 1,024-frame period.
Capture audio is not supported. Threads use SDK pthreads plus the public futex
syscalls for SDL synchronization. The monotonic clock backs ticks, performance
counters, delays, and wait deadlines.

`SDL_GetBasePath()` returns `System:`. `SDL_GetPrefPath()` creates an 8.3-safe,
deterministic per-application directory below `Data:SDL`; organization and app
strings are hashed rather than copied into a path. This prevents traversal and
keeps the interface usable on the current FAT data volume. The hash is a
namespace mapping, not an authentication primitive.

## Deliberate exclusions

The initial port disables dynamic object loading, HIDAPI, haptics, joysticks,
sensors, capture audio, OpenGL, Vulkan, and hardware render drivers. Thread
detach blocks until join because Phipia ABI v1 cannot reclaim a detached native
thread safely. These are explicit compatibility gaps, not silent dummy-driver
fallbacks.

The `native-sdl` QEMU proof exercises window creation and partial presentation,
injected keyboard and pointer input, non-silent PCM, synchronized persistent
state across a second process launch, clean exit, and resource census from
Ring 3. It retains the guest serial log, screenshot, video, data volume, and
WAV capture when QEMU exposes its WAV backend.

The signed-package lifecycle separately carries SDL's byte-exact upstream
`testdrawchessboard.c` application from the pinned 2.32.10 release. A small
Phipia harness runs its original event and software-render loop for eight
bounded frames, writes an exact receipt through `SDL_GetPrefPath()` and
`SDL_RWops`, synchronizes Data, and exits. The application is downloaded over
HTTPS, installed and updated as an authenticated package on journaled ext4,
launched from the authority-selected generation after reboot, and checked for
a clean process/window/file census and clean `e2fsck` result.

The Phipia event pump treats the ABI's empty-queue `-EAGAIN` as normal after it
has drained all pending events. The proof moves the deterministic initial PS/2
cursor into the SDL client before clicking, so both keyboard and pointer paths
are observed rather than relying on an ambient host cursor position. Its WAV
profile uses a non-repeating, frame-identifiable stereo sequence. It requires
at least 256 exact, forward-moving frames from each of the four 1,024-frame SDL
callbacks in both process launches, in callback order, against complete fixture
hash `0a10d573e70eacd28cc4a9297713d5f6a916a9bbe0c60d64a3d1db96839f5d55`.
QEMU WAV silence may split one callback into multiple delivery fragments; the
matcher ignores only those silent gaps and still rejects unknown, repeated,
backward, corrupted, reordered, or single-launch PCM.
