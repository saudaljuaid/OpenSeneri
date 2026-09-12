/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PM_TIMER_H
#define PHIPIA_PM_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include <phipia/acpi.h>

/*
 * ACPI 6.6 section 4.8.3.3 fixes the power management timer at exactly
 * 3.579545 MHz, one third of the 10.7386 MHz colour burst frequency the
 * original PC clock chain was built from. Phipia does not measure this rate
 * against anything, which is the entire reason the timer is worth having: it is
 * the first time reference here whose frequency is stated rather than derived.
 */
#define PM_TIMER_FREQUENCY_HZ UINT32_C(3579545)

enum pm_timer_status {
    PM_TIMER_STATUS_OK = 0,
    PM_TIMER_STATUS_NULL_ARGUMENT,
    PM_TIMER_STATUS_ABSENT,
    PM_TIMER_STATUS_ALREADY_INITIALIZED,
    PM_TIMER_STATUS_BAD_PORT,
    PM_TIMER_STATUS_BAD_COUNTER_WIDTH,
    PM_TIMER_STATUS_BAD_INTERVAL,
    PM_TIMER_STATUS_NOT_COUNTING,
    PM_TIMER_STATUS_STALLED
};

struct pm_timer_state {
    uint16_t port;
    uint8_t counter_bits;
    bool extended_address;
    bool present;
};

enum pm_timer_status pm_timer_initialize(const struct acpi_fadt *fadt);
struct pm_timer_state pm_timer_get_state(void);
bool pm_timer_is_present(void);

/*
 * Sample the counter. Returns zero when no timer has been discovered, which is
 * indistinguishable from a genuine zero reading, so callers check presence
 * first rather than reading a sentinel out of the value.
 */
uint32_t pm_timer_read(void);

/*
 * Convert two samples into the ticks between them, folding the single wrap the
 * counter may have made. Refuses a zero span, which is ambiguous: the counter
 * either did not advance or advanced exactly one full period.
 */
enum pm_timer_status pm_timer_span(
    uint32_t start,
    uint32_t end,
    uint32_t *ticks
);

/*
 * Spin until the counter has advanced by the requested ticks, then report what
 * it actually advanced by. The spin is bounded: a counter that stops answering
 * returns a status instead of hanging the kernel.
 */
enum pm_timer_status pm_timer_wait(uint32_t ticks, uint64_t *elapsed);

uint64_t pm_timer_ticks_to_nanoseconds(uint64_t ticks);
uint64_t pm_timer_nanoseconds_to_ticks(uint64_t nanoseconds);

/*
 * Phipia agreement policy, not an ACPI rule. Two measurements of one interval
 * agree when they differ by at most one part in the tolerance divisor of the
 * reference.
 *
 * A quarter is the working tolerance, and it is what clocks driven from the
 * same source are held to. It fails a rate wrong by a factor of two, which
 * shows up as a 50% disagreement, with twice the margin needed.
 *
 * Comparisons against the time-stamp counter use a half. Under emulation the
 * PIT and local APIC timer share the host's monotonic clock, while the TSC uses
 * a separate host counter. A scheduling stall during calibration can therefore
 * skew the measured TSC rate.
 *
 * A half therefore fails only errors beyond a factor of two. That is deliberate
 * and costs nothing: the factor-of-two case is caught by the local APIC timer
 * comparison, which is held to the quarter and does not cross clock domains.
 */
#define PM_TIMER_TOLERANCE_QUARTER UINT64_C(4)
#define PM_TIMER_TOLERANCE_HALF UINT64_C(2)

bool pm_timer_durations_agree(
    uint64_t measured,
    uint64_t reference,
    uint64_t tolerance_divisor
);

bool pm_timer_self_test(void);
const char *pm_timer_status_string(enum pm_timer_status status);

#endif
