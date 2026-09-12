/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_INTERRUPT_VECTOR_H
#define PHIPIA_INTERRUPT_VECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/interrupts.h>

enum interrupt_vector_status {
    INTERRUPT_VECTOR_STATUS_OK = 0,
    INTERRUPT_VECTOR_STATUS_NULL_ARGUMENT,
    INTERRUPT_VECTOR_STATUS_ALREADY_INITIALIZED,
    INTERRUPT_VECTOR_STATUS_NOT_INITIALIZED,
    INTERRUPT_VECTOR_STATUS_INTERRUPTS_ENABLED,
    INTERRUPT_VECTOR_STATUS_RESERVED,
    INTERRUPT_VECTOR_STATUS_ALREADY_ALLOCATED,
    INTERRUPT_VECTOR_STATUS_EXHAUSTED,
    INTERRUPT_VECTOR_STATUS_DOUBLE_RELEASE,
    INTERRUPT_VECTOR_STATUS_STALE_ALLOCATION,
    INTERRUPT_VECTOR_STATUS_COUNT
};

struct interrupt_vector_allocation {
    uint8_t vector;
    uint32_t generation;
    bool active;
};

struct interrupt_vector_state {
    size_t capacity;
    size_t allocated;
    size_t free;
    bool active;
};

enum interrupt_vector_status interrupt_vector_initialize(void);
enum interrupt_vector_status interrupt_vector_allocate(
    struct interrupt_vector_allocation *allocation
);
enum interrupt_vector_status interrupt_vector_allocate_specific(
    uint8_t vector,
    struct interrupt_vector_allocation *allocation
);
enum interrupt_vector_status interrupt_vector_release(
    struct interrupt_vector_allocation *allocation
);
bool interrupt_vector_is_allocated(uint8_t vector);
struct interrupt_vector_state interrupt_vector_get_state(void);
bool interrupt_vector_self_test(void);
const char *interrupt_vector_status_string(
    enum interrupt_vector_status status
);

#endif
