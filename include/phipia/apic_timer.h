/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_APIC_TIMER_H
#define PHIPIA_APIC_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include <phipia/interrupts.h>

#define APIC_TIMER_VECTOR ((uint8_t)INTERRUPT_LOCAL_APIC_BASE)

enum apic_timer_status {
    APIC_TIMER_STATUS_OK = 0,
    APIC_TIMER_STATUS_APIC_OFFLINE,
    APIC_TIMER_STATUS_INTERRUPTS_ENABLED,
    APIC_TIMER_STATUS_ALREADY_CALIBRATED,
    APIC_TIMER_STATUS_NOT_CALIBRATED,
    APIC_TIMER_STATUS_ALREADY_RUNNING,
    APIC_TIMER_STATUS_NOT_RUNNING,
    APIC_TIMER_STATUS_REFERENCE_FAILURE,
    APIC_TIMER_STATUS_NOT_COUNTING,
    APIC_TIMER_STATUS_COUNTER_EXHAUSTED,
    APIC_TIMER_STATUS_BAD_FREQUENCY,
    APIC_TIMER_STATUS_INTERRUPT_FAILURE,
    APIC_TIMER_STATUS_READBACK_MISMATCH,
    APIC_TIMER_STATUS_ALREADY_ARMED,
    APIC_TIMER_STATUS_NOT_ARMED,
    APIC_TIMER_STATUS_BAD_INTERVAL
};

/* Called from the timer interrupt when a one-shot deadline expires. */
typedef void (*apic_timer_expiry_t)(void *context);

enum apic_timer_status apic_timer_calibrate(void);
enum apic_timer_status apic_timer_start(uint32_t frequency_hz);
enum apic_timer_status apic_timer_stop(void);
enum apic_timer_status apic_timer_wait_for_ticks(uint64_t tick_count);
/*
 * Arm the timer to fire once, that many nanoseconds from now. One-shot and
 * periodic modes share the timer's one count register and one vector, so a timer
 * that is already running periodically refuses to be armed and the reverse.
 *
 * An interval the count register cannot express is refused rather than clamped:
 * at the calibrated rate the register tops out around a minute, and an interval
 * shorter than one count would arm a timer that never fires.
 */
enum apic_timer_status apic_timer_arm_oneshot(uint64_t nanoseconds);
enum apic_timer_status apic_timer_disarm(void);

/*
 * Install what the expiry of a one-shot deadline calls. The handler runs inside
 * the timer interrupt, so it must not block and must not arm from within itself
 * without expecting to be re-entered on the next expiry.
 */
enum apic_timer_status apic_timer_set_expiry_handler(
    apic_timer_expiry_t handler,
    void *context
);

bool apic_timer_is_armed(void);
uint64_t apic_timer_expiry_count(void);
uint64_t apic_timer_ticks(void);
uint64_t apic_timer_counts_per_second(void);
bool apic_timer_is_calibrated(void);
bool apic_timer_is_running(void);
bool apic_timer_self_test(void);
const char *apic_timer_status_string(enum apic_timer_status status);

#endif
