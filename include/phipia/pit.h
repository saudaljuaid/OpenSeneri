/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PIT_H
#define PHIPIA_PIT_H

#include <stdbool.h>
#include <stdint.h>

enum pit_status {
    PIT_STATUS_OK = 0,
    PIT_STATUS_NULL_ARGUMENT,
    PIT_STATUS_ALREADY_RUNNING,
    PIT_STATUS_NOT_RUNNING,
    PIT_STATUS_INTERRUPTS_ENABLED,
    PIT_STATUS_BAD_FREQUENCY,
    PIT_STATUS_BAD_INTERVAL,
    PIT_STATUS_INTERRUPT_FAILURE,
    PIT_STATUS_PIC_FAILURE,
    PIT_STATUS_BAD_ROUTE,
    PIT_STATUS_IOAPIC_FAILURE,
    PIT_STATUS_NO_REFERENCE,
    PIT_STATUS_DELIVERY_TIMEOUT,
    PIT_STATUS_RETIRED
};

/*
 * The same timer hardware reaches the processor by three different paths. Which
 * one is in use is the caller's decision, not a hidden default.
 *
 * The level-triggered path is the same redirection entry as the second, sampled
 * as a level rather than an edge, and it needs the 8254 in a different mode to
 * be worth sampling that way. Mode 2 pulses its output; mode 0 raises it at the
 * terminal count and holds it there until a new count is written. Holding it is
 * what makes the source level triggered in the way a device is: the line stays
 * asserted until software acknowledges the device, and only then can the end of
 * interrupt at the I/O APIC let the pin deliver again.
 */
enum pit_route {
    PIT_ROUTE_LEGACY_PIC = 0,
    PIT_ROUTE_IO_APIC,
    PIT_ROUTE_IO_APIC_LEVEL
};

/*
 * The longest interval pit_wait_for_ticks_bounded will wait for. The ACPI power
 * management timer supplies the bound and its narrowest counter is 24 bits at
 * 3.579545 MHz, so it wraps every 4.7 seconds and a span may only fold one
 * wrap. Two seconds keeps every bounded wait unambiguously inside that.
 */
#define PIT_MAX_WAIT_NS UINT64_C(2000000000)

enum pit_status pit_start(uint32_t frequency_hz, enum pit_route route);
enum pit_route pit_active_route(void);

/* The vector the active route delivers on, so callers need not rederive it. */
uint8_t pit_active_vector(void);
enum pit_status pit_stop(void);

/*
 * Take the 8254 off the machine for good. Nothing measures time against it any
 * more - the ACPI power management timer does that, and both derived clocks are
 * calibrated from it - so the counter is stopped, its redirection entry masked,
 * and the subsystem latched shut. Subsequent mutations are refused rather than
 * quietly re-arming a timer the kernel no longer reasons about.
 */
enum pit_status pit_retire(void);
bool pit_is_retired(void);
uint64_t pit_ticks(void);
uint32_t pit_frequency(void);
bool pit_is_running(void);
enum pit_status pit_wait_for_ticks(uint64_t tick_count);

/*
 * Wait for ticks with a deadline, and report how long the wait actually took.
 *
 * pit_wait_for_ticks halts until the next interrupt, which is the cheapest way
 * to wait and the wrong way to prove that a route delivers: a route that stops
 * delivering hangs there until something outside the kernel gives up, and a
 * timeout is not a diagnosis. This spins on the ACPI power management timer
 * instead, so a line that stops is a named status, and returns the measured
 * interval so a line that delivers far too fast is one too.
 */
enum pit_status pit_wait_for_ticks_bounded(
    uint64_t tick_count,
    uint64_t bound_ns,
    uint64_t *elapsed_ns
);

const char *pit_status_string(enum pit_status status);

#endif
