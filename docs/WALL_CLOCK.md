<!-- SPDX-License-Identifier: GPL-3.0-only -->

# UTC wall clock

Phipia exposes two clocks with different contracts:

- `clock_monotonic_ns()` is elapsed nanoseconds since boot and remains the only
  source for deadlines, sleeps, cancellation, retry budgets, and animation.
- `wall_clock_read_utc()` reads civil UTC from the PC-compatible CMOS/RTC, and
  `wall_clock_read_unix_seconds()` converts it to Unix epoch seconds for native
  `CLOCK_REALTIME`, `time()`, and certificate-validity checks.

The RTC reader waits for the update-in-progress bit to clear, reads all fields,
and accepts the value only after two complete samples agree. Both the UIP wait
and the number of sample attempts are bounded. Maskable interrupts are held
around the bounded double-sample operation so another interrupt handler cannot
change the shared index port; the RTC selector never disables NMI delivery.

Register B selects binary versus packed BCD and 24-hour versus 12-hour data.
The decoder validates BCD nibbles, applies the PM bit including the 12 AM/PM
edge cases, combines the year with the PC century byte at register `0x32`, and
accepts only valid UTC dates from 1970 through 9999. Month length follows the
Gregorian leap-year rules, including 2000 as a leap year and 2100 as a common
year. Seconds range from 0 through 59; leap-second notation is not accepted by
this RTC profile. A clock left frozen through Register B's update-inhibit bit is
also rejected.

The additive native call `0x0307 TIME_REALTIME()` requires `PHIPIA_CAP_TIME` and
returns whole Unix seconds. RTC instability, an update that never completes,
or invalid/out-of-range data fails with `-EIO`; no guessed time is returned.
`CLOCK_REALTIME` therefore currently has one-second resolution. There is no
timezone database, so `localtime[_r]` intentionally has UTC semantics.

QEMU proofs that depend on certificate validity must inject time explicitly,
for example with a fixed `-rtc base=...,clock=vm`. They must never inherit the
host wall clock. Fixed RTC time makes valid, expired, and not-yet-valid
certificate fixtures reproducible while monotonic network deadlines continue
to advance independently.

`wall_clock_self_test()` covers BCD/binary modes, 12-hour conversion, century
assembly, Gregorian leap boundaries, invalid digits/ranges, and known epoch
values. The standalone host tests additionally model stable, rolling,
permanently unstable, and stuck-UIP CMOS devices:

```sh
gcc -std=c11 -O2 -Iinclude tools/wall-clock-host-test.c \
    src/kernel/wall_clock.c -o /tmp/wall-clock-host-test
/tmp/wall-clock-host-test

gcc -std=c11 -O2 -Isdk/include -Iinclude tools/sdk-time-host-test.c \
    sdk/src/time.c -o /tmp/sdk-time-host-test
/tmp/sdk-time-host-test
```

The RTC supplies current time, not anti-rollback storage. TLS policy must still
decide how to treat a firmware clock moved backwards, and trust-store/update
work remains separate from this foundation.
