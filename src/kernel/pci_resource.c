/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cpu.h>
#include <phipia/memory.h>
#include <phipia/paging.h>
#include <phipia/pci.h>
#include <phipia/pci_resource.h>

#define PCI_REGISTER_BAR_BASE UINT16_C(0x10)
#define PCI_BAR_STRIDE UINT16_C(4)
#define PCI_BAR_IO_INDICATOR UINT32_C(0x00000001)
#define PCI_BAR_IO_BASE_MASK UINT32_C(0xFFFFFFFC)
#define PCI_BAR_MEMORY_TYPE_MASK UINT32_C(0x00000006)
#define PCI_BAR_MEMORY_TYPE_32 UINT32_C(0x00000000)
#define PCI_BAR_MEMORY_TYPE_64 UINT32_C(0x00000004)
#define PCI_BAR_MEMORY_PREFETCHABLE UINT32_C(0x00000008)
#define PCI_BAR_MEMORY_BASE_MASK UINT32_C(0xFFFFFFF0)

#define MMIO_ARENA_PAGE_COUNT \
    ((size_t)(PCI_DEVICE_MMIO_ARENA_SIZE / PAGING_PAGE_SIZE))
#define MMIO_ARENA_BITMAP_BYTES ((MMIO_ARENA_PAGE_COUNT + 7U) / 8U)

struct claim_record {
    struct pci_address device;
    uint64_t identifier;
    size_t bar_count;
    struct pci_bar_description bars[PCI_BAR_COUNT];
    bool active;
};

static struct pci_resource_state state;
static struct claim_record claim_records[PCI_ACTIVE_CLAIM_CAPACITY];
static uint8_t arena_bitmap[MMIO_ARENA_BITMAP_BYTES];
static uint64_t next_claim_identifier;

_Static_assert(PCI_DEVICE_MMIO_ARENA_BASE % PAGING_PAGE_SIZE == 0U,
    "device MMIO arena base is not page aligned");
_Static_assert(PCI_DEVICE_MMIO_ARENA_SIZE % PAGING_PAGE_SIZE == 0U,
    "device MMIO arena size is not page aligned");
_Static_assert(PCI_DEVICE_MMIO_ARENA_BASE > PCI_DEVICE_MMIO_ARENA_SIZE,
    "device MMIO arena guard arithmetic would underflow");
_Static_assert(PCI_DEVICE_MMIO_ARENA_BASE + PCI_DEVICE_MMIO_ARENA_SIZE >
    PCI_DEVICE_MMIO_ARENA_BASE,
    "device MMIO arena range overflows");

static bool addresses_equal(struct pci_address left, struct pci_address right)
{
    return left.segment == right.segment && left.bus == right.bus &&
        left.device == right.device && left.function == right.function;
}

static bool power_of_two(uint64_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static bool range_end_exclusive(
    uint64_t base,
    uint64_t size,
    uint64_t *end
)
{
    if (end == NULL || size == 0U || size > UINT64_MAX - base) {
        return false;
    }
    *end = base + size;
    return true;
}

static bool ranges_overlap(
    uint64_t first_base,
    uint64_t first_size,
    uint64_t second_base,
    uint64_t second_size
)
{
    uint64_t first_end;
    uint64_t second_end;

    return range_end_exclusive(first_base, first_size, &first_end) &&
        range_end_exclusive(second_base, second_size, &second_end) &&
        first_base < second_end && second_base < first_end;
}

static bool arena_bit(size_t page)
{
    return (arena_bitmap[page / 8U] &
        (uint8_t)(1U << (page % 8U))) != 0U;
}

static void arena_set(size_t page, bool used)
{
    const uint8_t mask = (uint8_t)(1U << (page % 8U));

    if (used) {
        arena_bitmap[page / 8U] |= mask;
    } else {
        arena_bitmap[page / 8U] &= (uint8_t)~mask;
    }
}

static enum pci_resource_status config_read(
    struct pci_address address,
    uint16_t offset,
    uint32_t *value
)
{
    return pci_config_read_port(address, offset, value) == PCI_STATUS_OK
        ? PCI_RESOURCE_STATUS_OK
        : PCI_RESOURCE_STATUS_CONFIG_ACCESS;
}

static enum pci_resource_status config_write(
    struct pci_address address,
    uint16_t offset,
    size_t width,
    uint32_t value
)
{
    return pci_config_write_port(address, offset, width, value) == PCI_STATUS_OK
        ? PCI_RESOURCE_STATUS_OK
        : PCI_RESOURCE_STATUS_CONFIG_ACCESS;
}

static enum pci_resource_status read_command(
    struct pci_address address,
    uint16_t *command
)
{
    uint32_t value = 0U;
    enum pci_resource_status status = config_read(address,
        PCI_REGISTER_COMMAND, &value);

    if (status == PCI_RESOURCE_STATUS_OK) {
        *command = (uint16_t)value;
    }
    return status;
}

static enum pci_resource_status write_command(
    struct pci_address address,
    uint16_t command
)
{
    return config_write(address, PCI_REGISTER_COMMAND, sizeof(uint16_t),
        command);
}

static enum pci_resource_status require_decode_disabled(uint16_t command)
{
    return (command & (PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE)) == 0U
        ? PCI_RESOURCE_STATUS_OK
        : PCI_RESOURCE_STATUS_DECODE_ENABLED;
}

static enum pci_resource_status validate_bar(
    struct pci_bar_description *bar
)
{
    uint64_t end;

    if (!bar->implemented) {
        return PCI_RESOURCE_STATUS_OK;
    }

    if (bar->size == 0U) {
        return PCI_RESOURCE_STATUS_ZERO_BAR_SIZE;
    }
    if (!power_of_two(bar->size)) {
        return PCI_RESOURCE_STATUS_NON_POWER_OF_TWO_BAR;
    }
    if (bar->base == 0U) {
        return PCI_RESOURCE_STATUS_UNASSIGNED_BAR;
    }
    if (bar->size - 1U > UINT64_MAX - bar->base) {
        return PCI_RESOURCE_STATUS_BAR_RANGE_OVERFLOW;
    }
    if ((bar->base & (bar->size - 1U)) != 0U) {
        return PCI_RESOURCE_STATUS_MISALIGNED_BAR;
    }
    end = bar->base + bar->size - 1U;
    if (end < bar->base) {
        return PCI_RESOURCE_STATUS_BAR_RANGE_OVERFLOW;
    }
    return PCI_RESOURCE_STATUS_OK;
}

static enum pci_resource_status validate_64_bit_pair(
    size_t bar_count,
    size_t index
)
{
    return index + 1U < bar_count
        ? PCI_RESOURCE_STATUS_OK
        : PCI_RESOURCE_STATUS_MALFORMED_64_BIT_PAIR;
}

static enum pci_resource_status restore_probe_state(
    struct pci_address address,
    const uint32_t original[PCI_BAR_COUNT],
    size_t bar_count,
    uint16_t command
)
{
    uint32_t value = 0U;
    uint16_t restored_command = 0U;

    for (size_t index = 0U; index < bar_count; ++index) {
        if (config_write(address,
                (uint16_t)(PCI_REGISTER_BAR_BASE + index * PCI_BAR_STRIDE),
                sizeof(uint32_t), original[index]) !=
                PCI_RESOURCE_STATUS_OK) {
            return PCI_RESOURCE_STATUS_RESTORE_FAILURE;
        }
    }

    if (write_command(address, command) != PCI_RESOURCE_STATUS_OK) {
        return PCI_RESOURCE_STATUS_RESTORE_FAILURE;
    }

    for (size_t index = 0U; index < bar_count; ++index) {
        if (config_read(address,
                (uint16_t)(PCI_REGISTER_BAR_BASE + index * PCI_BAR_STRIDE),
                &value) != PCI_RESOURCE_STATUS_OK || value != original[index]) {
            return PCI_RESOURCE_STATUS_RESTORE_FAILURE;
        }
    }

    if (read_command(address, &restored_command) != PCI_RESOURCE_STATUS_OK ||
        restored_command != command) {
        return PCI_RESOURCE_STATUS_RESTORE_FAILURE;
    }

    return PCI_RESOURCE_STATUS_OK;
}

static enum pci_resource_status probe_bars(
    const struct pci_function *function,
    struct pci_device_claim *claim,
    bool inject_failure
)
{
    uint32_t original[PCI_BAR_COUNT] = {0U};
    uint16_t command;
    uint16_t disabled;
    uint16_t observed = 0U;
    enum pci_resource_status status;
    enum pci_resource_status restore_status;
    size_t bar_count;

    if (function->header_type == PCI_HEADER_TYPE_ENDPOINT) {
        bar_count = PCI_BAR_COUNT;
    } else if (function->header_type == PCI_HEADER_TYPE_BRIDGE) {
        bar_count = 2U;
    } else {
        return PCI_RESOURCE_STATUS_UNSUPPORTED_HEADER;
    }

    status = read_command(function->address, &command);
    if (status != PCI_RESOURCE_STATUS_OK) {
        return status;
    }

    for (size_t index = 0U; index < bar_count; ++index) {
        status = config_read(function->address,
            (uint16_t)(PCI_REGISTER_BAR_BASE + index * PCI_BAR_STRIDE),
            &original[index]);
        if (status != PCI_RESOURCE_STATUS_OK) {
            return status;
        }
    }

    disabled = command & (uint16_t)~(PCI_COMMAND_IO_SPACE |
        PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);
    status = write_command(function->address, disabled);
    if (status != PCI_RESOURCE_STATUS_OK) {
        return status;
    }
    status = read_command(function->address, &observed);
    if (status == PCI_RESOURCE_STATUS_OK) {
        status = require_decode_disabled(observed);
    }

    for (size_t index = 0U;
         status == PCI_RESOURCE_STATUS_OK && index < bar_count;
         ++index) {
        const uint16_t offset = (uint16_t)(PCI_REGISTER_BAR_BASE +
            index * PCI_BAR_STRIDE);
        const uint32_t low = original[index];
        uint32_t mask_low = 0U;
        struct pci_bar_description *bar = &claim->bars[index];

        bar->device = function->address;
        bar->index = (uint8_t)index;
        bar->kind = PCI_BAR_UNIMPLEMENTED;
        bar->attribute_bits = 0U;
        bar->pair_index = UINT8_MAX;
        bar->base = 0U;
        bar->size = 0U;
        bar->prefetchable = false;
        bar->implemented = false;

        status = config_write(function->address, offset, sizeof(uint32_t),
            UINT32_MAX);
        if (status != PCI_RESOURCE_STATUS_OK) {
            break;
        }

        if (inject_failure) {
            status = PCI_RESOURCE_STATUS_INJECTED_FAILURE;
            break;
        }

        status = config_read(function->address, offset, &mask_low);
        if (status != PCI_RESOURCE_STATUS_OK) {
            break;
        }

        if (mask_low == 0U) {
            continue;
        }

        if ((low & PCI_BAR_IO_INDICATOR) != 0U) {
            const uint32_t mask = mask_low & PCI_BAR_IO_BASE_MASK;
            const uint32_t size = (uint32_t)(~mask + 1U);

            bar->kind = PCI_BAR_IO;
            bar->attribute_bits = (uint8_t)(low & ~PCI_BAR_IO_BASE_MASK);
            bar->base = low & PCI_BAR_IO_BASE_MASK;
            bar->size = size;
            bar->implemented = true;
            status = validate_bar(bar);
            continue;
        }

        bar->attribute_bits = (uint8_t)(low & ~PCI_BAR_MEMORY_BASE_MASK);
        bar->prefetchable = (low & PCI_BAR_MEMORY_PREFETCHABLE) != 0U;
        if ((low & PCI_BAR_MEMORY_TYPE_MASK) == PCI_BAR_MEMORY_TYPE_32) {
            const uint32_t mask = mask_low & PCI_BAR_MEMORY_BASE_MASK;
            const uint32_t size = (uint32_t)(~mask + 1U);

            bar->kind = PCI_BAR_MEMORY_32;
            bar->base = low & PCI_BAR_MEMORY_BASE_MASK;
            bar->size = size;
            bar->implemented = true;
            status = validate_bar(bar);
            continue;
        }

        if ((low & PCI_BAR_MEMORY_TYPE_MASK) != PCI_BAR_MEMORY_TYPE_64) {
            status = PCI_RESOURCE_STATUS_RESERVED_MEMORY_TYPE;
            break;
        }

        status = validate_64_bit_pair(bar_count, index);
        if (status != PCI_RESOURCE_STATUS_OK) {
            break;
        }

        uint32_t mask_high = 0U;
        const uint16_t high_offset = (uint16_t)(offset + PCI_BAR_STRIDE);
        status = config_write(function->address, high_offset,
            sizeof(uint32_t), UINT32_MAX);
        if (status == PCI_RESOURCE_STATUS_OK) {
            status = config_read(function->address, high_offset, &mask_high);
        }
        if (status != PCI_RESOURCE_STATUS_OK) {
            break;
        }

        const uint64_t mask = ((uint64_t)mask_high << 32U) |
            (mask_low & PCI_BAR_MEMORY_BASE_MASK);
        bar->kind = PCI_BAR_MEMORY_64;
        bar->pair_index = (uint8_t)(index + 1U);
        bar->base = ((uint64_t)original[index + 1U] << 32U) |
            (low & PCI_BAR_MEMORY_BASE_MASK);
        bar->size = ~mask + 1U;
        bar->implemented = true;
        status = validate_bar(bar);

        claim->bars[index + 1U].device = function->address;
        claim->bars[index + 1U].index = (uint8_t)(index + 1U);
        claim->bars[index + 1U].kind = PCI_BAR_UNIMPLEMENTED;
        claim->bars[index + 1U].attribute_bits = 0U;
        claim->bars[index + 1U].pair_index = (uint8_t)index;
        claim->bars[index + 1U].base = 0U;
        claim->bars[index + 1U].size = 0U;
        claim->bars[index + 1U].prefetchable = false;
        claim->bars[index + 1U].implemented = false;
        ++index;
    }

    restore_status = restore_probe_state(function->address, original,
        bar_count, command);
    if (restore_status != PCI_RESOURCE_STATUS_OK) {
        return restore_status;
    }
    if (status != PCI_RESOURCE_STATUS_OK) {
        return status;
    }

    claim->bar_count = bar_count;
    claim->original_command = command;
    claim->current_command = command & (uint16_t)~PCI_COMMAND_BUS_MASTER;
    if (claim->current_command != command &&
        write_command(function->address, claim->current_command) !=
            PCI_RESOURCE_STATUS_OK) {
        (void)write_command(function->address, command);
        return PCI_RESOURCE_STATUS_CONFIG_ACCESS;
    }
    return PCI_RESOURCE_STATUS_OK;
}

static enum pci_resource_status validate_claim_bars(
    const struct pci_device_claim *claim
)
{
    for (size_t left = 0U; left < claim->bar_count; ++left) {
        const struct pci_bar_description *first = &claim->bars[left];

        if (!first->implemented) {
            continue;
        }

        for (size_t right = left + 1U; right < claim->bar_count; ++right) {
            const struct pci_bar_description *second = &claim->bars[right];

            if (!second->implemented || (first->kind == PCI_BAR_IO) !=
                    (second->kind == PCI_BAR_IO)) {
                continue;
            }

            if (ranges_overlap(first->base, first->size,
                    second->base, second->size)) {
                return PCI_RESOURCE_STATUS_OVERLAPPING_BAR;
            }
        }
    }

    for (size_t record_index = 0U;
         record_index < PCI_ACTIVE_CLAIM_CAPACITY;
         ++record_index) {
        const struct claim_record *record = &claim_records[record_index];

        if (!record->active) {
            continue;
        }
        for (size_t current_index = 0U;
             current_index < claim->bar_count;
             ++current_index) {
            const struct pci_bar_description *current =
                &claim->bars[current_index];

            if (!current->implemented) {
                continue;
            }
            for (size_t owned_index = 0U;
                 owned_index < record->bar_count;
                 ++owned_index) {
                const struct pci_bar_description *owned =
                    &record->bars[owned_index];

                if (!owned->implemented ||
                    ((current->kind == PCI_BAR_IO) !=
                        (owned->kind == PCI_BAR_IO))) {
                    continue;
                }
                if (ranges_overlap(current->base, current->size,
                        owned->base, owned->size)) {
                    return PCI_RESOURCE_STATUS_OVERLAPPING_BAR;
                }
            }
        }
    }

    return PCI_RESOURCE_STATUS_OK;
}

static struct claim_record *find_claim_record(
    const struct pci_device_claim *claim
)
{
    for (size_t index = 0U; index < PCI_ACTIVE_CLAIM_CAPACITY; ++index) {
        if (claim_records[index].active &&
            claim_records[index].identifier == claim->identifier &&
            addresses_equal(claim_records[index].device, claim->device)) {
            return &claim_records[index];
        }
    }
    return NULL;
}

static enum pci_resource_status validate_active_claim(
    const struct pci_device_claim *claim
)
{
    if (claim == NULL) {
        return PCI_RESOURCE_STATUS_NULL_ARGUMENT;
    }
    if (!claim->active || find_claim_record(claim) == NULL) {
        return PCI_RESOURCE_STATUS_CLAIM_INCONSISTENT;
    }
    return PCI_RESOURCE_STATUS_OK;
}

enum pci_resource_status pci_resource_initialize(void)
{
    if (state.active) {
        return PCI_RESOURCE_STATUS_ALREADY_INITIALIZED;
    }
    if (!pci_is_initialized() || !paging_is_active()) {
        return PCI_RESOURCE_STATUS_NOT_INITIALIZED;
    }
    if (cpu_interrupts_enabled()) {
        return PCI_RESOURCE_STATUS_INTERRUPTS_ENABLED;
    }

    for (size_t index = 0U; index < PCI_ACTIVE_CLAIM_CAPACITY; ++index) {
        claim_records[index].active = false;
    }
    for (size_t index = 0U; index < MMIO_ARENA_BITMAP_BYTES; ++index) {
        arena_bitmap[index] = 0U;
    }
    state.active_claims = 0U;
    state.active_mappings = 0U;
    state.arena_pages = MMIO_ARENA_PAGE_COUNT;
    state.mapped_pages = 0U;
    state.bus_masters = 0U;
    state.active = true;
    next_claim_identifier = 1U;
    return PCI_RESOURCE_STATUS_OK;
}

enum pci_resource_status pci_claim_device(
    const struct pci_function *function,
    struct pci_device_claim *claim
)
{
    struct claim_record *record = NULL;
    enum pci_resource_status status;

    if (function == NULL || claim == NULL) {
        return PCI_RESOURCE_STATUS_NULL_ARGUMENT;
    }
    if (!state.active) {
        return PCI_RESOURCE_STATUS_NOT_INITIALIZED;
    }
    if (cpu_interrupts_enabled()) {
        return PCI_RESOURCE_STATUS_INTERRUPTS_ENABLED;
    }

    bool enumerated = false;
    for (size_t index = 0U; index < pci_function_count(); ++index) {
        const struct pci_function *known = pci_function_at(index);

        if (known != NULL && addresses_equal(known->address,
                function->address) && known->vendor_id == function->vendor_id &&
            known->device_id == function->device_id &&
            known->header_type == function->header_type) {
            enumerated = true;
            break;
        }
    }
    if (!enumerated) {
        return PCI_RESOURCE_STATUS_FUNCTION_NOT_ENUMERATED;
    }

    for (size_t index = 0U; index < PCI_ACTIVE_CLAIM_CAPACITY; ++index) {
        if (claim_records[index].active &&
            addresses_equal(claim_records[index].device, function->address)) {
            return PCI_RESOURCE_STATUS_ALREADY_CLAIMED;
        }
        if (!claim_records[index].active && record == NULL) {
            record = &claim_records[index];
        }
    }
    if (record == NULL) {
        return PCI_RESOURCE_STATUS_CLAIM_TABLE_FULL;
    }

    for (size_t byte = 0U; byte < sizeof(*claim); ++byte) {
        ((uint8_t *)claim)[byte] = 0U;
    }
    claim->device = function->address;
    status = probe_bars(function, claim, false);
    if (status == PCI_RESOURCE_STATUS_OK) {
        status = validate_claim_bars(claim);
    }
    if (status != PCI_RESOURCE_STATUS_OK) {
        if (claim->original_command != 0U || claim->current_command != 0U) {
            (void)write_command(function->address, claim->original_command);
        }
        return status;
    }

    record->device = function->address;
    record->identifier = next_claim_identifier++;
    record->bar_count = claim->bar_count;
    for (size_t index = 0U; index < claim->bar_count; ++index) {
        record->bars[index] = claim->bars[index];
    }
    if (next_claim_identifier == 0U) {
        next_claim_identifier = 1U;
    }
    record->active = true;
    claim->identifier = record->identifier;
    claim->active = true;
    claim->memory_decode_enabled =
        (claim->current_command & PCI_COMMAND_MEMORY_SPACE) != 0U;
    claim->bus_master_enabled = false;
    ++state.active_claims;
    return PCI_RESOURCE_STATUS_OK;
}

const struct pci_bar_description *pci_claim_bar(
    const struct pci_device_claim *claim,
    uint8_t bar_index
)
{
    if (validate_active_claim(claim) != PCI_RESOURCE_STATUS_OK ||
        bar_index >= claim->bar_count) {
        return NULL;
    }
    return &claim->bars[bar_index];
}

struct pci_mmio_region *pci_claim_mapped_bar(
    struct pci_device_claim *claim,
    uint8_t bar_index
)
{
    if (validate_active_claim(claim) != PCI_RESOURCE_STATUS_OK) {
        return NULL;
    }
    for (size_t index = 0U; index < claim->mapping_count; ++index) {
        if (claim->mappings[index].active &&
            claim->mappings[index].bar_index == bar_index) {
            return &claim->mappings[index];
        }
    }
    return NULL;
}

static enum pci_resource_status allocate_arena_pages(
    size_t page_count,
    size_t *first_page
)
{
    if (page_count == 0U || page_count > MMIO_ARENA_PAGE_COUNT) {
        return PCI_RESOURCE_STATUS_MMIO_ARENA_EXHAUSTED;
    }
    for (size_t first = 0U;
         first <= MMIO_ARENA_PAGE_COUNT - page_count;
         ++first) {
        bool free = true;
        for (size_t page = 0U; page < page_count; ++page) {
            if (arena_bit(first + page)) {
                free = false;
                break;
            }
        }
        if (!free) {
            continue;
        }
        for (size_t page = 0U; page < page_count; ++page) {
            arena_set(first + page, true);
        }
        *first_page = first;
        return PCI_RESOURCE_STATUS_OK;
    }
    return PCI_RESOURCE_STATUS_MMIO_ARENA_EXHAUSTED;
}

enum pci_resource_status pci_claim_map_bar(
    struct pci_device_claim *claim,
    uint8_t bar_index,
    struct pci_mmio_region **region
)
{
    const struct pci_bar_description *bar;
    struct pci_mmio_region *mapping;
    uint64_t page_offset;
    uint64_t rounded_length;
    size_t page_count;
    size_t first_page;
    enum pci_resource_status status;
    struct paging_translation translation;

    if (region == NULL) {
        return PCI_RESOURCE_STATUS_NULL_ARGUMENT;
    }
    *region = NULL;
    status = validate_active_claim(claim);
    if (status != PCI_RESOURCE_STATUS_OK) {
        return status;
    }
    if (cpu_interrupts_enabled()) {
        return PCI_RESOURCE_STATUS_INTERRUPTS_ENABLED;
    }
    if (bar_index >= claim->bar_count) {
        return PCI_RESOURCE_STATUS_BAD_BAR_INDEX;
    }
    if (pci_claim_mapped_bar(claim, bar_index) != NULL) {
        return PCI_RESOURCE_STATUS_BAR_ALREADY_MAPPED;
    }
    if (claim->mapping_count >= PCI_CLAIM_MAPPING_CAPACITY) {
        return PCI_RESOURCE_STATUS_MAPPING_CAPACITY;
    }

    bar = &claim->bars[bar_index];
    if (!bar->implemented ||
        (bar->kind != PCI_BAR_MEMORY_32 &&
            bar->kind != PCI_BAR_MEMORY_64)) {
        return PCI_RESOURCE_STATUS_IO_BAR_NOT_MAPPABLE;
    }

    page_offset = bar->base & (PAGING_PAGE_SIZE - 1U);
    if (bar->size > UINT64_MAX - page_offset ||
        bar->size + page_offset >
            UINT64_MAX - (PAGING_PAGE_SIZE - 1U)) {
        return PCI_RESOURCE_STATUS_MMIO_RANGE_OVERFLOW;
    }
    rounded_length = (bar->size + page_offset + PAGING_PAGE_SIZE - 1U) &
        ~(PAGING_PAGE_SIZE - 1U);
    if (rounded_length == 0U ||
        rounded_length > PCI_DEVICE_MMIO_ARENA_SIZE) {
        return PCI_RESOURCE_STATUS_MMIO_ARENA_EXHAUSTED;
    }
    if (frame_range_overlaps_allocatable_memory(bar->base - page_offset,
            rounded_length)) {
        return PCI_RESOURCE_STATUS_MMIO_RAM_OVERLAP;
    }

    for (size_t index = 0U; index < claim->mapping_count; ++index) {
        const struct pci_mmio_region *other = &claim->mappings[index];
        if (other->active && ranges_overlap(bar->base, bar->size,
                other->physical_base, other->size)) {
            return PCI_RESOURCE_STATUS_MMIO_RANGE_OVERLAP;
        }
    }

    page_count = (size_t)(rounded_length / PAGING_PAGE_SIZE);
    status = allocate_arena_pages(page_count, &first_page);
    if (status != PCI_RESOURCE_STATUS_OK) {
        return status;
    }

    mapping = &claim->mappings[claim->mapping_count];
    mapping->device = claim->device;
    mapping->bar_index = bar_index;
    mapping->physical_base = bar->base;
    mapping->size = bar->size;
    mapping->mapping_physical_base = bar->base - page_offset;
    mapping->mapping_virtual_base = PCI_DEVICE_MMIO_ARENA_BASE +
        (uint64_t)first_page * PAGING_PAGE_SIZE;
    mapping->mapping_length = rounded_length;
    mapping->virtual_base = mapping->mapping_virtual_base + page_offset;
    mapping->active = false;

    if (paging_map(mapping->mapping_virtual_base,
            mapping->mapping_physical_base, mapping->mapping_length,
            PAGING_WRITE | PAGING_UNCACHED) != PAGING_STATUS_OK) {
        for (size_t page = 0U; page < page_count; ++page) {
            arena_set(first_page + page, false);
        }
        return PCI_RESOURCE_STATUS_PAGING_FAILURE;
    }
    if (paging_translate(mapping->virtual_base, &translation) !=
            PAGING_STATUS_OK ||
        translation.physical_address != bar->base ||
        translation.memory_type != PAGING_MEMORY_UNCACHEABLE ||
        (translation.permissions & PAGING_WRITE) == 0U ||
        (translation.permissions & PAGING_EXECUTE) != 0U) {
        (void)paging_unmap(mapping->mapping_virtual_base,
            mapping->mapping_length);
        for (size_t page = 0U; page < page_count; ++page) {
            arena_set(first_page + page, false);
        }
        return PCI_RESOURCE_STATUS_PAGING_FAILURE;
    }

    mapping->active = true;
    ++claim->mapping_count;
    ++state.active_mappings;
    state.mapped_pages += page_count;

    if ((claim->current_command & PCI_COMMAND_MEMORY_SPACE) == 0U) {
        const uint16_t enabled = claim->current_command |
            PCI_COMMAND_MEMORY_SPACE;
        if (write_command(claim->device, enabled) != PCI_RESOURCE_STATUS_OK) {
            (void)paging_unmap(mapping->mapping_virtual_base,
                mapping->mapping_length);
            for (size_t page = 0U; page < page_count; ++page) {
                arena_set(first_page + page, false);
            }
            mapping->active = false;
            --claim->mapping_count;
            --state.active_mappings;
            state.mapped_pages -= page_count;
            return PCI_RESOURCE_STATUS_CONFIG_ACCESS;
        }
        claim->current_command = enabled;
        claim->memory_decode_enabled = true;
    }

    *region = mapping;
    return PCI_RESOURCE_STATUS_OK;
}

enum pci_resource_status pci_mmio_subregion(
    const struct pci_mmio_region *region,
    uint64_t offset,
    uint64_t length,
    volatile void **pointer
)
{
    if (region == NULL || pointer == NULL) {
        return PCI_RESOURCE_STATUS_NULL_ARGUMENT;
    }
    *pointer = NULL;
    if (!region->active || length == 0U || offset > region->size ||
        length > region->size - offset ||
        region->virtual_base > UINT64_MAX - offset) {
        return PCI_RESOURCE_STATUS_BAD_MMIO_SUBREGION;
    }
    *pointer = (volatile void *)(uintptr_t)(region->virtual_base + offset);
    return PCI_RESOURCE_STATUS_OK;
}

enum pci_resource_status pci_claim_unmap_last_bar(
    struct pci_device_claim *claim,
    uint8_t bar_index
)
{
    struct pci_mmio_region *mapping;
    size_t first_page;
    size_t page_count;
    enum pci_resource_status status = validate_active_claim(claim);

    if (status != PCI_RESOURCE_STATUS_OK) {
        return status;
    }
    if (cpu_interrupts_enabled()) {
        return PCI_RESOURCE_STATUS_INTERRUPTS_ENABLED;
    }
    if (claim->mapping_count == 0U) {
        return PCI_RESOURCE_STATUS_BAD_BAR_INDEX;
    }
    mapping = &claim->mappings[claim->mapping_count - 1U];
    if (!mapping->active || mapping->bar_index != bar_index) {
        return PCI_RESOURCE_STATUS_MAPPING_ORDER;
    }
    if (claim->bus_master_enabled) {
        return PCI_RESOURCE_STATUS_BUS_MASTER_ALREADY_ENABLED;
    }

    first_page = (size_t)(
        (mapping->mapping_virtual_base - PCI_DEVICE_MMIO_ARENA_BASE) /
        PAGING_PAGE_SIZE);
    page_count = (size_t)(mapping->mapping_length / PAGING_PAGE_SIZE);
    if (paging_unmap(mapping->mapping_virtual_base, mapping->mapping_length) !=
            PAGING_STATUS_OK) {
        return PCI_RESOURCE_STATUS_PAGING_FAILURE;
    }
    for (size_t page = 0U; page < page_count; ++page) {
        arena_set(first_page + page, false);
    }
    mapping->active = false;
    --claim->mapping_count;
    --state.active_mappings;
    state.mapped_pages -= page_count;
    return PCI_RESOURCE_STATUS_OK;
}

enum pci_resource_status pci_claim_enable_bus_master(
    struct pci_device_claim *claim,
    const struct pci_bus_master_request *request
)
{
    enum pci_resource_status status = validate_active_claim(claim);

    if (status != PCI_RESOURCE_STATUS_OK) {
        return status;
    }
    if (request == NULL || request->allocation_count == 0U ||
        request->allocation_count > PCI_BUS_MASTER_DMA_CAPACITY) {
        return PCI_RESOURCE_STATUS_NULL_ARGUMENT;
    }
    if (cpu_interrupts_enabled()) {
        return PCI_RESOURCE_STATUS_INTERRUPTS_ENABLED;
    }
    if (claim->bus_master_enabled) {
        return PCI_RESOURCE_STATUS_BUS_MASTER_ALREADY_ENABLED;
    }
    if (!claim->memory_decode_enabled || claim->mapping_count == 0U) {
        return PCI_RESOURCE_STATUS_DMA_NOT_PREPARED;
    }
    for (size_t index = 0U; index < request->allocation_count; ++index) {
        if (!dma_is_device_owned(request->allocations[index])) {
            return PCI_RESOURCE_STATUS_DMA_NOT_PREPARED;
        }
    }

    const uint16_t command = claim->current_command |
        PCI_COMMAND_BUS_MASTER;
    if (write_command(claim->device, command) != PCI_RESOURCE_STATUS_OK) {
        return PCI_RESOURCE_STATUS_CONFIG_ACCESS;
    }
    claim->current_command = command;
    claim->bus_master_enabled = true;
    ++state.bus_masters;
    return PCI_RESOURCE_STATUS_OK;
}

enum pci_resource_status pci_claim_disable_bus_master(
    struct pci_device_claim *claim
)
{
    enum pci_resource_status status = validate_active_claim(claim);

    if (status != PCI_RESOURCE_STATUS_OK) {
        return status;
    }
    if (cpu_interrupts_enabled()) {
        return PCI_RESOURCE_STATUS_INTERRUPTS_ENABLED;
    }
    if (!claim->bus_master_enabled) {
        return PCI_RESOURCE_STATUS_BUS_MASTER_DISABLED;
    }

    const uint16_t command = claim->current_command &
        (uint16_t)~PCI_COMMAND_BUS_MASTER;
    if (write_command(claim->device, command) != PCI_RESOURCE_STATUS_OK) {
        return PCI_RESOURCE_STATUS_CONFIG_ACCESS;
    }
    claim->current_command = command;
    claim->bus_master_enabled = false;
    --state.bus_masters;
    return PCI_RESOURCE_STATUS_OK;
}

enum pci_resource_status pci_release_device(struct pci_device_claim *claim)
{
    struct claim_record *record;
    enum pci_resource_status status = validate_active_claim(claim);

    if (status != PCI_RESOURCE_STATUS_OK) {
        return status;
    }
    if (cpu_interrupts_enabled()) {
        return PCI_RESOURCE_STATUS_INTERRUPTS_ENABLED;
    }
    record = find_claim_record(claim);

    if (claim->bus_master_enabled &&
        pci_claim_disable_bus_master(claim) != PCI_RESOURCE_STATUS_OK) {
        return PCI_RESOURCE_STATUS_CONFIG_ACCESS;
    }

    while (claim->mapping_count != 0U) {
        struct pci_mmio_region *mapping =
            &claim->mappings[claim->mapping_count - 1U];
        const size_t first_page = (size_t)(
            (mapping->mapping_virtual_base - PCI_DEVICE_MMIO_ARENA_BASE) /
            PAGING_PAGE_SIZE);
        const size_t page_count =
            (size_t)(mapping->mapping_length / PAGING_PAGE_SIZE);

        if (paging_unmap(mapping->mapping_virtual_base,
                mapping->mapping_length) != PAGING_STATUS_OK) {
            return PCI_RESOURCE_STATUS_PAGING_FAILURE;
        }
        for (size_t page = 0U; page < page_count; ++page) {
            arena_set(first_page + page, false);
        }
        mapping->active = false;
        --claim->mapping_count;
        --state.active_mappings;
        state.mapped_pages -= page_count;
    }

    if (write_command(claim->device, claim->original_command) !=
            PCI_RESOURCE_STATUS_OK) {
        return PCI_RESOURCE_STATUS_RESTORE_FAILURE;
    }
    claim->current_command = claim->original_command;
    claim->memory_decode_enabled =
        (claim->original_command & PCI_COMMAND_MEMORY_SPACE) != 0U;
    claim->active = false;
    record->active = false;
    --state.active_claims;
    return PCI_RESOURCE_STATUS_OK;
}

struct pci_resource_state pci_resource_get_state(void)
{
    return state;
}

enum pci_resource_status pci_resource_verify(void)
{
    size_t claims = 0U;
    size_t pages = 0U;

    if (!state.active) {
        return PCI_RESOURCE_STATUS_NOT_INITIALIZED;
    }
    for (size_t index = 0U; index < PCI_ACTIVE_CLAIM_CAPACITY; ++index) {
        if (claim_records[index].active) {
            ++claims;
        }
    }
    for (size_t page = 0U; page < MMIO_ARENA_PAGE_COUNT; ++page) {
        if (arena_bit(page)) {
            ++pages;
        }
    }
    if (claims != state.active_claims || pages != state.mapped_pages ||
        state.active_mappings > state.active_claims *
            PCI_CLAIM_MAPPING_CAPACITY ||
        state.bus_masters > state.active_claims) {
        return PCI_RESOURCE_STATUS_CLAIM_INCONSISTENT;
    }
    return PCI_RESOURCE_STATUS_OK;
}

bool pci_resource_self_test(const struct pci_function *probe_function)
{
    struct pci_device_claim synthetic = {0};
    uintptr_t ram_page = 0U;
    uint16_t command_before = 0U;
    uint16_t command_after = 0U;
    uint32_t bar_before[PCI_BAR_COUNT] = {0U};
    uint32_t bar_after = 0U;
    size_t count;

    if (!state.active || probe_function == NULL ||
        (probe_function->header_type != PCI_HEADER_TYPE_ENDPOINT &&
            probe_function->header_type != PCI_HEADER_TYPE_BRIDGE)) {
        return false;
    }
    if (require_decode_disabled(PCI_COMMAND_MEMORY_SPACE) !=
            PCI_RESOURCE_STATUS_DECODE_ENABLED) {
        return false;
    }

    count = probe_function->header_type == PCI_HEADER_TYPE_ENDPOINT
        ? PCI_BAR_COUNT
        : 2U;
    if (read_command(probe_function->address, &command_before) !=
            PCI_RESOURCE_STATUS_OK) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        if (config_read(probe_function->address,
                (uint16_t)(PCI_REGISTER_BAR_BASE + index * PCI_BAR_STRIDE),
                &bar_before[index]) != PCI_RESOURCE_STATUS_OK) {
            return false;
        }
    }

    if (probe_bars(probe_function, &synthetic, true) !=
            PCI_RESOURCE_STATUS_INJECTED_FAILURE ||
        read_command(probe_function->address, &command_after) !=
            PCI_RESOURCE_STATUS_OK || command_after != command_before) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        if (config_read(probe_function->address,
                (uint16_t)(PCI_REGISTER_BAR_BASE + index * PCI_BAR_STRIDE),
                &bar_after) != PCI_RESOURCE_STATUS_OK ||
            bar_after != bar_before[index]) {
            return false;
        }
    }

    if (validate_64_bit_pair(1U, 0U) !=
            PCI_RESOURCE_STATUS_MALFORMED_64_BIT_PAIR) {
        return false;
    }

    synthetic.bars[0].implemented = true;
    synthetic.bars[0].base = UINT64_MAX - 1U;
    synthetic.bars[0].size = UINT64_C(4);
    if (!ranges_overlap(UINT64_C(0x1000), UINT64_C(0x1000),
            UINT64_C(0x1800), UINT64_C(0x1000)) ||
        validate_bar(&synthetic.bars[0]) !=
            PCI_RESOURCE_STATUS_BAR_RANGE_OVERFLOW) {
        return false;
    }

    if (frame_allocate(&ram_page) != FRAME_STATUS_OK) {
        return false;
    }
    const bool ram_overlap = frame_range_overlaps_allocatable_memory(
        (uint64_t)ram_page, PHIPIA_PAGE_SIZE);
    if (frame_release(ram_page) != FRAME_STATUS_OK || !ram_overlap) {
        return false;
    }

    return pci_resource_verify() == PCI_RESOURCE_STATUS_OK;
}

const char *pci_resource_status_string(enum pci_resource_status status)
{
    static const char *const messages[PCI_RESOURCE_STATUS_COUNT] = {
        "ok", "null PCI resource argument",
        "PCI resource foundation was initialized twice",
        "PCI resource foundation is not initialized",
        "PCI resource mutation requires IF cleared",
        "PCI function was not produced by enumeration",
        "PCI header exposes no claimable BAR layout",
        "PCI device is already claimed", "PCI claim table is full",
        "PCI configuration access failed",
        "BAR probe requires I/O and memory decode disabled",
        "BAR probe could not restore command and BAR values",
        "BAR probe injected a failure after modification",
        "64-bit BAR has no valid upper pair",
        "memory BAR encodes a reserved type", "BAR size is zero",
        "BAR size is not a power of two",
        "platform left an implemented BAR unassigned",
        "BAR base is not aligned to its size", "BAR range overflows",
        "PCI BAR resources overlap", "PCI BAR index is out of range",
        "I/O or unimplemented BAR cannot be mapped as MMIO",
        "PCI BAR already has an MMIO mapping",
        "MMIO BAR mappings must be released in reverse order",
        "PCI claim mapping table is full", "MMIO page rounding overflows",
        "MMIO mapping overlaps an owned resource",
        "MMIO mapping aliases allocatable physical memory",
        "device MMIO arena is exhausted", "device MMIO paging failed",
        "MMIO subregion falls outside its mapped BAR",
        "bus mastering requires mapped initialized device-owned DMA",
        "PCI bus mastering is already enabled",
        "PCI bus mastering is already disabled",
        "PCI claim or mapping accounting is inconsistent"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        PCI_RESOURCE_STATUS_COUNT,
        "PCI resource status messages are out of sync");
    if (status < PCI_RESOURCE_STATUS_OK ||
        status >= PCI_RESOURCE_STATUS_COUNT) {
        return "unknown PCI resource status";
    }
    return messages[status];
}
