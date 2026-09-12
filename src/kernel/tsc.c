/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cpu.h>
#include <phipia/pm_timer.h>
#include <phipia/tsc.h>

/* Intel SDM volume 3B section 18.17: CPUID.01H:EDX[4] reports a TSC. */
#define CPUID_FEATURE_LEAF UINT32_C(1)
#define CPUID_FEATURE_TSC UINT32_C(0x00000010)

/*
 * Intel SDM volume 3B section 18.17.1 and AMD APM volume 3: the invariant TSC
 * bit lives in CPUID.80000007H:EDX[8], and leaf 0x80000000 reports how far the
 * extended leaves go, so it must be asked first.
 */
#define CPUID_EXTENDED_ROOT UINT32_C(0x80000000)
#define CPUID_EXTENDED_POWER UINT32_C(0x80000007)
#define CPUID_INVARIANT_TSC UINT32_C(0x00000100)

/*
 * The reference interval: a tenth of a second of the ACPI power management
 * timer, whose rate the ACPI specification fixes rather than one this kernel
 * measured. The PIT is not used because its earlier mode-3 edge count was not
 * an independent rate reference.
 */
#define REFERENCE_PM_TICKS (PM_TIMER_FREQUENCY_HZ / 10U)

#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/*
 * The nanosecond conversion multiplies a sub-second remainder by one billion,
 * so a rate above this bound would overflow it. No real processor approaches
 * eighteen gigahertz, and a calibration that reports one has measured noise.
 */
#define TSC_MAX_FREQUENCY UINT64_C(18000000000)

static struct tsc_state state;

static void detect_features(void)
{
    struct cpuid_result features;
    struct cpuid_result extended;

    cpu_cpuid(CPUID_FEATURE_LEAF, 0U, &features);
    state.present = (features.edx & CPUID_FEATURE_TSC) != 0U;
    state.invariant = false;

    if (!state.present) {
        return;
    }

    cpu_cpuid(CPUID_EXTENDED_ROOT, 0U, &extended);

    if (extended.eax < CPUID_EXTENDED_POWER) {
        return;
    }

    cpu_cpuid(CPUID_EXTENDED_POWER, 0U, &extended);
    state.invariant = (extended.edx & CPUID_INVARIANT_TSC) != 0U;
}

/*
 * Turn one observed span into a rate. Unlike the APIC timer the counter runs
 * up, so an end at or below the start means it never advanced.
 */
static enum tsc_status rate_from_span(
    uint64_t start,
    uint64_t end,
    uint32_t reference_frequency,
    uint64_t reference_ticks,
    uint64_t *rate
)
{
    uint64_t elapsed;

    if (reference_frequency == 0U || reference_ticks == 0U) {
        return TSC_STATUS_NOT_COUNTING;
    }

    if (end < start) {
        return TSC_STATUS_WENT_BACKWARDS;
    }

    if (end == start) {
        return TSC_STATUS_NOT_COUNTING;
    }

    elapsed = end - start;

    if (elapsed > UINT64_MAX / reference_frequency) {
        return TSC_STATUS_RATE_OUT_OF_RANGE;
    }

    *rate = elapsed * reference_frequency / reference_ticks;

    if (*rate == 0U) {
        return TSC_STATUS_NOT_COUNTING;
    }

    if (*rate > TSC_MAX_FREQUENCY) {
        return TSC_STATUS_RATE_OUT_OF_RANGE;
    }

    return TSC_STATUS_OK;
}

/*
 * Split the conversion so neither half overflows: whole seconds scale directly,
 * and the sub-second remainder is always below the rate, which the calibration
 * bound keeps small enough to multiply by one billion.
 */
static uint64_t span_to_nanoseconds(uint64_t elapsed, uint64_t rate)
{
    const uint64_t whole_seconds = elapsed / rate;
    const uint64_t remainder = elapsed % rate;

    return whole_seconds * NANOSECONDS_PER_SECOND +
        remainder * NANOSECONDS_PER_SECOND / rate;
}

uint64_t tsc_read(void)
{
    return cpu_read_tsc();
}

/*
 * Measure the counter against the ACPI power management timer, whose rate is
 * fixed by specification rather than measured. The span is scaled by the ticks
 * the reference actually advanced rather than by the ticks requested, so a
 * bounded wait's overshoot stays out of the rate.
 */
enum tsc_status tsc_calibrate(void)
{
    uint64_t start;
    uint64_t end;
    uint64_t rate = 0U;
    uint64_t reference_ticks = 0U;
    enum tsc_status status;
    enum pm_timer_status pm_status;

    detect_features();

    if (!state.present) {
        return TSC_STATUS_UNSUPPORTED;
    }

    if (cpu_interrupts_enabled()) {
        return TSC_STATUS_INTERRUPTS_ENABLED;
    }

    if (state.calibrated) {
        return TSC_STATUS_ALREADY_CALIBRATED;
    }

    if (!pm_timer_is_present()) {
        return TSC_STATUS_REFERENCE_FAILURE;
    }

    start = cpu_read_tsc();
    pm_status = pm_timer_wait(REFERENCE_PM_TICKS, &reference_ticks);
    end = cpu_read_tsc();

    if (pm_status != PM_TIMER_STATUS_OK) {
        return TSC_STATUS_REFERENCE_FAILURE;
    }

    status = rate_from_span(
        start,
        end,
        PM_TIMER_FREQUENCY_HZ,
        reference_ticks,
        &rate
    );

    if (status != TSC_STATUS_OK) {
        return status;
    }

    state.frequency_hz = rate;
    state.calibrated = true;
    return TSC_STATUS_OK;
}

struct tsc_state tsc_get_state(void)
{
    return state;
}

uint64_t tsc_frequency(void)
{
    return state.frequency_hz;
}

bool tsc_is_calibrated(void)
{
    return state.calibrated;
}

uint64_t tsc_span_nanoseconds(uint64_t start, uint64_t end)
{
    if (!state.calibrated || end < start) {
        return 0U;
    }

    return span_to_nanoseconds(end - start, state.frequency_hz);
}

/*
 * The conversion arithmetic decides what every measured duration means, and a
 * mistake in it is silent: durations come out wrong rather than absent.
 */
bool tsc_self_test(void)
{
    uint64_t rate = 0U;

    /*
     * One full second of the reference is PM_TIMER_FREQUENCY_HZ of its ticks, so
     * a 2 GHz counter advances two billion times across it. Expressing the
     * interval as exactly one second keeps the expected rate exact.
     */
    if (rate_from_span(
            UINT64_C(1000),
            UINT64_C(1000) + UINT64_C(2000000000),
            PM_TIMER_FREQUENCY_HZ,
            PM_TIMER_FREQUENCY_HZ,
            &rate
        ) != TSC_STATUS_OK ||
        rate != UINT64_C(2000000000)) {
        return false;
    }

    /*
     * Twice the reference span means the same count implies half the rate. Two
     * seconds is used rather than half a second because the reference rate is
     * odd, so halving it would truncate and the expectation would stop being
     * exact; doubling it cannot.
     */
    if (rate_from_span(
            0U,
            UINT64_C(4000000000),
            PM_TIMER_FREQUENCY_HZ,
            (uint64_t)PM_TIMER_FREQUENCY_HZ * 2U,
            &rate
        ) != TSC_STATUS_OK ||
        rate != UINT64_C(2000000000)) {
        return false;
    }

    if (rate_from_span(
            UINT64_C(1000),
            UINT64_C(1000),
            PM_TIMER_FREQUENCY_HZ,
            PM_TIMER_FREQUENCY_HZ,
            &rate
        ) != TSC_STATUS_NOT_COUNTING) {
        return false;
    }

    if (rate_from_span(
            UINT64_C(2000),
            UINT64_C(1000),
            PM_TIMER_FREQUENCY_HZ,
            PM_TIMER_FREQUENCY_HZ,
            &rate
        ) != TSC_STATUS_WENT_BACKWARDS) {
        return false;
    }

    /* A span that implies a rate no processor has is measurement noise. */
    if (rate_from_span(
            0U,
            UINT64_C(100000000000),
            PM_TIMER_FREQUENCY_HZ,
            PM_TIMER_FREQUENCY_HZ / 10U,
            &rate
        ) != TSC_STATUS_RATE_OUT_OF_RANGE) {
        return false;
    }

    /*
     * A span so wide that scaling it by the reference rate would overflow. At
     * the PIT's old rate of a hundred this branch needed a span no counter could
     * produce; against a 3.579545 MHz reference it is reachable, so it is
     * tested.
     */
    if (rate_from_span(
            0U,
            UINT64_MAX / PM_TIMER_FREQUENCY_HZ + 1U,
            PM_TIMER_FREQUENCY_HZ,
            PM_TIMER_FREQUENCY_HZ,
            &rate
        ) != TSC_STATUS_RATE_OUT_OF_RANGE) {
        return false;
    }

    /* A reference reporting no rate, and one reporting no elapsed ticks. */
    if (rate_from_span(
            0U,
            UINT64_C(1000),
            0U,
            PM_TIMER_FREQUENCY_HZ,
            &rate
        ) != TSC_STATUS_NOT_COUNTING ||
        rate_from_span(
            0U,
            UINT64_C(1000),
            PM_TIMER_FREQUENCY_HZ,
            0U,
            &rate
        ) != TSC_STATUS_NOT_COUNTING) {
        return false;
    }

    /* Exactly one second, and a half second, of a 2 GHz counter. */
    if (span_to_nanoseconds(UINT64_C(2000000000), UINT64_C(2000000000)) !=
            NANOSECONDS_PER_SECOND ||
        span_to_nanoseconds(UINT64_C(1000000000), UINT64_C(2000000000)) !=
            NANOSECONDS_PER_SECOND / 2U) {
        return false;
    }

    /* A sub-second remainder must not be lost to integer division. */
    if (span_to_nanoseconds(UINT64_C(2), UINT64_C(2000000000)) != UINT64_C(1) ||
        span_to_nanoseconds(0U, UINT64_C(2000000000)) != 0U) {
        return false;
    }

    /* An hour at 3 GHz still converts without overflowing. */
    if (span_to_nanoseconds(
            UINT64_C(3000000000) * UINT64_C(3600),
            UINT64_C(3000000000)
        ) != UINT64_C(3600) * NANOSECONDS_PER_SECOND) {
        return false;
    }

    return tsc_span_nanoseconds(UINT64_C(10), UINT64_C(5)) == 0U;
}

const char *tsc_status_string(enum tsc_status status)
{
    switch (status) {
    case TSC_STATUS_OK:
        return "ok";
    case TSC_STATUS_UNSUPPORTED:
        return "processor reports no time-stamp counter";
    case TSC_STATUS_INTERRUPTS_ENABLED:
        return "TSC calibration requires interrupts disabled";
    case TSC_STATUS_ALREADY_CALIBRATED:
        return "TSC was calibrated twice";
    case TSC_STATUS_NOT_CALIBRATED:
        return "TSC has no calibrated rate";
    case TSC_STATUS_REFERENCE_FAILURE:
        return "TSC reference clock failed";
    case TSC_STATUS_NOT_COUNTING:
        return "TSC did not advance across the reference interval";
    case TSC_STATUS_WENT_BACKWARDS:
        return "TSC ran backwards across the reference interval";
    case TSC_STATUS_RATE_OUT_OF_RANGE:
        return "TSC rate is outside the representable range";
    default:
        return "unknown TSC status";
    }
}
