/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_MEMORY_H
#define PHIPIA_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/boot.h>

#define PHIPIA_PAGE_SIZE UINT64_C(4096)
#define PHIPIA_LOW_MEMORY_RESERVATION UINT64_C(0x100000)
#define FRAME_CONTIGUOUS_ALLOCATION_CAPACITY 32U

enum frame_status {
    FRAME_STATUS_OK = 0,
    FRAME_STATUS_NULL_ARGUMENT,
    FRAME_STATUS_NOT_INITIALIZED,
    FRAME_STATUS_BAD_MEMORY_MAP,
    FRAME_STATUS_RANGE_OVERFLOW,
    FRAME_STATUS_RANGE_OUTSIDE_LIMIT,
    FRAME_STATUS_OUT_OF_MEMORY,
    FRAME_STATUS_UNALIGNED_ADDRESS,
    FRAME_STATUS_FRAME_NOT_ALLOCATABLE,
    FRAME_STATUS_FRAME_IN_USE,
    FRAME_STATUS_DOUBLE_FREE,
    FRAME_STATUS_ZERO_PAGE_COUNT,
    FRAME_STATUS_BAD_ALIGNMENT,
    FRAME_STATUS_ALIGNMENT_UNSATISFIABLE,
    FRAME_STATUS_ADDRESS_BOUND_UNSATISFIED,
    FRAME_STATUS_TOO_MANY_CONTIGUOUS_ALLOCATIONS,
    FRAME_STATUS_BAD_CONTIGUOUS_ALLOCATION,
    FRAME_STATUS_COUNT
};

struct frame_contiguous_request {
    size_t page_count;
    uint64_t alignment;
    uint64_t maximum_physical_address;
};

struct frame_contiguous_allocation {
    uintptr_t physical_base;
    size_t page_count;
    uint64_t alignment;
    uint64_t maximum_physical_address;
    uint64_t identifier;
    bool active;
};

struct frame_allocator_stats {
    size_t addressable_frames;
    size_t allocatable_frames;
    size_t free_frames;
    size_t allocated_frames;
    size_t reserved_frames;
    uint64_t highest_allocatable_address;
};

enum frame_status frame_allocator_initialize(
    const struct boot_information *information
);
enum frame_status frame_allocate(uintptr_t *physical_address);
enum frame_status frame_release(uintptr_t physical_address);
enum frame_status frame_allocate_contiguous(
    const struct frame_contiguous_request *request,
    struct frame_contiguous_allocation *allocation
);
enum frame_status frame_release_contiguous(
    struct frame_contiguous_allocation *allocation
);
enum frame_status frame_reserve_range(uint64_t base_address, uint64_t length);
bool frame_range_overlaps_allocatable_memory(
    uint64_t base_address,
    uint64_t length
);
struct frame_allocator_stats frame_allocator_get_stats(void);
const char *frame_status_string(enum frame_status status);

#endif
