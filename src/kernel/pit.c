/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/apic.h>
#include <phipia/cpu.h>
#include <phipia/interrupts.h>
#include <phipia/ioapic.h>
#include <phipia/pic.h>
#include <phipia/pit.h>
#include <phipia/pm_timer.h>

#define PIT_INPUT_FREQUENCY UINT32_C(1193182)
#define PIT_CHANNEL_ZERO UINT16_C(0x40)
#define PIT_COMMAND UINT16_C(0x43)
/*
 * Intel 8254 datasheet, control word: channel 0, lobyte/hibyte access, mode 2,
 * binary counting. Mode 2 is the rate generator, which drives its output low
 * for one input clock once per programmed period.
 *
 * Mode 3, the square wave generator, was programmed here until the ACPI power
 * management timer arrived. Mode 3 toggles its output twice per period, once at
 * the half count and once at the full count, so an edge-triggered handler
 * counts two interrupts for every period the divisor describes. Every interval
 * this kernel measured in PIT ticks was therefore half the length it believed,
 * and both clocks calibrated against it ran at half their true rate. Nothing
 * caught it, because they were wrong by the same factor and still agreed with
 * each other. The ACPI power management timer is now the reference.
 */
#define PIT_CHANNEL_ZERO_MODE_RATE_GENERATOR UINT8_C(0x34)
/*
 * Intel 8254 datasheet, control word: channel 0, lobyte/hibyte access, mode 0,
 * binary counting. Mode 0 is interrupt on terminal count. Its output goes low
 * when a count is written and high when that count expires, and it stays high
 * until the next count is written, which is what the level-triggered route
 * needs: a line that holds its assertion until software puts it down.
 */
#define PIT_CHANNEL_ZERO_MODE_TERMINAL_COUNT UINT8_C(0x30)
#define PIT_IRQ 0U
#define PIT_VECTOR INTERRUPT_PIC_MASTER_BASE
#define PIT_IOAPIC_VECTOR (INTERRUPT_IOAPIC_BASE + PIT_IRQ)

static volatile uint64_t tick_counter __attribute__((aligned(8)));
static uint32_t configured_frequency;
static uint16_t configured_divisor;
static bool running;
static bool retired;
static enum pit_route active_route;

static void load_counter(uint16_t divisor)
{
    cpu_out8(PIT_CHANNEL_ZERO, (uint8_t)(divisor & UINT16_C(0xFF)));
    cpu_out8(PIT_CHANNEL_ZERO, (uint8_t)((divisor >> 8U) & UINT16_C(0xFF)));
}

static void pit_interrupt_handler(struct interrupt_frame *frame, void *context)
{
    (void)frame;
    (void)context;
    ++tick_counter;

    /*
     * On the level-triggered route the 8254 is in mode 0 and its output is
     * still high, so the pin is still asserted while this runs. Writing the
     * next count lowers it, and that has to happen here rather than after the
     * end of interrupt: acknowledging the I/O APIC with the line still asserted
     * re-delivers the same interrupt immediately and the source never quiets.
     * This is the acknowledge-the-device step every level-triggered driver
     * owes its hardware; the 8254 is simply the one device this kernel has.
     */
    if (active_route == PIT_ROUTE_IO_APIC_LEVEL) {
        load_counter(configured_divisor);
    }
}

static bool route_uses_ioapic(enum pit_route route)
{
    return route == PIT_ROUTE_IO_APIC || route == PIT_ROUTE_IO_APIC_LEVEL;
}

static uint8_t route_vector(enum pit_route route)
{
    return route_uses_ioapic(route)
        ? (uint8_t)PIT_IOAPIC_VECTOR
        : (uint8_t)PIT_VECTOR;
}

/*
 * Unmask the timer at whichever interrupt controller owns it. The 8259 line
 * stays masked on the I/O APIC path, so exactly one controller can deliver
 * IRQ0 at a time and a duplicate tick would be a routing bug, not a race.
 */
static enum pit_status unmask_route(enum pit_route route)
{
    if (route_uses_ioapic(route)) {
        const enum ioapic_status status = ioapic_route_isa_irq_as(
            (uint8_t)PIT_IRQ,
            (uint8_t)PIT_IOAPIC_VECTOR,
            apic_get_state().id,
            route == PIT_ROUTE_IO_APIC_LEVEL
                ? IOAPIC_TRIGGER_FORCE_LEVEL
                : IOAPIC_TRIGGER_FIRMWARE
        );

        return status == IOAPIC_STATUS_OK
            ? PIT_STATUS_OK
            : PIT_STATUS_IOAPIC_FAILURE;
    }

    return pic_set_mask(PIT_IRQ, false) == PIC_STATUS_OK
        ? PIT_STATUS_OK
        : PIT_STATUS_PIC_FAILURE;
}

static enum pit_status mask_route(enum pit_route route)
{
    if (route_uses_ioapic(route)) {
        return ioapic_mask_isa_irq((uint8_t)PIT_IRQ) == IOAPIC_STATUS_OK
            ? PIT_STATUS_OK
            : PIT_STATUS_IOAPIC_FAILURE;
    }

    return pic_set_mask(PIT_IRQ, true) == PIC_STATUS_OK
        ? PIT_STATUS_OK
        : PIT_STATUS_PIC_FAILURE;
}

enum pit_status pit_start(uint32_t frequency_hz, enum pit_route route)
{
    uint32_t divisor;
    uint8_t vector;
    enum interrupt_status interrupt_status;
    enum pit_status status;

    if (retired) {
        return PIT_STATUS_RETIRED;
    }

    if (running) {
        return PIT_STATUS_ALREADY_RUNNING;
    }

    if (cpu_interrupts_enabled()) {
        return PIT_STATUS_INTERRUPTS_ENABLED;
    }

    if (route != PIT_ROUTE_LEGACY_PIC && route != PIT_ROUTE_IO_APIC &&
        route != PIT_ROUTE_IO_APIC_LEVEL) {
        return PIT_STATUS_BAD_ROUTE;
    }

    if (route_uses_ioapic(route) && !ioapic_is_initialized()) {
        return PIT_STATUS_IOAPIC_FAILURE;
    }

    if (frequency_hz == 0U || frequency_hz > PIT_INPUT_FREQUENCY) {
        return PIT_STATUS_BAD_FREQUENCY;
    }

    divisor = (PIT_INPUT_FREQUENCY + frequency_hz / 2U) / frequency_hz;

    if (divisor == 0U || divisor > UINT16_MAX) {
        return PIT_STATUS_BAD_FREQUENCY;
    }

    vector = route_vector(route);
    interrupt_status = interrupt_register_handler(
        vector,
        pit_interrupt_handler,
        NULL
    );

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        return PIT_STATUS_INTERRUPT_FAILURE;
    }

    cpu_out8(
        PIT_COMMAND,
        route == PIT_ROUTE_IO_APIC_LEVEL
            ? PIT_CHANNEL_ZERO_MODE_TERMINAL_COUNT
            : PIT_CHANNEL_ZERO_MODE_RATE_GENERATOR
    );

    tick_counter = 0U;
    configured_frequency = PIT_INPUT_FREQUENCY / divisor;
    configured_divisor = (uint16_t)divisor;

    /*
     * The route is published before the counter is loaded, because the handler
     * reads it to decide whether it owes the 8254 a reload and the first
     * terminal count can arrive as soon as the line is unmasked.
     */
    active_route = route;
    running = true;
    load_counter(configured_divisor);
    status = unmask_route(route);

    if (status != PIT_STATUS_OK) {
        running = false;
        configured_frequency = 0U;
        configured_divisor = 0U;
        (void)interrupt_unregister_handler(vector);
        return status;
    }

    return PIT_STATUS_OK;
}

enum pit_route pit_active_route(void)
{
    return active_route;
}

uint8_t pit_active_vector(void)
{
    return route_vector(active_route);
}

enum pit_status pit_stop(void)
{
    enum pit_status status;
    enum interrupt_status interrupt_status;

    if (!running) {
        return PIT_STATUS_NOT_RUNNING;
    }

    if (cpu_interrupts_enabled()) {
        return PIT_STATUS_INTERRUPTS_ENABLED;
    }

    status = mask_route(active_route);

    if (status != PIT_STATUS_OK) {
        return status;
    }

    interrupt_status = interrupt_unregister_handler(route_vector(active_route));

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        return PIT_STATUS_INTERRUPT_FAILURE;
    }

    running = false;
    configured_frequency = 0U;
    configured_divisor = 0U;
    return PIT_STATUS_OK;
}

/*
 * Retirement is one way. The counter is stopped if it was running, then its
 * redirection entry is masked even if it never ran, because firmware may have
 * left the line unmasked and a retired timer must not be able to deliver. The
 * 8259 is masked by its own retirement, so only the discovered route is touched
 * here; a machine whose I/O APIC was never initialized has no route to mask.
 */
enum pit_status pit_retire(void)
{
    if (retired) {
        return PIT_STATUS_RETIRED;
    }

    if (cpu_interrupts_enabled()) {
        return PIT_STATUS_INTERRUPTS_ENABLED;
    }

    if (running) {
        const enum pit_status status = pit_stop();

        if (status != PIT_STATUS_OK) {
            return status;
        }
    }

    if (ioapic_is_initialized() &&
        ioapic_mask_isa_irq((uint8_t)PIT_IRQ) != IOAPIC_STATUS_OK) {
        return PIT_STATUS_IOAPIC_FAILURE;
    }

    retired = true;
    return PIT_STATUS_OK;
}

bool pit_is_retired(void)
{
    return retired;
}

uint64_t pit_ticks(void)
{
    return tick_counter;
}

uint32_t pit_frequency(void)
{
    return configured_frequency;
}

bool pit_is_running(void)
{
    return running;
}

enum pit_status pit_wait_for_ticks(uint64_t tick_count)
{
    uint64_t target;

    if (!running) {
        return PIT_STATUS_NOT_RUNNING;
    }

    if (cpu_interrupts_enabled()) {
        return PIT_STATUS_INTERRUPTS_ENABLED;
    }

    if (tick_count == 0U) {
        return PIT_STATUS_OK;
    }

    if (tick_count > UINT64_MAX - tick_counter) {
        target = UINT64_MAX;
    } else {
        target = tick_counter + tick_count;
    }

    while (tick_counter < target) {
        cpu_enable_and_halt();
    }

    cpu_interrupt_disable();
    return PIT_STATUS_OK;
}

enum pit_status pit_wait_for_ticks_bounded(
    uint64_t tick_count,
    uint64_t bound_ns,
    uint64_t *elapsed_ns
)
{
    uint32_t start;
    uint32_t span = 0U;
    uint64_t target;

    if (elapsed_ns == NULL) {
        return PIT_STATUS_NULL_ARGUMENT;
    }

    *elapsed_ns = 0U;

    if (!running) {
        return PIT_STATUS_NOT_RUNNING;
    }

    if (cpu_interrupts_enabled()) {
        return PIT_STATUS_INTERRUPTS_ENABLED;
    }

    if (tick_count == 0U || bound_ns == 0U || bound_ns > PIT_MAX_WAIT_NS) {
        return PIT_STATUS_BAD_INTERVAL;
    }

    /* The bound is the reference clock, so there is no wait without one. */
    if (!pm_timer_is_present()) {
        return PIT_STATUS_NO_REFERENCE;
    }

    if (tick_count > UINT64_MAX - tick_counter) {
        target = UINT64_MAX;
    } else {
        target = tick_counter + tick_count;
    }

    start = pm_timer_read();
    cpu_interrupt_enable();

    /*
     * Spinning rather than halting is the whole point: an interrupt that never
     * arrives must still let this loop reach its deadline. The counter is
     * volatile because the handler that advances it runs between two of these
     * reads and the compiler has no way to know that.
     */
    while (tick_counter < target) {
        if (pm_timer_span(start, pm_timer_read(), &span) ==
                PM_TIMER_STATUS_OK &&
            pm_timer_ticks_to_nanoseconds(span) > bound_ns) {
            cpu_interrupt_disable();
            *elapsed_ns = pm_timer_ticks_to_nanoseconds(span);
            return PIT_STATUS_DELIVERY_TIMEOUT;
        }
    }

    cpu_interrupt_disable();

    /*
     * A span the reference cannot resolve leaves the elapsed time at zero
     * rather than failing here. That is not an error in this function - the
     * ticks did arrive - but it is what a line delivering as fast as the
     * processor can accept it looks like, so the caller compares the interval
     * against the one it asked for and names it.
     */
    if (pm_timer_span(start, pm_timer_read(), &span) == PM_TIMER_STATUS_OK) {
        *elapsed_ns = pm_timer_ticks_to_nanoseconds(span);
    }

    return PIT_STATUS_OK;
}

const char *pit_status_string(enum pit_status status)
{
    switch (status) {
    case PIT_STATUS_OK:
        return "ok";
    case PIT_STATUS_NULL_ARGUMENT:
        return "null PIT argument";
    case PIT_STATUS_ALREADY_RUNNING:
        return "PIT was started twice";
    case PIT_STATUS_NOT_RUNNING:
        return "PIT is not running";
    case PIT_STATUS_INTERRUPTS_ENABLED:
        return "PIT mutation requires interrupts disabled";
    case PIT_STATUS_BAD_FREQUENCY:
        return "PIT frequency cannot be represented";
    case PIT_STATUS_BAD_INTERVAL:
        return "PIT wait interval is outside the reference clock's range";
    case PIT_STATUS_INTERRUPT_FAILURE:
        return "PIT interrupt handler operation failed";
    case PIT_STATUS_PIC_FAILURE:
        return "PIT could not update the PIC mask";
    case PIT_STATUS_BAD_ROUTE:
        return "PIT was given an unknown interrupt route";
    case PIT_STATUS_IOAPIC_FAILURE:
        return "PIT could not route its interrupt through the I/O APIC";
    case PIT_STATUS_NO_REFERENCE:
        return "PIT has no reference clock to bound a wait with";
    case PIT_STATUS_DELIVERY_TIMEOUT:
        return "PIT route stopped delivering before its deadline";
    case PIT_STATUS_RETIRED:
        return "PIT is retired and no longer accepts mutation";
    default:
        return "unknown PIT status";
    }
}
