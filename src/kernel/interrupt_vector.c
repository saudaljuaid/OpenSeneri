/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/apic.h>
#include <phipia/cpu.h>
#include <phipia/interrupt_vector.h>
#include <phipia/interrupts.h>

#define DYNAMIC_VECTOR_COUNT \
    ((size_t)INTERRUPT_DYNAMIC_LIMIT - (size_t)INTERRUPT_DYNAMIC_BASE)

static bool allocated[DYNAMIC_VECTOR_COUNT];
static uint32_t generations[DYNAMIC_VECTOR_COUNT];
static struct interrupt_vector_state state;

_Static_assert(INTERRUPT_EXCEPTION_COUNT <= INTERRUPT_PIC_MASTER_BASE,
    "exceptions overlap the PIC range");
_Static_assert(INTERRUPT_PIC_LIMIT <= INTERRUPT_IOAPIC_BASE,
    "PIC vectors overlap I/O APIC vectors");
_Static_assert(INTERRUPT_IOAPIC_LIMIT <= INTERRUPT_LOCAL_APIC_BASE,
    "I/O APIC vectors overlap local APIC vectors");
_Static_assert(INTERRUPT_LOCAL_APIC_LIMIT <= INTERRUPT_UNEXPECTED_TEST_VECTOR,
    "local APIC vectors overlap the unexpected-vector proof");
_Static_assert(INTERRUPT_UNEXPECTED_TEST_VECTOR < INTERRUPT_DYNAMIC_BASE,
    "unexpected-vector proof entered the dynamic range");
_Static_assert(INTERRUPT_DYNAMIC_LIMIT <= INTERRUPT_IST_TEST_VECTOR,
    "dynamic vectors overlap the IST proof vector");
_Static_assert(INTERRUPT_IST_TEST_VECTOR < APIC_SPURIOUS_VECTOR,
    "IST proof vector overlaps the local APIC spurious vector");

static bool vector_is_dynamic(uint8_t vector)
{
    return vector >= INTERRUPT_DYNAMIC_BASE &&
        vector < INTERRUPT_DYNAMIC_LIMIT;
}

static size_t vector_index(uint8_t vector)
{
    return (size_t)(vector - INTERRUPT_DYNAMIC_BASE);
}

enum interrupt_vector_status interrupt_vector_initialize(void)
{
    if (state.active) {
        return INTERRUPT_VECTOR_STATUS_ALREADY_INITIALIZED;
    }

    if (!interrupts_ready() || !apic_is_online()) {
        return INTERRUPT_VECTOR_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return INTERRUPT_VECTOR_STATUS_INTERRUPTS_ENABLED;
    }

    for (size_t index = 0U; index < DYNAMIC_VECTOR_COUNT; ++index) {
        allocated[index] = false;
        generations[index] = 0U;
    }

    state.capacity = DYNAMIC_VECTOR_COUNT;
    state.allocated = 0U;
    state.free = DYNAMIC_VECTOR_COUNT;
    state.active = true;
    return INTERRUPT_VECTOR_STATUS_OK;
}

enum interrupt_vector_status interrupt_vector_allocate_specific(
    uint8_t vector,
    struct interrupt_vector_allocation *allocation
)
{
    size_t index;

    if (allocation == NULL) {
        return INTERRUPT_VECTOR_STATUS_NULL_ARGUMENT;
    }

    allocation->vector = 0U;
    allocation->generation = 0U;
    allocation->active = false;

    if (!state.active) {
        return INTERRUPT_VECTOR_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return INTERRUPT_VECTOR_STATUS_INTERRUPTS_ENABLED;
    }

    if (!vector_is_dynamic(vector)) {
        return INTERRUPT_VECTOR_STATUS_RESERVED;
    }

    index = vector_index(vector);
    if (allocated[index]) {
        return INTERRUPT_VECTOR_STATUS_ALREADY_ALLOCATED;
    }

    ++generations[index];
    if (generations[index] == 0U) {
        ++generations[index];
    }
    allocated[index] = true;
    ++state.allocated;
    --state.free;
    allocation->vector = vector;
    allocation->generation = generations[index];
    allocation->active = true;
    return INTERRUPT_VECTOR_STATUS_OK;
}

enum interrupt_vector_status interrupt_vector_allocate(
    struct interrupt_vector_allocation *allocation
)
{
    if (allocation == NULL) {
        return INTERRUPT_VECTOR_STATUS_NULL_ARGUMENT;
    }

    if (!state.active) {
        allocation->active = false;
        return INTERRUPT_VECTOR_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        allocation->active = false;
        return INTERRUPT_VECTOR_STATUS_INTERRUPTS_ENABLED;
    }

    for (uint16_t vector = INTERRUPT_DYNAMIC_BASE;
         vector < INTERRUPT_DYNAMIC_LIMIT;
         ++vector) {
        if (!allocated[vector_index((uint8_t)vector)]) {
            return interrupt_vector_allocate_specific((uint8_t)vector,
                allocation);
        }
    }

    allocation->vector = 0U;
    allocation->generation = 0U;
    allocation->active = false;
    return INTERRUPT_VECTOR_STATUS_EXHAUSTED;
}

enum interrupt_vector_status interrupt_vector_release(
    struct interrupt_vector_allocation *allocation
)
{
    size_t index;

    if (allocation == NULL) {
        return INTERRUPT_VECTOR_STATUS_NULL_ARGUMENT;
    }

    if (!state.active) {
        return INTERRUPT_VECTOR_STATUS_NOT_INITIALIZED;
    }

    if (cpu_interrupts_enabled()) {
        return INTERRUPT_VECTOR_STATUS_INTERRUPTS_ENABLED;
    }

    if (!vector_is_dynamic(allocation->vector)) {
        return INTERRUPT_VECTOR_STATUS_RESERVED;
    }

    index = vector_index(allocation->vector);
    if (!allocation->active || !allocated[index]) {
        return INTERRUPT_VECTOR_STATUS_DOUBLE_RELEASE;
    }

    if (generations[index] != allocation->generation) {
        return INTERRUPT_VECTOR_STATUS_STALE_ALLOCATION;
    }

    allocated[index] = false;
    allocation->active = false;
    --state.allocated;
    ++state.free;
    return INTERRUPT_VECTOR_STATUS_OK;
}

bool interrupt_vector_is_allocated(uint8_t vector)
{
    return state.active && vector_is_dynamic(vector) &&
        allocated[vector_index(vector)];
}

struct interrupt_vector_state interrupt_vector_get_state(void)
{
    return state;
}

bool interrupt_vector_self_test(void)
{
    struct interrupt_vector_allocation allocations[DYNAMIC_VECTOR_COUNT];
    struct interrupt_vector_allocation extra;
    struct interrupt_vector_allocation reserved;

    if (!state.active || state.allocated != 0U ||
        state.free != DYNAMIC_VECTOR_COUNT) {
        return false;
    }

    if (interrupt_vector_allocate_specific(
            (uint8_t)(INTERRUPT_LOCAL_APIC_LIMIT - 1U), &reserved) !=
            INTERRUPT_VECTOR_STATUS_RESERVED ||
        interrupt_vector_allocate_specific(INTERRUPT_UNEXPECTED_TEST_VECTOR,
            &reserved) != INTERRUPT_VECTOR_STATUS_RESERVED ||
        interrupt_vector_allocate_specific(INTERRUPT_IST_TEST_VECTOR,
            &reserved) != INTERRUPT_VECTOR_STATUS_RESERVED ||
        interrupt_vector_allocate_specific(APIC_SPURIOUS_VECTOR, &reserved) !=
            INTERRUPT_VECTOR_STATUS_RESERVED) {
        return false;
    }

    for (size_t index = 0U; index < DYNAMIC_VECTOR_COUNT; ++index) {
        if (interrupt_vector_allocate(&allocations[index]) !=
                INTERRUPT_VECTOR_STATUS_OK ||
            allocations[index].vector !=
                (uint8_t)(INTERRUPT_DYNAMIC_BASE + index)) {
            return false;
        }
    }

    if (interrupt_vector_allocate(&extra) !=
            INTERRUPT_VECTOR_STATUS_EXHAUSTED ||
        interrupt_vector_release(&allocations[0]) !=
            INTERRUPT_VECTOR_STATUS_OK ||
        interrupt_vector_release(&allocations[0]) !=
            INTERRUPT_VECTOR_STATUS_DOUBLE_RELEASE ||
        interrupt_vector_allocate_specific(INTERRUPT_DYNAMIC_BASE, &extra) !=
            INTERRUPT_VECTOR_STATUS_OK) {
        return false;
    }

    if (interrupt_vector_release(&extra) != INTERRUPT_VECTOR_STATUS_OK) {
        return false;
    }

    for (size_t index = 1U; index < DYNAMIC_VECTOR_COUNT; ++index) {
        if (interrupt_vector_release(&allocations[index]) !=
                INTERRUPT_VECTOR_STATUS_OK) {
            return false;
        }
    }

    return state.allocated == 0U && state.free == DYNAMIC_VECTOR_COUNT;
}

const char *interrupt_vector_status_string(
    enum interrupt_vector_status status
)
{
    static const char *const messages[INTERRUPT_VECTOR_STATUS_COUNT] = {
        "ok",
        "null interrupt-vector argument",
        "dynamic vector allocator was initialized twice",
        "dynamic vector allocator or local APIC is not initialized",
        "dynamic vector mutation requires IF cleared",
        "interrupt vector is reserved",
        "interrupt vector is already allocated",
        "dynamic interrupt vectors are exhausted",
        "interrupt vector was released twice",
        "interrupt vector allocation generation is stale"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        INTERRUPT_VECTOR_STATUS_COUNT,
        "interrupt-vector status messages are out of sync");

    if (status < INTERRUPT_VECTOR_STATUS_OK ||
        status >= INTERRUPT_VECTOR_STATUS_COUNT) {
        return "unknown interrupt-vector status";
    }

    return messages[status];
}
