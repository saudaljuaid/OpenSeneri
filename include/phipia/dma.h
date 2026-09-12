/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_DMA_H
#define PHIPIA_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/memory.h>

enum dma_owner {
    DMA_OWNER_NONE = 0,
    DMA_OWNER_CPU,
    DMA_OWNER_DEVICE,
    DMA_OWNER_COUNT
};

enum dma_status {
    DMA_STATUS_OK = 0,
    DMA_STATUS_NULL_ARGUMENT,
    DMA_STATUS_ALREADY_INITIALIZED,
    DMA_STATUS_NOT_INITIALIZED,
    DMA_STATUS_FRAME_ALLOCATION_FAILURE,
    DMA_STATUS_NOT_IDENTITY_MAPPED,
    DMA_STATUS_NOT_PREPARED,
    DMA_STATUS_WRONG_OWNER,
    DMA_STATUS_DOUBLE_FREE,
    DMA_STATUS_BAD_ALLOCATION,
    DMA_STATUS_COUNT
};

struct dma_request {
    size_t page_count;
    uint64_t alignment;
    uint64_t maximum_physical_address;
};

struct dma_allocation {
    struct frame_contiguous_allocation frames;
    void *cpu_address;
    uint64_t byte_length;
    enum dma_owner owner;
    bool initialized;
    bool active;
};

struct dma_state {
    size_t active_allocations;
    size_t cpu_owned_allocations;
    size_t device_owned_allocations;
    bool active;
};

enum dma_status dma_initialize(void);
enum dma_status dma_allocate(
    const struct dma_request *request,
    struct dma_allocation *allocation
);
enum dma_status dma_mark_initialized(struct dma_allocation *allocation);
enum dma_status dma_transfer_to_device(struct dma_allocation *allocation);
enum dma_status dma_transfer_to_cpu(struct dma_allocation *allocation);
enum dma_status dma_release(struct dma_allocation *allocation);
bool dma_is_device_owned(const struct dma_allocation *allocation);
struct dma_state dma_get_state(void);
enum dma_status dma_verify(void);
bool dma_self_test(void);
const char *dma_status_string(enum dma_status status);

#endif
