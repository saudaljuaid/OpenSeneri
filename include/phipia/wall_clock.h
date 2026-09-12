/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_WALL_CLOCK_H
#define PHIPIA_WALL_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define WALL_CLOCK_MIN_YEAR UINT16_C(1970)
#define WALL_CLOCK_MAX_YEAR UINT16_C(9999)

/*
 * A validated UTC civil time read from the platform real-time clock. This is
 * deliberately separate from clock_monotonic_ns(): UTC answers "when", while
 * the monotonic clock is the only clock permitted to drive deadlines.
 */
struct wall_clock_utc {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday;
};

enum wall_clock_status {
    WALL_CLOCK_STATUS_OK = 0,
    WALL_CLOCK_STATUS_NULL_ARGUMENT,
    WALL_CLOCK_STATUS_UPDATE_TIMEOUT,
    WALL_CLOCK_STATUS_UNSTABLE,
    WALL_CLOCK_STATUS_INVALID_DATA,
    WALL_CLOCK_STATUS_RANGE
};

/* Read one coherent, validated UTC value from the CMOS/RTC. */
enum wall_clock_status wall_clock_read_utc(struct wall_clock_utc *result);

/* Convert a validated UTC value to non-negative Unix epoch seconds. */
enum wall_clock_status wall_clock_utc_to_unix(
    const struct wall_clock_utc *utc,
    int64_t *seconds
);

/* The native syscall uses this bounded read-and-convert operation. */
enum wall_clock_status wall_clock_read_unix_seconds(int64_t *seconds);

bool wall_clock_self_test(void);
const char *wall_clock_status_string(enum wall_clock_status status);

#endif
