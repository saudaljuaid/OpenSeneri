<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Offline C SDK and porting guide

`make sdk` creates `build/sdk` without downloading anything. It contains
public ABI and runtime headers, `crt0.o`, `libphipia.a`, the static linker
script, and `bin/phipia-cc`. `make reproducible-sdk` builds the complete tree
twice in clean directories and compares all files byte-for-byte.

The reference compiler is Clang targeting `x86_64-unknown-none-elf`; linking
uses LLD and `sdk/linker.ld`. Applications are freestanding, static, large code
model, no red zone, local-exec TLS, no stack protector, and no builtin libc.
The linker fixes the native image arena, separates RX/R/RW loads, emits a
non-executable stack, and refuses orphan sections, unresolved relocations, and
W+X layout.

To add an application:

1. Depend only on headers listed in `LIBC_COVERAGE.md`, or add a tested SDK
   implementation before the port.
2. Compile every source with `$(SDK_CC) $(SDK_CFLAGS)` and link `crt0.o`, the
   objects, and `libphipia.a` using `$(SDK_LDFLAGS)`.
3. Add a version 1 manifest with the minimum capabilities and realistic memory,
   handle, and thread limits.
4. Build and inspect the package with `tools/phipia-package.py`.
5. Add the package to a System image and put mutable fixtures under its Data
   namespace.
6. Run `make port-tests` and a Ring 3 QEMU scenario that proves output and the
   final resource census.

The runtime supplies startup/exit, negative-error translation to `errno`, a
page-backed heap, buffered `FILE`, FAT file/directory wrappers, time, entropy,
pthread/TLS/futex support, Phipia surfaces/input, and typed networking
wrappers. `<phipia/audio.h>` exposes the fixed 48 kHz S16LE stereo output
handle, whole-chunk submission, Q15 volume, drain, cancellation and close.
Familiar names are a source-porting layer; the kernel remains the Phipia ABI
rather than a Linux personality.

The installed SDK also provides zlib 1.3.2 as `libz.a` and installs its public
`zlib.h`/`zconf.h`. Phipia builds the upstream `Z_SOLO` profile: checksums and
streaming deflate/inflate are available, while `gz*`, `compress*`, and
`uncompress*` are not. Include `<phipia/zlib.h>` and call
`phipia_zlib_stream_prepare()` before `deflateInit*` or `inflateInit*` to use
the checked SDK allocation adapter. Every successful initialization still
requires its matching `deflateEnd` or `inflateEnd` call. `phipia-cc` supplies
the required `Z_SOLO` and fixed-width configuration definitions to every
consumer so headers cannot advertise omitted hosted APIs.

Time APIs keep elapsed and civil time separate. `clock()` and
`CLOCK_MONOTONIC` use boot-relative monotonic nanoseconds; `time()` and
`CLOCK_REALTIME` use validated RTC UTC with one-second resolution. `gmtime_r`
and `localtime_r` support Unix seconds from 1970 through 9999, with local time
defined as UTC until a timezone database exists. See
[`WALL_CLOCK.md`](WALL_CLOCK.md).

`make native-apps` builds the C proof, Lua, SQLite, Canvas, the native network
client, and the Rust proof entirely from checked-in inputs. The Lua and SQLite
scripts extract their pinned archives into disposable build directories and do
not modify the upstream source archives.
