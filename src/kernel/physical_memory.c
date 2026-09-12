/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/boot.h>
#include <phipia/memory.h>

#define FRAME_COUNT ((size_t)(PHIPIA_EARLY_PHYSICAL_LIMIT / PHIPIA_PAGE_SIZE))
#define BITMAP_BYTE_COUNT ((FRAME_COUNT + 7U) / 8U)

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

static uint8_t eligible_bitmap[BITMAP_BYTE_COUNT];
static uint8_t used_bitmap[BITMAP_BYTE_COUNT];
static struct frame_allocator_stats allocator_stats;
static size_t next_search_index;
static bool allocator_initialized;

struct contiguous_record {
    uintptr_t physical_base;
    size_t page_count;
    uint64_t identifier;
    bool active;
};

static struct contiguous_record contiguous_records[
    FRAME_CONTIGUOUS_ALLOCATION_CAPACITY
];
static uint64_t next_contiguous_identifier;

static bool bitmap_get(const uint8_t *bitmap, size_t frame_index)
{
    const uint8_t mask = (uint8_t)(1U << (frame_index % 8U));

    return (bitmap[frame_index / 8U] & mask) != 0U;
}

static void bitmap_set(uint8_t *bitmap, size_t frame_index, bool value)
{
    const uint8_t mask = (uint8_t)(1U << (frame_index % 8U));
    uint8_t *byte = &bitmap[frame_index / 8U];

    if (value) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t)~mask;
    }
}

static void bitmap_fill(uint8_t *bitmap, uint8_t value)
{
    for (size_t index = 0; index < BITMAP_BYTE_COUNT; ++index) {
        bitmap[index] = value;
    }
}

static bool checked_range_end(uint64_t base, uint64_t length, uint64_t *end)
{
    if (length > UINT64_MAX - base) {
        return false;
    }

    *end = base + length;
    return true;
}

static bool power_of_two(uint64_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static bool frame_in_contiguous_allocation(size_t frame)
{
    for (size_t index = 0U;
         index < FRAME_CONTIGUOUS_ALLOCATION_CAPACITY;
         ++index) {
        const struct contiguous_record *record = &contiguous_records[index];
        const size_t first = (size_t)((uint64_t)record->physical_base /
            PHIPIA_PAGE_SIZE);

        if (record->active && frame >= first &&
            frame - first < record->page_count) {
            return true;
        }
    }

    return false;
}

static enum frame_status available_frame_bounds(
    uint64_t base,
    uint64_t length,
    size_t *first_frame,
    size_t *past_last_frame
)
{
    const uint64_t page_mask = PHIPIA_PAGE_SIZE - 1U;
    uint64_t end;
    uint64_t clipped_end;
    uint64_t aligned_base;

    if (!checked_range_end(base, length, &end)) {
        return FRAME_STATUS_RANGE_OVERFLOW;
    }

    if (length == 0U || base >= PHIPIA_EARLY_PHYSICAL_LIMIT) {
        *first_frame = 0;
        *past_last_frame = 0;
        return FRAME_STATUS_OK;
    }

    clipped_end = end < PHIPIA_EARLY_PHYSICAL_LIMIT
        ? end
        : PHIPIA_EARLY_PHYSICAL_LIMIT;
    aligned_base = (base + page_mask) & ~page_mask;

    if (aligned_base >= clipped_end) {
        *first_frame = 0;
        *past_last_frame = 0;
        return FRAME_STATUS_OK;
    }

    *first_frame = (size_t)(aligned_base / PHIPIA_PAGE_SIZE);
    *past_last_frame = (size_t)((clipped_end & ~page_mask) / PHIPIA_PAGE_SIZE);
    return FRAME_STATUS_OK;
}

static enum frame_status covering_frame_bounds(
    uint64_t base,
    uint64_t length,
    size_t *first_frame,
    size_t *past_last_frame
)
{
    uint64_t end;
    uint64_t clipped_end;

    if (!checked_range_end(base, length, &end)) {
        return FRAME_STATUS_RANGE_OVERFLOW;
    }

    if (length == 0U) {
        *first_frame = 0;
        *past_last_frame = 0;
        return FRAME_STATUS_OK;
    }

    if (base >= PHIPIA_EARLY_PHYSICAL_LIMIT) {
        return FRAME_STATUS_RANGE_OUTSIDE_LIMIT;
    }

    clipped_end = end < PHIPIA_EARLY_PHYSICAL_LIMIT
        ? end
        : PHIPIA_EARLY_PHYSICAL_LIMIT;
    *first_frame = (size_t)(base / PHIPIA_PAGE_SIZE);
    *past_last_frame = (size_t)(clipped_end / PHIPIA_PAGE_SIZE);

    if ((clipped_end & (PHIPIA_PAGE_SIZE - 1U)) != 0U) {
        ++*past_last_frame;
    }

    return FRAME_STATUS_OK;
}

static void mark_available_frames(size_t first_frame, size_t past_last_frame)
{
    for (size_t frame = first_frame; frame < past_last_frame; ++frame) {
        bitmap_set(eligible_bitmap, frame, true);
        bitmap_set(used_bitmap, frame, false);
    }
}

static void mark_reserved_frames(size_t first_frame, size_t past_last_frame)
{
    for (size_t frame = first_frame; frame < past_last_frame; ++frame) {
        bitmap_set(eligible_bitmap, frame, false);
        bitmap_set(used_bitmap, frame, true);
    }
}

static enum frame_status reserve_internal(uint64_t base, uint64_t length)
{
    size_t first_frame;
    size_t past_last_frame;
    enum frame_status status = covering_frame_bounds(
        base,
        length,
        &first_frame,
        &past_last_frame
    );

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    mark_reserved_frames(first_frame, past_last_frame);
    return FRAME_STATUS_OK;
}

static void recompute_stats(void)
{
    struct frame_allocator_stats stats = {
        .addressable_frames = FRAME_COUNT,
        .allocatable_frames = 0,
        .free_frames = 0,
        .allocated_frames = 0,
        .reserved_frames = 0,
        .highest_allocatable_address = 0
    };

    for (size_t frame = 0; frame < FRAME_COUNT; ++frame) {
        if (!bitmap_get(eligible_bitmap, frame)) {
            ++stats.reserved_frames;
            continue;
        }

        ++stats.allocatable_frames;
        stats.highest_allocatable_address =
            ((uint64_t)frame + 1U) * PHIPIA_PAGE_SIZE;

        if (bitmap_get(used_bitmap, frame)) {
            ++stats.allocated_frames;
        } else {
            ++stats.free_frames;
        }
    }

    allocator_stats = stats;
}

static enum frame_status apply_memory_map(
    const struct boot_information *context
)
{
    for (size_t pass = 0; pass < 2U; ++pass) {
        for (size_t index = 0; index < context->memory_map_entry_count; ++index) {
            struct boot_memory_region region;
            size_t first_frame;
            size_t past_last_frame;
            enum frame_status status;
            bool available;

            if (!boot_information_region_at(context, index, &region)) {
                return FRAME_STATUS_BAD_MEMORY_MAP;
            }

            available = region.type == MULTIBOOT2_MEMORY_AVAILABLE;

            if ((pass == 0U) != available) {
                continue;
            }

            if (available) {
                status = available_frame_bounds(
                    region.base_address,
                    region.length,
                    &first_frame,
                    &past_last_frame
                );
            } else {
                status = covering_frame_bounds(
                    region.base_address,
                    region.length,
                    &first_frame,
                    &past_last_frame
                );

                if (status == FRAME_STATUS_RANGE_OUTSIDE_LIMIT) {
                    continue;
                }
            }

            if (status != FRAME_STATUS_OK) {
                return status;
            }

            if (available) {
                mark_available_frames(first_frame, past_last_frame);
            } else {
                mark_reserved_frames(first_frame, past_last_frame);
            }
        }
    }

    return FRAME_STATUS_OK;
}

enum frame_status frame_allocator_initialize(
    const struct boot_information *context
)
{
    enum frame_status status;
    uint64_t kernel_start;
    uint64_t kernel_end;

    if (context == NULL || context->memory_map == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    allocator_initialized = false;
    next_search_index = 0;
    next_contiguous_identifier = 1U;
    bitmap_fill(eligible_bitmap, 0U);
    bitmap_fill(used_bitmap, UINT8_MAX);

    for (size_t index = 0U;
         index < FRAME_CONTIGUOUS_ALLOCATION_CAPACITY;
         ++index) {
        contiguous_records[index].active = false;
        contiguous_records[index].physical_base = 0U;
        contiguous_records[index].page_count = 0U;
        contiguous_records[index].identifier = 0U;
    }

    status = apply_memory_map(context);

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    status = reserve_internal(0U, PHIPIA_LOW_MEMORY_RESERVATION);

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    kernel_start = (uint64_t)(uintptr_t)__kernel_start;
    kernel_end = (uint64_t)(uintptr_t)__kernel_end;

    if (kernel_end < kernel_start) {
        return FRAME_STATUS_RANGE_OVERFLOW;
    }

    status = reserve_internal(kernel_start, kernel_end - kernel_start);

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    status = reserve_internal(
        context->information_start,
        context->information_end - context->information_start
    );

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    recompute_stats();

    if (allocator_stats.free_frames == 0U) {
        return FRAME_STATUS_OUT_OF_MEMORY;
    }

    next_search_index = (size_t)(PHIPIA_LOW_MEMORY_RESERVATION / PHIPIA_PAGE_SIZE);
    allocator_initialized = true;
    return FRAME_STATUS_OK;
}

enum frame_status frame_allocate(uintptr_t *physical_address)
{
    if (physical_address == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (allocator_stats.free_frames == 0U) {
        return FRAME_STATUS_OUT_OF_MEMORY;
    }

    for (size_t step = 0; step < FRAME_COUNT; ++step) {
        size_t frame = next_search_index + step;

        if (frame >= FRAME_COUNT) {
            frame -= FRAME_COUNT;
        }

        if (bitmap_get(eligible_bitmap, frame) && !bitmap_get(used_bitmap, frame)) {
            bitmap_set(used_bitmap, frame, true);
            --allocator_stats.free_frames;
            ++allocator_stats.allocated_frames;
            next_search_index = frame + 1U;

            if (next_search_index == FRAME_COUNT) {
                next_search_index = 0;
            }

            *physical_address = (uintptr_t)((uint64_t)frame * PHIPIA_PAGE_SIZE);
            return FRAME_STATUS_OK;
        }
    }

    return FRAME_STATUS_OUT_OF_MEMORY;
}

enum frame_status frame_release(uintptr_t physical_address)
{
    size_t frame;

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (((uint64_t)physical_address & (PHIPIA_PAGE_SIZE - 1U)) != 0U) {
        return FRAME_STATUS_UNALIGNED_ADDRESS;
    }

    if ((uint64_t)physical_address >= PHIPIA_EARLY_PHYSICAL_LIMIT) {
        return FRAME_STATUS_RANGE_OUTSIDE_LIMIT;
    }

    frame = (size_t)((uint64_t)physical_address / PHIPIA_PAGE_SIZE);

    if (!bitmap_get(eligible_bitmap, frame)) {
        return FRAME_STATUS_FRAME_NOT_ALLOCATABLE;
    }

    if (!bitmap_get(used_bitmap, frame)) {
        return FRAME_STATUS_DOUBLE_FREE;
    }

    if (frame_in_contiguous_allocation(frame)) {
        return FRAME_STATUS_FRAME_IN_USE;
    }

    bitmap_set(used_bitmap, frame, false);
    ++allocator_stats.free_frames;
    --allocator_stats.allocated_frames;

    if (frame < next_search_index) {
        next_search_index = frame;
    }

    return FRAME_STATUS_OK;
}

enum frame_status frame_allocate_contiguous(
    const struct frame_contiguous_request *request,
    struct frame_contiguous_allocation *allocation
)
{
    struct contiguous_record *record = NULL;
    uint64_t length;
    uint64_t bound_end;
    size_t maximum_past_frame;

    if (request == NULL || allocation == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    allocation->physical_base = 0U;
    allocation->page_count = 0U;
    allocation->alignment = 0U;
    allocation->maximum_physical_address = 0U;
    allocation->identifier = 0U;
    allocation->active = false;

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (request->page_count == 0U) {
        return FRAME_STATUS_ZERO_PAGE_COUNT;
    }

    if (!power_of_two(request->alignment) ||
        request->alignment < PHIPIA_PAGE_SIZE ||
        request->alignment % PHIPIA_PAGE_SIZE != 0U) {
        return FRAME_STATUS_BAD_ALIGNMENT;
    }

    if (request->alignment > PHIPIA_EARLY_PHYSICAL_LIMIT ||
        request->alignment > request->maximum_physical_address +
            (request->maximum_physical_address != UINT64_MAX ? 1U : 0U)) {
        return FRAME_STATUS_ALIGNMENT_UNSATISFIABLE;
    }

    if (request->page_count > UINT64_MAX / PHIPIA_PAGE_SIZE) {
        return FRAME_STATUS_RANGE_OVERFLOW;
    }

    length = (uint64_t)request->page_count * PHIPIA_PAGE_SIZE;
    if (length == 0U || request->maximum_physical_address < length - 1U) {
        return FRAME_STATUS_ADDRESS_BOUND_UNSATISFIED;
    }

    bound_end = request->maximum_physical_address == UINT64_MAX
        ? UINT64_MAX
        : request->maximum_physical_address + 1U;
    if (bound_end > PHIPIA_EARLY_PHYSICAL_LIMIT) {
        bound_end = PHIPIA_EARLY_PHYSICAL_LIMIT;
    }
    maximum_past_frame = (size_t)(bound_end / PHIPIA_PAGE_SIZE);
    if (maximum_past_frame < request->page_count) {
        return FRAME_STATUS_ADDRESS_BOUND_UNSATISFIED;
    }

    for (size_t index = 0U;
         index < FRAME_CONTIGUOUS_ALLOCATION_CAPACITY;
         ++index) {
        if (!contiguous_records[index].active) {
            record = &contiguous_records[index];
            break;
        }
    }

    if (record == NULL) {
        return FRAME_STATUS_TOO_MANY_CONTIGUOUS_ALLOCATIONS;
    }

    for (size_t first = 0U;
         first <= maximum_past_frame - request->page_count;
         ++first) {
        const uint64_t base = (uint64_t)first * PHIPIA_PAGE_SIZE;
        bool available = true;

        if ((base & (request->alignment - 1U)) != 0U) {
            continue;
        }

        for (size_t offset = 0U; offset < request->page_count; ++offset) {
            const size_t frame = first + offset;

            if (!bitmap_get(eligible_bitmap, frame) ||
                bitmap_get(used_bitmap, frame)) {
                available = false;
                break;
            }
        }

        if (!available) {
            continue;
        }

        for (size_t offset = 0U; offset < request->page_count; ++offset) {
            bitmap_set(used_bitmap, first + offset, true);
        }

        record->physical_base = (uintptr_t)base;
        record->page_count = request->page_count;
        record->identifier = next_contiguous_identifier++;
        if (next_contiguous_identifier == 0U) {
            next_contiguous_identifier = 1U;
        }
        record->active = true;

        allocator_stats.free_frames -= request->page_count;
        allocator_stats.allocated_frames += request->page_count;
        next_search_index = first + request->page_count;
        if (next_search_index >= FRAME_COUNT) {
            next_search_index = 0U;
        }

        allocation->physical_base = record->physical_base;
        allocation->page_count = record->page_count;
        allocation->alignment = request->alignment;
        allocation->maximum_physical_address =
            request->maximum_physical_address;
        allocation->identifier = record->identifier;
        allocation->active = true;
        return FRAME_STATUS_OK;
    }

    return FRAME_STATUS_OUT_OF_MEMORY;
}

enum frame_status frame_release_contiguous(
    struct frame_contiguous_allocation *allocation
)
{
    struct contiguous_record *record = NULL;
    size_t first;

    if (allocation == NULL) {
        return FRAME_STATUS_NULL_ARGUMENT;
    }

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    if (!allocation->active) {
        return FRAME_STATUS_DOUBLE_FREE;
    }

    for (size_t index = 0U;
         index < FRAME_CONTIGUOUS_ALLOCATION_CAPACITY;
         ++index) {
        if (contiguous_records[index].active &&
            contiguous_records[index].identifier == allocation->identifier) {
            record = &contiguous_records[index];
            break;
        }
    }

    if (record == NULL) {
        return FRAME_STATUS_DOUBLE_FREE;
    }

    if (record->physical_base != allocation->physical_base ||
        record->page_count != allocation->page_count) {
        return FRAME_STATUS_BAD_CONTIGUOUS_ALLOCATION;
    }

    first = (size_t)((uint64_t)record->physical_base / PHIPIA_PAGE_SIZE);
    for (size_t offset = 0U; offset < record->page_count; ++offset) {
        if (!bitmap_get(eligible_bitmap, first + offset) ||
            !bitmap_get(used_bitmap, first + offset)) {
            return FRAME_STATUS_BAD_CONTIGUOUS_ALLOCATION;
        }
    }

    for (size_t offset = 0U; offset < record->page_count; ++offset) {
        bitmap_set(used_bitmap, first + offset, false);
    }

    allocator_stats.free_frames += record->page_count;
    allocator_stats.allocated_frames -= record->page_count;
    if (first < next_search_index) {
        next_search_index = first;
    }

    record->active = false;
    allocation->active = false;
    return FRAME_STATUS_OK;
}

enum frame_status frame_reserve_range(uint64_t base_address, uint64_t length)
{
    size_t first_frame;
    size_t past_last_frame;
    enum frame_status status;

    if (!allocator_initialized) {
        return FRAME_STATUS_NOT_INITIALIZED;
    }

    status = covering_frame_bounds(
        base_address,
        length,
        &first_frame,
        &past_last_frame
    );

    if (status != FRAME_STATUS_OK) {
        return status;
    }

    for (size_t frame = first_frame; frame < past_last_frame; ++frame) {
        if (bitmap_get(eligible_bitmap, frame) && bitmap_get(used_bitmap, frame)) {
            return FRAME_STATUS_FRAME_IN_USE;
        }
    }

    mark_reserved_frames(first_frame, past_last_frame);
    recompute_stats();
    return FRAME_STATUS_OK;
}

bool frame_range_overlaps_allocatable_memory(
    uint64_t base_address,
    uint64_t length
)
{
    uint64_t end;
    size_t first_frame;
    size_t past_last_frame;

    if (!allocator_initialized || length == 0U ||
        !checked_range_end(base_address, length, &end)) {
        return false;
    }
    if (base_address >= PHIPIA_EARLY_PHYSICAL_LIMIT) {
        return false;
    }
    if (end > PHIPIA_EARLY_PHYSICAL_LIMIT) {
        end = PHIPIA_EARLY_PHYSICAL_LIMIT;
    }

    first_frame = (size_t)(base_address / PHIPIA_PAGE_SIZE);
    past_last_frame = (size_t)(end / PHIPIA_PAGE_SIZE);
    if ((end & (PHIPIA_PAGE_SIZE - 1U)) != 0U) {
        ++past_last_frame;
    }
    for (size_t frame = first_frame; frame < past_last_frame; ++frame) {
        if (bitmap_get(eligible_bitmap, frame)) {
            return true;
        }
    }
    return false;
}

struct frame_allocator_stats frame_allocator_get_stats(void)
{
    return allocator_stats;
}

const char *frame_status_string(enum frame_status status)
{
    switch (status) {
    case FRAME_STATUS_OK:
        return "ok";
    case FRAME_STATUS_NULL_ARGUMENT:
        return "null frame allocator argument";
    case FRAME_STATUS_NOT_INITIALIZED:
        return "frame allocator is not initialized";
    case FRAME_STATUS_BAD_MEMORY_MAP:
        return "invalid frame allocator memory map";
    case FRAME_STATUS_RANGE_OVERFLOW:
        return "physical range overflows";
    case FRAME_STATUS_RANGE_OUTSIDE_LIMIT:
        return "physical range is outside the early map";
    case FRAME_STATUS_OUT_OF_MEMORY:
        return "no physical frame is available";
    case FRAME_STATUS_UNALIGNED_ADDRESS:
        return "physical frame address is unaligned";
    case FRAME_STATUS_FRAME_NOT_ALLOCATABLE:
        return "physical frame is permanently reserved";
    case FRAME_STATUS_FRAME_IN_USE:
        return "physical frame is already allocated";
    case FRAME_STATUS_DOUBLE_FREE:
        return "physical frame was released twice";
    case FRAME_STATUS_ZERO_PAGE_COUNT:
        return "contiguous allocation requests zero pages";
    case FRAME_STATUS_BAD_ALIGNMENT:
        return "contiguous allocation alignment is not a page-sized power of two";
    case FRAME_STATUS_ALIGNMENT_UNSATISFIABLE:
        return "contiguous allocation alignment cannot fit below its address bound";
    case FRAME_STATUS_ADDRESS_BOUND_UNSATISFIED:
        return "contiguous allocation cannot fit below its inclusive address bound";
    case FRAME_STATUS_TOO_MANY_CONTIGUOUS_ALLOCATIONS:
        return "contiguous allocation record table is full";
    case FRAME_STATUS_BAD_CONTIGUOUS_ALLOCATION:
        return "contiguous allocation handle does not own its frame range";
    case FRAME_STATUS_COUNT:
        break;
    default:
        break;
    }

    return "unknown frame allocator status";
}
