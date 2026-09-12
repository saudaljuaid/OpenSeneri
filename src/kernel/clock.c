/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/clock.h>
#include <phipia/tsc.h>

static struct clock_state state;

/*
 * Monotonicity is this subsystem's whole contract, so a reading below the last
 * one is clamped rather than reported. It is also counted. A clock that silently
 * repaired itself would hide a counter that has stopped behaving, and the count
 * is exposed through clock_get_state so the repair is observable rather than
 * invisible. On the supported target the time-stamp counter is not invariant, so
 * this is a real possibility across a power transition rather than a formality.
 */
static uint64_t monotonic_step(
    uint64_t candidate,
    uint64_t last,
    uint64_t *backward_steps
)
{
    if (candidate < last) {
        ++*backward_steps;
        return last;
    }

    return candidate;
}

enum clock_status clock_start(void)
{
    if (state.started) {
        return CLOCK_STATUS_ALREADY_STARTED;
    }

    /*
     * The time-stamp counter is the source rather than the ACPI power management
     * timer, even though the ACPI timer is what fixes the rate. The ACPI counter
     * is 24 bits on the supported target and wraps every 4.687 seconds, so using
     * it directly would oblige every caller to sample often enough to catch each
     * wrap. The TSC is 64 bits and cannot wrap in any span this kernel will see,
     * and its rate is derived from the ACPI timer, so nothing is given up.
     */
    if (!tsc_is_calibrated()) {
        return CLOCK_STATUS_NO_SOURCE;
    }

    state.source = CLOCK_SOURCE_TSC;
    state.origin = tsc_read();
    state.last_reported_ns = 0U;
    state.backward_steps = 0U;
    state.started = true;
    return CLOCK_STATUS_OK;
}

uint64_t clock_monotonic_ns(void)
{
    uint64_t candidate;

    if (!state.started) {
        return 0U;
    }

    candidate = tsc_span_nanoseconds(state.origin, tsc_read());
    state.last_reported_ns = monotonic_step(
        candidate,
        state.last_reported_ns,
        &state.backward_steps
    );

    return state.last_reported_ns;
}

struct clock_state clock_get_state(void)
{
    return state;
}

bool clock_is_started(void)
{
    return state.started;
}

const char *clock_source_string(enum clock_source source)
{
    switch (source) {
    case CLOCK_SOURCE_NONE:
        return "none";
    case CLOCK_SOURCE_TSC:
        return "time-stamp counter";
    default:
        return "unknown clock source";
    }
}

/*
 * The monotonic guarantee is provable without hardware, and it is worth proving:
 * a clock that steps backwards once would let a deadline that has not arrived
 * look as though it has, and every sleep built on it would return early.
 *
 * This runs before clock_start, so it also pins down what the clock reports
 * before it has an origin.
 */
bool clock_self_test(void)
{
    uint64_t backward = 0U;

    if (state.started) {
        return false;
    }

    /* An unstarted clock reports no elapsed time and refuses to be read into. */
    if (clock_is_started() || clock_monotonic_ns() != 0U ||
        clock_get_state().source != CLOCK_SOURCE_NONE) {
        return false;
    }

    /* A rising counter passes through untouched and counts no repair. */
    if (monotonic_step(UINT64_C(1000), UINT64_C(900), &backward) !=
            UINT64_C(1000) ||
        backward != 0U) {
        return false;
    }

    /* An equal reading is not backwards. */
    if (monotonic_step(UINT64_C(1000), UINT64_C(1000), &backward) !=
            UINT64_C(1000) ||
        backward != 0U) {
        return false;
    }

    /* A reading below the last is clamped, and the repair is counted. */
    if (monotonic_step(UINT64_C(900), UINT64_C(1000), &backward) !=
            UINT64_C(1000) ||
        backward != 1U) {
        return false;
    }

    /* Every further step backwards is counted separately, not collapsed. */
    if (monotonic_step(0U, UINT64_C(1000), &backward) != UINT64_C(1000) ||
        backward != 2U) {
        return false;
    }

    /* Zero is not a special case in either position. */
    return monotonic_step(0U, 0U, &backward) == 0U && backward == 2U;
}

const char *clock_status_string(enum clock_status status)
{
    switch (status) {
    case CLOCK_STATUS_OK:
        return "ok";
    case CLOCK_STATUS_NOT_STARTED:
        return "monotonic clock has no origin yet";
    case CLOCK_STATUS_ALREADY_STARTED:
        return "monotonic clock was started twice";
    case CLOCK_STATUS_NO_SOURCE:
        return "monotonic clock has no calibrated counter to run on";
    default:
        return "unknown monotonic clock status";
    }
}
