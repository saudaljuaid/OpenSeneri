/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_CLOCK_H
#define PHIPIA_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

/* Monotonic time since boot, independent of the underlying counter. */

enum clock_source {
    CLOCK_SOURCE_NONE = 0,
    CLOCK_SOURCE_TSC
};

enum clock_status {
    CLOCK_STATUS_OK = 0,
    CLOCK_STATUS_NOT_STARTED,
    CLOCK_STATUS_ALREADY_STARTED,
    CLOCK_STATUS_NO_SOURCE
};

struct clock_state {
    enum clock_source source;
    uint64_t origin;
    uint64_t last_reported_ns;
    uint64_t backward_steps;
    bool started;
};

/*
 * Fix the origin of the clock. Requires a time-stamp counter calibrated against
 * the independent ACPI power management timer. The TSC is used after calibration
 * because it provides a cheap monotonic read without an I/O transaction.
 */
enum clock_status clock_start(void);

/*
 * Nanoseconds since clock_start. Returns zero before it, which is the same value
 * the instant after it, because there is no honest duration to report either way.
 */
uint64_t clock_monotonic_ns(void);

struct clock_state clock_get_state(void);
bool clock_is_started(void);
const char *clock_source_string(enum clock_source source);
bool clock_self_test(void);
const char *clock_status_string(enum clock_status status);

#endif
