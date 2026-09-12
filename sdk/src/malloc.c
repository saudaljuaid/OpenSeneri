/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdlib.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"

#define ARENA_BYTES (64U * 1024U)
#define BLOCK_MAGIC UINT64_C(0x5341504F54454D45)

struct allocation_block {
    size_t size;
    struct allocation_block *next;
    uint64_t magic;
    int free;
};

static struct allocation_block *blocks;
static volatile uint32_t allocator_lock;

static size_t align_size(size_t value)
{
    return (value + 15U) & ~(size_t)15U;
}

static struct allocation_block *new_arena(size_t minimum)
{
    struct phipia_memory_map_response response;
    struct allocation_block *block;
    size_t length = minimum + sizeof(*block);

    if (length < ARENA_BYTES) {
        length = ARENA_BYTES;
    }
    if (phipia_memory_allocate(length, PHIPIA_MEMORY_READ |
            PHIPIA_MEMORY_WRITE | PHIPIA_MEMORY_GUARD_BEFORE |
            PHIPIA_MEMORY_GUARD_AFTER, &response) < 0) {
        return NULL;
    }
    block = (struct allocation_block *)(uintptr_t)response.address;
    block->size = (size_t)response.length - sizeof(*block);
    block->next = blocks;
    block->magic = BLOCK_MAGIC;
    block->free = 1;
    blocks = block;
    return block;
}

static void split_block(struct allocation_block *block, size_t size)
{
    if (block->size >= size + sizeof(*block) + 32U) {
        struct allocation_block *tail = (struct allocation_block *)(void *)
            ((unsigned char *)(void *)(block + 1) + size);

        tail->size = block->size - size - sizeof(*block);
        tail->next = block->next;
        tail->magic = BLOCK_MAGIC;
        tail->free = 1;
        block->next = tail;
        block->size = size;
    }
}

void *malloc(size_t size)
{
    struct allocation_block *block;

    if (size == 0U) {
        size = 1U;
    }
    if (size > SIZE_MAX - 15U) {
        errno = ENOMEM;
        return NULL;
    }
    size = align_size(size);
    phipia_runtime_lock(&allocator_lock);
    for (block = blocks; block != NULL; block = block->next) {
        if (block->free && block->size >= size) {
            break;
        }
    }
    if (block == NULL) {
        block = new_arena(size);
    }
    if (block != NULL) {
        split_block(block, size);
        block->free = 0;
    }
    phipia_runtime_unlock(&allocator_lock);
    if (block == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    return block + 1;
}

void free(void *pointer)
{
    struct allocation_block *block;

    if (pointer == NULL) {
        return;
    }
    block = (struct allocation_block *)pointer - 1;
    phipia_runtime_lock(&allocator_lock);
    if (block->magic != BLOCK_MAGIC || block->free) {
        phipia_runtime_unlock(&allocator_lock);
        abort();
    }
    block->free = 1;
    for (struct allocation_block *cursor = blocks; cursor != NULL;
         cursor = cursor->next) {
        unsigned char *end = (unsigned char *)(void *)(cursor + 1) +
            cursor->size;

        if (cursor->free && cursor->next != NULL && cursor->next->free &&
            end == (unsigned char *)(void *)cursor->next) {
            cursor->size += sizeof(*cursor) + cursor->next->size;
            cursor->next = cursor->next->next;
        }
    }
    phipia_runtime_unlock(&allocator_lock);
}

size_t phipia_allocation_size(const void *pointer)
{
    const struct allocation_block *block =
        (const struct allocation_block *)pointer - 1;

    return pointer != NULL && block->magic == BLOCK_MAGIC && !block->free ?
        block->size : 0U;
}

void *calloc(size_t count, size_t size)
{
    void *result;

    if (size != 0U && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    result = malloc(count * size);
    if (result != NULL) {
        (void)memset(result, 0, count * size);
    }
    return result;
}

void *realloc(void *pointer, size_t size)
{
    void *result;
    size_t old_size;

    if (pointer == NULL) {
        return malloc(size);
    }
    if (size == 0U) {
        free(pointer);
        return NULL;
    }
    old_size = phipia_allocation_size(pointer);
    if (old_size >= size) {
        return pointer;
    }
    result = malloc(size);
    if (result != NULL) {
        (void)memcpy(result, pointer, old_size);
        free(pointer);
    }
    return result;
}
