/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_HEAP_H
#define PHIPIA_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The heap's virtual window. It sits above the identity map, above the paging
 * probe page at 8 GiB, and above the 4 GiB address the page-fault scenario
 * needs to stay absent, so nothing here can collide with an existing mapping.
 */
#define HEAP_BASE UINT64_C(0x0000000400000000)
#define HEAP_SIZE (UINT64_C(16) * 1024U * 1024U)

/*
 * One unmapped page on each side. Neither is ever mapped, so a walk off either
 * end of the heap is a page fault naming the guard address rather than a silent
 * write into whatever happened to be next.
 */
#define HEAP_GUARD_BELOW (HEAP_BASE - UINT64_C(4096))
#define HEAP_GUARD_ABOVE (HEAP_BASE + HEAP_SIZE)

/*
 * The System V AMD64 ABI requires max_align_t alignment from a general
 * allocator, which is sixteen bytes on this target. It is also the allocation
 * granularity: a request is satisfied by a block rounded up to it.
 */
#define HEAP_ALIGNMENT UINT64_C(16)

/*
 * A Phipia policy bound on how many blocks the heap may be divided into, not an
 * architectural one. The block table is fixed storage because the thing that
 * would replace it is a heap, and this is it.
 */
#define HEAP_MAX_BLOCKS 256U

enum heap_status {
    HEAP_STATUS_OK = 0,
    HEAP_STATUS_NULL_ARGUMENT,
    HEAP_STATUS_ALREADY_INITIALIZED,
    HEAP_STATUS_NOT_INITIALIZED,
    HEAP_STATUS_INTERRUPTS_ENABLED,
    HEAP_STATUS_PAGING_INACTIVE,
    HEAP_STATUS_GUARD_MAPPED,
    HEAP_STATUS_ZERO_SIZE,
    HEAP_STATUS_TOO_LARGE,
    HEAP_STATUS_OUT_OF_MEMORY,
    HEAP_STATUS_OUT_OF_BLOCKS,
    HEAP_STATUS_MAPPING_FAILURE,
    HEAP_STATUS_BAD_POINTER,
    HEAP_STATUS_DOUBLE_FREE,
    HEAP_STATUS_VALIDATION_FAILURE
};

/*
 * One span of the committed region. The blocks tile [0, committed) in ascending
 * offset order with no gap and no overlap, which is the invariant every
 * operation restores and heap_verify checks.
 */
struct heap_block {
    uint64_t offset;
    uint64_t size;
    bool used;
};

struct heap_state {
    uint64_t base_address;
    uint64_t size;
    uint64_t committed_bytes;
    uint64_t allocated_bytes;
    size_t block_count;
    size_t live_allocations;
    size_t mapped_pages;
    bool active;
};

enum heap_status heap_initialize(void);
enum heap_status heap_allocate(uint64_t size, void **pointer);
enum heap_status heap_free(void *pointer);
enum heap_status heap_verify(void);
struct heap_state heap_get_state(void);
bool heap_is_active(void);
bool heap_self_test(void);
const char *heap_status_string(enum heap_status status);

#endif
