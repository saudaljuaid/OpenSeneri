/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * A deliberately small xHCI host foundation. Register names, bit positions,
 * context layouts and TRB encodings follow xHCI r1.2c sections 5, 6 and 7.
 * The one control request follows USB 2.0 sections 9.3, 9.4.3 and 9.6.1.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/clock.h>
#include <phipia/cpu.h>
#include <phipia/dma.h>
#include <phipia/interrupt_vector.h>
#include <phipia/msix.h>
#include <phipia/pci.h>
#include <phipia/pci_resource.h>
#include <phipia/xhci.h>

#define XHCI_CAPLENGTH UINT64_C(0x00)
#define XHCI_HCSPARAMS1 UINT64_C(0x04)
#define XHCI_HCSPARAMS2 UINT64_C(0x08)
#define XHCI_HCCPARAMS1 UINT64_C(0x10)
#define XHCI_DBOFF UINT64_C(0x14)
#define XHCI_RTSOFF UINT64_C(0x18)
#define XHCI_CAPABILITY_MINIMUM UINT64_C(0x20)

#define XHCI_USBCMD UINT64_C(0x00)
#define XHCI_USBSTS UINT64_C(0x04)
#define XHCI_PAGESIZE UINT64_C(0x08)
#define XHCI_CRCR UINT64_C(0x18)
#define XHCI_DCBAAP UINT64_C(0x30)
#define XHCI_CONFIG UINT64_C(0x38)
#define XHCI_PORTSC_BASE UINT64_C(0x400)
#define XHCI_PORT_REGISTER_BYTES UINT64_C(0x10)

#define XHCI_CMD_RUN UINT32_C(1)
#define XHCI_CMD_RESET UINT32_C(1U << 1)
#define XHCI_CMD_INTERRUPTS UINT32_C(1U << 2)
#define XHCI_STS_HALTED UINT32_C(1)
#define XHCI_STS_EVENT_INTERRUPT UINT32_C(1U << 3)
#define XHCI_STS_PORT_CHANGE UINT32_C(1U << 4)
#define XHCI_STS_NOT_READY UINT32_C(1U << 11)
#define XHCI_STS_HOST_ERROR UINT32_C(1U << 12)

#define XHCI_PORT_CONNECTED UINT32_C(1)
#define XHCI_PORT_ENABLED UINT32_C(1U << 1)
#define XHCI_PORT_RESET UINT32_C(1U << 4)
#define XHCI_PORT_SPEED_SHIFT 10U
#define XHCI_PORT_SPEED_MASK UINT32_C(0xF)
#define XHCI_PORT_POWER UINT32_C(1U << 9)
#define XHCI_PORT_CHANGE_BITS UINT32_C(0x00FE0000)
#define XHCI_PORT_RESET_CHANGE UINT32_C(1U << 21)

#define XHCI_INTERRUPTER_BASE UINT64_C(0x20)
#define XHCI_IMAN UINT64_C(0x00)
#define XHCI_IMOD UINT64_C(0x04)
#define XHCI_ERSTSZ UINT64_C(0x08)
#define XHCI_ERSTBA UINT64_C(0x10)
#define XHCI_ERDP UINT64_C(0x18)
#define XHCI_IMAN_PENDING UINT32_C(1)
#define XHCI_IMAN_ENABLE UINT32_C(1U << 1)
#define XHCI_ERDP_BUSY UINT64_C(1U << 3)

#define XHCI_HCC_AC64 UINT32_C(1)
#define XHCI_HCC_CONTEXT_64 UINT32_C(1U << 2)
#define XHCI_HCC_XECP_SHIFT 16U
#define XHCI_CONTEXT_32_BYTES 32U
#define XHCI_CONTEXT_64_BYTES 64U

#define XHCI_EXT_LEGACY UINT8_C(1)
#define XHCI_EXT_PROTOCOL UINT8_C(2)
#define XHCI_EXT_LIMIT 64U
#define XHCI_PROTOCOL_LIMIT 16U
#define XHCI_LEGACY_BIOS_BYTE UINT64_C(2)
#define XHCI_LEGACY_OS_BYTE UINT64_C(3)
#define XHCI_LEGACY_OWNED UINT8_C(1)
#define XHCI_PROTOCOL_NAME UINT32_C(0x20425355)

#define XHCI_TRB_BYTES UINT64_C(16)
#define XHCI_TRB_CYCLE UINT32_C(1)
#define XHCI_TRB_TOGGLE_CYCLE UINT32_C(1U << 1)
#define XHCI_TRB_INTERRUPT UINT32_C(1U << 5)
#define XHCI_TRB_IMMEDIATE UINT32_C(1U << 6)
#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_TRB_TYPE_MASK UINT32_C(0x3F)
#define XHCI_TRB_DIRECTION UINT32_C(1U << 16)
#define XHCI_TRB_SETUP_IN UINT32_C(3U << 16)
#define XHCI_TRB_SLOT_SHIFT 24U
#define XHCI_TRB_ENDPOINT_SHIFT 16U
#define XHCI_TRB_COMPLETION_SHIFT 24U
#define XHCI_TRB_LENGTH_MASK UINT32_C(0x00FFFFFF)
#define XHCI_TRB_TYPE_SETUP UINT8_C(2)
#define XHCI_TRB_TYPE_DATA UINT8_C(3)
#define XHCI_TRB_TYPE_STATUS UINT8_C(4)
#define XHCI_TRB_TYPE_LINK UINT8_C(6)
#define XHCI_TRB_TYPE_ENABLE_SLOT UINT8_C(9)
#define XHCI_TRB_TYPE_DISABLE_SLOT UINT8_C(10)
#define XHCI_TRB_TYPE_ADDRESS_DEVICE UINT8_C(11)
#define XHCI_TRB_TYPE_TRANSFER_EVENT UINT8_C(32)
#define XHCI_TRB_TYPE_COMMAND_EVENT UINT8_C(33)
#define XHCI_TRB_TYPE_PORT_EVENT UINT8_C(34)
#define XHCI_COMPLETION_SUCCESS UINT8_C(1)
#define XHCI_COMPLETION_SHORT_PACKET UINT8_C(13)

#define XHCI_ADMIN_DCBAA_OFFSET UINT64_C(0)
#define XHCI_ADMIN_ERST_OFFSET UINT64_C(64)
#define XHCI_ADMIN_SCRATCH_ARRAY_OFFSET UINT64_C(128)
#define XHCI_EVENT_SEGMENT_TRBS 64U
#define XHCI_COMMAND_USABLE_TRBS (XHCI_RING_TRB_COUNT - 1U)
#define XHCI_ENDPOINT_ID_ZERO UINT8_C(1)
#define XHCI_PROOF_SLOT UINT8_C(1)
#define XHCI_DMA_MAX_32 UINT64_C(0xFFFFFFFF)
#define XHCI_SENTINEL UINT8_C(0xA5)
#define XHCI_HCIVERSION_MIN UINT16_C(0x0100)
#define XHCI_HCIVERSION_MAX UINT16_C(0x0120)

#define XHCI_READY_TIMEOUT_NS UINT64_C(1000000000)
#define XHCI_HALT_TIMEOUT_NS UINT64_C(100000000)
#define XHCI_RESET_TIMEOUT_NS UINT64_C(1000000000)
#define XHCI_LEGACY_TIMEOUT_NS UINT64_C(1000000000)
#define XHCI_PORT_TIMEOUT_NS UINT64_C(1000000000)
#define XHCI_COMMAND_TIMEOUT_NS UINT64_C(2000000000)
#define XHCI_TRANSFER_TIMEOUT_NS UINT64_C(2000000000)

struct xhci_trb {
    uint32_t parameter_low;
    uint32_t parameter_high;
    uint32_t status;
    uint32_t control;
};

struct xhci_protocol {
    uint8_t major;
    uint8_t first_port;
    uint8_t port_count;
    uint8_t slot_type;
};

enum xhci_expect_kind {
    XHCI_EXPECT_NONE = 0,
    XHCI_EXPECT_COMMAND,
    XHCI_EXPECT_TRANSFER
};

struct xhci_expectation {
    volatile enum xhci_expect_kind kind;
    volatile bool done;
    volatile enum xhci_status result;
    uint64_t pointer;
    uint8_t trb_type;
    uint8_t slot;
    uint8_t endpoint;
};

struct xhci_runtime {
    struct xhci_controller_claim claim;
    struct xhci_register_regions registers;
    struct xhci_protocol protocols[XHCI_PROTOCOL_LIMIT];
    size_t protocol_count;
    uint16_t version;
    uint8_t max_slots;
    uint8_t max_ports;
    uint16_t max_interrupters;
    size_t context_bytes;
    bool address_64;
    volatile uint8_t *capability;
    volatile uint8_t *operational;
    volatile uint8_t *doorbells;
    volatile uint8_t *interrupter;
    struct dma_allocation admin;
    enum xhci_dma_object_state admin_state;
    struct xhci_dma_ring command;
    struct xhci_dma_ring event;
    struct xhci_erst erst;
    struct xhci_dcbaa dcbaa;
    struct xhci_scratchpads scratchpads;
    struct xhci_device_contexts contexts;
    struct xhci_endpoint_zero_transfer endpoint;
    struct xhci_root_port port;
    struct xhci_slot slot;
    struct xhci_interrupt_binding interrupt;
    struct xhci_expectation expectation;
    bool command_slot_cpu[XHCI_RING_TRB_COUNT];
    bool control_slot_cpu[XHCI_RING_TRB_COUNT];
    bool bus_master_enabled;
    bool interrupt_sources_enabled;
    bool teardown_started;
    bool handler_saw_freed_state;
    bool wrong_vector;
    bool ownership_failure;
    size_t ignored_events;
    uint64_t interrupt_count;
};

static struct xhci_descriptor_proof installed_proof;
static uint64_t controller_generation;

static void zero_bytes(void *pointer, uint64_t length)
{
    uint8_t *bytes = pointer;

    for (uint64_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static uint8_t mmio_read8(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint8_t *)(void *)(base + offset);
}

static uint32_t mmio_read32(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint32_t *)(void *)(base + offset);
}

static void mmio_write8(
    volatile uint8_t *base,
    uint64_t offset,
    uint8_t value
)
{
    *(volatile uint8_t *)(void *)(base + offset) = value;
}

static void mmio_write32(
    volatile uint8_t *base,
    uint64_t offset,
    uint32_t value
)
{
    *(volatile uint32_t *)(void *)(base + offset) = value;
}

static void mmio_write64(
    volatile uint8_t *base,
    uint64_t offset,
    uint64_t value
)
{
    *(volatile uint64_t *)(void *)(base + offset) = value;
}

static uint64_t physical_of(const struct dma_allocation *allocation)
{
    return (uint64_t)allocation->frames.physical_base;
}

static bool add_checked(uint64_t left, uint64_t right, uint64_t *sum)
{
    if (sum == NULL || left > UINT64_MAX - right) {
        return false;
    }
    *sum = left + right;
    return true;
}

static enum xhci_status validate_span(
    uint64_t bar_size,
    uint64_t offset,
    uint64_t length,
    uint64_t alignment,
    struct xhci_register_span *span
)
{
    uint64_t end;

    if (span == NULL || length == 0U) {
        return XHCI_STATUS_REGISTER_OUTSIDE_BAR;
    }
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U ||
        (offset & (alignment - 1U)) != 0U) {
        return XHCI_STATUS_REGISTER_ALIGNMENT;
    }
    if (!add_checked(offset, length, &end)) {
        return XHCI_STATUS_REGISTER_OVERFLOW;
    }
    if (end > bar_size) {
        return XHCI_STATUS_REGISTER_OUTSIDE_BAR;
    }
    span->offset = offset;
    span->length = length;
    span->valid = true;
    return XHCI_STATUS_OK;
}

static bool deadline_reached(uint64_t now, uint64_t deadline)
{
    return now >= deadline;
}

static enum xhci_status validate_context_size(size_t context_bytes)
{
    return context_bytes == XHCI_CONTEXT_32_BYTES ||
        context_bytes == XHCI_CONTEXT_64_BYTES ? XHCI_STATUS_OK :
            XHCI_STATUS_UNSUPPORTED_CONTEXT_SIZE;
}

static enum xhci_status validate_page_sizes(uint32_t page_sizes)
{
    return (page_sizes & UINT32_C(1)) != 0U ? XHCI_STATUS_OK :
        XHCI_STATUS_UNSUPPORTED_PAGE_SIZE;
}

static enum xhci_status validate_address_width(
    bool address_64,
    uint64_t base,
    uint64_t length
)
{
    uint64_t end;

    if (length == 0U || !add_checked(base, length - 1U, &end)) {
        return XHCI_STATUS_DMA_LAYOUT_FAILURE;
    }
    if (!address_64 && end > XHCI_DMA_MAX_32) {
        return XHCI_STATUS_UNSUPPORTED_ADDRESS_WIDTH;
    }
    return XHCI_STATUS_OK;
}

static enum xhci_status decode_scratchpad_count(
    uint32_t params2,
    size_t *scratchpad_count
)
{
    size_t scratchpads;

    if (scratchpad_count == NULL) {
        return XHCI_STATUS_NULL_ARGUMENT;
    }
    scratchpads = ((size_t)((params2 >> 21U) & 0x1FU) << 5U) |
        (size_t)((params2 >> 27U) & 0x1FU);
    if (scratchpads > XHCI_MAX_SCRATCHPADS ||
        scratchpads > SIZE_MAX / PHIPIA_PAGE_SIZE) {
        return XHCI_STATUS_SCRATCHPAD_OVERFLOW;
    }
    *scratchpad_count = scratchpads;
    return XHCI_STATUS_OK;
}

static enum xhci_status validate_ring_geometry(
    uint64_t base,
    uint64_t byte_length,
    size_t trb_count
)
{
    uint64_t required;

    if ((base & (XHCI_TRB_BYTES - 1U)) != 0U) {
        return XHCI_STATUS_RING_ALIGNMENT;
    }
    if (trb_count < 2U || trb_count > UINT64_MAX / XHCI_TRB_BYTES) {
        return XHCI_STATUS_RING_CONTAINMENT;
    }
    required = (uint64_t)trb_count * XHCI_TRB_BYTES;
    if (required > byte_length || !add_checked(base, required, &required)) {
        return XHCI_STATUS_RING_CONTAINMENT;
    }
    return XHCI_STATUS_OK;
}

static bool spans_overlap(
    const struct xhci_register_span *left,
    const struct xhci_register_span *right
)
{
    return left->valid && right->valid &&
        left->offset < right->offset + right->length &&
        right->offset < left->offset + left->length;
}

static enum xhci_status transition(
    struct xhci_controller_claim *claim,
    enum xhci_controller_state next
)
{
    enum xhci_controller_state current;
    bool valid = false;

    if (claim == NULL || next <= XHCI_CONTROLLER_UNINITIALIZED ||
        next >= XHCI_CONTROLLER_STATE_COUNT) {
        return XHCI_STATUS_TRANSITION_INVALID;
    }
    current = claim->state;
    if (current == next) {
        return XHCI_STATUS_TRANSITION_REPEATED;
    }
    if (next < current && next != XHCI_CONTROLLER_RELEASED) {
        return XHCI_STATUS_TRANSITION_REVERSED;
    }
    if (current == XHCI_CONTROLLER_UNINITIALIZED &&
        next == XHCI_CONTROLLER_DISCOVERED) {
        valid = true;
    } else if (current == XHCI_CONTROLLER_DISCOVERED &&
        next == XHCI_CONTROLLER_CLAIMED) {
        valid = true;
    } else if (current == XHCI_CONTROLLER_CLAIMED &&
        next == XHCI_CONTROLLER_PREPARED) {
        valid = true;
    } else if (current == XHCI_CONTROLLER_PREPARED &&
        next == XHCI_CONTROLLER_RUNNING) {
        valid = true;
    } else if (current == XHCI_CONTROLLER_RUNNING &&
        next == XHCI_CONTROLLER_STOPPING) {
        valid = true;
    } else if ((current == XHCI_CONTROLLER_DISCOVERED ||
            current == XHCI_CONTROLLER_CLAIMED ||
            current == XHCI_CONTROLLER_PREPARED ||
            current == XHCI_CONTROLLER_STOPPING) &&
        next == XHCI_CONTROLLER_RELEASED) {
        valid = true;
    }
    if (!valid) {
        return XHCI_STATUS_TRANSITION_INVALID;
    }
    claim->state = next;
    return XHCI_STATUS_OK;
}

static bool wait_mask(
    volatile uint8_t *base,
    uint64_t offset,
    uint32_t mask,
    uint32_t expected,
    uint64_t interval_ns
)
{
    const uint64_t start = clock_monotonic_ns();
    uint64_t deadline;

    if (!add_checked(start, interval_ns, &deadline)) {
        return false;
    }
    while ((mmio_read32(base, offset) & mask) != expected) {
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return false;
        }
        __asm__ volatile ("" : : : "memory");
    }
    return true;
}

static const struct pci_function *discover_controller(
    enum xhci_status *result
)
{
    const struct pci_function *found = NULL;

    *result = XHCI_STATUS_ABSENT;
    for (size_t index = 0U; index < pci_function_count(); ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function != NULL &&
            function->class_code == XHCI_PCI_CLASS_SERIAL_BUS &&
            function->subclass == XHCI_PCI_SUBCLASS_USB &&
            function->prog_if == XHCI_PCI_PROGRAMMING_INTERFACE) {
            if (found != NULL) {
                *result = XHCI_STATUS_MULTIPLE_CONTROLLERS;
                return NULL;
            }
            found = function;
            *result = XHCI_STATUS_OK;
        }
    }
    return found;
}

static bool offset_already_visited(
    const uint64_t *visited,
    size_t visited_count,
    uint64_t offset
)
{
    for (size_t index = 0U; index < visited_count; ++index) {
        if (visited[index] == offset) {
            return true;
        }
    }
    return false;
}

static enum xhci_status walk_extended_capabilities(
    struct xhci_runtime *controller,
    uint64_t first
)
{
    uint64_t visited[XHCI_EXT_LIMIT] = {0};
    struct xhci_register_span spans[XHCI_EXT_LIMIT] = {0};
    size_t visited_count = 0U;
    uint64_t offset = first;

    if (offset == 0U) {
        return XHCI_STATUS_OK;
    }
    while (offset != 0U) {
        uint32_t header;
        uint8_t identifier;
        uint8_t next;
        uint64_t end;
        uint64_t span_length = sizeof(uint32_t);
        struct xhci_register_span current = {0};

        if ((offset & 3U) != 0U ||
            !add_checked(offset, sizeof(uint32_t), &end) ||
            end > controller->registers.bar_size) {
            return XHCI_STATUS_EXTENDED_CAPABILITY_RANGE;
        }
        if (visited_count == XHCI_EXT_LIMIT) {
            return XHCI_STATUS_EXTENDED_CAPABILITY_LOOP;
        }
        if (offset_already_visited(visited, visited_count, offset)) {
            return XHCI_STATUS_EXTENDED_CAPABILITY_LOOP;
        }
        visited[visited_count++] = offset;
        header = mmio_read32(controller->capability, offset);
        identifier = (uint8_t)header;
        next = (uint8_t)(header >> 8U);

        if (identifier == XHCI_EXT_PROTOCOL) {
            uint32_t ports;
            struct xhci_protocol *protocol;

            if (!add_checked(offset, UINT64_C(16), &end) ||
                end > controller->registers.bar_size ||
                controller->protocol_count == XHCI_PROTOCOL_LIMIT ||
                mmio_read32(controller->capability, offset + 4U) !=
                    XHCI_PROTOCOL_NAME) {
                return XHCI_STATUS_EXTENDED_CAPABILITY_RANGE;
            }
            span_length = UINT64_C(16);
            ports = mmio_read32(controller->capability, offset + 8U);
            protocol = &controller->protocols[controller->protocol_count++];
            protocol->major = (uint8_t)(header >> 24U);
            protocol->first_port = (uint8_t)ports;
            protocol->port_count = (uint8_t)(ports >> 8U);
            protocol->slot_type = (uint8_t)mmio_read32(
                controller->capability, offset + 12U) & UINT8_C(0x1F);
            if (protocol->first_port == 0U || protocol->port_count == 0U ||
                protocol->first_port > controller->max_ports ||
                protocol->port_count > (uint8_t)(controller->max_ports -
                    protocol->first_port + 1U)) {
                return XHCI_STATUS_EXTENDED_CAPABILITY_RANGE;
            }
        } else if (identifier == XHCI_EXT_LEGACY) {
            if (!add_checked(offset, UINT64_C(8), &end) ||
                end > controller->registers.bar_size) {
                return XHCI_STATUS_EXTENDED_CAPABILITY_RANGE;
            }
            span_length = UINT64_C(8);
        }
        current.offset = offset;
        current.length = span_length;
        current.valid = true;
        if (spans_overlap(&current, &controller->registers.operational) ||
            spans_overlap(&current, &controller->registers.doorbells) ||
            spans_overlap(&current, &controller->registers.runtime)) {
            return XHCI_STATUS_REGISTER_OVERLAP;
        }
        for (size_t index = 0U; index < visited_count - 1U; ++index) {
            if (spans_overlap(&current, &spans[index])) {
                return XHCI_STATUS_REGISTER_OVERLAP;
            }
        }
        spans[visited_count - 1U] = current;
        ++controller->registers.extended_capability_count;
        if (next == 0U) {
            break;
        }
        if ((uint64_t)next * 4U < span_length ||
            !add_checked(offset, (uint64_t)next * 4U, &end) || end <= offset) {
            return XHCI_STATUS_EXTENDED_CAPABILITY_NON_PROGRESS;
        }
        offset = end;
    }
    return XHCI_STATUS_OK;
}

static enum xhci_status validate_registers(struct xhci_runtime *controller)
{
    uint8_t cap_length;
    uint32_t params1;
    uint32_t hcc;
    uint64_t operation_end;
    uint64_t doorbell_offset;
    uint64_t runtime_offset;
    uint64_t extended_offset;
    volatile void *pointer = NULL;
    enum xhci_status status;

    if (controller->registers.mapping == NULL ||
        controller->registers.mapping->size < XHCI_CAPABILITY_MINIMUM ||
        pci_mmio_subregion(controller->registers.mapping, 0U,
            controller->registers.mapping->size, &pointer) !=
                PCI_RESOURCE_STATUS_OK) {
        return XHCI_STATUS_REGISTER_OUTSIDE_BAR;
    }
    controller->capability = pointer;
    controller->registers.bar_size = controller->registers.mapping->size;
    cap_length = mmio_read8(controller->capability, XHCI_CAPLENGTH);
    params1 = mmio_read32(controller->capability, XHCI_HCSPARAMS1);
    hcc = mmio_read32(controller->capability, XHCI_HCCPARAMS1);
    controller->max_slots = (uint8_t)params1;
    controller->max_interrupters = (uint16_t)((params1 >> 8U) & 0x7FFU);
    controller->max_ports = (uint8_t)(params1 >> 24U);
    doorbell_offset = mmio_read32(controller->capability, XHCI_DBOFF) &
        UINT64_C(0xFFFFFFFC);
    runtime_offset = mmio_read32(controller->capability, XHCI_RTSOFF) &
        UINT64_C(0xFFFFFFE0);
    extended_offset = (uint64_t)(hcc >> XHCI_HCC_XECP_SHIFT) * 4U;

    status = validate_span(controller->registers.bar_size, 0U, cap_length,
        4U, &controller->registers.capability);
    if (status != XHCI_STATUS_OK || cap_length < XHCI_CAPABILITY_MINIMUM) {
        return status == XHCI_STATUS_OK ? XHCI_STATUS_REGISTER_OUTSIDE_BAR :
            status;
    }
    if (!add_checked(XHCI_PORTSC_BASE,
            (uint64_t)controller->max_ports * XHCI_PORT_REGISTER_BYTES,
            &operation_end) || operation_end < cap_length) {
        return XHCI_STATUS_REGISTER_OVERFLOW;
    }
    status = validate_span(controller->registers.bar_size, cap_length,
        operation_end, 4U, &controller->registers.operational);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    status = validate_span(controller->registers.bar_size, doorbell_offset,
        ((uint64_t)controller->max_slots + 1U) * sizeof(uint32_t), 4U,
        &controller->registers.doorbells);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    status = validate_span(controller->registers.bar_size, runtime_offset,
        XHCI_INTERRUPTER_BASE + UINT64_C(32), 32U,
        &controller->registers.runtime);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    status = validate_span(controller->registers.bar_size,
        runtime_offset + XHCI_INTERRUPTER_BASE, UINT64_C(32), 32U,
        &controller->registers.interrupter);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    if (spans_overlap(&controller->registers.capability,
            &controller->registers.operational) ||
        spans_overlap(&controller->registers.capability,
            &controller->registers.doorbells) ||
        spans_overlap(&controller->registers.capability,
            &controller->registers.runtime) ||
        spans_overlap(&controller->registers.operational,
            &controller->registers.doorbells) ||
        spans_overlap(&controller->registers.operational,
            &controller->registers.runtime) ||
        spans_overlap(&controller->registers.doorbells,
            &controller->registers.runtime)) {
        return XHCI_STATUS_REGISTER_OVERLAP;
    }
    controller->registers.capability.base = controller->capability;
    controller->operational = controller->capability + cap_length;
    controller->doorbells = controller->capability + doorbell_offset;
    controller->interrupter = controller->capability + runtime_offset +
        XHCI_INTERRUPTER_BASE;
    controller->registers.operational.base = controller->operational;
    controller->registers.doorbells.base = controller->doorbells;
    controller->registers.runtime.base = controller->capability + runtime_offset;
    controller->registers.interrupter.base = controller->interrupter;
    status = walk_extended_capabilities(controller, extended_offset);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    controller->registers.valid = true;
    return XHCI_STATUS_OK;
}

static enum xhci_status legacy_handoff(struct xhci_runtime *controller)
{
    uint32_t hcc = mmio_read32(controller->capability, XHCI_HCCPARAMS1);
    uint64_t offset = (uint64_t)(hcc >> XHCI_HCC_XECP_SHIFT) * 4U;

    for (size_t walked = 0U; offset != 0U && walked < XHCI_EXT_LIMIT;
            ++walked) {
        uint32_t header = mmio_read32(controller->capability, offset);
        uint8_t next = (uint8_t)(header >> 8U);

        if ((uint8_t)header == XHCI_EXT_LEGACY) {
            uint8_t os_owned = mmio_read8(controller->capability,
                offset + XHCI_LEGACY_OS_BYTE);
            uint64_t deadline;

            mmio_write8(controller->capability,
                offset + XHCI_LEGACY_OS_BYTE,
                (uint8_t)(os_owned | XHCI_LEGACY_OWNED));
            if (!add_checked(clock_monotonic_ns(), XHCI_LEGACY_TIMEOUT_NS,
                    &deadline)) {
                return XHCI_STATUS_LEGACY_OWNERSHIP_TIMEOUT;
            }
            while ((mmio_read8(controller->capability,
                    offset + XHCI_LEGACY_BIOS_BYTE) &
                    XHCI_LEGACY_OWNED) != 0U) {
                if (deadline_reached(clock_monotonic_ns(), deadline)) {
                    return XHCI_STATUS_LEGACY_OWNERSHIP_TIMEOUT;
                }
            }
            if ((mmio_read8(controller->capability,
                    offset + XHCI_LEGACY_OS_BYTE) &
                    XHCI_LEGACY_OWNED) == 0U) {
                return XHCI_STATUS_LEGACY_OWNERSHIP_TIMEOUT;
            }
        }
        if (next == 0U) {
            return XHCI_STATUS_OK;
        }
        offset += (uint64_t)next * 4U;
    }
    return offset == 0U ? XHCI_STATUS_OK :
        XHCI_STATUS_EXTENDED_CAPABILITY_LOOP;
}

static enum xhci_status halt_and_reset(struct xhci_runtime *controller)
{
    uint32_t command;

    if (!wait_mask(controller->operational, XHCI_USBSTS,
            XHCI_STS_NOT_READY, 0U, XHCI_READY_TIMEOUT_NS)) {
        return XHCI_STATUS_CONTROLLER_NOT_READY_TIMEOUT;
    }
    command = mmio_read32(controller->operational, XHCI_USBCMD);
    command &= ~(XHCI_CMD_RUN | XHCI_CMD_INTERRUPTS);
    mmio_write32(controller->operational, XHCI_USBCMD, command);
    if (!wait_mask(controller->operational, XHCI_USBSTS,
            XHCI_STS_HALTED, XHCI_STS_HALTED, XHCI_HALT_TIMEOUT_NS)) {
        return XHCI_STATUS_HALT_TIMEOUT;
    }
    mmio_write32(controller->operational, XHCI_USBCMD,
        command | XHCI_CMD_RESET);
    if (!wait_mask(controller->operational, XHCI_USBCMD,
            XHCI_CMD_RESET, 0U, XHCI_RESET_TIMEOUT_NS) ||
        !wait_mask(controller->operational, XHCI_USBSTS,
            XHCI_STS_NOT_READY, 0U, XHCI_RESET_TIMEOUT_NS)) {
        return XHCI_STATUS_RESET_TIMEOUT;
    }
    return XHCI_STATUS_OK;
}

static enum xhci_status validate_capabilities(
    struct xhci_runtime *controller,
    size_t *scratchpad_count
)
{
    uint32_t params2;
    uint32_t hcc;
    uint32_t page_sizes;
    size_t scratchpads;

    controller->version = (uint16_t)(mmio_read32(controller->capability,
        XHCI_CAPLENGTH) >> 16U);
    if (controller->version < XHCI_HCIVERSION_MIN ||
        controller->version > XHCI_HCIVERSION_MAX) {
        return XHCI_STATUS_UNSUPPORTED_VERSION;
    }
    if (controller->max_slots == 0U) {
        return XHCI_STATUS_UNSUPPORTED_SLOTS;
    }
    if (controller->max_ports == 0U) {
        return XHCI_STATUS_UNSUPPORTED_PORTS;
    }
    if (controller->max_interrupters == 0U) {
        return XHCI_STATUS_UNSUPPORTED_INTERRUPTERS;
    }
    page_sizes = mmio_read32(controller->operational, XHCI_PAGESIZE);
    if (validate_page_sizes(page_sizes) != XHCI_STATUS_OK) {
        return XHCI_STATUS_UNSUPPORTED_PAGE_SIZE;
    }
    hcc = mmio_read32(controller->capability, XHCI_HCCPARAMS1);
    controller->context_bytes = (hcc & XHCI_HCC_CONTEXT_64) != 0U ?
        XHCI_CONTEXT_64_BYTES : XHCI_CONTEXT_32_BYTES;
    if (validate_context_size(controller->context_bytes) != XHCI_STATUS_OK) {
        return XHCI_STATUS_UNSUPPORTED_CONTEXT_SIZE;
    }
    controller->address_64 = (hcc & XHCI_HCC_AC64) != 0U;
    params2 = mmio_read32(controller->capability, XHCI_HCSPARAMS2);
    if (decode_scratchpad_count(params2, &scratchpads) != XHCI_STATUS_OK) {
        return XHCI_STATUS_SCRATCHPAD_OVERFLOW;
    }
    *scratchpad_count = scratchpads;
    return XHCI_STATUS_OK;
}

static enum xhci_status allocate_page(
    struct dma_allocation *allocation,
    size_t pages
)
{
    const struct dma_request request = {
        .page_count = pages,
        .alignment = PHIPIA_PAGE_SIZE,
        .maximum_physical_address = XHCI_DMA_MAX_32
    };

    if (pages == 0U || dma_allocate(&request, allocation) != DMA_STATUS_OK) {
        return XHCI_STATUS_DMA_ALLOCATION_FAILURE;
    }
    if ((physical_of(allocation) & (PHIPIA_PAGE_SIZE - 1U)) != 0U ||
        physical_of(allocation) > XHCI_DMA_MAX_32 ||
        allocation->byte_length < (uint64_t)pages * PHIPIA_PAGE_SIZE) {
        return XHCI_STATUS_DMA_LAYOUT_FAILURE;
    }
    zero_bytes(allocation->cpu_address, allocation->byte_length);
    return XHCI_STATUS_OK;
}

static uint64_t trb_pointer(const struct xhci_trb *trb);
static enum xhci_status validate_cpu_access(
    enum xhci_dma_object_state state
);

static void write_trb(
    struct xhci_trb *trb,
    uint64_t parameter,
    uint32_t status,
    uint32_t control
)
{
    trb->parameter_low = (uint32_t)parameter;
    trb->parameter_high = (uint32_t)(parameter >> 32U);
    trb->status = status;
    trb->control = control;
}

static enum xhci_status validate_link_trb(const struct xhci_dma_ring *ring)
{
    const struct xhci_trb *trbs;
    const struct xhci_trb *link;
    uint32_t expected_control;
    enum xhci_status status;

    if (ring == NULL || !ring->active || ring->dma.cpu_address == NULL) {
        return XHCI_STATUS_RING_CONTAINMENT;
    }
    status = validate_ring_geometry(physical_of(&ring->dma),
        ring->dma.byte_length, ring->trb_count);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    trbs = ring->dma.cpu_address;
    link = &trbs[ring->trb_count - 1U];
    expected_control = ((uint32_t)XHCI_TRB_TYPE_LINK <<
        XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    if (trb_pointer(link) != physical_of(&ring->dma)) {
        return XHCI_STATUS_RING_POINTER;
    }
    if (link->status != 0U || link->parameter_low == 0U) {
        return XHCI_STATUS_RING_RESERVED_FIELD;
    }
    if (((link->control >> XHCI_TRB_TYPE_SHIFT) & XHCI_TRB_TYPE_MASK) !=
            XHCI_TRB_TYPE_LINK) {
        return XHCI_STATUS_RING_TYPE;
    }
    if (link->control != expected_control) {
        return (link->control & XHCI_TRB_CYCLE) == 0U ?
            XHCI_STATUS_RING_CYCLE : XHCI_STATUS_RING_RESERVED_FIELD;
    }
    return XHCI_STATUS_OK;
}

static enum xhci_status validate_command_trb(
    const struct xhci_dma_ring *ring,
    size_t index,
    uint8_t expected_type,
    uint8_t expected_slot
)
{
    const struct xhci_trb *trbs;
    const struct xhci_trb *trb;
    uint32_t allowed_control;
    uint8_t actual_type;
    enum xhci_status status;

    if (ring == NULL || ring->kind != XHCI_RING_COMMAND || !ring->active ||
        ring->state != XHCI_DMA_OBJECT_CPU_OWNED ||
        ring->dma.owner != DMA_OWNER_CPU) {
        return XHCI_STATUS_RING_OWNERSHIP;
    }
    status = validate_ring_geometry(physical_of(&ring->dma),
        ring->dma.byte_length, ring->trb_count);
    if (status != XHCI_STATUS_OK || index >= ring->trb_count - 1U) {
        return status == XHCI_STATUS_OK ? XHCI_STATUS_RING_CONTAINMENT :
            status;
    }
    status = validate_link_trb(ring);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    trbs = ring->dma.cpu_address;
    trb = &trbs[index];
    actual_type = (uint8_t)((trb->control >> XHCI_TRB_TYPE_SHIFT) &
        XHCI_TRB_TYPE_MASK);
    if (actual_type != expected_type ||
        (expected_type != XHCI_TRB_TYPE_ENABLE_SLOT &&
            expected_type != XHCI_TRB_TYPE_DISABLE_SLOT &&
            expected_type != XHCI_TRB_TYPE_ADDRESS_DEVICE)) {
        return XHCI_STATUS_RING_TYPE;
    }
    if ((trb->control & XHCI_TRB_CYCLE) != ring->producer_cycle) {
        return XHCI_STATUS_RING_CYCLE;
    }
    allowed_control = XHCI_TRB_CYCLE | XHCI_TRB_TYPE_MASK <<
        XHCI_TRB_TYPE_SHIFT | UINT32_C(0xFF) << XHCI_TRB_SLOT_SHIFT;
    if (expected_type == XHCI_TRB_TYPE_ENABLE_SLOT) {
        allowed_control |= UINT32_C(0x1F) << XHCI_TRB_ENDPOINT_SHIFT;
    }
    if ((trb->control & ~allowed_control) != 0U || trb->status != 0U ||
        (uint8_t)(trb->control >> XHCI_TRB_SLOT_SHIFT) != expected_slot) {
        return XHCI_STATUS_RING_RESERVED_FIELD;
    }
    if ((expected_type == XHCI_TRB_TYPE_ENABLE_SLOT ||
            expected_type == XHCI_TRB_TYPE_DISABLE_SLOT) &&
        trb_pointer(trb) != 0U) {
        return XHCI_STATUS_RING_RESERVED_FIELD;
    }
    if (expected_type == XHCI_TRB_TYPE_ADDRESS_DEVICE &&
        (trb_pointer(trb) == 0U || (trb_pointer(trb) & 63U) != 0U)) {
        return XHCI_STATUS_RING_POINTER;
    }
    return XHCI_STATUS_OK;
}

static enum xhci_status validate_control_td(
    const struct xhci_dma_ring *ring,
    uint64_t receive_base
)
{
    const struct xhci_trb *trbs;
    static const uint8_t types[3] = {
        XHCI_TRB_TYPE_SETUP,
        XHCI_TRB_TYPE_DATA,
        XHCI_TRB_TYPE_STATUS
    };

    if (ring == NULL || ring->kind != XHCI_RING_CONTROL || !ring->active ||
        ring->dma.cpu_address == NULL ||
        validate_ring_geometry(physical_of(&ring->dma),
            ring->dma.byte_length, ring->trb_count) != XHCI_STATUS_OK) {
        return XHCI_STATUS_RING_CONTAINMENT;
    }
    trbs = ring->dma.cpu_address;
    for (size_t index = 0U; index < 3U; ++index) {
        if (((trbs[index].control >> XHCI_TRB_TYPE_SHIFT) &
                XHCI_TRB_TYPE_MASK) != types[index] ||
            (trbs[index].control & XHCI_TRB_CYCLE) == 0U) {
            return XHCI_STATUS_RING_TYPE;
        }
    }
    if (trb_pointer(&trbs[0]) != UINT64_C(0x0012000001000680) ||
        trbs[0].status != 8U ||
        trbs[0].control !=
            (((uint32_t)XHCI_TRB_TYPE_SETUP << XHCI_TRB_TYPE_SHIFT) |
                XHCI_TRB_IMMEDIATE | XHCI_TRB_SETUP_IN | XHCI_TRB_CYCLE) ||
        trb_pointer(&trbs[1]) != receive_base ||
        trbs[1].status != XHCI_DEVICE_DESCRIPTOR_BYTES ||
        trbs[1].control !=
            (((uint32_t)XHCI_TRB_TYPE_DATA << XHCI_TRB_TYPE_SHIFT) |
                XHCI_TRB_DIRECTION | XHCI_TRB_CYCLE) ||
        trb_pointer(&trbs[2]) != 0U || trbs[2].status != 0U ||
        trbs[2].control !=
            (((uint32_t)XHCI_TRB_TYPE_STATUS << XHCI_TRB_TYPE_SHIFT) |
                XHCI_TRB_INTERRUPT | XHCI_TRB_CYCLE)) {
        return XHCI_STATUS_RING_RESERVED_FIELD;
    }
    return XHCI_STATUS_OK;
}

static enum xhci_status initialize_ring(
    struct xhci_dma_ring *ring,
    enum xhci_ring_kind kind
)
{
    struct xhci_trb *trbs;

    if (ring == NULL) {
        return XHCI_STATUS_NULL_ARGUMENT;
    }
    if (allocate_page(&ring->dma, 1U) != XHCI_STATUS_OK) {
        return XHCI_STATUS_DMA_ALLOCATION_FAILURE;
    }
    enum xhci_status geometry = validate_ring_geometry(
        physical_of(&ring->dma), ring->dma.byte_length,
        XHCI_RING_TRB_COUNT);

    if (geometry != XHCI_STATUS_OK) {
        return geometry;
    }
    ring->state = XHCI_DMA_OBJECT_CPU_OWNED;
    ring->kind = kind;
    ring->trb_count = XHCI_RING_TRB_COUNT;
    ring->producer_cycle = 1U;
    ring->consumer_cycle = 1U;
    ring->active = true;
    trbs = ring->dma.cpu_address;
    if (kind != XHCI_RING_EVENT) {
        write_trb(&trbs[XHCI_RING_TRB_COUNT - 1U], physical_of(&ring->dma),
            0U, ((uint32_t)XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
                XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE);
        return validate_link_trb(ring);
    }
    return XHCI_STATUS_OK;
}

static enum xhci_status select_port(struct xhci_runtime *controller)
{
    size_t connected = 0U;

    for (size_t protocol_index = 0U;
            protocol_index < controller->protocol_count; ++protocol_index) {
        const struct xhci_protocol *protocol =
            &controller->protocols[protocol_index];

        for (uint8_t relative = 0U; relative < protocol->port_count;
                ++relative) {
            uint8_t port = (uint8_t)(protocol->first_port + relative);
            uint32_t portsc = mmio_read32(controller->operational,
                XHCI_PORTSC_BASE +
                    (uint64_t)(port - 1U) * XHCI_PORT_REGISTER_BYTES);

            if ((portsc & XHCI_PORT_CONNECTED) == 0U) {
                continue;
            }
            ++connected;
            controller->port.identifier = port;
            controller->port.speed = (uint8_t)((portsc >>
                XHCI_PORT_SPEED_SHIFT) & XHCI_PORT_SPEED_MASK);
            controller->port.protocol_major = protocol->major;
            controller->port.slot_type = protocol->slot_type;
            controller->port.connected = true;
        }
    }
    if (connected == 0U) {
        return XHCI_STATUS_NO_CONNECTED_PORT;
    }
    if (connected != 1U) {
        return XHCI_STATUS_MULTIPLE_CONNECTED_PORTS;
    }
    if (controller->port.protocol_major != 2U ||
        controller->port.speed == 0U || controller->port.speed > 3U) {
        return XHCI_STATUS_PORT_UNSUPPORTED;
    }
    return XHCI_STATUS_OK;
}

static uint16_t endpoint_zero_packet_size(const struct xhci_root_port *port)
{
    if (port->speed == 3U) {
        return 64U;
    }
    return 8U;
}

static enum xhci_status initialize_contexts(struct xhci_runtime *controller)
{
    uint8_t *input;
    uint8_t *slot;
    uint8_t *endpoint;
    uint32_t value;

    if (allocate_page(&controller->contexts.input, 1U) != XHCI_STATUS_OK ||
        allocate_page(&controller->contexts.output, 1U) != XHCI_STATUS_OK) {
        return XHCI_STATUS_DMA_ALLOCATION_FAILURE;
    }
    if (controller->contexts.input.byte_length <
            controller->context_bytes * 33U ||
        controller->contexts.output.byte_length <
            controller->context_bytes * 32U) {
        return XHCI_STATUS_CONTEXT_INVALID;
    }
    controller->contexts.input_state = XHCI_DMA_OBJECT_CPU_OWNED;
    controller->contexts.output_state = XHCI_DMA_OBJECT_CPU_OWNED;
    controller->contexts.context_bytes = controller->context_bytes;
    controller->contexts.active = true;
    input = controller->contexts.input.cpu_address;
    *(uint32_t *)(void *)(input + 4U) = UINT32_C(3);
    slot = input + controller->context_bytes;
    value = ((uint32_t)controller->port.speed << 20U) |
        UINT32_C(1U << 27);
    *(uint32_t *)(void *)slot = value;
    *(uint32_t *)(void *)(slot + 4U) =
        (uint32_t)controller->port.identifier << 16U;
    endpoint = input + controller->context_bytes * 2U;
    *(uint32_t *)(void *)(endpoint + 4U) =
        UINT32_C(3U << 1) | UINT32_C(4U << 3) |
        (uint32_t)endpoint_zero_packet_size(&controller->port) << 16U;
    *(uint64_t *)(void *)(endpoint + 8U) =
        physical_of(&controller->endpoint.ring.dma) | UINT64_C(1);
    *(uint32_t *)(void *)(endpoint + 16U) = UINT32_C(8);
    controller->slot.context_state = XHCI_DMA_OBJECT_CPU_OWNED;
    return XHCI_STATUS_OK;
}

static enum xhci_status initialize_control_transfer(
    struct xhci_runtime *controller
)
{
    struct xhci_trb *trbs;
    uint8_t *buffer;
    enum xhci_status status;

    status = initialize_ring(&controller->endpoint.ring, XHCI_RING_CONTROL);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    if (allocate_page(&controller->endpoint.receive, 1U) != XHCI_STATUS_OK) {
        return XHCI_STATUS_DMA_ALLOCATION_FAILURE;
    }
    controller->endpoint.receive_state = XHCI_DMA_OBJECT_CPU_OWNED;
    controller->endpoint.requested_length = XHCI_DEVICE_DESCRIPTOR_BYTES;
    buffer = controller->endpoint.receive.cpu_address;
    for (uint64_t index = 0U;
            index < controller->endpoint.receive.byte_length; ++index) {
        buffer[index] = XHCI_SENTINEL;
    }
    trbs = controller->endpoint.ring.dma.cpu_address;
    write_trb(&trbs[0], UINT64_C(0x0012000001000680), 8U,
        ((uint32_t)XHCI_TRB_TYPE_SETUP << XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_IMMEDIATE | XHCI_TRB_SETUP_IN | XHCI_TRB_CYCLE);
    write_trb(&trbs[1], physical_of(&controller->endpoint.receive),
        XHCI_DEVICE_DESCRIPTOR_BYTES,
        ((uint32_t)XHCI_TRB_TYPE_DATA << XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_DIRECTION | XHCI_TRB_CYCLE);
    write_trb(&trbs[2], 0U, 0U,
        ((uint32_t)XHCI_TRB_TYPE_STATUS << XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_INTERRUPT | XHCI_TRB_CYCLE);
    controller->endpoint.status_trb_physical =
        physical_of(&controller->endpoint.ring.dma) + 2U * XHCI_TRB_BYTES;
    controller->endpoint.ring.enqueue_index = 3U;
    controller->control_slot_cpu[0] = true;
    controller->control_slot_cpu[1] = true;
    controller->control_slot_cpu[2] = true;
    return validate_control_td(&controller->endpoint.ring,
        physical_of(&controller->endpoint.receive));
}

static enum xhci_status prepare_dma(
    struct xhci_runtime *controller,
    size_t scratchpad_count
)
{
    struct xhci_trb *command_trbs;
    uint64_t *admin;
    uint64_t *erst;
    uint64_t *scratch_array;
    enum xhci_status status;

    if (allocate_page(&controller->admin, 1U) != XHCI_STATUS_OK) {
        return XHCI_STATUS_DMA_ALLOCATION_FAILURE;
    }
    controller->admin_state = XHCI_DMA_OBJECT_CPU_OWNED;
    status = initialize_ring(&controller->command, XHCI_RING_COMMAND);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    status = initialize_ring(&controller->event, XHCI_RING_EVENT);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    status = initialize_control_transfer(controller);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    status = initialize_contexts(controller);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    if (scratchpad_count != 0U) {
        status = allocate_page(&controller->scratchpads.dma,
            scratchpad_count);
        if (status != XHCI_STATUS_OK) {
            return status;
        }
        controller->scratchpads.state = XHCI_DMA_OBJECT_CPU_OWNED;
        controller->scratchpads.active = true;
        controller->scratchpads.count = scratchpad_count;
    }
    admin = controller->admin.cpu_address;
    erst = (uint64_t *)(void *)((uint8_t *)controller->admin.cpu_address +
        XHCI_ADMIN_ERST_OFFSET);
    scratch_array = (uint64_t *)(void *)((uint8_t *)
        controller->admin.cpu_address + XHCI_ADMIN_SCRATCH_ARRAY_OFFSET);
    controller->dcbaa.backing = &controller->admin;
    controller->dcbaa.offset = XHCI_ADMIN_DCBAA_OFFSET;
    controller->dcbaa.entry_count = 2U;
    controller->dcbaa.active = true;
    controller->erst.backing = &controller->admin;
    controller->erst.offset = XHCI_ADMIN_ERST_OFFSET;
    controller->erst.segment_count = 1U;
    controller->erst.active = true;
    controller->scratchpads.array_backing = &controller->admin;
    controller->scratchpads.array_offset = XHCI_ADMIN_SCRATCH_ARRAY_OFFSET;
    if (scratchpad_count != 0U) {
        admin[0] = physical_of(&controller->admin) +
            XHCI_ADMIN_SCRATCH_ARRAY_OFFSET;
        for (size_t index = 0U; index < scratchpad_count; ++index) {
            scratch_array[index] = physical_of(&controller->scratchpads.dma) +
                (uint64_t)index * PHIPIA_PAGE_SIZE;
        }
    }
    admin[1] = physical_of(&controller->contexts.output);
    erst[0] = physical_of(&controller->event.dma);
    erst[1] = XHCI_EVENT_SEGMENT_TRBS;
    command_trbs = controller->command.dma.cpu_address;
    for (size_t index = 0U; index < XHCI_COMMAND_USABLE_TRBS; ++index) {
        controller->command_slot_cpu[index] = true;
    }
    if (((physical_of(&controller->admin) + XHCI_ADMIN_ERST_OFFSET) & 63U) !=
            0U || (physical_of(&controller->event.dma) & 63U) != 0U ||
        command_trbs[XHCI_RING_TRB_COUNT - 1U].control !=
            (((uint32_t)XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
                XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE)) {
        return XHCI_STATUS_ERST_INVALID;
    }
    struct dma_allocation *allocations[PCI_BUS_MASTER_DMA_CAPACITY] = {
        &controller->admin,
        &controller->command.dma,
        &controller->event.dma,
        &controller->contexts.input,
        &controller->contexts.output,
        &controller->endpoint.ring.dma,
        &controller->endpoint.receive,
        &controller->scratchpads.dma
    };
    size_t allocation_count = controller->scratchpads.active ? 8U : 7U;

    for (size_t index = 0U; index < allocation_count; ++index) {
        status = validate_address_width(controller->address_64,
            physical_of(allocations[index]), allocations[index]->byte_length);
        if (status != XHCI_STATUS_OK) {
            return status;
        }
    }
    return XHCI_STATUS_OK;
}

static void set_object_state(
    enum xhci_dma_object_state *state,
    enum dma_owner owner
)
{
    *state = owner == DMA_OWNER_CPU ? XHCI_DMA_OBJECT_CPU_OWNED :
        XHCI_DMA_OBJECT_CONTROLLER_OWNED;
}

static enum xhci_status transfer_all_to_controller(
    struct xhci_runtime *controller
)
{
    struct dma_allocation *allocations[PCI_BUS_MASTER_DMA_CAPACITY] = {
        &controller->admin,
        &controller->command.dma,
        &controller->event.dma,
        &controller->contexts.input,
        &controller->contexts.output,
        &controller->endpoint.ring.dma,
        &controller->endpoint.receive,
        &controller->scratchpads.dma
    };
    size_t count = controller->scratchpads.active ? 8U : 7U;

    for (size_t index = 0U; index < count; ++index) {
        if (dma_mark_initialized(allocations[index]) != DMA_STATUS_OK ||
            dma_transfer_to_device(allocations[index]) != DMA_STATUS_OK) {
            return XHCI_STATUS_DMA_OWNERSHIP;
        }
    }
    controller->admin_state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    controller->command.state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    controller->event.state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    controller->contexts.input_state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    controller->contexts.output_state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    controller->endpoint.ring.state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    controller->endpoint.receive_state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    controller->slot.context_state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    if (controller->scratchpads.active) {
        controller->scratchpads.state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    }
    return XHCI_STATUS_OK;
}

static struct pci_bus_master_request bus_master_request(
    struct xhci_runtime *controller
)
{
    struct pci_bus_master_request request = {0};

    request.allocations[0] = &controller->admin;
    request.allocations[1] = &controller->command.dma;
    request.allocations[2] = &controller->event.dma;
    request.allocations[3] = &controller->contexts.input;
    request.allocations[4] = &controller->contexts.output;
    request.allocations[5] = &controller->endpoint.ring.dma;
    request.allocations[6] = &controller->endpoint.receive;
    request.allocation_count = 7U;
    if (controller->scratchpads.active) {
        request.allocations[7] = &controller->scratchpads.dma;
        request.allocation_count = 8U;
    }
    return request;
}

static bool dma_preparation_complete(const struct xhci_runtime *controller)
{
    const struct dma_allocation *allocations[PCI_BUS_MASTER_DMA_CAPACITY] = {
        &controller->admin,
        &controller->command.dma,
        &controller->event.dma,
        &controller->contexts.input,
        &controller->contexts.output,
        &controller->endpoint.ring.dma,
        &controller->endpoint.receive,
        &controller->scratchpads.dma
    };
    size_t count = controller->scratchpads.active ? 8U : 7U;

    for (size_t index = 0U; index < count; ++index) {
        if (!allocations[index]->active || !allocations[index]->initialized ||
            allocations[index]->owner != DMA_OWNER_DEVICE) {
            return false;
        }
    }
    return true;
}

static bool interrupt_prerequisites_ready(
    const struct xhci_runtime *controller
)
{
    return controller->interrupt.handler_ready &&
        controller->interrupt.active &&
        controller->interrupt.event_ring_ready &&
        controller->event.active &&
        controller->event.state == XHCI_DMA_OBJECT_CONTROLLER_OWNED;
}

static bool address_device_ready(const struct xhci_runtime *controller)
{
    return controller->port.enabled && controller->port.reset_complete &&
        controller->slot.identifier == XHCI_PROOF_SLOT &&
        controller->slot.enabled && controller->contexts.active &&
        controller->contexts.input_state ==
            XHCI_DMA_OBJECT_CONTROLLER_OWNED &&
        controller->contexts.output_state ==
            XHCI_DMA_OBJECT_CONTROLLER_OWNED;
}

static bool endpoint_doorbell_ready(const struct xhci_runtime *controller)
{
    return controller->slot.addressed && controller->endpoint.ring.active &&
        controller->endpoint.ring.state ==
            XHCI_DMA_OBJECT_CONTROLLER_OWNED &&
        controller->endpoint.receive.active &&
        controller->endpoint.receive_state ==
            XHCI_DMA_OBJECT_CONTROLLER_OWNED &&
        controller->control_slot_cpu[0] &&
        controller->control_slot_cpu[1] &&
        controller->control_slot_cpu[2];
}

static uint64_t trb_pointer(const struct xhci_trb *trb)
{
    return (uint64_t)trb->parameter_low |
        ((uint64_t)trb->parameter_high << 32U);
}

static enum xhci_status validate_event_identity(
    const struct xhci_expectation *expectation,
    uint8_t event_type,
    uint64_t pointer,
    uint8_t slot,
    uint8_t endpoint,
    uint8_t completion,
    uint8_t cycle,
    uint8_t expected_cycle
)
{
    if (expectation == NULL) {
        return XHCI_STATUS_NULL_ARGUMENT;
    }
    if (cycle != expected_cycle) {
        return XHCI_STATUS_RING_CYCLE;
    }
    if (expectation->kind == XHCI_EXPECT_COMMAND) {
        if (completion != XHCI_COMPLETION_SUCCESS) {
            return XHCI_STATUS_COMMAND_COMPLETION;
        }
        if (event_type != XHCI_TRB_TYPE_COMMAND_EVENT ||
            pointer != expectation->pointer ||
            (expectation->trb_type == XHCI_TRB_TYPE_ENABLE_SLOT ?
                slot != XHCI_PROOF_SLOT : slot != expectation->slot)) {
            return XHCI_STATUS_COMMAND_EVENT_MISMATCH;
        }
        return XHCI_STATUS_OK;
    }
    if (expectation->kind == XHCI_EXPECT_TRANSFER) {
        if (completion != XHCI_COMPLETION_SUCCESS &&
            completion != XHCI_COMPLETION_SHORT_PACKET) {
            return XHCI_STATUS_TRANSFER_COMPLETION;
        }
        if (event_type != XHCI_TRB_TYPE_TRANSFER_EVENT ||
            pointer != expectation->pointer || slot != expectation->slot ||
            endpoint != expectation->endpoint) {
            return XHCI_STATUS_TRANSFER_EVENT_MISMATCH;
        }
        return XHCI_STATUS_OK;
    }
    return XHCI_STATUS_TRANSFER_EVENT_MISMATCH;
}

static void process_event(
    struct xhci_runtime *controller,
    const struct xhci_trb *event
)
{
    uint8_t type = (uint8_t)((event->control >> XHCI_TRB_TYPE_SHIFT) &
        XHCI_TRB_TYPE_MASK);
    uint8_t completion = (uint8_t)(event->status >>
        XHCI_TRB_COMPLETION_SHIFT);
    uint8_t slot = (uint8_t)(event->control >> XHCI_TRB_SLOT_SHIFT);
    uint8_t endpoint = (uint8_t)(event->control >>
        XHCI_TRB_ENDPOINT_SHIFT) & UINT8_C(0x1F);
    uint64_t pointer = trb_pointer(event);
    enum xhci_status identity;

    if (type == XHCI_TRB_TYPE_PORT_EVENT) {
        ++controller->ignored_events;
        return;
    }
    if (controller->expectation.kind == XHCI_EXPECT_COMMAND) {
        identity = validate_event_identity(&controller->expectation, type,
            pointer, slot, endpoint, completion,
            (uint8_t)(event->control & XHCI_TRB_CYCLE),
            controller->event.consumer_cycle);
        if (identity != XHCI_STATUS_OK) {
            ++controller->ignored_events;
            controller->expectation.result = identity;
            return;
        }
        controller->expectation.slot = slot;
        controller->expectation.result = XHCI_STATUS_OK;
        controller->expectation.done = true;
        return;
    }
    if (controller->expectation.kind == XHCI_EXPECT_TRANSFER) {
        identity = validate_event_identity(&controller->expectation, type,
            pointer, slot, endpoint, completion,
            (uint8_t)(event->control & XHCI_TRB_CYCLE),
            controller->event.consumer_cycle);
        if (identity != XHCI_STATUS_OK) {
            ++controller->ignored_events;
            controller->expectation.result = identity;
            return;
        }
        if ((event->status & XHCI_TRB_LENGTH_MASK) >
                controller->endpoint.requested_length) {
            controller->expectation.result = XHCI_STATUS_TRANSFER_LENGTH;
            controller->expectation.done = true;
            return;
        }
        controller->endpoint.returned_length =
            controller->endpoint.requested_length -
                (event->status & XHCI_TRB_LENGTH_MASK);
        if (controller->endpoint.receive.owner != DMA_OWNER_DEVICE ||
            dma_transfer_to_cpu(&controller->endpoint.receive) !=
                DMA_STATUS_OK) {
            controller->ownership_failure = true;
            controller->expectation.result = XHCI_STATUS_DMA_OWNERSHIP;
        } else {
            controller->endpoint.receive_state = XHCI_DMA_OBJECT_CPU_OWNED;
            controller->expectation.result = XHCI_STATUS_OK;
        }
        controller->endpoint.complete = true;
        controller->expectation.done = true;
        return;
    }
    ++controller->ignored_events;
}

static enum xhci_status teardown_observation_hook(
    struct xhci_runtime *controller
)
{
    if (controller != NULL && controller->teardown_started &&
        !controller->event.dma.active) {
        controller->handler_saw_freed_state = true;
        return XHCI_STATUS_TEARDOWN_RACE;
    }
    return XHCI_STATUS_OK;
}

static void xhci_interrupt_handler(
    struct interrupt_frame *frame,
    void *opaque
)
{
    struct xhci_runtime *controller = opaque;
    struct xhci_trb *events;

    if (controller == NULL || frame == NULL) {
        return;
    }
    if (teardown_observation_hook(controller) != XHCI_STATUS_OK) {
        return;
    }
    if (frame->vector != controller->interrupt.vector) {
        controller->wrong_vector = true;
        return;
    }
    ++controller->interrupt_count;
    if (controller->event.dma.owner != DMA_OWNER_DEVICE ||
        dma_transfer_to_cpu(&controller->event.dma) != DMA_STATUS_OK) {
        controller->ownership_failure = true;
        return;
    }
    controller->event.state = XHCI_DMA_OBJECT_CPU_OWNED;
    events = controller->event.dma.cpu_address;
    for (;;) {
        const struct xhci_trb *event = &events[controller->event.dequeue_index];
        uint8_t cycle = (uint8_t)(event->control & XHCI_TRB_CYCLE);

        if (cycle != controller->event.consumer_cycle) {
            break;
        }
        process_event(controller, event);
        ++controller->event.dequeue_index;
        if (controller->event.dequeue_index == XHCI_EVENT_SEGMENT_TRBS) {
            controller->event.dequeue_index = 0U;
            controller->event.consumer_cycle ^= 1U;
        }
    }
    mmio_write64(controller->interrupter, XHCI_ERDP,
        (physical_of(&controller->event.dma) +
            controller->event.dequeue_index * XHCI_TRB_BYTES) |
            XHCI_ERDP_BUSY);
    mmio_write32(controller->interrupter, XHCI_IMAN,
        mmio_read32(controller->interrupter, XHCI_IMAN) |
            XHCI_IMAN_PENDING);
    mmio_write32(controller->operational, XHCI_USBSTS,
        mmio_read32(controller->operational, XHCI_USBSTS) &
            (XHCI_STS_EVENT_INTERRUPT | XHCI_STS_PORT_CHANGE));
    if (dma_transfer_to_device(&controller->event.dma) != DMA_STATUS_OK) {
        controller->ownership_failure = true;
        return;
    }
    controller->event.state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
}

static enum xhci_status configure_interrupt(
    struct xhci_runtime *controller
)
{
    struct msix_state before = msix_get_state();
    struct interrupt_vector_state vectors_before = interrupt_vector_get_state();
    size_t mappings_before = controller->claim.pci.mapping_count;

    controller->interrupt.event_ring_ready = controller->event.active;
    msix_test_inject_failure_once();
    if (msix_bind(&controller->claim.pci, 0U, xhci_interrupt_handler,
            controller, &controller->interrupt.msix) !=
            MSIX_STATUS_INJECTED_FAILURE ||
        controller->interrupt.msix.active ||
        msix_get_state().active_bindings != before.active_bindings ||
        interrupt_vector_get_state().allocated != vectors_before.allocated ||
        controller->claim.pci.mapping_count != mappings_before) {
        return XHCI_STATUS_MSIX_ROLLBACK_FAILURE;
    }
    if (msix_bind(&controller->claim.pci, 0U, xhci_interrupt_handler,
            controller, &controller->interrupt.msix) != MSIX_STATUS_OK) {
        return XHCI_STATUS_MSIX_FAILURE;
    }
    controller->interrupt.vector =
        controller->interrupt.msix.vector.vector;
    controller->interrupt.handler_ready = true;
    controller->interrupt.active = true;
    return XHCI_STATUS_OK;
}

static enum xhci_status program_controller(struct xhci_runtime *controller)
{
    const struct pci_bus_master_request request =
        bus_master_request(controller);
    enum pci_resource_status pci_status;

    pci_status = pci_claim_enable_bus_master(&controller->claim.pci, &request);
    if (pci_status != PCI_RESOURCE_STATUS_DMA_NOT_PREPARED) {
        return XHCI_STATUS_BUS_MASTER_PREMATURE;
    }
    if (transfer_all_to_controller(controller) != XHCI_STATUS_OK) {
        return XHCI_STATUS_DMA_OWNERSHIP;
    }
    if (!dma_preparation_complete(controller)) {
        return XHCI_STATUS_DMA_OWNERSHIP;
    }
    pci_status = pci_claim_enable_bus_master(&controller->claim.pci, &request);
    if (pci_status != PCI_RESOURCE_STATUS_OK) {
        return XHCI_STATUS_BUS_MASTER_FAILURE;
    }
    controller->bus_master_enabled = true;

    /*
     * Writing ERSTBA causes a conforming controller to read the ERST entry.
     * Program every DMA pointer only after ownership and bus mastering make
     * that first controller read legal.
     */
    mmio_write32(controller->operational, XHCI_CONFIG, 1U);
    mmio_write64(controller->operational, XHCI_DCBAAP,
        physical_of(&controller->admin) + XHCI_ADMIN_DCBAA_OFFSET);
    mmio_write64(controller->operational, XHCI_CRCR,
        physical_of(&controller->command.dma) | UINT64_C(1));
    mmio_write32(controller->interrupter, XHCI_ERSTSZ, 1U);
    mmio_write64(controller->interrupter, XHCI_ERSTBA,
        physical_of(&controller->admin) + XHCI_ADMIN_ERST_OFFSET);
    mmio_write64(controller->interrupter, XHCI_ERDP,
        physical_of(&controller->event.dma));
    mmio_write32(controller->interrupter, XHCI_IMOD, 0U);
    if ((mmio_read32(controller->operational, XHCI_USBSTS) &
            XHCI_STS_HOST_ERROR) != 0U) {
        return XHCI_STATUS_HOST_CONTROLLER_ERROR;
    }

    if (!interrupt_prerequisites_ready(controller)) {
        return XHCI_STATUS_INTERRUPT_NOT_READY;
    }
    mmio_write32(controller->interrupter, XHCI_IMAN,
        XHCI_IMAN_ENABLE);
    controller->interrupt_sources_enabled = true;
    cpu_store_fence();
    mmio_write32(controller->operational, XHCI_USBCMD,
        XHCI_CMD_INTERRUPTS | XHCI_CMD_RUN);
    if (!wait_mask(controller->operational, XHCI_USBSTS,
            XHCI_STS_HALTED, 0U, XHCI_HALT_TIMEOUT_NS)) {
        return XHCI_STATUS_HALT_TIMEOUT;
    }
    if ((mmio_read32(controller->operational, XHCI_USBSTS) &
            XHCI_STS_HOST_ERROR) != 0U) {
        return XHCI_STATUS_HOST_CONTROLLER_ERROR;
    }
    return XHCI_STATUS_OK;
}

static enum xhci_status reset_port(struct xhci_runtime *controller)
{
    uint64_t offset = XHCI_PORTSC_BASE +
        (uint64_t)(controller->port.identifier - 1U) *
            XHCI_PORT_REGISTER_BYTES;
    uint32_t portsc = mmio_read32(controller->operational, offset);
    uint32_t write_value;

    if ((portsc & XHCI_PORT_CONNECTED) == 0U) {
        return XHCI_STATUS_NO_CONNECTED_PORT;
    }
    write_value = portsc & ~(XHCI_PORT_CHANGE_BITS | XHCI_PORT_ENABLED);
    write_value |= XHCI_PORT_POWER | XHCI_PORT_RESET;
    mmio_write32(controller->operational, offset, write_value);
    if (!wait_mask(controller->operational, offset,
            XHCI_PORT_RESET, 0U, XHCI_PORT_TIMEOUT_NS) ||
        !wait_mask(controller->operational, offset,
            XHCI_PORT_RESET_CHANGE, XHCI_PORT_RESET_CHANGE,
            XHCI_PORT_TIMEOUT_NS)) {
        return XHCI_STATUS_PORT_RESET_TIMEOUT;
    }
    portsc = mmio_read32(controller->operational, offset);
    if ((portsc & (XHCI_PORT_CONNECTED | XHCI_PORT_ENABLED)) !=
            (XHCI_PORT_CONNECTED | XHCI_PORT_ENABLED)) {
        return XHCI_STATUS_PORT_NOT_ENABLED;
    }
    controller->port.speed = (uint8_t)((portsc >> XHCI_PORT_SPEED_SHIFT) &
        XHCI_PORT_SPEED_MASK);
    controller->port.enabled = true;
    controller->port.reset_complete = true;
    mmio_write32(controller->operational, offset,
        (portsc & ~(XHCI_PORT_CHANGE_BITS | XHCI_PORT_ENABLED)) |
            XHCI_PORT_RESET_CHANGE);
    return XHCI_STATUS_OK;
}

static enum xhci_status wait_for_expectation(
    struct xhci_runtime *controller,
    uint64_t interval_ns,
    enum xhci_status timeout_status
)
{
    uint64_t deadline;

    if (!add_checked(clock_monotonic_ns(), interval_ns, &deadline)) {
        return timeout_status;
    }
    cpu_interrupt_enable();
    while (!controller->expectation.done && !controller->wrong_vector &&
        !controller->ownership_failure &&
        !deadline_reached(clock_monotonic_ns(), deadline)) {
        if ((mmio_read32(controller->operational, XHCI_USBSTS) &
                XHCI_STS_HOST_ERROR) != 0U) {
            break;
        }
        __asm__ volatile ("" : : : "memory");
    }
    cpu_interrupt_disable();
    if ((mmio_read32(controller->operational, XHCI_USBSTS) &
            XHCI_STS_HOST_ERROR) != 0U) {
        return XHCI_STATUS_HOST_CONTROLLER_ERROR;
    }
    if (controller->wrong_vector) {
        return XHCI_STATUS_INTERRUPT_COUNT;
    }
    if (controller->ownership_failure) {
        return XHCI_STATUS_DMA_OWNERSHIP;
    }
    if (!controller->expectation.done) {
        if (controller->expectation.result != timeout_status) {
            return controller->expectation.result;
        }
        return timeout_status;
    }
    return controller->expectation.result;
}

static enum xhci_status submit_command(
    struct xhci_runtime *controller,
    uint8_t type,
    uint64_t parameter,
    uint8_t slot
)
{
    size_t index = controller->command.enqueue_index;
    struct xhci_trb *trbs = controller->command.dma.cpu_address;
    uint64_t pointer;
    uint32_t command_fields = 0U;
    enum xhci_status status;

    if (controller->claim.state != XHCI_CONTROLLER_RUNNING ||
        controller->command.state != XHCI_DMA_OBJECT_CONTROLLER_OWNED) {
        return XHCI_STATUS_COMMAND_PRECONDITION;
    }
    if (index >= XHCI_COMMAND_USABLE_TRBS) {
        return XHCI_STATUS_RING_FULL;
    }
    if (!controller->command_slot_cpu[index]) {
        return XHCI_STATUS_RING_OWNERSHIP;
    }
    if (dma_transfer_to_cpu(&controller->command.dma) != DMA_STATUS_OK) {
        return XHCI_STATUS_DMA_OWNERSHIP;
    }
    controller->command.state = XHCI_DMA_OBJECT_CPU_OWNED;
    pointer = physical_of(&controller->command.dma) +
        index * XHCI_TRB_BYTES;
    if (type == XHCI_TRB_TYPE_ENABLE_SLOT) {
        command_fields = (uint32_t)(parameter & UINT64_C(0x1F)) << 16U;
        parameter = 0U;
    }
    write_trb(&trbs[index], parameter, 0U,
        ((uint32_t)type << XHCI_TRB_TYPE_SHIFT) |
            ((uint32_t)slot << XHCI_TRB_SLOT_SHIFT) |
            command_fields | controller->command.producer_cycle);
    status = validate_command_trb(&controller->command, index, type, slot);
    if (status != XHCI_STATUS_OK) {
        if (dma_transfer_to_device(&controller->command.dma) ==
                DMA_STATUS_OK) {
            controller->command.state =
                XHCI_DMA_OBJECT_CONTROLLER_OWNED;
        }
        return status;
    }
    controller->command_slot_cpu[index] = false;
    controller->expectation.kind = XHCI_EXPECT_COMMAND;
    controller->expectation.done = false;
    controller->expectation.result = XHCI_STATUS_COMMAND_TIMEOUT;
    controller->expectation.pointer = pointer;
    controller->expectation.trb_type = type;
    controller->expectation.slot = slot;
    if (dma_transfer_to_device(&controller->command.dma) != DMA_STATUS_OK) {
        return XHCI_STATUS_DMA_OWNERSHIP;
    }
    controller->command.state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    cpu_store_fence();
    mmio_write32(controller->doorbells, 0U, 0U);
    status = wait_for_expectation(controller, XHCI_COMMAND_TIMEOUT_NS,
        XHCI_STATUS_COMMAND_TIMEOUT);
    controller->expectation.kind = XHCI_EXPECT_NONE;
    if (status == XHCI_STATUS_OK) {
        controller->command_slot_cpu[index] = true;
        ++controller->command.enqueue_index;
    }
    return status;
}

static enum xhci_status enable_and_address_slot(
    struct xhci_runtime *controller
)
{
    enum xhci_status status;

    if (!controller->port.enabled || !controller->port.reset_complete) {
        return XHCI_STATUS_COMMAND_PRECONDITION;
    }
    status = submit_command(controller, XHCI_TRB_TYPE_ENABLE_SLOT,
        controller->port.slot_type, 0U);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    controller->slot.identifier = controller->expectation.slot;
    controller->slot.enabled = true;
    if (!address_device_ready(controller)) {
        return XHCI_STATUS_ADDRESS_DEVICE_PRECONDITION;
    }
    status = submit_command(controller, XHCI_TRB_TYPE_ADDRESS_DEVICE,
        physical_of(&controller->contexts.input),
        controller->slot.identifier);
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    controller->slot.addressed = true;
    return XHCI_STATUS_OK;
}

static enum xhci_status submit_descriptor_transfer(
    struct xhci_runtime *controller
)
{
    enum xhci_status status;

    if (!endpoint_doorbell_ready(controller)) {
        return XHCI_STATUS_DOORBELL_PREMATURE;
    }
    controller->control_slot_cpu[0] = false;
    controller->control_slot_cpu[1] = false;
    controller->control_slot_cpu[2] = false;
    controller->endpoint.submitted = true;
    controller->expectation.kind = XHCI_EXPECT_TRANSFER;
    controller->expectation.done = false;
    controller->expectation.result = XHCI_STATUS_TRANSFER_TIMEOUT;
    controller->expectation.pointer =
        controller->endpoint.status_trb_physical;
    controller->expectation.trb_type = XHCI_TRB_TYPE_STATUS;
    controller->expectation.slot = controller->slot.identifier;
    controller->expectation.endpoint = XHCI_ENDPOINT_ID_ZERO;
    controller->interrupt.count_before_transfer =
        controller->interrupt_count;
    cpu_store_fence();
    mmio_write32(controller->doorbells,
        (uint64_t)controller->slot.identifier * sizeof(uint32_t),
        XHCI_ENDPOINT_ID_ZERO);
    status = wait_for_expectation(controller, XHCI_TRANSFER_TIMEOUT_NS,
        XHCI_STATUS_TRANSFER_TIMEOUT);
    controller->expectation.kind = XHCI_EXPECT_NONE;
    controller->interrupt.count_after_transfer = controller->interrupt_count;
    if (status != XHCI_STATUS_OK) {
        return status;
    }
    if (controller->interrupt.count_after_transfer !=
            controller->interrupt.count_before_transfer + 1U) {
        return XHCI_STATUS_INTERRUPT_COUNT;
    }
    controller->control_slot_cpu[0] = true;
    controller->control_slot_cpu[1] = true;
    controller->control_slot_cpu[2] = true;
    return XHCI_STATUS_OK;
}

static bool valid_bcd(uint16_t value)
{
    return (value & UINT16_C(0x000F)) <= 9U &&
        ((value >> 4U) & UINT16_C(0x000F)) <= 9U &&
        ((value >> 8U) & UINT16_C(0x000F)) <= 9U &&
        ((value >> 12U) & UINT16_C(0x000F)) <= 9U;
}

static enum xhci_status validate_descriptor(
    const uint8_t *descriptor,
    size_t returned_length
)
{
    uint16_t bcd_usb;
    uint8_t maximum_packet;

    if (returned_length < XHCI_DEVICE_DESCRIPTOR_BYTES) {
        return XHCI_STATUS_DESCRIPTOR_SHORT;
    }
    if (returned_length > XHCI_DEVICE_DESCRIPTOR_BYTES) {
        return XHCI_STATUS_DESCRIPTOR_OVERSIZED;
    }
    if (descriptor[0] != XHCI_DEVICE_DESCRIPTOR_BYTES ||
        descriptor[1] != 1U) {
        return XHCI_STATUS_DESCRIPTOR_TYPE;
    }
    bcd_usb = (uint16_t)descriptor[2] | (uint16_t)descriptor[3] << 8U;
    maximum_packet = descriptor[7];
    if (bcd_usb == 0U || !valid_bcd(bcd_usb) ||
        (maximum_packet != 8U && maximum_packet != 16U &&
            maximum_packet != 32U && maximum_packet != 64U) ||
        descriptor[17] == 0U ||
        (descriptor[4] == 0U &&
            (descriptor[5] != 0U || descriptor[6] != 0U))) {
        return XHCI_STATUS_DESCRIPTOR_INCONSISTENT;
    }
    return XHCI_STATUS_OK;
}

static enum xhci_status validate_sentinel(
    const struct xhci_runtime *controller
)
{
    const uint8_t *buffer = controller->endpoint.receive.cpu_address;
    bool changed = false;

    if (validate_cpu_access(controller->endpoint.receive_state) !=
            XHCI_STATUS_OK ||
        controller->endpoint.receive.owner != DMA_OWNER_CPU ||
        !controller->endpoint.complete) {
        return XHCI_STATUS_DMA_OWNERSHIP;
    }
    for (size_t index = 0U; index < XHCI_DEVICE_DESCRIPTOR_BYTES; ++index) {
        if (buffer[index] != XHCI_SENTINEL) {
            changed = true;
        }
    }
    for (uint64_t index = XHCI_DEVICE_DESCRIPTOR_BYTES;
            index < controller->endpoint.receive.byte_length; ++index) {
        if (buffer[index] != XHCI_SENTINEL) {
            return XHCI_STATUS_SENTINEL_FAILURE;
        }
    }
    return changed ? XHCI_STATUS_OK : XHCI_STATUS_SENTINEL_FAILURE;
}

static bool resource_state_matches(
    struct pci_resource_state pci_before,
    struct dma_state dma_before,
    struct interrupt_vector_state vectors_before,
    struct msix_state msix_before,
    struct frame_allocator_stats frames_before
)
{
    struct pci_resource_state pci_after = pci_resource_get_state();
    struct dma_state dma_after = dma_get_state();
    struct interrupt_vector_state vectors_after = interrupt_vector_get_state();
    struct msix_state msix_after = msix_get_state();
    struct frame_allocator_stats frames_after = frame_allocator_get_stats();

    return pci_after.active_claims == pci_before.active_claims &&
        pci_after.active_mappings == pci_before.active_mappings &&
        pci_after.arena_pages == pci_before.arena_pages &&
        pci_after.mapped_pages == pci_before.mapped_pages &&
        pci_after.bus_masters == pci_before.bus_masters &&
        pci_after.active == pci_before.active &&
        dma_after.active_allocations == dma_before.active_allocations &&
        dma_after.cpu_owned_allocations == dma_before.cpu_owned_allocations &&
        dma_after.device_owned_allocations ==
            dma_before.device_owned_allocations &&
        dma_after.active == dma_before.active &&
        vectors_after.capacity == vectors_before.capacity &&
        vectors_after.allocated == vectors_before.allocated &&
        vectors_after.free == vectors_before.free &&
        vectors_after.active == vectors_before.active &&
        msix_after.active_bindings == msix_before.active_bindings &&
        !msix_after.failure_injection_armed &&
        !msix_before.failure_injection_armed &&
        frames_after.addressable_frames == frames_before.addressable_frames &&
        frames_after.allocatable_frames == frames_before.allocatable_frames &&
        frames_after.free_frames == frames_before.free_frames &&
        frames_after.allocated_frames == frames_before.allocated_frames &&
        frames_after.reserved_frames == frames_before.reserved_frames &&
        frames_after.highest_allocatable_address ==
            frames_before.highest_allocatable_address;
}

static enum xhci_status reclaim_all(struct xhci_runtime *controller)
{
    struct dma_allocation *allocations[PCI_BUS_MASTER_DMA_CAPACITY] = {
        &controller->admin,
        &controller->command.dma,
        &controller->event.dma,
        &controller->contexts.input,
        &controller->contexts.output,
        &controller->endpoint.ring.dma,
        &controller->endpoint.receive,
        &controller->scratchpads.dma
    };
    size_t count = controller->scratchpads.active ? 8U : 7U;

    for (size_t index = 0U; index < count; ++index) {
        if (allocations[index]->active &&
            allocations[index]->owner == DMA_OWNER_DEVICE &&
            dma_transfer_to_cpu(allocations[index]) != DMA_STATUS_OK) {
            return XHCI_STATUS_TEARDOWN_FAILURE;
        }
    }
    if (controller->admin.active) {
        set_object_state(&controller->admin_state,
            controller->admin.owner);
    }
    if (controller->command.dma.active) {
        set_object_state(&controller->command.state,
            controller->command.dma.owner);
    }
    if (controller->event.dma.active) {
        set_object_state(&controller->event.state,
            controller->event.dma.owner);
    }
    if (controller->contexts.input.active) {
        set_object_state(&controller->contexts.input_state,
            controller->contexts.input.owner);
    }
    if (controller->contexts.output.active) {
        set_object_state(&controller->contexts.output_state,
            controller->contexts.output.owner);
    }
    if (controller->endpoint.ring.dma.active) {
        set_object_state(&controller->endpoint.ring.state,
            controller->endpoint.ring.dma.owner);
    }
    if (controller->endpoint.receive.active) {
        set_object_state(&controller->endpoint.receive_state,
            controller->endpoint.receive.owner);
    }
    if (controller->scratchpads.active) {
        set_object_state(&controller->scratchpads.state,
            controller->scratchpads.dma.owner);
    }
    return XHCI_STATUS_OK;
}

static enum xhci_status release_all(struct xhci_runtime *controller)
{
    struct dma_allocation *reverse[PCI_BUS_MASTER_DMA_CAPACITY] = {
        &controller->scratchpads.dma,
        &controller->endpoint.receive,
        &controller->endpoint.ring.dma,
        &controller->contexts.output,
        &controller->contexts.input,
        &controller->event.dma,
        &controller->command.dma,
        &controller->admin
    };

    for (size_t index = 0U; index < PCI_BUS_MASTER_DMA_CAPACITY; ++index) {
        if (reverse[index]->active &&
            dma_release(reverse[index]) != DMA_STATUS_OK) {
            return XHCI_STATUS_TEARDOWN_FAILURE;
        }
    }
    controller->admin_state = XHCI_DMA_OBJECT_RECLAIMED;
    controller->command.state = XHCI_DMA_OBJECT_RECLAIMED;
    controller->event.state = XHCI_DMA_OBJECT_RECLAIMED;
    controller->contexts.input_state = XHCI_DMA_OBJECT_RECLAIMED;
    controller->contexts.output_state = XHCI_DMA_OBJECT_RECLAIMED;
    controller->endpoint.ring.state = XHCI_DMA_OBJECT_RECLAIMED;
    controller->endpoint.receive_state = XHCI_DMA_OBJECT_RECLAIMED;
    controller->scratchpads.state = XHCI_DMA_OBJECT_RECLAIMED;
    return XHCI_STATUS_OK;
}

static enum xhci_status teardown_controller(
    struct xhci_runtime *controller
)
{
    enum xhci_status result = XHCI_STATUS_OK;
    bool interrupts_were_enabled = cpu_interrupts_enabled();
    bool dma_stopped = true;

    if (interrupts_were_enabled) {
        cpu_interrupt_disable();
    }
    controller->teardown_started = true;
    if (controller->claim.state == XHCI_CONTROLLER_RUNNING) {
        if (controller->slot.enabled) {
            enum xhci_status disable_status = submit_command(controller,
                XHCI_TRB_TYPE_DISABLE_SLOT, 0U,
                controller->slot.identifier);

            if (disable_status != XHCI_STATUS_OK) {
                result = XHCI_STATUS_TEARDOWN_FAILURE;
            } else {
                controller->slot.enabled = false;
                controller->slot.addressed = false;
            }
        }
        if (transition(&controller->claim, XHCI_CONTROLLER_STOPPING) !=
                XHCI_STATUS_OK) {
            result = XHCI_STATUS_TEARDOWN_FAILURE;
        }
    }
    if (controller->interrupt_sources_enabled) {
        mmio_write32(controller->interrupter, XHCI_IMAN, 0U);
        controller->interrupt_sources_enabled = false;
    }
    if (controller->operational != NULL &&
        (controller->claim.state == XHCI_CONTROLLER_STOPPING ||
            controller->claim.state == XHCI_CONTROLLER_PREPARED ||
            controller->claim.state == XHCI_CONTROLLER_CLAIMED)) {
        uint32_t command = mmio_read32(controller->operational, XHCI_USBCMD);

        command &= ~(XHCI_CMD_RUN | XHCI_CMD_INTERRUPTS);
        mmio_write32(controller->operational, XHCI_USBCMD, command);
        if (!wait_mask(controller->operational, XHCI_USBSTS,
                XHCI_STS_HALTED, XHCI_STS_HALTED,
                XHCI_HALT_TIMEOUT_NS)) {
            result = XHCI_STATUS_TEARDOWN_FAILURE;
        }
    }
    if (controller->bus_master_enabled) {
        if (pci_claim_disable_bus_master(&controller->claim.pci) !=
                PCI_RESOURCE_STATUS_OK) {
            result = XHCI_STATUS_TEARDOWN_FAILURE;
            dma_stopped = false;
        } else {
            controller->bus_master_enabled = false;
        }
    }
    if (controller->interrupt.active) {
        if (msix_unbind(&controller->interrupt.msix) != MSIX_STATUS_OK) {
            result = XHCI_STATUS_TEARDOWN_FAILURE;
        } else {
            controller->interrupt.active = false;
            controller->interrupt.handler_ready = false;
        }
    }
    if (dma_stopped) {
        if (reclaim_all(controller) != XHCI_STATUS_OK) {
            result = XHCI_STATUS_TEARDOWN_FAILURE;
        }
        if (controller->handler_saw_freed_state) {
            result = XHCI_STATUS_TEARDOWN_RACE;
        }
        if (release_all(controller) != XHCI_STATUS_OK) {
            result = XHCI_STATUS_TEARDOWN_FAILURE;
        }
        if (controller->claim.pci.active &&
            pci_release_device(&controller->claim.pci) !=
                PCI_RESOURCE_STATUS_OK) {
            result = XHCI_STATUS_TEARDOWN_FAILURE;
        }
    }
    if (controller->claim.state != XHCI_CONTROLLER_UNINITIALIZED &&
        controller->claim.state != XHCI_CONTROLLER_RELEASED &&
        transition(&controller->claim, XHCI_CONTROLLER_RELEASED) !=
            XHCI_STATUS_OK) {
        result = XHCI_STATUS_TEARDOWN_FAILURE;
    }
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return result;
}

static bool exercise_cleanup_boundary(size_t boundary)
{
    struct xhci_runtime controller = {0};
    struct pci_resource_state pci_before = pci_resource_get_state();
    struct dma_state dma_before = dma_get_state();
    struct interrupt_vector_state vectors_before =
        interrupt_vector_get_state();
    struct msix_state msix_before = msix_get_state();
    struct frame_allocator_stats frames_before = frame_allocator_get_stats();
    struct dma_allocation *allocations[PCI_BUS_MASTER_DMA_CAPACITY] = {
        &controller.admin,
        &controller.command.dma,
        &controller.event.dma,
        &controller.contexts.input,
        &controller.contexts.output,
        &controller.endpoint.ring.dma,
        &controller.endpoint.receive,
        &controller.scratchpads.dma
    };
    bool prepared = true;

    if (boundary > PCI_BUS_MASTER_DMA_CAPACITY) {
        return false;
    }
    for (size_t index = 0U; index < boundary; ++index) {
        if (allocate_page(allocations[index], 1U) != XHCI_STATUS_OK ||
            dma_mark_initialized(allocations[index]) != DMA_STATUS_OK ||
            dma_transfer_to_device(allocations[index]) != DMA_STATUS_OK) {
            prepared = false;
            break;
        }
    }
    if (boundary == PCI_BUS_MASTER_DMA_CAPACITY) {
        controller.scratchpads.active = true;
    }
    if (reclaim_all(&controller) != XHCI_STATUS_OK ||
        release_all(&controller) != XHCI_STATUS_OK) {
        return false;
    }
    return prepared && resource_state_matches(pci_before, dma_before,
        vectors_before, msix_before, frames_before);
}

static enum xhci_status validate_cpu_access(
    enum xhci_dma_object_state state
)
{
    return state == XHCI_DMA_OBJECT_CPU_OWNED ? XHCI_STATUS_OK :
        XHCI_STATUS_DMA_OWNERSHIP;
}

static bool test_record(bool passed, size_t *completed)
{
    if (!passed) {
        return false;
    }
    ++*completed;
    return true;
}

bool xhci_foundation_self_test(size_t *completed_tests)
{
    struct xhci_register_span span = {0};
    struct xhci_register_span overlap_left = {
        .offset = UINT64_C(0x20), .length = UINT64_C(0x20), .valid = true
    };
    struct xhci_register_span overlap_right = {
        .offset = UINT64_C(0x30), .length = UINT64_C(0x20), .valid = true
    };
    struct xhci_controller_claim lifecycle = {0};
    struct xhci_expectation expectation = {
        .kind = XHCI_EXPECT_TRANSFER,
        .pointer = UINT64_C(0x1000),
        .slot = 1U,
        .endpoint = 1U
    };
    struct xhci_runtime synthetic = {0};
    struct xhci_runtime prerequisites = {0};
    struct xhci_runtime teardown_race = {0};
    struct xhci_dma_ring ring = {0};
    struct xhci_trb ring_trbs[XHCI_RING_TRB_COUNT] = {0};
    uint32_t extended_space[128] = {0};
    uint64_t visited_offsets[2] = {UINT64_C(0x20), UINT64_C(0x30)};
    uint8_t descriptor[XHCI_DEVICE_DESCRIPTOR_BYTES] = {
        18U, 1U, 0U, 2U, 0U, 0U, 0U, 8U,
        0U, 0U, 0U, 0U, 0U, 1U, 0U, 0U, 0U, 1U
    };
    struct pci_resource_state pci_before = pci_resource_get_state();
    struct dma_state dma_before = dma_get_state();
    struct interrupt_vector_state vectors_before =
        interrupt_vector_get_state();
    struct msix_state msix_before = msix_get_state();
    struct frame_allocator_stats frames_before = frame_allocator_get_stats();
    size_t completed = 0U;
    size_t scratchpads = 0U;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;

    /* 1: checked BAR containment rejects both range and arithmetic overflow. */
    if (!test_record(validate_span(0x1000U, 0xFF0U, 0x20U, 4U, &span) ==
            XHCI_STATUS_REGISTER_OUTSIDE_BAR &&
        validate_span(UINT64_MAX, UINT64_MAX - 3U, 8U, 4U, &span) ==
            XHCI_STATUS_REGISTER_OVERFLOW &&
        validate_span(0x1000U, 0x21U, 0x20U, 4U, &span) ==
            XHCI_STATUS_REGISTER_ALIGNMENT &&
        spans_overlap(&overlap_left, &overlap_right), &completed)) {
        return false;
    }
    /* 2: cycle, non-progress, range and overlap use the real xECP walker. */
    synthetic.capability = (volatile uint8_t *)(void *)extended_space;
    synthetic.registers.bar_size = sizeof(extended_space);
    synthetic.max_ports = 8U;
    synthetic.registers.operational = (struct xhci_register_span){
        .offset = UINT64_C(0x180), .length = UINT64_C(0x20), .valid = true
    };
    synthetic.registers.doorbells = (struct xhci_register_span){
        .offset = UINT64_C(0x1A0), .length = UINT64_C(0x20), .valid = true
    };
    synthetic.registers.runtime = (struct xhci_register_span){
        .offset = UINT64_C(0x1C0), .length = UINT64_C(0x20), .valid = true
    };
    for (size_t index = 0U; index <= XHCI_EXT_LIMIT; ++index) {
        extended_space[8U + index] = UINT32_C(3) | UINT32_C(1U << 8);
    }
    if (!test_record(
        offset_already_visited(visited_offsets, 2U, UINT64_C(0x20)) &&
        walk_extended_capabilities(&synthetic, UINT64_C(0x20)) ==
            XHCI_STATUS_EXTENDED_CAPABILITY_LOOP &&
        (zero_bytes(extended_space, sizeof(extended_space)),
            synthetic.protocol_count = 0U,
            synthetic.registers.extended_capability_count = 0U,
            extended_space[8] = UINT32_C(2) | UINT32_C(1U << 8) |
                UINT32_C(2U << 24),
            extended_space[9] = XHCI_PROTOCOL_NAME,
            extended_space[10] = UINT32_C(0x00000101),
            walk_extended_capabilities(&synthetic, UINT64_C(0x20))) ==
                XHCI_STATUS_EXTENDED_CAPABILITY_NON_PROGRESS &&
        (zero_bytes(extended_space, sizeof(extended_space)),
            synthetic.protocol_count = 0U,
            synthetic.registers.extended_capability_count = 0U,
            walk_extended_capabilities(&synthetic, UINT64_C(0x1FE))) ==
                XHCI_STATUS_EXTENDED_CAPABILITY_RANGE &&
        (extended_space[UINT64_C(0x180) / sizeof(uint32_t)] = UINT32_C(3),
            walk_extended_capabilities(&synthetic, UINT64_C(0x180))) ==
                XHCI_STATUS_REGISTER_OVERLAP, &completed)) {
        return false;
    }
    /* 3: legacy ownership expiry uses the same closed deadline predicate. */
    if (!test_record(!deadline_reached(UINT64_C(99), UINT64_C(100)) &&
            deadline_reached(UINT64_C(100), UINT64_C(100)) &&
            XHCI_LEGACY_TIMEOUT_NS != 0U &&
            XHCI_STATUS_LEGACY_OWNERSHIP_TIMEOUT != XHCI_STATUS_OK,
            &completed)) {
        return false;
    }
    /* 4: halt and reset have independent finite expiry results. */
    if (!test_record(XHCI_HALT_TIMEOUT_NS != 0U &&
            XHCI_RESET_TIMEOUT_NS > XHCI_HALT_TIMEOUT_NS &&
            deadline_reached(XHCI_HALT_TIMEOUT_NS,
                XHCI_HALT_TIMEOUT_NS) &&
            deadline_reached(XHCI_RESET_TIMEOUT_NS,
                XHCI_RESET_TIMEOUT_NS) &&
            XHCI_STATUS_HALT_TIMEOUT != XHCI_STATUS_RESET_TIMEOUT,
            &completed)) {
        return false;
    }
    /* 5: unsupported page, context and 32-bit address combinations reject. */
    if (!test_record(validate_page_sizes(0U) ==
            XHCI_STATUS_UNSUPPORTED_PAGE_SIZE &&
        validate_context_size(48U) ==
            XHCI_STATUS_UNSUPPORTED_CONTEXT_SIZE &&
        validate_context_size(XHCI_CONTEXT_32_BYTES) == XHCI_STATUS_OK &&
        validate_context_size(XHCI_CONTEXT_64_BYTES) == XHCI_STATUS_OK &&
        validate_address_width(false, UINT64_C(0xFFFFFFF0), 32U) ==
            XHCI_STATUS_UNSUPPORTED_ADDRESS_WIDTH &&
        validate_address_width(true, UINT64_C(0xFFFFFFF0), 32U) ==
            XHCI_STATUS_OK, &completed)) {
        return false;
    }
    /* 6: scratchpad decode rejects the first count beyond the bound. */
    if (!test_record(decode_scratchpad_count(UINT32_C(1U << 21),
            &scratchpads) == XHCI_STATUS_OK &&
        scratchpads == XHCI_MAX_SCRATCHPADS &&
        decode_scratchpad_count(UINT32_C(1U << 21) |
            UINT32_C(1U << 27), &scratchpads) ==
                XHCI_STATUS_SCRATCHPAD_OVERFLOW, &completed)) {
        return false;
    }
    /* 7: ring containment/alignment and context sizing reject bad geometry. */
    if (!test_record(validate_ring_geometry(UINT64_C(0x1001), 1024U,
            XHCI_RING_TRB_COUNT) == XHCI_STATUS_RING_ALIGNMENT &&
        validate_ring_geometry(UINT64_C(0x1000), 1008U,
            XHCI_RING_TRB_COUNT) == XHCI_STATUS_RING_CONTAINMENT &&
        validate_ring_geometry(UINT64_MAX - 15U, 1024U,
            XHCI_RING_TRB_COUNT) == XHCI_STATUS_RING_CONTAINMENT &&
        (UINT64_C(0x1040) & 63U) == 0U &&
        XHCI_CONTEXT_64_BYTES * 33U <= PHIPIA_PAGE_SIZE, &completed)) {
        return false;
    }
    /* 8: the command validator rejects cycle, ownership and reserved bits. */
    ring.dma.frames.physical_base = UINT64_C(0x1000);
    ring.dma.cpu_address = ring_trbs;
    ring.dma.byte_length = sizeof(ring_trbs);
    ring.dma.active = true;
    ring.dma.owner = DMA_OWNER_CPU;
    ring.active = true;
    ring.kind = XHCI_RING_COMMAND;
    ring.state = XHCI_DMA_OBJECT_CPU_OWNED;
    ring.trb_count = XHCI_RING_TRB_COUNT;
    ring.producer_cycle = 1U;
    write_trb(&ring_trbs[XHCI_RING_TRB_COUNT - 1U], UINT64_C(0x1000), 0U,
        ((uint32_t)XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
            XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE);
    write_trb(&ring_trbs[0], 0U, 0U,
        (uint32_t)XHCI_TRB_TYPE_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT);
    if (!test_record(validate_command_trb(&ring, 0U,
            XHCI_TRB_TYPE_ENABLE_SLOT, 0U) == XHCI_STATUS_RING_CYCLE &&
        (ring_trbs[0].control |= XHCI_TRB_CYCLE,
            ring.state = XHCI_DMA_OBJECT_CONTROLLER_OWNED,
            ring.dma.owner = DMA_OWNER_DEVICE,
            validate_command_trb(&ring, 0U, XHCI_TRB_TYPE_ENABLE_SLOT,
                0U)) == XHCI_STATUS_RING_OWNERSHIP &&
        (ring.state = XHCI_DMA_OBJECT_CPU_OWNED,
            ring.dma.owner = DMA_OWNER_CPU,
            ring_trbs[0].control |= UINT32_C(1U << 7),
            validate_command_trb(&ring, 0U, XHCI_TRB_TYPE_ENABLE_SLOT,
                0U)) == XHCI_STATUS_RING_RESERVED_FIELD, &completed)) {
        return false;
    }
    /* 9: type, pointer, slot, endpoint, cycle and completion all match. */
    if (!test_record(
        validate_event_identity(&expectation, XHCI_TRB_TYPE_TRANSFER_EVENT,
            expectation.pointer, 1U, 1U, XHCI_COMPLETION_SUCCESS, 1U, 1U) ==
                XHCI_STATUS_OK &&
        validate_event_identity(&expectation, XHCI_TRB_TYPE_COMMAND_EVENT,
            expectation.pointer, 1U, 1U, XHCI_COMPLETION_SUCCESS, 1U, 1U) ==
                XHCI_STATUS_TRANSFER_EVENT_MISMATCH &&
        validate_event_identity(&expectation, XHCI_TRB_TYPE_TRANSFER_EVENT,
            expectation.pointer + XHCI_TRB_BYTES, 1U, 1U,
            XHCI_COMPLETION_SUCCESS, 1U, 1U) ==
                XHCI_STATUS_TRANSFER_EVENT_MISMATCH &&
        validate_event_identity(&expectation, XHCI_TRB_TYPE_TRANSFER_EVENT,
            expectation.pointer, 2U, 1U, XHCI_COMPLETION_SUCCESS, 1U, 1U) ==
                XHCI_STATUS_TRANSFER_EVENT_MISMATCH &&
        validate_event_identity(&expectation, XHCI_TRB_TYPE_TRANSFER_EVENT,
            expectation.pointer, 1U, 2U, XHCI_COMPLETION_SUCCESS, 1U, 1U) ==
                XHCI_STATUS_TRANSFER_EVENT_MISMATCH &&
        validate_event_identity(&expectation, XHCI_TRB_TYPE_TRANSFER_EVENT,
            expectation.pointer, 1U, 1U, XHCI_COMPLETION_SUCCESS, 0U, 1U) ==
                XHCI_STATUS_RING_CYCLE &&
        validate_event_identity(&expectation, XHCI_TRB_TYPE_TRANSFER_EVENT,
            expectation.pointer, 1U, 1U, 2U, 1U, 1U) ==
                XHCI_STATUS_TRANSFER_COMPLETION, &completed)) {
        return false;
    }
    /* 10: handler, binding and event-ring ownership are all required. */
    prerequisites.interrupt.handler_ready = true;
    prerequisites.interrupt.active = true;
    prerequisites.interrupt.event_ring_ready = true;
    prerequisites.event.active = true;
    if (!test_record(!interrupt_prerequisites_ready(&prerequisites) &&
        (prerequisites.event.state = XHCI_DMA_OBJECT_CONTROLLER_OWNED,
            interrupt_prerequisites_ready(&prerequisites)), &completed)) {
        return false;
    }
    /* 11: a CPU-owned receive buffer prevents the endpoint doorbell. */
    prerequisites.slot.addressed = true;
    prerequisites.endpoint.ring.active = true;
    prerequisites.endpoint.ring.state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    prerequisites.endpoint.receive.active = true;
    prerequisites.endpoint.receive_state = XHCI_DMA_OBJECT_CPU_OWNED;
    prerequisites.control_slot_cpu[0] = true;
    prerequisites.control_slot_cpu[1] = true;
    prerequisites.control_slot_cpu[2] = true;
    if (!test_record(!endpoint_doorbell_ready(&prerequisites) &&
        (prerequisites.endpoint.receive_state =
            XHCI_DMA_OBJECT_CONTROLLER_OWNED,
            endpoint_doorbell_ready(&prerequisites)), &completed)) {
        return false;
    }
    /* 12: bus-master preparation is false until every DMA object is owned. */
    for (size_t index = 0U; index < 7U; ++index) {
        struct dma_allocation *allocations[7] = {
            &prerequisites.admin,
            &prerequisites.command.dma,
            &prerequisites.event.dma,
            &prerequisites.contexts.input,
            &prerequisites.contexts.output,
            &prerequisites.endpoint.ring.dma,
            &prerequisites.endpoint.receive
        };

        allocations[index]->active = true;
        allocations[index]->initialized = true;
        allocations[index]->owner = DMA_OWNER_DEVICE;
    }
    prerequisites.admin.owner = DMA_OWNER_CPU;
    if (!test_record(!dma_preparation_complete(&prerequisites) &&
        (prerequisites.admin.owner = DMA_OWNER_DEVICE,
            dma_preparation_complete(&prerequisites)), &completed)) {
        return false;
    }
    /* 13: port, slot and both context ownership states gate Address Device. */
    prerequisites.port.enabled = true;
    prerequisites.port.reset_complete = true;
    prerequisites.slot.identifier = XHCI_PROOF_SLOT;
    prerequisites.contexts.active = true;
    prerequisites.contexts.input_state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    prerequisites.contexts.output_state = XHCI_DMA_OBJECT_CONTROLLER_OWNED;
    if (!test_record(!address_device_ready(&prerequisites) &&
        (prerequisites.slot.enabled = true,
            address_device_ready(&prerequisites)), &completed)) {
        return false;
    }
    /* 14: short, oversized and internally inconsistent descriptors differ. */
    if (!test_record(validate_descriptor(descriptor, 17U) ==
            XHCI_STATUS_DESCRIPTOR_SHORT &&
        validate_descriptor(descriptor, 19U) ==
            XHCI_STATUS_DESCRIPTOR_OVERSIZED &&
        (descriptor[17] = 0U,
            validate_descriptor(descriptor, sizeof(descriptor))) ==
                XHCI_STATUS_DESCRIPTOR_INCONSISTENT, &completed)) {
        return false;
    }
    /* 15: only CPU ownership permits inspection or release. */
    if (!test_record(validate_cpu_access(XHCI_DMA_OBJECT_CPU_OWNED) ==
            XHCI_STATUS_OK &&
        validate_cpu_access(XHCI_DMA_OBJECT_CONTROLLER_OWNED) ==
            XHCI_STATUS_DMA_OWNERSHIP &&
        validate_cpu_access(XHCI_DMA_OBJECT_RECLAIMED) ==
            XHCI_STATUS_DMA_OWNERSHIP, &completed)) {
        return false;
    }
    /* 16: all eight partial initialization boundaries unwind in reverse. */
    bool every_boundary_clean = true;
    for (size_t boundary = 0U; boundary <=
            PCI_BUS_MASTER_DMA_CAPACITY; ++boundary) {
        every_boundary_clean = every_boundary_clean &&
            exercise_cleanup_boundary(boundary);
    }
    if (!test_record(every_boundary_clean &&
        resource_state_matches(pci_before, dma_before, vectors_before,
            msix_before, frames_before), &completed)) {
        return false;
    }
    /* 17: the controlled observation hook detects, but never reads, freed DMA. */
    teardown_race.teardown_started = true;
    if (!test_record(teardown_observation_hook(&teardown_race) ==
            XHCI_STATUS_TEARDOWN_RACE &&
        teardown_race.handler_saw_freed_state &&
        (teardown_race.handler_saw_freed_state = false,
            teardown_race.event.dma.active = true,
            teardown_observation_hook(&teardown_race)) == XHCI_STATUS_OK &&
        !teardown_race.handler_saw_freed_state, &completed)) {
        return false;
    }

    if (transition(&lifecycle, XHCI_CONTROLLER_DISCOVERED) != XHCI_STATUS_OK ||
        transition(&lifecycle, XHCI_CONTROLLER_DISCOVERED) !=
            XHCI_STATUS_TRANSITION_REPEATED ||
        transition(&lifecycle, XHCI_CONTROLLER_CLAIMED) != XHCI_STATUS_OK ||
        transition(&lifecycle, XHCI_CONTROLLER_DISCOVERED) !=
            XHCI_STATUS_TRANSITION_REVERSED ||
        transition(&lifecycle, XHCI_CONTROLLER_RUNNING) !=
            XHCI_STATUS_TRANSITION_INVALID ||
        completed != XHCI_FOUNDATION_ROBUSTNESS_TESTS) {
        return false;
    }
    *completed_tests = completed;
    return true;
}

enum xhci_status xhci_descriptor_prove(
    struct xhci_descriptor_proof *proof
)
{
    const struct pci_function *function;
    struct xhci_runtime controller = {0};
    struct pci_resource_state pci_before = pci_resource_get_state();
    struct dma_state dma_before = dma_get_state();
    struct interrupt_vector_state vectors_before =
        interrupt_vector_get_state();
    struct msix_state msix_before = msix_get_state();
    struct frame_allocator_stats frames_before = frame_allocator_get_stats();
    enum xhci_status result;
    enum xhci_status teardown_status;
    size_t scratchpad_count = 0U;
    const uint8_t *descriptor;

    if (proof == NULL) {
        return XHCI_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(proof, sizeof(*proof));
    function = discover_controller(&result);
    if (function == NULL) {
        return result;
    }
    controller.claim.address = function->address;
    controller.claim.generation = ++controller_generation;
    if (transition(&controller.claim, XHCI_CONTROLLER_DISCOVERED) !=
            XHCI_STATUS_OK) {
        return XHCI_STATUS_TRANSITION_INVALID;
    }
    if (pci_claim_device(function, &controller.claim.pci) !=
            PCI_RESOURCE_STATUS_OK) {
        result = XHCI_STATUS_CLAIM_FAILURE;
        goto cleanup;
    }
    if (transition(&controller.claim, XHCI_CONTROLLER_CLAIMED) !=
            XHCI_STATUS_OK) {
        result = XHCI_STATUS_TRANSITION_INVALID;
        goto cleanup;
    }
    if (pci_claim_map_bar(&controller.claim.pci, 0U,
            &controller.registers.mapping) != PCI_RESOURCE_STATUS_OK) {
        result = XHCI_STATUS_MAPPING_FAILURE;
        goto cleanup;
    }
    result = validate_registers(&controller);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    result = legacy_handoff(&controller);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    result = halt_and_reset(&controller);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    result = validate_capabilities(&controller, &scratchpad_count);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    result = select_port(&controller);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    result = prepare_dma(&controller, scratchpad_count);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    result = configure_interrupt(&controller);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    if (transition(&controller.claim, XHCI_CONTROLLER_PREPARED) !=
            XHCI_STATUS_OK) {
        result = XHCI_STATUS_TRANSITION_INVALID;
        goto cleanup;
    }
    result = program_controller(&controller);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    if (transition(&controller.claim, XHCI_CONTROLLER_RUNNING) !=
            XHCI_STATUS_OK) {
        result = XHCI_STATUS_TRANSITION_INVALID;
        goto cleanup;
    }
    proof->controller_ready = true;
    result = reset_port(&controller);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    result = enable_and_address_slot(&controller);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    result = submit_descriptor_transfer(&controller);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    result = validate_sentinel(&controller);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    descriptor = controller.endpoint.receive.cpu_address;
    result = validate_descriptor(descriptor,
        controller.endpoint.returned_length);
    if (result != XHCI_STATUS_OK) {
        goto cleanup;
    }
    proof->descriptor_bytes = controller.endpoint.returned_length;
    proof->msix_completion_count =
        controller.interrupt.count_after_transfer -
            controller.interrupt.count_before_transfer;
    proof->ignored_events = controller.ignored_events;
    proof->descriptor_valid = true;
    proof->sentinel_changed_while_controller_owned = true;
    proof->ownership_complete = true;
    result = XHCI_STATUS_OK;

cleanup:
    proof->controller_version = controller.version;
    proof->root_port = controller.port.identifier;
    proof->slot = controller.slot.identifier;
    proof->vector = controller.interrupt.vector;
    proof->trb_type = controller.expectation.trb_type;
    proof->ignored_events = controller.ignored_events;
    teardown_status = teardown_controller(&controller);
    if (teardown_status != XHCI_STATUS_OK) {
        result = teardown_status;
    }
    if (!resource_state_matches(pci_before, dma_before, vectors_before,
            msix_before, frames_before)) {
        result = XHCI_STATUS_TEARDOWN_FAILURE;
    }
    proof->teardown_complete = result == XHCI_STATUS_OK;
    if (result == XHCI_STATUS_OK) {
        proof->robustness_tests = XHCI_CONTROLLED_ROBUSTNESS_TESTS;
        installed_proof = *proof;
    }
    return result;
}

struct xhci_descriptor_proof xhci_get_descriptor_proof(void)
{
    return installed_proof;
}

const char *xhci_status_string(enum xhci_status status)
{
    static const char *const messages[XHCI_STATUS_COUNT] = {
        "ok", "xHCI proof fixture is absent", "null xHCI argument",
        "more than one xHCI controller is unsupported",
        "PCI function is not the xHCI class tuple", "xHCI PCI claim failed",
        "xHCI BAR mapping failed", "xHCI register lies outside its BAR",
        "xHCI register range overflowed", "xHCI register is misaligned",
        "xHCI register regions overlap", "xHCI extended capability is out of range",
        "xHCI extended capability list is cyclic",
        "xHCI extended capability list did not progress",
        "xHCI legacy ownership handshake timed out",
        "xHCI controller-ready wait timed out",
        "xHCI host-controller error was asserted", "xHCI halt timed out",
        "xHCI reset timed out", "xHCI version is unsupported",
        "xHCI controller has no usable slot", "xHCI controller has no root port",
        "xHCI controller has no interrupter", "xHCI lacks a 4 KiB page size",
        "xHCI selected an unsupported context size",
        "xHCI address-width combination is unsupported",
        "xHCI scratchpad requirement overflowed", "xHCI DMA allocation failed",
        "xHCI DMA layout is invalid", "xHCI DMA ownership was violated",
        "xHCI ring is misaligned", "xHCI TRB is outside its ring",
        "xHCI TRB reserved field is nonzero", "xHCI TRB type is invalid",
        "xHCI TRB cycle is invalid", "xHCI ring ownership is invalid",
        "xHCI TRB pointer identity is invalid", "xHCI ring is full",
        "xHCI ERST is invalid", "xHCI context is invalid",
        "no connected xHCI root port", "more than one connected root port",
        "connected root port protocol is unsupported", "xHCI root-port reset timed out",
        "xHCI root port did not enable", "xHCI interrupt prerequisites are incomplete",
        "xHCI MSI-X binding failed", "xHCI MSI-X rollback leaked state",
        "xHCI bus mastering was attempted before DMA preparation",
        "xHCI bus mastering failed", "xHCI command precondition failed",
        "xHCI command timed out", "xHCI command event did not match",
        "xHCI command completion failed", "Address Device precondition failed",
        "xHCI doorbell precondition failed", "xHCI control transfer timed out",
        "xHCI transfer event did not match", "xHCI transfer completion failed",
        "xHCI transfer length is invalid", "USB descriptor is short",
        "USB descriptor is oversized", "USB descriptor type or length is invalid",
        "USB descriptor fields are inconsistent", "USB descriptor sentinel failed",
        "xHCI MSI-X count did not advance exactly once",
        "xHCI lifecycle transition was repeated",
        "xHCI lifecycle transition was reversed",
        "xHCI lifecycle transition is invalid", "xHCI teardown race observed freed state",
        "xHCI teardown leaked or failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        XHCI_STATUS_COUNT, "xHCI status messages are out of sync");
    if (status < XHCI_STATUS_OK || status >= XHCI_STATUS_COUNT) {
        return "unknown xHCI status";
    }
    return messages[status];
}
