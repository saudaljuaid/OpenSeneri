/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <phipia/audio.h>
#include <phipia/clock.h>
#include <phipia/cpu.h>
#include <phipia/dma.h>
#include <phipia/interrupt_vector.h>
#include <phipia/memory.h>
#include <phipia/native_handle.h>
#include <phipia/paging.h>
#include <phipia/pci_resource.h>
#include <phipia/msix.h>

static enum pci_resource_status host_bus_master_status =
    PCI_RESOURCE_STATUS_INJECTED_FAILURE;

uint64_t clock_monotonic_ns(void)
{
    return 0U;
}

bool cpu_interrupts_enabled(void)
{
    return false;
}

void cpu_interrupt_disable(void) {}
void cpu_interrupt_enable(void) {}
void cpu_store_fence(void) {}

bool pci_is_initialized(void)
{
    return false;
}

size_t pci_function_count(void)
{
    return 0U;
}

const struct pci_function *pci_function_at(size_t index)
{
    (void)index;
    return NULL;
}

struct frame_allocator_stats frame_allocator_get_stats(void)
{
    return (struct frame_allocator_stats){0};
}

struct paging_state paging_get_state(void)
{
    return (struct paging_state){0};
}

struct interrupt_vector_state interrupt_vector_get_state(void)
{
    return (struct interrupt_vector_state){0};
}

struct msix_state msix_get_state(void)
{
    return (struct msix_state){0};
}

struct pci_resource_state pci_resource_get_state(void)
{
    return (struct pci_resource_state){0};
}

enum pci_resource_status pci_claim_device(
    const struct pci_function *function,
    struct pci_device_claim *claim
)
{
    (void)function;
    (void)claim;
    return PCI_RESOURCE_STATUS_INJECTED_FAILURE;
}

enum pci_resource_status pci_claim_map_bar(
    struct pci_device_claim *claim,
    uint8_t bar_index,
    struct pci_mmio_region **region
)
{
    (void)claim;
    (void)bar_index;
    (void)region;
    return PCI_RESOURCE_STATUS_INJECTED_FAILURE;
}

enum pci_resource_status pci_mmio_subregion(
    const struct pci_mmio_region *region,
    uint64_t offset,
    uint64_t length,
    volatile void **pointer
)
{
    (void)region;
    (void)offset;
    (void)length;
    (void)pointer;
    return PCI_RESOURCE_STATUS_INJECTED_FAILURE;
}

enum dma_status dma_allocate(
    const struct dma_request *request,
    struct dma_allocation *allocation
)
{
    (void)request;
    (void)allocation;
    return DMA_STATUS_FRAME_ALLOCATION_FAILURE;
}

enum dma_status dma_mark_initialized(struct dma_allocation *allocation)
{
    (void)allocation;
    return DMA_STATUS_OK;
}

enum dma_status dma_transfer_to_device(struct dma_allocation *allocation)
{
    (void)allocation;
    return DMA_STATUS_OK;
}

bool dma_is_device_owned(const struct dma_allocation *allocation)
{
    (void)allocation;
    return false;
}

enum pci_resource_status pci_claim_enable_bus_master(
    struct pci_device_claim *claim,
    const struct pci_bus_master_request *request
)
{
    (void)claim;
    (void)request;
    return PCI_RESOURCE_STATUS_INJECTED_FAILURE;
}

enum pci_resource_status pci_claim_disable_bus_master(
    struct pci_device_claim *claim
)
{
    (void)claim;
    return host_bus_master_status;
}

enum pci_resource_status pci_claim_unmap_last_bar(
    struct pci_device_claim *claim,
    uint8_t bar_index
)
{
    (void)claim;
    (void)bar_index;
    return PCI_RESOURCE_STATUS_OK;
}

enum pci_resource_status pci_release_device(struct pci_device_claim *claim)
{
    (void)claim;
    return PCI_RESOURCE_STATUS_OK;
}

enum pci_resource_status pci_resource_verify(void)
{
    return PCI_RESOURCE_STATUS_OK;
}

enum dma_status dma_transfer_to_cpu(struct dma_allocation *allocation)
{
    (void)allocation;
    return DMA_STATUS_OK;
}

enum dma_status dma_release(struct dma_allocation *allocation)
{
    (void)allocation;
    return DMA_STATUS_OK;
}

enum dma_status dma_verify(void)
{
    return DMA_STATUS_OK;
}

#include "../src/kernel/audio.c"

struct audio_close_state {
    uint64_t generation;
    uint64_t token;
    unsigned calls;
};

static enum native_resource_close_result close_audio(
    uint8_t type,
    const struct native_resource *resource,
    void *context
)
{
    struct audio_close_state *state;
    bool consumed = false;

    assert(type == PHIPIA_HANDLE_AUDIO_OUTPUT && resource != NULL &&
        context != NULL);
    state = context;
    assert(resource->words[0] == state->token);
    const enum audio_native_status status = audio_native_close_report(
        state->generation, resource->words[0], &consumed);

    ++state->calls;
    return status == AUDIO_NATIVE_OK ? NATIVE_RESOURCE_CLOSED :
        consumed ? NATIVE_RESOURCE_CLOSED_WITH_ERROR :
            NATIVE_RESOURCE_RETAINED;
}

int main(void)
{
    const uint64_t generation = UINT64_C(41);
    const uint64_t token = UINT64_C(73);
    const struct native_resource resource = {{token, 0U, 0U, 0U}};
    struct audio_close_state state = {generation, token, 0U};
    struct native_handle_table table;
    phipia_handle_t first;
    phipia_handle_t duplicate;
    bool consumed = true;

    zero_bytes(&audio_native, sizeof(audio_native));
    audio_native.initialized = true;
    audio_native.owner_generation = generation;
    audio_native.streams[0].open = true;
    audio_native.streams[0].owner_generation = generation;
    audio_native.streams[0].token = token;
    audio_native.controller.bus_master = true;

    assert(native_handle_table_initialize(&table, 2U) == NATIVE_HANDLE_OK);
    assert(native_handle_install(&table, PHIPIA_HANDLE_AUDIO_OUTPUT,
        &resource, &first) == NATIVE_HANDLE_OK);
    assert(native_handle_duplicate(&table, first, &duplicate) == NATIVE_HANDLE_OK);
    assert(native_handle_close(&table, first, close_audio, &state) ==
        NATIVE_HANDLE_OK);
    assert(state.calls == 0U && table.active_handles == 1U &&
        table.active_objects == 1U);
    assert(native_handle_close(&table, duplicate, close_audio, &state) ==
        NATIVE_HANDLE_CLOSE_FAILED);
    assert(state.calls == 1U && table.active_handles == 0U &&
        table.active_objects == 0U);
    assert(audio_native_close_report(generation, token, &consumed) ==
        AUDIO_NATIVE_STALE && !consumed);
    assert(native_handle_close_all(&table, close_audio, &state) ==
        NATIVE_HANDLE_OK && state.calls == 1U);
    puts("native audio handle close: consumed teardown errors retire duplicate wrappers PASS");
    return 0;
}
