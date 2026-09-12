/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_POINTER_H
#define PHIPIA_POINTER_H

#include <stdbool.h>
#include <stdint.h>

#include <phipia/ui.h>

#define POINTER_PACKET_SIZE 3U

enum pointer_status {
    POINTER_STATUS_OK = 0,
    POINTER_STATUS_ABSENT,
    POINTER_STATUS_ALREADY_DECIDED,
    POINTER_STATUS_KEYBOARD_REQUIRED,
    POINTER_STATUS_INTERRUPTS_ENABLED,
    POINTER_STATUS_NO_IOAPIC,
    POINTER_STATUS_CONTROLLER_TIMEOUT,
    POINTER_STATUS_AUXILIARY_CLOCK_STUCK,
    POINTER_STATUS_PORT_TEST_FAILED,
    POINTER_STATUS_DEVICE_REFUSED,
    POINTER_STATUS_HANDLER_FAILURE,
    POINTER_STATUS_IOAPIC_FAILURE,
    POINTER_STATUS_NOT_AVAILABLE,
    POINTER_STATUS_BAD_BOUNDS,
    POINTER_STATUS_INJECTION_FAILURE
};

struct pointer_state {
    bool decided;
    bool present;
    bool active;
    bool left;
    bool middle;
    bool right;
    uint32_t x;
    uint32_t y;
    uint32_t bound_width;
    uint32_t bound_height;
    uint8_t packet_index;
    uint8_t packet[POINTER_PACKET_SIZE];
    uint64_t interrupts;
    uint64_t bytes;
    uint64_t packets;
    uint64_t movements;
    uint64_t button_transitions;
    uint64_t overflows;
    uint64_t desynchronizations;
};

/* Optional hardware decision. ABSENT is a valid, completed decision. */
enum pointer_status pointer_initialize(void);
enum pointer_status pointer_set_bounds(uint32_t width, uint32_t height);
struct pointer_state pointer_get_state(void);
bool pointer_is_present(void);

/* Real 8042 auxiliary-output injection used by the installed QEMU proof. */
enum pointer_status pointer_inject_packet(
    uint8_t flags,
    uint8_t delta_x,
    uint8_t delta_y
);

bool pointer_self_test(void);
const char *pointer_self_test_failure(void);
const char *pointer_status_string(enum pointer_status status);

#endif
