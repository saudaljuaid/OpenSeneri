/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cpu.h>
#include <phipia/heap.h>
#include <phipia/memory.h>
#include <phipia/paging.h>

_Static_assert(HEAP_SIZE % PAGING_PAGE_SIZE == 0U,
               "the heap window is not a whole number of pages");
_Static_assert(PAGING_PAGE_SIZE % HEAP_ALIGNMENT == 0U,
               "the allocation granularity does not divide a page");
_Static_assert(HEAP_BASE % PAGING_PAGE_SIZE == 0U,
               "the heap window does not start on a page");
_Static_assert(HEAP_BASE > PAGING_PROBE_ADDRESS,
               "the heap window would collide with the paging probe page");

/*
 * Two blocks are the fewest a split can produce from one, so a table that can
 * hold two can always represent a heap with one allocation in it. Everything
 * above that is capacity, and running out of it is a reported status.
 */
_Static_assert(HEAP_MAX_BLOCKS >= 2U,
               "the block table cannot represent a single split");

static struct heap_block blocks[HEAP_MAX_BLOCKS];
static struct heap_state state;
static size_t block_count;

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

/*
 * First fit. A kernel allocator at this stage is asked for a handful of blocks
 * during boot, so the scan cost is irrelevant and the predictability is worth
 * more: the same sequence of calls always produces the same layout, which is
 * what lets the self-test pin exact offsets.
 */
static bool find_fit(
    const struct heap_block *table,
    size_t count,
    uint64_t size,
    size_t *index
)
{
    for (size_t position = 0; position < count; ++position) {
        if (!table[position].used && table[position].size >= size) {
            *index = position;
            return true;
        }
    }

    return false;
}

static bool block_index_for_offset(
    const struct heap_block *table,
    size_t count,
    uint64_t offset,
    size_t *index
)
{
    for (size_t position = 0; position < count; ++position) {
        if (table[position].offset == offset) {
            *index = position;
            return true;
        }
    }

    return false;
}

static void remove_block(struct heap_block *table, size_t *count, size_t index)
{
    for (size_t position = index; position + 1U < *count; ++position) {
        table[position] = table[position + 1U];
    }

    --*count;
}

/*
 * Take the front of a free block for an allocation. The remainder becomes a
 * free block of its own unless it is too small to hold anything, in which case
 * it stays with the allocation: the caller gets more than it asked for, which
 * is safe, rather than the heap gaining a block no request can ever fit.
 */
static bool split_block(
    struct heap_block *table,
    size_t *count,
    size_t index,
    uint64_t size
)
{
    const uint64_t remainder = table[index].size - size;

    if (remainder >= HEAP_ALIGNMENT) {
        if (*count == HEAP_MAX_BLOCKS) {
            return false;
        }

        for (size_t position = *count; position > index + 1U; --position) {
            table[position] = table[position - 1U];
        }

        table[index + 1U].offset = table[index].offset + size;
        table[index + 1U].size = remainder;
        table[index + 1U].used = false;
        table[index].size = size;
        ++*count;
    }

    table[index].used = true;
    return true;
}

/*
 * Absorb any free neighbour on either side. Merging forwards first keeps the
 * index of the block being freed valid for the backward merge; doing it the
 * other way round would leave it pointing at the wrong entry.
 */
static void merge_free_neighbours(
    struct heap_block *table,
    size_t *count,
    size_t index
)
{
    if (index + 1U < *count && !table[index + 1U].used) {
        table[index].size += table[index + 1U].size;
        remove_block(table, count, index + 1U);
    }

    if (index > 0U && !table[index - 1U].used) {
        table[index - 1U].size += table[index].size;
        remove_block(table, count, index);
    }
}

/*
 * The invariant, checked from the table alone: the blocks tile the committed
 * region exactly, in ascending order, aligned, with no gap, no overlap, and no
 * two free blocks side by side. The last of those is what proves coalescing
 * actually ran - a heap that never merged would satisfy everything else while
 * fragmenting itself to death.
 */
static bool validate_table(
    const struct heap_block *table,
    size_t count,
    uint64_t committed
)
{
    uint64_t expected_offset = 0U;

    if (count > HEAP_MAX_BLOCKS) {
        return false;
    }

    if (count == 0U) {
        return committed == 0U;
    }

    /*
     * Offsets are not checked for alignment. The tiling starts at zero and each
     * offset is the running sum of the sizes before it, so once every size is
     * aligned every offset is too - a separate check could never fail, and no
     * test could reach it.
     */
    for (size_t position = 0; position < count; ++position) {
        if (table[position].offset != expected_offset ||
            table[position].size == 0U ||
            table[position].size % HEAP_ALIGNMENT != 0U) {
            return false;
        }

        if (position > 0U && !table[position - 1U].used &&
            !table[position].used) {
            return false;
        }

        expected_offset += table[position].size;
    }

    return expected_offset == committed;
}

/*
 * Give back every page in a half-open range of the window. A frame is released
 * only after its mapping has actually gone: if the unmap refuses, the frame
 * stays owned by this heap rather than being handed back while something can
 * still reach it through a stale translation.
 */
static void release_pages(uint64_t from, uint64_t to)
{
    for (uint64_t offset = from; offset < to; offset += PAGING_PAGE_SIZE) {
        const uint64_t address = HEAP_BASE + offset;
        struct paging_translation translation;

        if (paging_translate(address, &translation) != PAGING_STATUS_OK) {
            continue;
        }

        if (paging_unmap(address, PAGING_PAGE_SIZE) != PAGING_STATUS_OK) {
            continue;
        }

        (void)frame_release((uintptr_t)translation.physical_address);
        --state.mapped_pages;
    }
}

/*
 * Back a half-open range of the window with freshly allocated frames. The heap
 * is not physically contiguous and does not need to be, so each page is mapped
 * on its own. A failure part way through unwinds everything this call mapped,
 * so growth either happens completely or leaves no trace.
 */
static enum heap_status commit_pages(uint64_t from, uint64_t to)
{
    for (uint64_t offset = from; offset < to; offset += PAGING_PAGE_SIZE) {
        uintptr_t frame = 0U;

        if (frame_allocate(&frame) != FRAME_STATUS_OK) {
            release_pages(from, offset);
            return HEAP_STATUS_OUT_OF_MEMORY;
        }

        if (paging_map(
                HEAP_BASE + offset,
                frame,
                PAGING_PAGE_SIZE,
                PAGING_WRITE
            ) != PAGING_STATUS_OK) {
            (void)frame_release(frame);
            release_pages(from, offset);
            return HEAP_STATUS_MAPPING_FAILURE;
        }

        ++state.mapped_pages;
    }

    return HEAP_STATUS_OK;
}

/*
 * Extend the committed region until a block of `size` exists, and report which
 * block that is. Every refusal happens before a single page is mapped, so a
 * heap that cannot grow is left exactly as it was.
 */
static enum heap_status grow_for(uint64_t size, size_t *index)
{
    const bool extend_last = block_count > 0U && !blocks[block_count - 1U].used;
    const uint64_t trailing = extend_last ? blocks[block_count - 1U].size : 0U;
    uint64_t growth;
    enum heap_status status;

    /* find_fit already refused the trailing block, so it is short of `size`. */
    growth = align_up(size - trailing, PAGING_PAGE_SIZE);

    if (growth > HEAP_SIZE - state.committed_bytes) {
        return HEAP_STATUS_OUT_OF_MEMORY;
    }

    if (!extend_last && block_count == HEAP_MAX_BLOCKS) {
        return HEAP_STATUS_OUT_OF_BLOCKS;
    }

    status = commit_pages(
        state.committed_bytes,
        state.committed_bytes + growth
    );

    if (status != HEAP_STATUS_OK) {
        return status;
    }

    if (extend_last) {
        blocks[block_count - 1U].size += growth;
    } else {
        blocks[block_count].offset = state.committed_bytes;
        blocks[block_count].size = growth;
        blocks[block_count].used = false;
        ++block_count;
    }

    state.committed_bytes += growth;
    *index = block_count - 1U;
    return HEAP_STATUS_OK;
}

static void refresh_state(void)
{
    state.block_count = block_count;
    state.allocated_bytes = 0U;
    state.live_allocations = 0U;

    for (size_t position = 0; position < block_count; ++position) {
        if (blocks[position].used) {
            state.allocated_bytes += blocks[position].size;
            ++state.live_allocations;
        }
    }
}

enum heap_status heap_initialize(void)
{
    struct paging_translation translation;

    if (state.active) {
        return HEAP_STATUS_ALREADY_INITIALIZED;
    }

    /*
     * The heap is a consumer of the page tables, not a peer of them. Without
     * them there is no window to hand out and no way to back one.
     */
    if (!paging_is_active()) {
        return HEAP_STATUS_PAGING_INACTIVE;
    }

    /*
     * Growth maps pages one at a time and unwinds on failure. An interrupt
     * arriving mid-transaction could only be handled by code that does not
     * allocate, so the transaction is simply not interruptible.
     */
    if (cpu_interrupts_enabled()) {
        return HEAP_STATUS_INTERRUPTS_ENABLED;
    }

    if (paging_translate(HEAP_GUARD_BELOW, &translation) !=
            PAGING_STATUS_NOT_MAPPED ||
        paging_translate(HEAP_GUARD_ABOVE, &translation) !=
            PAGING_STATUS_NOT_MAPPED) {
        return HEAP_STATUS_GUARD_MAPPED;
    }

    block_count = 0U;
    state.base_address = HEAP_BASE;
    state.size = HEAP_SIZE;
    state.committed_bytes = 0U;
    state.mapped_pages = 0U;
    state.active = true;
    refresh_state();
    return HEAP_STATUS_OK;
}

enum heap_status heap_allocate(uint64_t size, void **pointer)
{
    uint64_t requested;
    size_t index = 0U;
    enum heap_status status;

    if (pointer == NULL) {
        return HEAP_STATUS_NULL_ARGUMENT;
    }

    *pointer = NULL;

    if (!state.active) {
        return HEAP_STATUS_NOT_INITIALIZED;
    }

    if (size == 0U) {
        return HEAP_STATUS_ZERO_SIZE;
    }

    /* Checked before rounding, so the rounding itself cannot overflow. */
    if (size > HEAP_SIZE) {
        return HEAP_STATUS_TOO_LARGE;
    }

    requested = align_up(size, HEAP_ALIGNMENT);

    if (requested > HEAP_SIZE) {
        return HEAP_STATUS_TOO_LARGE;
    }

    if (!find_fit(blocks, block_count, requested, &index)) {
        status = grow_for(requested, &index);

        if (status != HEAP_STATUS_OK) {
            return status;
        }
    }

    if (!split_block(blocks, &block_count, index, requested)) {
        return HEAP_STATUS_OUT_OF_BLOCKS;
    }

    refresh_state();
    *pointer = (void *)(uintptr_t)(HEAP_BASE + blocks[index].offset);
    return HEAP_STATUS_OK;
}

enum heap_status heap_free(void *pointer)
{
    uint64_t address;
    size_t index = 0U;

    if (!state.active) {
        return HEAP_STATUS_NOT_INITIALIZED;
    }

    if (pointer == NULL) {
        return HEAP_STATUS_NULL_ARGUMENT;
    }

    address = (uint64_t)(uintptr_t)pointer;

    /*
     * Only the exact address a previous allocation returned is a free. An
     * interior pointer, an address outside the window, and one inside the
     * window but past what has been committed are all refused by name rather
     * than being followed into the block table.
     */
    if (address < HEAP_BASE ||
        address >= HEAP_BASE + state.committed_bytes) {
        return HEAP_STATUS_BAD_POINTER;
    }

    if (!block_index_for_offset(
            blocks,
            block_count,
            address - HEAP_BASE,
            &index
        )) {
        return HEAP_STATUS_BAD_POINTER;
    }

    if (!blocks[index].used) {
        return HEAP_STATUS_DOUBLE_FREE;
    }

    blocks[index].used = false;
    merge_free_neighbours(blocks, &block_count, index);
    refresh_state();
    return HEAP_STATUS_OK;
}

/*
 * Re-derive everything about the heap from the table and the page tables, and
 * compare it against what the heap believes. This is the heap's counterpart to
 * paging_verify: the block table is a description of memory, and a description
 * that has drifted from the memory it describes is exactly how an allocator
 * starts handing out the same bytes twice.
 */
enum heap_status heap_verify(void)
{
    struct paging_translation translation;
    uint64_t allocated = 0U;
    size_t live = 0U;

    if (!state.active) {
        return HEAP_STATUS_NOT_INITIALIZED;
    }

    if (state.committed_bytes > HEAP_SIZE ||
        state.committed_bytes % PAGING_PAGE_SIZE != 0U ||
        state.mapped_pages != state.committed_bytes / PAGING_PAGE_SIZE) {
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    if (!validate_table(blocks, block_count, state.committed_bytes)) {
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    /* Both guards must still be absent, or an overrun would go unnoticed. */
    if (paging_translate(HEAP_GUARD_BELOW, &translation) !=
            PAGING_STATUS_NOT_MAPPED ||
        paging_translate(HEAP_GUARD_ABOVE, &translation) !=
            PAGING_STATUS_NOT_MAPPED) {
        return HEAP_STATUS_GUARD_MAPPED;
    }

    /*
     * Every committed page must be a 4 KiB writable, non-executable mapping.
     * An executable heap page would be a W^X violation that the paging audit
     * would also catch; checking here names the subsystem that caused it.
     */
    for (uint64_t offset = 0U; offset < state.committed_bytes;
         offset += PAGING_PAGE_SIZE) {
        if (paging_translate(HEAP_BASE + offset, &translation) !=
                PAGING_STATUS_OK ||
            translation.permissions != PAGING_WRITE ||
            translation.level != 1U) {
            return HEAP_STATUS_VALIDATION_FAILURE;
        }
    }

    for (size_t position = 0; position < block_count; ++position) {
        if (blocks[position].used) {
            allocated += blocks[position].size;
            ++live;
        }
    }

    if (allocated != state.allocated_bytes ||
        live != state.live_allocations ||
        block_count != state.block_count) {
        return HEAP_STATUS_VALIDATION_FAILURE;
    }

    return HEAP_STATUS_OK;
}

struct heap_state heap_get_state(void)
{
    return state;
}

bool heap_is_active(void)
{
    return state.active;
}

static bool test_table_operations(void)
{
    struct heap_block table[HEAP_MAX_BLOCKS];
    size_t count = 1U;
    size_t index = 0U;

    /* One free block of a page, split into 64 used and the rest free. */
    table[0].offset = 0U;
    table[0].size = PAGING_PAGE_SIZE;
    table[0].used = false;

    if (!find_fit(table, count, 64U, &index) || index != 0U) {
        return false;
    }

    if (!split_block(table, &count, 0U, 64U) || count != 2U ||
        !table[0].used || table[0].size != 64U || table[0].offset != 0U ||
        table[1].used || table[1].offset != 64U ||
        table[1].size != PAGING_PAGE_SIZE - 64U) {
        return false;
    }

    if (!validate_table(table, count, PAGING_PAGE_SIZE)) {
        return false;
    }

    /* A used block is never a fit, however large it is. */
    if (find_fit(table, count, 32U, &index) && index == 0U) {
        return false;
    }

    /* A second allocation takes the front of the remaining free block. */
    if (!find_fit(table, count, 32U, &index) || index != 1U ||
        !split_block(table, &count, 1U, 32U) || count != 3U ||
        table[1].offset != 64U || table[1].size != 32U || !table[1].used ||
        table[2].offset != 96U || table[2].used) {
        return false;
    }

    /*
     * Freeing the middle block must not merge anything: its left neighbour is
     * used, and merging into a used block is how a heap hands out live memory.
     */
    table[1].used = false;
    merge_free_neighbours(table, &count, 1U);

    if (count != 2U || table[1].offset != 64U ||
        table[1].size != PAGING_PAGE_SIZE - 64U || table[1].used) {
        return false;
    }

    /* Freeing the first block now merges forward into the whole window. */
    table[0].used = false;
    merge_free_neighbours(table, &count, 0U);

    if (count != 1U || table[0].offset != 0U ||
        table[0].size != PAGING_PAGE_SIZE || table[0].used) {
        return false;
    }

    return validate_table(table, count, PAGING_PAGE_SIZE);
}

static bool test_table_validation(void)
{
    struct heap_block table[3] = {
        {0U, 0U, false},
        {0U, 0U, false},
        {0U, 0U, false}
    };

    /* An empty table describes an uncommitted heap and nothing else. */
    if (!validate_table(table, 0U, 0U) ||
        validate_table(table, 0U, PAGING_PAGE_SIZE)) {
        return false;
    }

    table[0].offset = 0U;
    table[0].size = 64U;
    table[0].used = true;
    table[1].offset = 64U;
    table[1].size = 64U;
    table[1].used = false;

    if (!validate_table(table, 2U, 128U)) {
        return false;
    }

    /* A total that disagrees with the committed region. */
    if (validate_table(table, 2U, 129U) || validate_table(table, 2U, 64U)) {
        return false;
    }

    /* A gap between two blocks, and an overlap. */
    table[1].offset = 80U;

    if (validate_table(table, 2U, 144U)) {
        return false;
    }

    table[1].offset = 48U;

    if (validate_table(table, 2U, 112U)) {
        return false;
    }

    table[1].offset = 64U;

    /* A size that is not a whole number of allocation units, and an empty
       block: neither can be produced by a split, and both would break the
       alignment every returned pointer is promised. */
    table[1].size = 65U;

    if (validate_table(table, 2U, 129U)) {
        return false;
    }

    table[1].size = 0U;

    if (validate_table(table, 2U, 64U)) {
        return false;
    }

    table[1].size = 64U;

    /* Two free blocks side by side means a free did not coalesce. */
    table[0].used = false;

    if (validate_table(table, 2U, 128U)) {
        return false;
    }

    table[0].used = true;
    return validate_table(table, 2U, 128U);
}

static bool test_block_lookup_and_capacity(void)
{
    struct heap_block table[HEAP_MAX_BLOCKS];
    size_t count = 2U;
    size_t index = 0U;

    table[0].offset = 0U;
    table[0].size = 64U;
    table[0].used = true;
    table[1].offset = 64U;
    table[1].size = 64U;
    table[1].used = false;

    if (!block_index_for_offset(table, count, 64U, &index) || index != 1U) {
        return false;
    }

    /* An interior offset names no block, which is what makes a free exact. */
    if (block_index_for_offset(table, count, 32U, &index) ||
        block_index_for_offset(table, count, 128U, &index) ||
        block_index_for_offset(table, 0U, 0U, &index)) {
        return false;
    }

    /* A split that would need a slot the full table cannot supply. */
    count = HEAP_MAX_BLOCKS;

    for (size_t position = 0; position < count; ++position) {
        table[position].offset = (uint64_t)position * HEAP_ALIGNMENT;
        table[position].size = HEAP_ALIGNMENT;
        table[position].used = true;
    }

    table[count - 1U].size = HEAP_ALIGNMENT * 4U;
    table[count - 1U].used = false;

    if (split_block(table, &count, count - 1U, HEAP_ALIGNMENT)) {
        return false;
    }

    /*
     * The same block split so the remainder is below the granularity: no new
     * slot is needed, so a full table still succeeds and the caller keeps the
     * few spare bytes rather than the heap gaining an unusable block.
     */
    if (!split_block(table, &count, count - 1U, HEAP_ALIGNMENT * 4U) ||
        count != HEAP_MAX_BLOCKS ||
        table[count - 1U].size != HEAP_ALIGNMENT * 4U ||
        !table[count - 1U].used) {
        return false;
    }

    return true;
}

/*
 * Everything above the hardware: fitting, splitting, coalescing, the tiling
 * invariant, and every refusal the public interface can produce before it has a
 * window to hand out. The table operations are driven against a private table
 * so a failing case cannot disturb the real heap.
 */
bool heap_self_test(void)
{
    void *pointer = (void *)(uintptr_t)UINT64_C(1);
    struct heap_state snapshot;

    if (!test_table_operations() || !test_table_validation() ||
        !test_block_lookup_and_capacity()) {
        return false;
    }

    if (align_up(0U, HEAP_ALIGNMENT) != 0U ||
        align_up(1U, HEAP_ALIGNMENT) != HEAP_ALIGNMENT ||
        align_up(HEAP_ALIGNMENT, HEAP_ALIGNMENT) != HEAP_ALIGNMENT ||
        align_up(HEAP_ALIGNMENT + 1U, HEAP_ALIGNMENT) != HEAP_ALIGNMENT * 2U) {
        return false;
    }

    /* Nothing works before initialization, and the output is cleared anyway. */
    if (heap_is_active()) {
        return false;
    }

    if (heap_allocate(16U, &pointer) != HEAP_STATUS_NOT_INITIALIZED ||
        pointer != NULL) {
        return false;
    }

    if (heap_free((void *)(uintptr_t)HEAP_BASE) !=
            HEAP_STATUS_NOT_INITIALIZED ||
        heap_verify() != HEAP_STATUS_NOT_INITIALIZED ||
        heap_allocate(16U, NULL) != HEAP_STATUS_NULL_ARGUMENT) {
        return false;
    }

    snapshot = heap_get_state();

    return !snapshot.active && snapshot.committed_bytes == 0U &&
        snapshot.block_count == 0U && snapshot.live_allocations == 0U;
}

const char *heap_status_string(enum heap_status status)
{
    switch (status) {
    case HEAP_STATUS_OK:
        return "ok";
    case HEAP_STATUS_NULL_ARGUMENT:
        return "null heap argument";
    case HEAP_STATUS_ALREADY_INITIALIZED:
        return "kernel heap was initialized twice";
    case HEAP_STATUS_NOT_INITIALIZED:
        return "kernel heap is not initialized";
    case HEAP_STATUS_INTERRUPTS_ENABLED:
        return "heap initialization requires interrupts disabled";
    case HEAP_STATUS_PAGING_INACTIVE:
        return "kernel heap requires the kernel page tables";
    case HEAP_STATUS_GUARD_MAPPED:
        return "a kernel heap guard page is mapped";
    case HEAP_STATUS_ZERO_SIZE:
        return "heap allocation size is zero";
    case HEAP_STATUS_TOO_LARGE:
        return "heap allocation is larger than the heap window";
    case HEAP_STATUS_OUT_OF_MEMORY:
        return "kernel heap cannot grow any further";
    case HEAP_STATUS_OUT_OF_BLOCKS:
        return "kernel heap block table is full";
    case HEAP_STATUS_MAPPING_FAILURE:
        return "kernel heap could not map a page";
    case HEAP_STATUS_BAD_POINTER:
        return "pointer does not name a heap allocation";
    case HEAP_STATUS_DOUBLE_FREE:
        return "heap allocation was released twice";
    case HEAP_STATUS_VALIDATION_FAILURE:
        return "kernel heap state does not match its memory";
    default:
        return "unknown heap status";
    }
}
