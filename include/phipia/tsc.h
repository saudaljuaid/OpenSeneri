/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_TSC_H
#define PHIPIA_TSC_H

#include <stdbool.h>
#include <stdint.h>

enum tsc_status {
    TSC_STATUS_OK = 0,
    TSC_STATUS_UNSUPPORTED,
    TSC_STATUS_INTERRUPTS_ENABLED,
    TSC_STATUS_ALREADY_CALIBRATED,
    TSC_STATUS_NOT_CALIBRATED,
    TSC_STATUS_REFERENCE_FAILURE,
    TSC_STATUS_NOT_COUNTING,
    TSC_STATUS_WENT_BACKWARDS,
    TSC_STATUS_RATE_OUT_OF_RANGE
};

struct tsc_state {
    uint64_t frequency_hz;
    bool present;
    bool invariant;
    bool calibrated;
};

enum tsc_status tsc_calibrate(void);
struct tsc_state tsc_get_state(void);
uint64_t tsc_read(void);
uint64_t tsc_frequency(void);
bool tsc_is_calibrated(void);

/*
 * Convert a counter span into nanoseconds. Returns zero when the counter is
 * uncalibrated or the span runs backwards, because there is no honest duration
 * to report in either case.
 */
uint64_t tsc_span_nanoseconds(uint64_t start, uint64_t end);

bool tsc_self_test(void);
const char *tsc_status_string(enum tsc_status status);

#endif
