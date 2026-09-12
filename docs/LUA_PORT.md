<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Lua 5.4.7 port

The port compiles the unmodified official Lua 5.4.7 source archive against the
Phipia C SDK with `LUA_USE_C89` and without a jump table. It is packaged as
`LUA.APP`; there is no Lua-specific loader rule, digest profile, or kernel
syscall.

A project-owned link wrapper measures the interval from the native entry probe
through Lua state creation and standard-library initialization. It does not
alter the pinned upstream source.

The manifest starts Lua with `SCRIPT.LUA` inside the `LUA` Data namespace. The
QEMU fixture injects real PS/2 keyboard text after Lua blocks on native stdin.
The script allocates through the SDK heap, reads the input, calculates and
checks a sum and math result, writes `LUA/RESULT.TXT`, and exits. The kernel
reopens the exact guest-written file and verifies its bytes before accepting a
clean resource census.

Build with `make build/ports/lua/LUA.SPK` or as part of `make native-apps`.
Run the installed proof with `make qemu-test-native-lua`.
