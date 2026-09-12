/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/apic.h>
#include <phipia/cpu.h>
#include <phipia/interrupt_vector.h>
#include <phipia/interrupts.h>
#include <phipia/msix.h>
#include <phipia/pci.h>
#include <phipia/pci_resource.h>

#define MSIX_CAPABILITY_LENGTH UINT16_C(12)
#define MSIX_CONTROL_OFFSET UINT16_C(2)
#define MSIX_TABLE_OFFSET UINT16_C(4)
#define MSIX_PBA_OFFSET UINT16_C(8)
#define MSIX_CONTROL_TABLE_SIZE UINT16_C(0x07FF)
#define MSIX_CONTROL_FUNCTION_MASK UINT16_C(0x4000)
#define MSIX_CONTROL_ENABLE UINT16_C(0x8000)
#define MSIX_BIR_MASK UINT32_C(0x00000007)
#define MSIX_BAR_OFFSET_MASK UINT32_C(0xFFFFFFF8)
#define MSIX_TABLE_ENTRY_SIZE UINT64_C(16)
#define MSIX_VECTOR_MASK UINT32_C(0x00000001)
#define MSIX_MESSAGE_ADDRESS_BASE UINT32_C(0xFEE00000)
#define MSIX_MESSAGE_DESTINATION_SHIFT 12U

static struct msix_state state;

static const struct pci_function *function_for_claim(
    const struct pci_device_claim *claim
)
{
    for (size_t index = 0U; index < pci_function_count(); ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function != NULL &&
            function->address.segment == claim->device.segment &&
            function->address.bus == claim->device.bus &&
            function->address.device == claim->device.device &&
            function->address.function == claim->device.function) {
            return function;
        }
    }
    return NULL;
}

static enum msix_status config_read(
    struct pci_address address,
    uint16_t offset,
    uint32_t *value
)
{
    return pci_config_read_port(address, offset, value) == PCI_STATUS_OK
        ? MSIX_STATUS_OK
        : MSIX_STATUS_CAPABILITY_MALFORMED;
}

static enum msix_status write_control(
    struct pci_address address,
    uint8_t capability_offset,
    uint16_t control
)
{
    return pci_config_write_port(address,
        (uint16_t)(capability_offset + MSIX_CONTROL_OFFSET),
        sizeof(uint16_t), control) == PCI_STATUS_OK
        ? MSIX_STATUS_OK
        : MSIX_STATUS_PROGRAMMING_FAILURE;
}

static enum msix_status validate_span(
    const struct pci_bar_description *bar,
    uint64_t offset,
    uint64_t length,
    enum msix_status outside_status
)
{
    if (bar == NULL || !bar->implemented ||
        (bar->kind != PCI_BAR_MEMORY_32 &&
            bar->kind != PCI_BAR_MEMORY_64)) {
        return MSIX_STATUS_BAD_BIR;
    }
    if (length == 0U || offset > bar->size || length > bar->size - offset) {
        return outside_status;
    }
    return MSIX_STATUS_OK;
}

static enum msix_status enable_delivery(
    struct msix_binding *binding
)
{
    uint16_t control;

    if (!binding->handler_installed) {
        return MSIX_STATUS_HANDLER_NOT_INSTALLED;
    }

    control = (uint16_t)(binding->original_control |
        MSIX_CONTROL_ENABLE | MSIX_CONTROL_FUNCTION_MASK);
    if (write_control(binding->claim->device, binding->capability_offset,
            control) != MSIX_STATUS_OK) {
        return MSIX_STATUS_PROGRAMMING_FAILURE;
    }

    binding->entry[3] &= ~MSIX_VECTOR_MASK;
    cpu_store_fence();
    __asm__ volatile ("" : : : "memory");

    control &= (uint16_t)~MSIX_CONTROL_FUNCTION_MASK;
    if (write_control(binding->claim->device, binding->capability_offset,
            control) != MSIX_STATUS_OK) {
        binding->entry[3] |= MSIX_VECTOR_MASK;
        cpu_store_fence();
        return MSIX_STATUS_PROGRAMMING_FAILURE;
    }
    binding->delivery_masked = false;
    return MSIX_STATUS_OK;
}

static enum msix_status enable_masked(
    struct msix_binding *binding
)
{
    uint16_t control;

    if (!binding->handler_installed) {
        return MSIX_STATUS_HANDLER_NOT_INSTALLED;
    }
    control = (uint16_t)(binding->original_control |
        MSIX_CONTROL_ENABLE | MSIX_CONTROL_FUNCTION_MASK);
    if (write_control(binding->claim->device, binding->capability_offset,
            control) != MSIX_STATUS_OK) {
        return MSIX_STATUS_PROGRAMMING_FAILURE;
    }
    binding->entry[3] |= MSIX_VECTOR_MASK;
    cpu_store_fence();
    __asm__ volatile ("" : : : "memory");
    binding->delivery_masked = true;
    return MSIX_STATUS_OK;
}

static enum msix_status rollback_binding(struct msix_binding *binding)
{
    bool failed = false;
    uint32_t restored_header = 0U;

    if (binding->claim != NULL && binding->capability_offset != 0U) {
        if (write_control(binding->claim->device, binding->capability_offset,
                (uint16_t)(binding->original_control |
                    MSIX_CONTROL_FUNCTION_MASK)) != MSIX_STATUS_OK) {
            failed = true;
        }
    }

    if (binding->entry != NULL) {
        binding->entry[3] |= MSIX_VECTOR_MASK;
        cpu_store_fence();
    }

    if (binding->handler_installed) {
        if (interrupt_unregister_handler(binding->vector.vector) !=
                INTERRUPT_STATUS_OK) {
            failed = true;
        } else {
            binding->handler_installed = false;
        }
    }

    if (binding->entry != NULL) {
        binding->entry[0] = binding->original_entry[0];
        binding->entry[1] = binding->original_entry[1];
        binding->entry[2] = binding->original_entry[2];
        binding->entry[3] = binding->original_entry[3];
        cpu_store_fence();
        for (size_t index = 0U; index < 4U; ++index) {
            if (binding->entry[index] != binding->original_entry[index]) {
                failed = true;
            }
        }
    }

    /* Never recycle a vector while a handler may still be reachable. */
    if (binding->vector.active) {
        if (binding->handler_installed) {
            failed = true;
        } else if (interrupt_vector_release(&binding->vector) !=
                INTERRUPT_VECTOR_STATUS_OK) {
            failed = true;
        }
    }

    if (binding->pba_mapped_here && binding->pba_bar != binding->table_bar) {
        if (pci_claim_unmap_last_bar(binding->claim, binding->pba_bar) !=
                PCI_RESOURCE_STATUS_OK) {
            failed = true;
        } else {
            binding->pba_mapped_here = false;
        }
    }
    if (binding->table_mapped_here) {
        if (pci_claim_unmap_last_bar(binding->claim, binding->table_bar) !=
                PCI_RESOURCE_STATUS_OK) {
            failed = true;
        } else {
            binding->table_mapped_here = false;
        }
    }

    if (binding->claim != NULL && binding->capability_offset != 0U &&
        write_control(binding->claim->device, binding->capability_offset,
            binding->original_control) != MSIX_STATUS_OK) {
        failed = true;
    } else if (binding->claim != NULL && binding->capability_offset != 0U &&
        (config_read(binding->claim->device, binding->capability_offset,
                &restored_header) != MSIX_STATUS_OK ||
            (uint16_t)(restored_header >> 16U) !=
                binding->original_control)) {
        failed = true;
    }

    binding->active = false;
    binding->delivery_masked = true;
    binding->entry = NULL;
    ++state.rollback_count;
    return failed ? MSIX_STATUS_ROLLBACK_FAILURE : MSIX_STATUS_OK;
}

static enum msix_status bind_internal(
    struct pci_device_claim *claim,
    uint16_t entry_index,
    interrupt_handler_t handler,
    void *context,
    struct msix_binding *binding,
    bool initially_masked
)
{
    const struct pci_function *function;
    const struct pci_bar_description *table_bar;
    const struct pci_bar_description *pba_bar;
    struct pci_mmio_region *table_region;
    struct pci_mmio_region *pba_region;
    volatile void *entry_pointer = NULL;
    uint32_t header = 0U;
    uint32_t table = 0U;
    uint32_t pba = 0U;
    uint64_t table_length;
    uint64_t pba_length;
    uint64_t entry_offset;
    enum msix_status status = MSIX_STATUS_OK;
    const bool inject_failure = state.failure_injection_armed;

    /* The hook belongs to exactly the next bind attempt, including refusal. */
    state.failure_injection_armed = false;

    if (claim == NULL || handler == NULL || binding == NULL) {
        return MSIX_STATUS_NULL_ARGUMENT;
    }
    if (cpu_interrupts_enabled()) {
        return MSIX_STATUS_INTERRUPTS_ENABLED;
    }
    if (!claim->active) {
        return MSIX_STATUS_BAD_CLAIM;
    }
    if (binding->active) {
        return MSIX_STATUS_ALREADY_BOUND;
    }

    for (size_t byte = 0U; byte < sizeof(*binding); ++byte) {
        ((uint8_t *)binding)[byte] = 0U;
    }
    binding->claim = claim;
    function = function_for_claim(claim);
    if (function == NULL) {
        return MSIX_STATUS_BAD_CLAIM;
    }
    if (function->msi_x_offset == 0U ||
        function->msi_x_offset > PCI_CONFIG_SPACE_SIZE -
            MSIX_CAPABILITY_LENGTH) {
        return MSIX_STATUS_CAPABILITY_MISSING;
    }
    binding->capability_offset = function->msi_x_offset;

    if (config_read(claim->device, function->msi_x_offset, &header) !=
            MSIX_STATUS_OK ||
        config_read(claim->device,
            (uint16_t)(function->msi_x_offset + MSIX_TABLE_OFFSET), &table) !=
            MSIX_STATUS_OK ||
        config_read(claim->device,
            (uint16_t)(function->msi_x_offset + MSIX_PBA_OFFSET), &pba) !=
            MSIX_STATUS_OK ||
        (uint8_t)header != PCI_CAPABILITY_MSI_X) {
        return MSIX_STATUS_CAPABILITY_MALFORMED;
    }

    binding->original_control = (uint16_t)(header >> 16U);
    if ((binding->original_control & MSIX_CONTROL_ENABLE) != 0U) {
        return MSIX_STATUS_ALREADY_ENABLED;
    }
    binding->table_size = (uint16_t)((binding->original_control &
        MSIX_CONTROL_TABLE_SIZE) + 1U);
    /* The encoded N-1 field cannot produce zero; retain a defensive guard. */
    if (binding->table_size == 0U) {
        return MSIX_STATUS_BAD_TABLE_SIZE;
    }
    if (entry_index >= binding->table_size) {
        return MSIX_STATUS_BAD_ENTRY_INDEX;
    }
    binding->entry_index = entry_index;
    binding->table_bar = (uint8_t)(table & MSIX_BIR_MASK);
    binding->pba_bar = (uint8_t)(pba & MSIX_BIR_MASK);
    if (binding->table_bar >= PCI_BAR_COUNT ||
        binding->pba_bar >= PCI_BAR_COUNT) {
        return MSIX_STATUS_BAD_BIR;
    }

    table_length = (uint64_t)binding->table_size * MSIX_TABLE_ENTRY_SIZE;
    pba_length = ((uint64_t)binding->table_size + 63U) / 64U * 8U;
    table_bar = pci_claim_bar(claim, binding->table_bar);
    pba_bar = pci_claim_bar(claim, binding->pba_bar);
    status = validate_span(table_bar, table & MSIX_BAR_OFFSET_MASK,
        table_length, MSIX_STATUS_TABLE_OUTSIDE_BAR);
    if (status == MSIX_STATUS_OK) {
        status = validate_span(pba_bar, pba & MSIX_BAR_OFFSET_MASK,
            pba_length, MSIX_STATUS_PBA_OUTSIDE_BAR);
    }
    if (status != MSIX_STATUS_OK) {
        return status;
    }

    status = write_control(claim->device, binding->capability_offset,
        (uint16_t)(binding->original_control | MSIX_CONTROL_FUNCTION_MASK));
    if (status != MSIX_STATUS_OK) {
        return status;
    }

    table_region = pci_claim_mapped_bar(claim, binding->table_bar);
    if (table_region == NULL) {
        if (pci_claim_map_bar(claim, binding->table_bar, &table_region) !=
                PCI_RESOURCE_STATUS_OK) {
            status = MSIX_STATUS_MAPPING_FAILURE;
            goto fail;
        }
        binding->table_mapped_here = true;
    }

    pba_region = pci_claim_mapped_bar(claim, binding->pba_bar);
    if (pba_region == NULL) {
        if (pci_claim_map_bar(claim, binding->pba_bar, &pba_region) !=
                PCI_RESOURCE_STATUS_OK) {
            status = MSIX_STATUS_MAPPING_FAILURE;
            goto fail;
        }
        binding->pba_mapped_here = true;
    }

    entry_offset = (table & MSIX_BAR_OFFSET_MASK) +
        (uint64_t)entry_index * MSIX_TABLE_ENTRY_SIZE;
    if (pci_mmio_subregion(table_region, entry_offset,
            MSIX_TABLE_ENTRY_SIZE, &entry_pointer) != PCI_RESOURCE_STATUS_OK) {
        status = MSIX_STATUS_MAPPING_FAILURE;
        goto fail;
    }
    binding->entry = (volatile uint32_t *)entry_pointer;
    for (size_t index = 0U; index < 4U; ++index) {
        binding->original_entry[index] = binding->entry[index];
    }

    if (interrupt_vector_allocate(&binding->vector) !=
            INTERRUPT_VECTOR_STATUS_OK) {
        status = MSIX_STATUS_VECTOR_FAILURE;
        goto fail;
    }

    const struct apic_state apic = apic_get_state();
    binding->entry[3] = binding->original_entry[3] | MSIX_VECTOR_MASK;
    binding->entry[0] = MSIX_MESSAGE_ADDRESS_BASE |
        ((apic.id & UINT32_C(0xFF)) << MSIX_MESSAGE_DESTINATION_SHIFT);
    binding->entry[1] = 0U;
    binding->entry[2] = binding->vector.vector;
    cpu_store_fence();
    if (binding->entry[0] != (MSIX_MESSAGE_ADDRESS_BASE |
            ((apic.id & UINT32_C(0xFF)) <<
                MSIX_MESSAGE_DESTINATION_SHIFT)) ||
        binding->entry[1] != 0U ||
        (binding->entry[2] & UINT32_C(0xFF)) != binding->vector.vector ||
        (binding->entry[3] & MSIX_VECTOR_MASK) == 0U) {
        status = MSIX_STATUS_PROGRAMMING_FAILURE;
        goto fail;
    }

    if (interrupt_register_handler(binding->vector.vector, handler, context) !=
            INTERRUPT_STATUS_OK) {
        status = MSIX_STATUS_HANDLER_FAILURE;
        goto fail;
    }
    binding->handler_installed = true;

    if (inject_failure) {
        status = MSIX_STATUS_INJECTED_FAILURE;
        goto fail;
    }

    status = initially_masked ? enable_masked(binding) :
        enable_delivery(binding);
    if (status != MSIX_STATUS_OK) {
        goto fail;
    }

    binding->active = true;
    ++state.active_bindings;
    return MSIX_STATUS_OK;

fail:
    if (rollback_binding(binding) != MSIX_STATUS_OK) {
        return MSIX_STATUS_ROLLBACK_FAILURE;
    }
    return status;
}

enum msix_status msix_bind(
    struct pci_device_claim *claim,
    uint16_t entry_index,
    interrupt_handler_t handler,
    void *context,
    struct msix_binding *binding
)
{
    return bind_internal(claim, entry_index, handler, context, binding, false);
}

enum msix_status msix_bind_masked(
    struct pci_device_claim *claim,
    uint16_t entry_index,
    interrupt_handler_t handler,
    void *context,
    struct msix_binding *binding
)
{
    return bind_internal(claim, entry_index, handler, context, binding, true);
}

enum msix_status msix_set_masked(
    struct msix_binding *binding,
    bool masked
)
{
    if (binding == NULL) {
        return MSIX_STATUS_NULL_ARGUMENT;
    }
    if (cpu_interrupts_enabled()) {
        return MSIX_STATUS_INTERRUPTS_ENABLED;
    }
    if (!binding->active) {
        return MSIX_STATUS_NOT_BOUND;
    }
    if (masked == binding->delivery_masked) {
        return MSIX_STATUS_OK;
    }
    if (masked) {
        return enable_masked(binding);
    }
    return enable_delivery(binding);
}

enum msix_status msix_unbind(struct msix_binding *binding)
{
    enum msix_status status;

    if (binding == NULL) {
        return MSIX_STATUS_NULL_ARGUMENT;
    }
    if (cpu_interrupts_enabled()) {
        return MSIX_STATUS_INTERRUPTS_ENABLED;
    }
    if (!binding->active) {
        return MSIX_STATUS_NOT_BOUND;
    }

    binding->active = false;
    --state.active_bindings;
    status = rollback_binding(binding);
    return status;
}

void msix_test_inject_failure_once(void)
{
    state.failure_injection_armed = true;
}

struct msix_state msix_get_state(void)
{
    return state;
}

bool msix_self_test(void)
{
    struct pci_bar_description bar = {
        .kind = PCI_BAR_MEMORY_32,
        .base = UINT64_C(0x10000000),
        .size = UINT64_C(0x1000),
        .implemented = true
    };
    struct msix_binding binding = {0};

    if (validate_span(&bar, UINT64_C(0xFF0), UINT64_C(0x20),
            MSIX_STATUS_TABLE_OUTSIDE_BAR) !=
            MSIX_STATUS_TABLE_OUTSIDE_BAR ||
        validate_span(&bar, UINT64_C(0x800), UINT64_C(0x100),
            MSIX_STATUS_TABLE_OUTSIDE_BAR) != MSIX_STATUS_OK) {
        return false;
    }

    binding.handler_installed = false;
    if (enable_delivery(&binding) != MSIX_STATUS_HANDLER_NOT_INSTALLED) {
        return false;
    }

    return state.active_bindings == 0U;
}

const char *msix_status_string(enum msix_status status)
{
    static const char *const messages[MSIX_STATUS_COUNT] = {
        "ok", "null MSI-X argument", "MSI-X mutation requires IF cleared",
        "MSI-X binding does not own its PCI device",
        "PCI function has no MSI-X capability",
        "MSI-X capability structure is malformed",
        "MSI-X was already enabled before ownership",
        "MSI-X table size is invalid", "MSI-X table entry is out of range",
        "MSI-X BIR does not name a memory BAR",
        "MSI-X table falls outside its sized BAR",
        "MSI-X pending-bit array falls outside its sized BAR",
        "MSI-X BAR mapping failed", "MSI-X vector allocation failed",
        "MSI-X handler registration failed",
        "MSI-X delivery cannot precede handler registration",
        "MSI-X table or control programming failed",
        "MSI-X binding is already active", "MSI-X binding is not active",
        "MSI-X rollback did not release every acquired resource",
        "MSI-X setup injected a failure after handler registration"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        MSIX_STATUS_COUNT, "MSI-X status messages are out of sync");
    if (status < MSIX_STATUS_OK || status >= MSIX_STATUS_COUNT) {
        return "unknown MSI-X status";
    }
    return messages[status];
}
