/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/apic_timer.h>
#include <phipia/clock.h>
#include <phipia/cpu.h>
#include <phipia/heap.h>
#include <phipia/timer.h>

/*
 * Phipia policy bound on the shortest deadline that may be armed. The local APIC
 * timer counts at tens of megahertz, so a deadline closer than this would often
 * have passed by the time the count is written, and rearming from inside the
 * expiry handler would then spin the interrupt rather than make progress.
 */
#define TIMER_MINIMUM_INTERVAL_NS UINT64_C(100000)

/*
 * Phipia policy bound on a sleep. A deadline that has not arrived by twice its
 * own interval plus this grace has not been delivered, and the sleep gives up
 * with a status instead of halting forever. The grace covers the case of a very
 * short interval where twice it is still shorter than one timer interrupt.
 */
#define TIMER_SLEEP_GRACE_NS UINT64_C(50000000)

struct timer_entry {
    uint64_t deadline_ns;
    uint64_t identifier;
    timer_callback_t callback;
    void *context;
    bool in_use;
};

/*
 * The pending-deadline table: one heap allocation, obtained once by timer_start
 * and released by timer_stop. Every loop below is bounded by `capacity`, so a
 * table that has not been obtained is a capacity of zero and every scan is
 * simply empty. That is what makes a null `entries` safe rather than a hazard,
 * and it is why none of these helpers needs a null check of its own.
 */
static struct timer_entry *entries;
static size_t capacity;
static uint64_t next_identifier = 1U;
static volatile uint64_t expiry_counter __attribute__((aligned(8)));
static bool started;

/*
 * Set by a sleep's own callback inside the timer interrupt and read by the sleep
 * loop outside it, so the compiler must not cache it across the halt.
 */
static volatile bool sleep_expired;

/*
 * The table operations are kept free of hardware so all of them are provable
 * from synthetic entries: which deadline is next, which identifier maps to which
 * slot, what a full table does, and which entries a given instant has passed.
 */
static size_t slot_of(uint64_t identifier)
{
    if (identifier == 0U) {
        return capacity;
    }

    for (size_t index = 0; index < capacity; ++index) {
        if (entries[index].in_use &&
            entries[index].identifier == identifier) {
            return index;
        }
    }

    return capacity;
}

static size_t free_slot(void)
{
    for (size_t index = 0; index < capacity; ++index) {
        if (!entries[index].in_use) {
            return index;
        }
    }

    return capacity;
}

/*
 * The earliest pending deadline. Ties keep the lower slot, which makes the order
 * deterministic rather than dependent on scan direction.
 */
static bool earliest_deadline(uint64_t *deadline_ns)
{
    bool found = false;

    for (size_t index = 0; index < capacity; ++index) {
        if (!entries[index].in_use) {
            continue;
        }

        if (!found || entries[index].deadline_ns < *deadline_ns) {
            *deadline_ns = entries[index].deadline_ns;
            found = true;
        }
    }

    return found;
}

static size_t pending_slots(void)
{
    size_t count = 0;

    for (size_t index = 0; index < capacity; ++index) {
        if (entries[index].in_use) {
            ++count;
        }
    }

    return count;
}

/*
 * Fire every deadline this instant has reached, earliest first, and return how
 * many fired. Each entry is released before its callback runs, so a callback may
 * arm a fresh deadline into the slot it just vacated, and the scan restarts after
 * every callback because a callback is allowed to cancel or arm anything.
 */
static size_t expire_reached(uint64_t now_ns)
{
    size_t fired = 0;

    for (;;) {
        size_t chosen = capacity;
        struct timer_entry due;

        for (size_t index = 0; index < capacity; ++index) {
            if (!entries[index].in_use ||
                entries[index].deadline_ns > now_ns) {
                continue;
            }

            if (chosen == capacity ||
                entries[index].deadline_ns < entries[chosen].deadline_ns) {
                chosen = index;
            }
        }

        if (chosen == capacity) {
            return fired;
        }

        due = entries[chosen];
        entries[chosen].in_use = false;
        entries[chosen].callback = NULL;
        entries[chosen].context = NULL;
        entries[chosen].identifier = 0U;
        entries[chosen].deadline_ns = 0U;
        ++fired;

        if (due.callback != NULL) {
            due.callback(due.deadline_ns, due.context);
        }
    }
}

/*
 * Point the hardware at whatever is now the nearest deadline. A deadline that has
 * already passed is armed at the minimum interval rather than skipped, so the
 * interrupt still arrives and expire_reached still runs; refusing here would
 * leave an entry pending with nothing scheduled to fire it.
 */
static enum timer_status reprogram(void)
{
    uint64_t deadline_ns = 0U;
    uint64_t now_ns;
    uint64_t interval_ns;

    if (apic_timer_is_armed() &&
        apic_timer_disarm() != APIC_TIMER_STATUS_OK) {
        return TIMER_STATUS_HARDWARE_FAILURE;
    }

    if (!earliest_deadline(&deadline_ns)) {
        return TIMER_STATUS_OK;
    }

    now_ns = clock_monotonic_ns();
    interval_ns = deadline_ns > now_ns ? deadline_ns - now_ns : 0U;

    if (interval_ns < TIMER_MINIMUM_INTERVAL_NS) {
        interval_ns = TIMER_MINIMUM_INTERVAL_NS;
    }

    if (apic_timer_arm_oneshot(interval_ns) != APIC_TIMER_STATUS_OK) {
        return TIMER_STATUS_HARDWARE_FAILURE;
    }

    return TIMER_STATUS_OK;
}

static void on_expiry(void *context)
{
    (void)context;
    expiry_counter += expire_reached(clock_monotonic_ns());
    (void)reprogram();
}

enum timer_status timer_start(void)
{
    void *table = NULL;

    if (started) {
        return TIMER_STATUS_ALREADY_STARTED;
    }

    /*
     * Not started must mean no table held. Anything else is a subsystem that
     * already owns memory it does not know about, and starting over the top of
     * it would lose that block for the life of the kernel. This is the check
     * that catches a self-test which borrowed the table and forgot to give it
     * back, which is otherwise invisible.
     */
    if (entries != NULL || capacity != 0U) {
        return TIMER_STATUS_ALREADY_STARTED;
    }

    if (cpu_interrupts_enabled()) {
        return TIMER_STATUS_HARDWARE_FAILURE;
    }

    if (!clock_is_started()) {
        return TIMER_STATUS_NO_CLOCK;
    }

    /*
     * The one heap allocation this subsystem makes, and it is made here rather
     * than per arm because timer_arm runs inside the timer interrupt when a
     * callback arms a fresh deadline, and the heap must not be entered from
     * there. Obtained before the hardware handler is installed, so a heap that
     * cannot supply it leaves the timer exactly as it was.
     */
    if (heap_allocate(sizeof(struct timer_entry) * TIMER_MAX_PENDING, &table) !=
        HEAP_STATUS_OK) {
        return TIMER_STATUS_NO_MEMORY;
    }

    entries = (struct timer_entry *)table;
    capacity = TIMER_MAX_PENDING;

    for (size_t index = 0; index < capacity; ++index) {
        entries[index].in_use = false;
        entries[index].callback = NULL;
        entries[index].context = NULL;
        entries[index].identifier = 0U;
        entries[index].deadline_ns = 0U;
    }

    if (apic_timer_set_expiry_handler(on_expiry, NULL) !=
        APIC_TIMER_STATUS_OK) {
        (void)heap_free(entries);
        entries = NULL;
        capacity = 0U;
        return TIMER_STATUS_HARDWARE_FAILURE;
    }

    started = true;
    return TIMER_STATUS_OK;
}

enum timer_status timer_stop(void)
{
    if (!started) {
        return TIMER_STATUS_NOT_STARTED;
    }

    if (cpu_interrupts_enabled()) {
        return TIMER_STATUS_HARDWARE_FAILURE;
    }

    if (apic_timer_is_armed() &&
        apic_timer_disarm() != APIC_TIMER_STATUS_OK) {
        return TIMER_STATUS_HARDWARE_FAILURE;
    }

    for (size_t index = 0; index < capacity; ++index) {
        entries[index].in_use = false;
        entries[index].callback = NULL;
        entries[index].context = NULL;
        entries[index].identifier = 0U;
        entries[index].deadline_ns = 0U;
    }

    /*
     * The handler goes before the table does. Releasing memory the expiry path
     * can still be entered through would leave an interrupt walking a table the
     * heap has already handed to somebody else.
     */
    if (apic_timer_set_expiry_handler(NULL, NULL) != APIC_TIMER_STATUS_OK) {
        return TIMER_STATUS_HARDWARE_FAILURE;
    }

    if (heap_free(entries) != HEAP_STATUS_OK) {
        return TIMER_STATUS_NO_MEMORY;
    }

    entries = NULL;
    capacity = 0U;
    started = false;
    return TIMER_STATUS_OK;
}

bool timer_is_started(void)
{
    return started;
}

enum timer_status timer_arm(
    uint64_t deadline_ns,
    timer_callback_t callback,
    void *context,
    uint64_t *identifier
)
{
    size_t slot;
    uint64_t now_ns;

    if (callback == NULL || identifier == NULL) {
        return TIMER_STATUS_NULL_ARGUMENT;
    }

    *identifier = 0U;

    if (!started) {
        return TIMER_STATUS_NOT_STARTED;
    }

    now_ns = clock_monotonic_ns();

    /*
     * A deadline in the past, or one too near to program, is refused rather than
     * fired immediately. A caller that computed one has a bug, and firing would
     * hide it behind a callback that appeared to work.
     */
    if (deadline_ns < now_ns ||
        deadline_ns - now_ns < TIMER_MINIMUM_INTERVAL_NS) {
        return TIMER_STATUS_BAD_INTERVAL;
    }

    slot = free_slot();

    if (slot == capacity) {
        return TIMER_STATUS_NO_CAPACITY;
    }

    entries[slot].deadline_ns = deadline_ns;
    entries[slot].identifier = next_identifier;
    entries[slot].callback = callback;
    entries[slot].context = context;
    entries[slot].in_use = true;
    ++next_identifier;

    if (reprogram() != TIMER_STATUS_OK) {
        entries[slot].in_use = false;
        entries[slot].callback = NULL;
        entries[slot].context = NULL;
        entries[slot].identifier = 0U;
        entries[slot].deadline_ns = 0U;
        return TIMER_STATUS_HARDWARE_FAILURE;
    }

    *identifier = entries[slot].identifier;
    return TIMER_STATUS_OK;
}

enum timer_status timer_cancel(uint64_t identifier)
{
    const size_t slot = slot_of(identifier);

    if (!started) {
        return TIMER_STATUS_NOT_STARTED;
    }

    if (slot == capacity) {
        return TIMER_STATUS_UNKNOWN_TIMER;
    }

    entries[slot].in_use = false;
    entries[slot].callback = NULL;
    entries[slot].context = NULL;
    entries[slot].identifier = 0U;
    entries[slot].deadline_ns = 0U;
    return reprogram();
}

static void sleep_callback(uint64_t deadline_ns, void *context)
{
    (void)deadline_ns;
    (void)context;
    sleep_expired = true;
}

enum timer_status timer_sleep_ns(uint64_t nanoseconds)
{
    uint64_t identifier = 0U;
    uint64_t deadline_ns;
    uint64_t abandon_ns;
    enum timer_status status;

    if (!started) {
        return TIMER_STATUS_NOT_STARTED;
    }

    if (nanoseconds < TIMER_MINIMUM_INTERVAL_NS) {
        return TIMER_STATUS_BAD_INTERVAL;
    }

    deadline_ns = clock_monotonic_ns() + nanoseconds;
    abandon_ns = deadline_ns + nanoseconds + TIMER_SLEEP_GRACE_NS;
    sleep_expired = false;
    status = timer_arm(deadline_ns, sleep_callback, NULL, &identifier);

    if (status != TIMER_STATUS_OK) {
        return status;
    }

    /*
     * Bounded by the clock rather than by an iteration count, because the clock
     * is the thing this subsystem exists to wait on. A deadline that has not
     * arrived by twice its interval was not delivered, and the entry is cancelled
     * on the way out so it cannot fire into a caller that has given up.
     *
     * The hardware is checked before every halt as well. Halting waits for an
     * interrupt, so if nothing is armed then nothing is going to arrive and the
     * clock bound above would never be reached to notice - the processor would
     * simply stop. Refusing to halt with no deadline armed is what turns that
     * into a status. An armed deadline that the hardware never delivers still
     * halts, as every other wait loop in this kernel does; software cannot
     * distinguish that from a stopped processor.
     */
    while (!sleep_expired) {
        if (clock_monotonic_ns() > abandon_ns) {
            (void)timer_cancel(identifier);
            return TIMER_STATUS_EXPIRED_LATE;
        }

        if (!apic_timer_is_armed()) {
            (void)timer_cancel(identifier);
            return TIMER_STATUS_HARDWARE_FAILURE;
        }

        cpu_enable_and_halt();
    }

    cpu_interrupt_disable();
    return TIMER_STATUS_OK;
}

size_t timer_capacity(void)
{
    return capacity;
}

size_t timer_pending_count(void)
{
    return pending_slots();
}

uint64_t timer_expiry_count(void)
{
    return expiry_counter;
}

/*
 * The self-test runs during early boot, long before the heap exists, so it
 * lends the table operations a private static table for the duration and puts
 * them back afterwards. Eight entries rather than TIMER_MAX_PENDING because the
 * full-table cases scan the whole thing and nothing here is timing sensitive.
 */
#define TIMER_SELF_TEST_CAPACITY 8U

static struct timer_entry self_test_entries[TIMER_SELF_TEST_CAPACITY];
static uint64_t self_test_fired_mask;

static void self_test_callback(uint64_t deadline_ns, void *context)
{
    (void)deadline_ns;
    self_test_fired_mask |= (uint64_t)1U << *(const uint8_t *)context;
}

/*
 * The table decides which deadline is next and which have been reached, and a
 * mistake there is silent: a timer fires at the wrong moment, or never, and the
 * caller waits. All of it is provable from synthetic entries with no hardware and
 * no timing, which is why the table is kept separate from the arming.
 *
 * This runs before timer_start, so it also pins down what the subsystem does
 * before it has a clock.
 */
static bool test_refusals_before_start(void)
{
    static const uint8_t indices[1] = {0U};
    uint64_t identifier = 0U;

    (void)indices;

    /* Nothing works before the subsystem is started, and nothing pretends to. */
    if (timer_is_started() || timer_pending_count() != 0U ||
        timer_arm(UINT64_C(1000000), self_test_callback, NULL, &identifier) !=
            TIMER_STATUS_NOT_STARTED ||
        identifier != 0U ||
        timer_cancel(1U) != TIMER_STATUS_NOT_STARTED ||
        timer_sleep_ns(UINT64_C(1000000)) != TIMER_STATUS_NOT_STARTED ||
        timer_stop() != TIMER_STATUS_NOT_STARTED) {
        return false;
    }

    /* Null arguments are refused ahead of the started check, being worse. */
    return timer_arm(UINT64_C(1000000), NULL, NULL, &identifier) ==
            TIMER_STATUS_NULL_ARGUMENT &&
        timer_arm(UINT64_C(1000000), self_test_callback, NULL, NULL) ==
            TIMER_STATUS_NULL_ARGUMENT &&
        timer_capacity() == 0U;
}

static bool test_table_operations(void)
{
    static const uint8_t indices[3] = {0U, 1U, 2U};
    uint64_t deadline_ns = 0U;

    /* An empty table has no next deadline and no slot for any identifier. */
    if (earliest_deadline(&deadline_ns) || slot_of(0U) != TIMER_SELF_TEST_CAPACITY ||
        slot_of(1U) != TIMER_SELF_TEST_CAPACITY || free_slot() != 0U ||
        pending_slots() != 0U) {
        return false;
    }

    /* Three deadlines out of order; the earliest wins regardless of slot. */
    entries[0].deadline_ns = UINT64_C(300);
    entries[0].identifier = UINT64_C(11);
    entries[0].callback = self_test_callback;
    entries[0].context = (void *)&indices[0];
    entries[0].in_use = true;
    entries[1].deadline_ns = UINT64_C(100);
    entries[1].identifier = UINT64_C(12);
    entries[1].callback = self_test_callback;
    entries[1].context = (void *)&indices[1];
    entries[1].in_use = true;
    entries[2].deadline_ns = UINT64_C(200);
    entries[2].identifier = UINT64_C(13);
    entries[2].callback = self_test_callback;
    entries[2].context = (void *)&indices[2];
    entries[2].in_use = true;

    if (!earliest_deadline(&deadline_ns) || deadline_ns != UINT64_C(100) ||
        pending_slots() != 3U || free_slot() != 3U ||
        slot_of(UINT64_C(12)) != 1U || slot_of(UINT64_C(13)) != 2U ||
        slot_of(UINT64_C(99)) != TIMER_SELF_TEST_CAPACITY) {
        return false;
    }

    /* An instant before every deadline reaches none of them. */
    self_test_fired_mask = 0U;

    if (expire_reached(UINT64_C(99)) != 0U || self_test_fired_mask != 0U ||
        pending_slots() != 3U) {
        return false;
    }

    /* A deadline is reached by an instant equal to it, not only past it. */
    if (expire_reached(UINT64_C(100)) != 1U ||
        self_test_fired_mask != UINT64_C(2) ||
        pending_slots() != 2U ||
        slot_of(UINT64_C(12)) != TIMER_SELF_TEST_CAPACITY) {
        return false;
    }

    /* An instant past several fires all of them, and the table empties. */
    if (expire_reached(UINT64_C(1000)) != 2U ||
        self_test_fired_mask != UINT64_C(7) ||
        pending_slots() != 0U ||
        earliest_deadline(&deadline_ns)) {
        return false;
    }

    /* A full table refuses a further entry rather than overwriting one. */
    for (size_t index = 0; index < TIMER_SELF_TEST_CAPACITY; ++index) {
        entries[index].deadline_ns = UINT64_C(1000) + index;
        entries[index].identifier = UINT64_C(100) + index;
        entries[index].callback = NULL;
        entries[index].context = NULL;
        entries[index].in_use = true;
    }

    if (free_slot() != TIMER_SELF_TEST_CAPACITY ||
        pending_slots() != TIMER_SELF_TEST_CAPACITY ||
        !earliest_deadline(&deadline_ns) ||
        deadline_ns != UINT64_C(1000)) {
        return false;
    }

    /* An entry with no callback is still released, and counted as fired. */
    if (expire_reached(UINT64_MAX) != TIMER_SELF_TEST_CAPACITY ||
        pending_slots() != 0U || free_slot() != 0U) {
        return false;
    }

    /* Ties are broken by slot, so the order does not depend on scan direction. */
    entries[0].deadline_ns = UINT64_C(500);
    entries[0].identifier = UINT64_C(21);
    entries[0].in_use = true;
    entries[1].deadline_ns = UINT64_C(500);
    entries[1].identifier = UINT64_C(22);
    entries[1].in_use = true;

    if (!earliest_deadline(&deadline_ns) || deadline_ns != UINT64_C(500)) {
        return false;
    }

    entries[0].in_use = false;
    entries[1].in_use = false;
    self_test_fired_mask = 0U;
    return pending_slots() == 0U;
}

/*
 * The table decides which deadline is next and which have been reached, and a
 * mistake there is silent: a timer fires at the wrong moment, or never, and the
 * caller waits. All of it is provable from synthetic entries with no hardware
 * and no timing, which is why the table is kept separate from the arming.
 *
 * This runs before timer_start and before the heap exists, so it also pins down
 * what the subsystem does with no table at all.
 */
bool timer_self_test(void)
{
    bool passed;

    if (started || entries != NULL || capacity != 0U) {
        return false;
    }

    if (!test_refusals_before_start()) {
        return false;
    }

    entries = self_test_entries;
    capacity = TIMER_SELF_TEST_CAPACITY;
    passed = test_table_operations();

    for (size_t index = 0; index < TIMER_SELF_TEST_CAPACITY; ++index) {
        self_test_entries[index].in_use = false;
        self_test_entries[index].callback = NULL;
        self_test_entries[index].context = NULL;
        self_test_entries[index].identifier = 0U;
        self_test_entries[index].deadline_ns = 0U;
    }

    entries = NULL;
    capacity = 0U;
    return passed;
}

const char *timer_status_string(enum timer_status status)
{
    switch (status) {
    case TIMER_STATUS_OK:
        return "ok";
    case TIMER_STATUS_NULL_ARGUMENT:
        return "null deadline timer argument";
    case TIMER_STATUS_NOT_STARTED:
        return "deadline timers are not started";
    case TIMER_STATUS_ALREADY_STARTED:
        return "deadline timers were started twice";
    case TIMER_STATUS_NO_CLOCK:
        return "deadline timers require a started monotonic clock";
    case TIMER_STATUS_NO_CAPACITY:
        return "deadline timer table is full";
    case TIMER_STATUS_NO_MEMORY:
        return "deadline timer table could not be obtained from the heap";
    case TIMER_STATUS_BAD_INTERVAL:
        return "deadline is in the past or too near to program";
    case TIMER_STATUS_UNKNOWN_TIMER:
        return "deadline timer identifier does not name a pending timer";
    case TIMER_STATUS_HARDWARE_FAILURE:
        return "deadline timer could not program the local APIC timer";
    case TIMER_STATUS_EXPIRED_LATE:
        return "deadline did not arrive within its bound";
    default:
        return "unknown deadline timer status";
    }
}
