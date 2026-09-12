/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/acpi.h>
#include <phipia/acpi_util.h>
#include <phipia/cpu.h>
#include <phipia/pm_timer.h>

#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/*
 * Phipia policy bound, not an ACPI limit. One wait may not ask for more than a
 * quarter of the narrowest counter's period, about 1.17 seconds, which keeps a
 * bounded spin from becoming an excuse to sit in the kernel indefinitely.
 */
#define PM_TIMER_MAX_WAIT_TICKS (UINT32_C(1) << (ACPI_PM_TIMER_BASE_BITS - 2U))

/*
 * Phipia policy bound on the spin. Each iteration is one 32-bit port read of
 * the ACPI timer block, and nothing can make that cheaper than a bus
 * transaction, so a counter running at 3.579545 MHz advances at least once
 * every few reads. Allowing sixty-four polls for every requested tick is orders
 * of magnitude more slack than a working counter needs, and it turns a counter
 * that has stopped answering into a status rather than a hung kernel.
 */
#define PM_TIMER_POLLS_PER_TICK UINT64_C(64)

/*
 * A wait in progress. The port read is kept out of this structure on purpose:
 * the wrap folding, the completion test, and the poll bound are then all
 * provable from synthetic samples, with no hardware and no timing.
 */
struct pm_timer_wait {
    uint64_t target;
    uint64_t elapsed;
    uint64_t polls;
    uint64_t poll_limit;
    uint32_t previous;
    uint8_t counter_bits;
};

static struct pm_timer_state state;

static bool counter_width_is_supported(uint8_t counter_bits)
{
    return counter_bits == ACPI_PM_TIMER_BASE_BITS ||
        counter_bits == ACPI_PM_TIMER_EXTENDED_BITS;
}

/* Only ever called for a width counter_width_is_supported has accepted. */
static uint32_t mask_for_width(uint8_t counter_bits)
{
    return counter_bits == ACPI_PM_TIMER_EXTENDED_BITS
        ? UINT32_MAX
        : (UINT32_C(1) << ACPI_PM_TIMER_BASE_BITS) - 1U;
}

static void reset_state(void)
{
    state.port = 0U;
    state.counter_bits = 0U;
    state.extended_address = false;
    state.present = false;
}

/*
 * Turn two samples into the ticks between them. The counter is 24 or 32 bits
 * wide and wraps silently, so an end that reads below the start is one wrap
 * rather than an error; unsigned subtraction is already modular, and masking
 * the difference to the counter's own width folds that wrap out without a
 * comparison. Two wraps look exactly like one, which is why the wait bound
 * exists and why the width is carried rather than assumed.
 *
 * A span of zero is refused because it is ambiguous: either the counter did not
 * advance, or it advanced exactly one full period. Neither is a duration.
 */
static enum pm_timer_status span_from_samples(
    uint32_t start,
    uint32_t end,
    uint8_t counter_bits,
    uint32_t *ticks
)
{
    uint32_t mask;

    *ticks = 0U;

    if (!counter_width_is_supported(counter_bits)) {
        return PM_TIMER_STATUS_BAD_COUNTER_WIDTH;
    }

    mask = mask_for_width(counter_bits);
    *ticks = ((end & mask) - (start & mask)) & mask;

    if (*ticks == 0U) {
        return PM_TIMER_STATUS_NOT_COUNTING;
    }

    return PM_TIMER_STATUS_OK;
}

static enum pm_timer_status wait_begin(
    struct pm_timer_wait *wait,
    uint32_t ticks,
    uint8_t counter_bits,
    uint32_t first_sample
)
{
    if (!counter_width_is_supported(counter_bits)) {
        return PM_TIMER_STATUS_BAD_COUNTER_WIDTH;
    }

    if (ticks == 0U || ticks > PM_TIMER_MAX_WAIT_TICKS) {
        return PM_TIMER_STATUS_BAD_INTERVAL;
    }

    wait->target = ticks;
    wait->elapsed = 0U;
    wait->polls = 0U;
    wait->poll_limit = (uint64_t)ticks * PM_TIMER_POLLS_PER_TICK;
    wait->previous = first_sample & mask_for_width(counter_bits);
    wait->counter_bits = counter_bits;
    return PM_TIMER_STATUS_OK;
}

/*
 * Fold one sample into a wait. Consecutive samples are separated by a single
 * port read, so the gap between them is far below one counter period and the
 * masked difference is exact; the total may therefore run past a period without
 * ever becoming ambiguous.
 */
static enum pm_timer_status wait_step(
    struct pm_timer_wait *wait,
    uint32_t sample,
    bool *complete
)
{
    const uint32_t mask = mask_for_width(wait->counter_bits);
    const uint32_t masked = sample & mask;

    *complete = false;

    if (wait->polls >= wait->poll_limit) {
        return PM_TIMER_STATUS_STALLED;
    }

    ++wait->polls;
    wait->elapsed += (uint64_t)((masked - wait->previous) & mask);
    wait->previous = masked;
    *complete = wait->elapsed >= wait->target;
    return PM_TIMER_STATUS_OK;
}

enum pm_timer_status pm_timer_initialize(const struct acpi_fadt *fadt)
{
    if (fadt == NULL) {
        return PM_TIMER_STATUS_NULL_ARGUMENT;
    }

    if (state.present) {
        return PM_TIMER_STATUS_ALREADY_INITIALIZED;
    }

    reset_state();

    /*
     * The discovered description has already been validated once. It is checked
     * again here because this subsystem is what actually issues the port read,
     * and a reader that trusts its caller's bounds has no bounds of its own.
     */
    if (fadt->pm_timer_port == 0U ||
        (uint32_t)fadt->pm_timer_port >
            (uint32_t)UINT16_MAX - (ACPI_PM_TIMER_BLOCK_LENGTH - 1U)) {
        return PM_TIMER_STATUS_BAD_PORT;
    }

    if (!counter_width_is_supported(fadt->pm_timer_counter_bits)) {
        return PM_TIMER_STATUS_BAD_COUNTER_WIDTH;
    }

    state.port = fadt->pm_timer_port;
    state.counter_bits = fadt->pm_timer_counter_bits;
    state.extended_address = fadt->pm_timer_extended_address;
    state.present = true;
    return PM_TIMER_STATUS_OK;
}

struct pm_timer_state pm_timer_get_state(void)
{
    return state;
}

bool pm_timer_is_present(void)
{
    return state.present;
}

/*
 * A 24-bit counter leaves the register's upper bits reserved, so the sample is
 * masked here and no caller ever sees a bit the counter does not own.
 */
uint32_t pm_timer_read(void)
{
    if (!state.present) {
        return 0U;
    }

    return cpu_in32(state.port) & mask_for_width(state.counter_bits);
}

enum pm_timer_status pm_timer_span(
    uint32_t start,
    uint32_t end,
    uint32_t *ticks
)
{
    if (ticks == NULL) {
        return PM_TIMER_STATUS_NULL_ARGUMENT;
    }

    *ticks = 0U;

    if (!state.present) {
        return PM_TIMER_STATUS_ABSENT;
    }

    return span_from_samples(start, end, state.counter_bits, ticks);
}

enum pm_timer_status pm_timer_wait(uint32_t ticks, uint64_t *elapsed)
{
    struct pm_timer_wait wait;
    enum pm_timer_status status;
    bool complete = false;

    if (elapsed == NULL) {
        return PM_TIMER_STATUS_NULL_ARGUMENT;
    }

    *elapsed = 0U;

    if (!state.present) {
        return PM_TIMER_STATUS_ABSENT;
    }

    status = wait_begin(&wait, ticks, state.counter_bits, pm_timer_read());

    if (status != PM_TIMER_STATUS_OK) {
        return status;
    }

    while (!complete) {
        status = wait_step(&wait, pm_timer_read(), &complete);

        if (status != PM_TIMER_STATUS_OK) {
            return status;
        }
    }

    *elapsed = wait.elapsed;
    return PM_TIMER_STATUS_OK;
}

/*
 * Both conversions split the value so neither half can overflow. The remainder
 * is always below its divisor, so the widest product either one forms is
 * 3579544 times a billion, about 3.6e15, and the whole part would have to reach
 * five centuries of ticks before its own product mattered. The bounds are
 * therefore proved by the arithmetic rather than guarded by a branch.
 */
uint64_t pm_timer_ticks_to_nanoseconds(uint64_t ticks)
{
    const uint64_t whole_seconds = ticks / PM_TIMER_FREQUENCY_HZ;
    const uint64_t remainder = ticks % PM_TIMER_FREQUENCY_HZ;

    return whole_seconds * NANOSECONDS_PER_SECOND +
        remainder * NANOSECONDS_PER_SECOND / PM_TIMER_FREQUENCY_HZ;
}

uint64_t pm_timer_nanoseconds_to_ticks(uint64_t nanoseconds)
{
    const uint64_t whole_seconds = nanoseconds / NANOSECONDS_PER_SECOND;
    const uint64_t remainder = nanoseconds % NANOSECONDS_PER_SECOND;

    return whole_seconds * PM_TIMER_FREQUENCY_HZ +
        remainder * PM_TIMER_FREQUENCY_HZ / NANOSECONDS_PER_SECOND;
}

bool pm_timer_durations_agree(
    uint64_t measured,
    uint64_t reference,
    uint64_t tolerance_divisor
)
{
    const uint64_t difference = measured > reference
        ? measured - reference
        : reference - measured;

    /*
     * A zero reference has no tolerance to be inside, so nothing agrees with
     * it. Reporting that as disagreement is deliberate: it is how a clock that
     * produced no duration at all fails, rather than trivially passing against
     * another that did the same. A zero divisor describes no tolerance at all
     * and would divide by zero, so it agrees with nothing either.
     */
    return reference != 0U && tolerance_divisor != 0U &&
        difference <= reference / tolerance_divisor;
}

/*
 * The wrap arithmetic decides what every interval this timer reports means, and
 * a mistake in it is silent: a folded wrap comes out as a plausible duration
 * rather than as an error. All of it is provable from synthetic samples.
 *
 * This runs before discovery, so it also pins down what the subsystem does
 * before it has a timer: it reports absence rather than reading port zero.
 */
bool pm_timer_self_test(void)
{
    struct pm_timer_wait wait;
    struct acpi_fadt probe;
    uint64_t elapsed = 0U;
    uint32_t ticks = 0U;
    bool complete = false;

    if (state.present) {
        return false;
    }

    /* An undiscovered timer answers with absence, never with a reading. */
    if (pm_timer_is_present() || pm_timer_read() != 0U ||
        pm_timer_span(0U, UINT32_C(1000), &ticks) != PM_TIMER_STATUS_ABSENT ||
        ticks != 0U ||
        pm_timer_wait(UINT32_C(1000), &elapsed) != PM_TIMER_STATUS_ABSENT ||
        elapsed != 0U ||
        pm_timer_span(0U, UINT32_C(1000), NULL) !=
            PM_TIMER_STATUS_NULL_ARGUMENT ||
        pm_timer_wait(UINT32_C(1000), NULL) != PM_TIMER_STATUS_NULL_ARGUMENT ||
        pm_timer_initialize(NULL) != PM_TIMER_STATUS_NULL_ARGUMENT) {
        return false;
    }

    /* A span that does not wrap, at each width. */
    if (span_from_samples(
            UINT32_C(0x000100),
            UINT32_C(0x000200),
            ACPI_PM_TIMER_BASE_BITS,
            &ticks
        ) != PM_TIMER_STATUS_OK ||
        ticks != UINT32_C(256)) {
        return false;
    }

    if (span_from_samples(
            UINT32_C(0x10000000),
            UINT32_C(0x10000100),
            ACPI_PM_TIMER_EXTENDED_BITS,
            &ticks
        ) != PM_TIMER_STATUS_OK ||
        ticks != UINT32_C(256)) {
        return false;
    }

    /* A 24-bit counter wrapping past its top. */
    if (span_from_samples(
            UINT32_C(0xFFFF00),
            UINT32_C(0x0000FF),
            ACPI_PM_TIMER_BASE_BITS,
            &ticks
        ) != PM_TIMER_STATUS_OK ||
        ticks != UINT32_C(511)) {
        return false;
    }

    /* And a 32-bit one, which folds at a different place entirely. */
    if (span_from_samples(
            UINT32_C(0xFFFFFF00),
            UINT32_C(0x000000FF),
            ACPI_PM_TIMER_EXTENDED_BITS,
            &ticks
        ) != PM_TIMER_STATUS_OK ||
        ticks != UINT32_C(511)) {
        return false;
    }

    /*
     * The width is a decoded property, not an assumption: at 24 bits the
     * register's upper bits are reserved and must not reach the arithmetic,
     * and a difference that exists only above bit 23 is not a span at all.
     */
    if (span_from_samples(
            UINT32_C(0xFF000100),
            UINT32_C(0xAB000200),
            ACPI_PM_TIMER_BASE_BITS,
            &ticks
        ) != PM_TIMER_STATUS_OK ||
        ticks != UINT32_C(256)) {
        return false;
    }

    if (span_from_samples(
            UINT32_C(0x00000100),
            UINT32_C(0x01000100),
            ACPI_PM_TIMER_BASE_BITS,
            &ticks
        ) != PM_TIMER_STATUS_NOT_COUNTING ||
        ticks != 0U) {
        return false;
    }

    /* A zero span is ambiguous rather than short, so it is refused. */
    if (span_from_samples(
            UINT32_C(0x1234),
            UINT32_C(0x1234),
            ACPI_PM_TIMER_EXTENDED_BITS,
            &ticks
        ) != PM_TIMER_STATUS_NOT_COUNTING ||
        ticks != 0U) {
        return false;
    }

    /* A width the specification never defines cannot be masked at all. */
    if (span_from_samples(
            0U,
            UINT32_C(1000),
            16U,
            &ticks
        ) != PM_TIMER_STATUS_BAD_COUNTER_WIDTH ||
        ticks != 0U) {
        return false;
    }

    /* A wait that its first sample already satisfies. */
    if (wait_begin(
            &wait,
            UINT32_C(1000),
            ACPI_PM_TIMER_BASE_BITS,
            UINT32_C(0x000100)
        ) != PM_TIMER_STATUS_OK ||
        wait_step(&wait, UINT32_C(0x0004E8), &complete) !=
            PM_TIMER_STATUS_OK ||
        !complete || wait.elapsed != UINT32_C(1000)) {
        return false;
    }

    /* One that takes several samples and wraps in the middle of them. */
    if (wait_begin(
            &wait,
            UINT32_C(1000),
            ACPI_PM_TIMER_BASE_BITS,
            UINT32_C(0xFFFFC0)
        ) != PM_TIMER_STATUS_OK ||
        wait_step(&wait, UINT32_C(0xFFFFFF), &complete) !=
            PM_TIMER_STATUS_OK ||
        complete || wait.elapsed != UINT64_C(63) ||
        wait_step(&wait, UINT32_C(0x000200), &complete) !=
            PM_TIMER_STATUS_OK ||
        complete || wait.elapsed != UINT64_C(576) ||
        wait_step(&wait, UINT32_C(0x000400), &complete) !=
            PM_TIMER_STATUS_OK ||
        !complete || wait.elapsed != UINT64_C(1088)) {
        return false;
    }

    /* A counter that stops answering exhausts the poll bound and gives up. */
    if (wait_begin(&wait, 1U, ACPI_PM_TIMER_BASE_BITS, UINT32_C(0x1234)) !=
        PM_TIMER_STATUS_OK) {
        return false;
    }

    for (uint64_t poll = 0; poll < PM_TIMER_POLLS_PER_TICK; ++poll) {
        if (wait_step(&wait, UINT32_C(0x1234), &complete) !=
                PM_TIMER_STATUS_OK ||
            complete) {
            return false;
        }
    }

    if (wait_step(&wait, UINT32_C(0x1234), &complete) !=
            PM_TIMER_STATUS_STALLED ||
        complete) {
        return false;
    }

    /* The interval bounds, including the largest wait that is still allowed. */
    if (wait_begin(&wait, 0U, ACPI_PM_TIMER_BASE_BITS, 0U) !=
            PM_TIMER_STATUS_BAD_INTERVAL ||
        wait_begin(
            &wait,
            PM_TIMER_MAX_WAIT_TICKS + 1U,
            ACPI_PM_TIMER_BASE_BITS,
            0U
        ) != PM_TIMER_STATUS_BAD_INTERVAL ||
        wait_begin(
            &wait,
            PM_TIMER_MAX_WAIT_TICKS,
            ACPI_PM_TIMER_BASE_BITS,
            0U
        ) != PM_TIMER_STATUS_OK ||
        wait_begin(&wait, UINT32_C(1000), 16U, 0U) !=
            PM_TIMER_STATUS_BAD_COUNTER_WIDTH) {
        return false;
    }

    /* One second of ticks is one billion nanoseconds, in both directions. */
    if (pm_timer_ticks_to_nanoseconds(PM_TIMER_FREQUENCY_HZ) !=
            NANOSECONDS_PER_SECOND ||
        pm_timer_nanoseconds_to_ticks(NANOSECONDS_PER_SECOND) !=
            PM_TIMER_FREQUENCY_HZ ||
        pm_timer_ticks_to_nanoseconds(0U) != 0U ||
        pm_timer_nanoseconds_to_ticks(0U) != 0U) {
        return false;
    }

    /* A sub-second remainder must survive rather than be divided away. */
    if (pm_timer_nanoseconds_to_ticks(UINT64_C(100000000)) !=
            UINT64_C(357954) ||
        pm_timer_ticks_to_nanoseconds(UINT64_C(357954)) !=
            UINT64_C(99999860)) {
        return false;
    }

    /* An hour of ticks still converts without overflowing. */
    if (pm_timer_ticks_to_nanoseconds(
            (uint64_t)PM_TIMER_FREQUENCY_HZ * UINT64_C(3600)
        ) != UINT64_C(3600) * NANOSECONDS_PER_SECOND) {
        return false;
    }

    /*
     * The agreement policy, at and just past both boundaries. A reference of
     * zero agrees with nothing, including itself, so a clock that reported no
     * duration cannot pass by comparing equal to another that did the same.
     */
    if (!pm_timer_durations_agree(
            UINT64_C(100),
            UINT64_C(100),
            PM_TIMER_TOLERANCE_QUARTER
        ) ||
        !pm_timer_durations_agree(
            UINT64_C(125),
            UINT64_C(100),
            PM_TIMER_TOLERANCE_QUARTER
        ) ||
        !pm_timer_durations_agree(
            UINT64_C(75),
            UINT64_C(100),
            PM_TIMER_TOLERANCE_QUARTER
        ) ||
        pm_timer_durations_agree(
            UINT64_C(126),
            UINT64_C(100),
            PM_TIMER_TOLERANCE_QUARTER
        ) ||
        pm_timer_durations_agree(
            UINT64_C(74),
            UINT64_C(100),
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        return false;
    }

    /* A factor of two must fail the quarter, and a factor of four the half. */
    if (pm_timer_durations_agree(
            UINT64_C(50),
            UINT64_C(100),
            PM_TIMER_TOLERANCE_QUARTER
        ) ||
        !pm_timer_durations_agree(
            UINT64_C(50),
            UINT64_C(100),
            PM_TIMER_TOLERANCE_HALF
        ) ||
        pm_timer_durations_agree(
            UINT64_C(25),
            UINT64_C(100),
            PM_TIMER_TOLERANCE_HALF
        )) {
        return false;
    }

    /* Neither a zero reference nor a zero tolerance describes an agreement. */
    if (pm_timer_durations_agree(0U, 0U, PM_TIMER_TOLERANCE_QUARTER) ||
        pm_timer_durations_agree(
            UINT64_C(100),
            0U,
            PM_TIMER_TOLERANCE_QUARTER
        ) ||
        pm_timer_durations_agree(UINT64_C(100), UINT64_C(100), 0U)) {
        return false;
    }

    /*
     * Initialization refusals. Each one must leave the subsystem absent, so a
     * rejected description can never be mistaken for a discovered timer.
     */
    acpi_bytes_zero(&probe, sizeof(probe));
    probe.pm_timer_counter_bits = ACPI_PM_TIMER_BASE_BITS;

    if (pm_timer_initialize(&probe) != PM_TIMER_STATUS_BAD_PORT) {
        return false;
    }

    /* A port whose four-byte block would run off the end of the I/O space. */
    probe.pm_timer_port = UINT16_C(0xFFFE);

    if (pm_timer_initialize(&probe) != PM_TIMER_STATUS_BAD_PORT) {
        return false;
    }

    probe.pm_timer_port = UINT16_C(0x0608);
    probe.pm_timer_counter_bits = 16U;

    return pm_timer_initialize(&probe) ==
            PM_TIMER_STATUS_BAD_COUNTER_WIDTH &&
        !pm_timer_is_present();
}

const char *pm_timer_status_string(enum pm_timer_status status)
{
    switch (status) {
    case PM_TIMER_STATUS_OK:
        return "ok";
    case PM_TIMER_STATUS_NULL_ARGUMENT:
        return "null ACPI PM timer argument";
    case PM_TIMER_STATUS_ABSENT:
        return "ACPI PM timer was never discovered";
    case PM_TIMER_STATUS_ALREADY_INITIALIZED:
        return "ACPI PM timer was initialized twice";
    case PM_TIMER_STATUS_BAD_PORT:
        return "ACPI PM timer port is outside the I/O space";
    case PM_TIMER_STATUS_BAD_COUNTER_WIDTH:
        return "ACPI PM timer counter width is not 24 or 32 bits";
    case PM_TIMER_STATUS_BAD_INTERVAL:
        return "ACPI PM timer interval is zero or beyond the wait bound";
    case PM_TIMER_STATUS_NOT_COUNTING:
        return "ACPI PM timer span is zero and cannot be a duration";
    case PM_TIMER_STATUS_STALLED:
        return "ACPI PM timer stopped advancing during a bounded wait";
    default:
        return "unknown ACPI PM timer status";
    }
}
