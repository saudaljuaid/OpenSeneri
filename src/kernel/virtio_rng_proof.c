/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/clock.h>
#include <phipia/cpu.h>
#include <phipia/device_substrate.h>
#include <phipia/dma.h>
#include <phipia/interrupt_vector.h>
#include <phipia/interrupts.h>
#include <phipia/msix.h>
#include <phipia/pci.h>
#include <phipia/pci_resource.h>

#define VIRTIO_VENDOR_ID UINT16_C(0x1AF4)
#define VIRTIO_RNG_MODERN_DEVICE_ID UINT16_C(0x1044)
#define VIRTIO_PCI_CAP_COMMON UINT8_C(1)
#define VIRTIO_PCI_CAP_NOTIFY UINT8_C(2)
#define VIRTIO_PCI_CAP_ISR UINT8_C(3)
#define VIRTIO_PCI_CAP_MIN_LENGTH UINT8_C(16)
#define VIRTIO_PCI_NOTIFY_CAP_LENGTH UINT8_C(20)

#define VIRTIO_STATUS_ACKNOWLEDGE UINT8_C(1)
#define VIRTIO_STATUS_DRIVER UINT8_C(2)
#define VIRTIO_STATUS_DRIVER_OK UINT8_C(4)
#define VIRTIO_STATUS_FEATURES_OK UINT8_C(8)
#define VIRTIO_F_VERSION_1 UINT32_C(1)

#define VIRTIO_COMMON_DEVICE_FEATURE_SELECT UINT64_C(0)
#define VIRTIO_COMMON_DEVICE_FEATURE UINT64_C(4)
#define VIRTIO_COMMON_DRIVER_FEATURE_SELECT UINT64_C(8)
#define VIRTIO_COMMON_DRIVER_FEATURE UINT64_C(12)
#define VIRTIO_COMMON_NUM_QUEUES UINT64_C(18)
#define VIRTIO_COMMON_DEVICE_STATUS UINT64_C(20)
#define VIRTIO_COMMON_QUEUE_SELECT UINT64_C(22)
#define VIRTIO_COMMON_QUEUE_SIZE UINT64_C(24)
#define VIRTIO_COMMON_QUEUE_MSIX_VECTOR UINT64_C(26)
#define VIRTIO_COMMON_QUEUE_ENABLE UINT64_C(28)
#define VIRTIO_COMMON_QUEUE_NOTIFY_OFF UINT64_C(30)
#define VIRTIO_COMMON_QUEUE_DESC UINT64_C(32)
#define VIRTIO_COMMON_QUEUE_DRIVER UINT64_C(40)
#define VIRTIO_COMMON_QUEUE_DEVICE UINT64_C(48)
#define VIRTIO_COMMON_MIN_LENGTH UINT64_C(56)

#define VIRTIO_QUEUE_INDEX UINT16_C(0)
#define VIRTIO_QUEUE_NO_VECTOR UINT16_C(0xFFFF)
#define VIRTIO_PROOF_QUEUE_SIZE UINT16_C(8)
#define VIRTIO_RNG_BYTES DEVICE_SUBSTRATE_DMA_BYTES
#define VIRTQ_DESC_SIZE UINT64_C(16)
#define VIRTQ_DESC_FLAG_WRITE UINT16_C(2)
#define VIRTIO_PROOF_TIMEOUT_NS UINT64_C(2000000000)

struct virtio_capability_region {
    uint8_t type;
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
    uint32_t notify_multiplier;
    struct pci_mmio_region *mapping;
    volatile uint8_t *base;
    bool present;
};

struct proof_interrupt_context {
    struct dma_allocation *queue;
    struct dma_allocation *buffer;
    uint8_t expected_vector;
    volatile uint64_t count;
    volatile bool wrong_vector;
    volatile bool ownership_failure;
};

static struct device_substrate_proof installed_proof;

_Static_assert(VIRTIO_RNG_BYTES <= PHIPIA_PAGE_SIZE,
    "VirtIO RNG proof receive buffer exceeds its DMA page");

static uint32_t config_dword(
    const struct pci_function *function,
    uint16_t offset,
    bool *ok
)
{
    uint32_t value = 0U;

    if (pci_config_read_port(function->address,
            (uint16_t)(offset & ~UINT16_C(3)), &value) != PCI_STATUS_OK) {
        *ok = false;
    }
    return value;
}

static uint8_t config_byte(
    const struct pci_function *function,
    uint16_t offset,
    bool *ok
)
{
    return (uint8_t)(config_dword(function, offset, ok) >>
        ((offset & UINT16_C(3)) * 8U));
}

static uint32_t config_u32(
    const struct pci_function *function,
    uint16_t offset,
    bool *ok
)
{
    if ((offset & UINT16_C(3)) != 0U) {
        *ok = false;
        return 0U;
    }
    return config_dword(function, offset, ok);
}

static enum device_substrate_status collect_capability_regions(
    const struct pci_function *function,
    struct virtio_capability_region *common,
    struct virtio_capability_region *notify,
    struct virtio_capability_region *isr
)
{
    for (size_t index = 0U; index < function->capability_count; ++index) {
        const struct pci_capability *capability =
            &function->capabilities[index];
        struct virtio_capability_region *region = NULL;
        bool ok = true;
        uint8_t length;
        uint8_t type;

        if (capability->identifier != PCI_CAPABILITY_VENDOR) {
            continue;
        }
        length = config_byte(function,
            (uint16_t)(capability->offset + 2U), &ok);
        type = config_byte(function,
            (uint16_t)(capability->offset + 3U), &ok);
        if (!ok || length < VIRTIO_PCI_CAP_MIN_LENGTH ||
            (uint16_t)capability->offset + length > PCI_CONFIG_SPACE_SIZE) {
            return DEVICE_SUBSTRATE_STATUS_CAPABILITY_FAILURE;
        }
        if (type == VIRTIO_PCI_CAP_COMMON) {
            region = common;
        } else if (type == VIRTIO_PCI_CAP_NOTIFY) {
            region = notify;
        } else if (type == VIRTIO_PCI_CAP_ISR) {
            region = isr;
        } else {
            continue;
        }
        if (region->present) {
            return DEVICE_SUBSTRATE_STATUS_CAPABILITY_FAILURE;
        }

        region->type = type;
        region->bar = config_byte(function,
            (uint16_t)(capability->offset + 4U), &ok);
        region->offset = config_u32(function,
            (uint16_t)(capability->offset + 8U), &ok);
        region->length = config_u32(function,
            (uint16_t)(capability->offset + 12U), &ok);
        if (type == VIRTIO_PCI_CAP_NOTIFY) {
            if (length < VIRTIO_PCI_NOTIFY_CAP_LENGTH) {
                return DEVICE_SUBSTRATE_STATUS_CAPABILITY_FAILURE;
            }
            region->notify_multiplier = config_u32(function,
                (uint16_t)(capability->offset + 16U), &ok);
        }
        if (!ok || region->bar >= PCI_BAR_COUNT || region->length == 0U) {
            return DEVICE_SUBSTRATE_STATUS_CAPABILITY_FAILURE;
        }
        region->present = true;
    }

    if (!common->present || !notify->present || !isr->present ||
        common->length < VIRTIO_COMMON_MIN_LENGTH) {
        return DEVICE_SUBSTRATE_STATUS_CAPABILITY_FAILURE;
    }
    return DEVICE_SUBSTRATE_STATUS_OK;
}

static enum device_substrate_status map_capability(
    struct pci_device_claim *claim,
    struct virtio_capability_region *capability
)
{
    volatile void *pointer = NULL;

    capability->mapping = pci_claim_mapped_bar(claim, capability->bar);
    if (capability->mapping == NULL &&
        pci_claim_map_bar(claim, capability->bar, &capability->mapping) !=
            PCI_RESOURCE_STATUS_OK) {
        return DEVICE_SUBSTRATE_STATUS_MAPPING_FAILURE;
    }
    if (pci_mmio_subregion(capability->mapping, capability->offset,
            capability->length, &pointer) != PCI_RESOURCE_STATUS_OK) {
        return DEVICE_SUBSTRATE_STATUS_MAPPING_FAILURE;
    }
    capability->base = (volatile uint8_t *)pointer;
    return DEVICE_SUBSTRATE_STATUS_OK;
}

static uint16_t mmio_read16(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint16_t *)(void *)(base + offset);
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

static void mmio_write16(
    volatile uint8_t *base,
    uint64_t offset,
    uint16_t value
)
{
    *(volatile uint16_t *)(void *)(base + offset) = value;
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

static bool reset_device(volatile uint8_t *common)
{
    mmio_write8(common, VIRTIO_COMMON_DEVICE_STATUS, 0U);
    for (uint64_t spin = 0U; spin < UINT64_C(10000000); ++spin) {
        if (*(volatile uint8_t *)(void *)(common +
                VIRTIO_COMMON_DEVICE_STATUS) == 0U) {
            return true;
        }
    }
    return false;
}

static void zero_bytes(void *pointer, uint64_t length)
{
    uint8_t *bytes = pointer;

    for (uint64_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static void write_u16(uint8_t *base, uint64_t offset, uint16_t value)
{
    *(uint16_t *)(void *)(base + offset) = value;
}

static void write_u32(uint8_t *base, uint64_t offset, uint32_t value)
{
    *(uint32_t *)(void *)(base + offset) = value;
}

static void write_u64(uint8_t *base, uint64_t offset, uint64_t value)
{
    *(uint64_t *)(void *)(base + offset) = value;
}

static uint16_t read_u16(const uint8_t *base, uint64_t offset)
{
    return *(const volatile uint16_t *)(const void *)(base + offset);
}

static uint32_t read_u32(const uint8_t *base, uint64_t offset)
{
    return *(const volatile uint32_t *)(const void *)(base + offset);
}

static void proof_interrupt(
    struct interrupt_frame *frame,
    void *opaque
)
{
    struct proof_interrupt_context *context = opaque;

    if (frame == NULL || context == NULL ||
        frame->vector != context->expected_vector) {
        if (context != NULL) {
            context->wrong_vector = true;
        }
        return;
    }

    ++context->count;
    if (context->queue->owner == DMA_OWNER_DEVICE &&
        dma_transfer_to_cpu(context->queue) != DMA_STATUS_OK) {
        context->ownership_failure = true;
    }
    if (context->buffer->owner == DMA_OWNER_DEVICE &&
        dma_transfer_to_cpu(context->buffer) != DMA_STATUS_OK) {
        context->ownership_failure = true;
    }
}

enum device_substrate_status device_substrate_prove(
    struct device_substrate_proof *proof
)
{
    const struct pci_function *function;
    struct pci_device_claim claim = {0};
    struct virtio_capability_region common = {0};
    struct virtio_capability_region notify = {0};
    struct virtio_capability_region isr = {0};
    struct dma_allocation queue_dma = {0};
    struct dma_allocation buffer_dma = {0};
    struct msix_binding binding = {0};
    struct proof_interrupt_context interrupt_context = {0};
    struct pci_bus_master_request bus_master = {0};
    enum device_substrate_status result = DEVICE_SUBSTRATE_STATUS_OK;
    bool device_reset = false;
    uint8_t *queue;
    uint8_t *buffer;
    uint16_t queue_size;
    uint16_t notify_offset;
    uint64_t available_offset;
    uint64_t used_offset;
    uint64_t deadline;
    bool bus_master_enabled = false;

    if (proof == NULL) {
        return DEVICE_SUBSTRATE_STATUS_CAPABILITY_FAILURE;
    }
    for (size_t byte = 0U; byte < sizeof(*proof); ++byte) {
        ((uint8_t *)proof)[byte] = 0U;
    }
    function = pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_RNG_MODERN_DEVICE_ID);
    if (function == NULL) {
        return DEVICE_SUBSTRATE_STATUS_ABSENT;
    }

    if (pci_claim_device(function, &claim) != PCI_RESOURCE_STATUS_OK) {
        return DEVICE_SUBSTRATE_STATUS_CLAIM_FAILURE;
    }
    result = collect_capability_regions(function, &common, &notify, &isr);
    if (result != DEVICE_SUBSTRATE_STATUS_OK) {
        goto cleanup;
    }
    if (map_capability(&claim, &common) != DEVICE_SUBSTRATE_STATUS_OK ||
        map_capability(&claim, &notify) != DEVICE_SUBSTRATE_STATUS_OK ||
        map_capability(&claim, &isr) != DEVICE_SUBSTRATE_STATUS_OK) {
        result = DEVICE_SUBSTRATE_STATUS_MAPPING_FAILURE;
        goto cleanup;
    }

    if (!reset_device(common.base)) {
        result = DEVICE_SUBSTRATE_STATUS_DEVICE_RESET_FAILURE;
        goto cleanup;
    }
    device_reset = true;
    mmio_write8(common.base, VIRTIO_COMMON_DEVICE_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    mmio_write32(common.base, VIRTIO_COMMON_DEVICE_FEATURE_SELECT, 1U);
    if ((mmio_read32(common.base, VIRTIO_COMMON_DEVICE_FEATURE) &
            VIRTIO_F_VERSION_1) == 0U) {
        result = DEVICE_SUBSTRATE_STATUS_FEATURE_NEGOTIATION_FAILURE;
        goto cleanup;
    }
    mmio_write32(common.base, VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 0U);
    mmio_write32(common.base, VIRTIO_COMMON_DRIVER_FEATURE, 0U);
    mmio_write32(common.base, VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 1U);
    mmio_write32(common.base, VIRTIO_COMMON_DRIVER_FEATURE,
        VIRTIO_F_VERSION_1);
    mmio_write8(common.base, VIRTIO_COMMON_DEVICE_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
            VIRTIO_STATUS_FEATURES_OK);
    if ((*(volatile uint8_t *)(void *)(common.base +
            VIRTIO_COMMON_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0U) {
        result = DEVICE_SUBSTRATE_STATUS_FEATURE_NEGOTIATION_FAILURE;
        goto cleanup;
    }

    if (mmio_read16(common.base, VIRTIO_COMMON_NUM_QUEUES) == 0U) {
        result = DEVICE_SUBSTRATE_STATUS_QUEUE_FAILURE;
        goto cleanup;
    }
    mmio_write16(common.base, VIRTIO_COMMON_QUEUE_SELECT, VIRTIO_QUEUE_INDEX);
    queue_size = mmio_read16(common.base, VIRTIO_COMMON_QUEUE_SIZE);
    if (queue_size == 0U) {
        result = DEVICE_SUBSTRATE_STATUS_QUEUE_FAILURE;
        goto cleanup;
    }
    if (queue_size > VIRTIO_PROOF_QUEUE_SIZE) {
        queue_size = VIRTIO_PROOF_QUEUE_SIZE;
        mmio_write16(common.base, VIRTIO_COMMON_QUEUE_SIZE, queue_size);
    }
    proof->queue_size = queue_size;

    struct dma_request dma_request = {
        .page_count = 1U,
        .alignment = PHIPIA_PAGE_SIZE,
        .maximum_physical_address = UINT32_MAX
    };
    if (dma_allocate(&dma_request, &queue_dma) != DMA_STATUS_OK ||
        dma_allocate(&dma_request, &buffer_dma) != DMA_STATUS_OK) {
        result = DEVICE_SUBSTRATE_STATUS_DMA_FAILURE;
        goto cleanup;
    }
    queue = queue_dma.cpu_address;
    buffer = buffer_dma.cpu_address;
    zero_bytes(queue, queue_dma.byte_length);
    zero_bytes(buffer, buffer_dma.byte_length);

    available_offset = VIRTQ_DESC_SIZE * queue_size;
    used_offset = align_up(available_offset + 6U +
        (uint64_t)queue_size * 2U, 4U);
    if (used_offset + 6U + (uint64_t)queue_size * 8U >
            queue_dma.byte_length) {
        result = DEVICE_SUBSTRATE_STATUS_QUEUE_FAILURE;
        goto cleanup;
    }

    write_u64(queue, 0U, (uint64_t)buffer_dma.frames.physical_base);
    write_u32(queue, 8U, VIRTIO_RNG_BYTES);
    write_u16(queue, 12U, VIRTQ_DESC_FLAG_WRITE);
    write_u16(queue, 14U, 0U);
    write_u16(queue, available_offset, 0U);
    write_u16(queue, available_offset + 4U, 0U);
    write_u16(queue, available_offset + 2U, 1U);
    proof->used_before = read_u16(queue, used_offset + 2U);
    interrupt_context.queue = &queue_dma;
    interrupt_context.buffer = &buffer_dma;

    msix_test_inject_failure_once();
    if (msix_bind(&claim, 0U, proof_interrupt, &interrupt_context,
            &binding) != MSIX_STATUS_INJECTED_FAILURE || binding.active ||
        msix_get_state().active_bindings != 0U ||
        interrupt_vector_get_state().allocated != 0U) {
        result = DEVICE_SUBSTRATE_STATUS_MSIX_NEGATIVE_CONTROL_FAILURE;
        goto cleanup;
    }
    ++proof->negative_controls;

    if (msix_bind(&claim, 0U, proof_interrupt, &interrupt_context,
            &binding) != MSIX_STATUS_OK) {
        result = DEVICE_SUBSTRATE_STATUS_MSIX_FAILURE;
        goto cleanup;
    }
    interrupt_context.expected_vector = binding.vector.vector;
    mmio_write16(common.base, VIRTIO_COMMON_QUEUE_MSIX_VECTOR, 0U);
    if (mmio_read16(common.base, VIRTIO_COMMON_QUEUE_MSIX_VECTOR) ==
            VIRTIO_QUEUE_NO_VECTOR) {
        result = DEVICE_SUBSTRATE_STATUS_MSIX_FAILURE;
        goto cleanup;
    }

    mmio_write64(common.base, VIRTIO_COMMON_QUEUE_DESC,
        (uint64_t)queue_dma.frames.physical_base);
    mmio_write64(common.base, VIRTIO_COMMON_QUEUE_DRIVER,
        (uint64_t)queue_dma.frames.physical_base + available_offset);
    mmio_write64(common.base, VIRTIO_COMMON_QUEUE_DEVICE,
        (uint64_t)queue_dma.frames.physical_base + used_offset);
    notify_offset = mmio_read16(common.base,
        VIRTIO_COMMON_QUEUE_NOTIFY_OFF);
    mmio_write16(common.base, VIRTIO_COMMON_QUEUE_ENABLE, 1U);
    if (mmio_read16(common.base, VIRTIO_COMMON_QUEUE_ENABLE) != 1U) {
        result = DEVICE_SUBSTRATE_STATUS_QUEUE_FAILURE;
        goto cleanup;
    }

    bus_master.allocations[0] = &queue_dma;
    bus_master.allocations[1] = &buffer_dma;
    bus_master.allocation_count = 2U;
    if (pci_claim_enable_bus_master(&claim, &bus_master) !=
            PCI_RESOURCE_STATUS_DMA_NOT_PREPARED) {
        result = DEVICE_SUBSTRATE_STATUS_BUS_MASTER_GUARD_FAILURE;
        goto cleanup;
    }
    ++proof->negative_controls;

    if (dma_mark_initialized(&queue_dma) != DMA_STATUS_OK ||
        dma_mark_initialized(&buffer_dma) != DMA_STATUS_OK ||
        dma_transfer_to_device(&queue_dma) != DMA_STATUS_OK ||
        dma_transfer_to_device(&buffer_dma) != DMA_STATUS_OK) {
        result = DEVICE_SUBSTRATE_STATUS_OWNERSHIP_FAILURE;
        goto cleanup;
    }
    if (pci_claim_enable_bus_master(&claim, &bus_master) !=
            PCI_RESOURCE_STATUS_OK) {
        result = DEVICE_SUBSTRATE_STATUS_BUS_MASTER_FAILURE;
        goto cleanup;
    }
    bus_master_enabled = true;

    mmio_write8(common.base, VIRTIO_COMMON_DEVICE_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
            VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    const uint64_t notify_displacement =
        (uint64_t)notify_offset * notify.notify_multiplier;
    if (notify_displacement > notify.length ||
        sizeof(uint16_t) > notify.length - notify_displacement) {
        result = DEVICE_SUBSTRATE_STATUS_QUEUE_FAILURE;
        goto cleanup;
    }
    cpu_store_fence();
    *(volatile uint16_t *)(void *)(notify.base + notify_displacement) =
        VIRTIO_QUEUE_INDEX;

    deadline = clock_monotonic_ns() + VIRTIO_PROOF_TIMEOUT_NS;
    cpu_interrupt_enable();
    while (interrupt_context.count == 0U &&
        !interrupt_context.wrong_vector &&
        clock_monotonic_ns() < deadline) {
        __asm__ volatile ("" : : : "memory");
    }
    cpu_interrupt_disable();
    if (interrupt_context.wrong_vector) {
        result = DEVICE_SUBSTRATE_STATUS_WRONG_INTERRUPT;
        goto cleanup;
    }
    if (interrupt_context.count == 0U) {
        result = DEVICE_SUBSTRATE_STATUS_INTERRUPT_TIMEOUT;
        goto cleanup;
    }
    if (interrupt_context.count != 1U) {
        result = DEVICE_SUBSTRATE_STATUS_WRONG_INTERRUPT;
        goto cleanup;
    }
    if (interrupt_context.ownership_failure ||
        queue_dma.owner != DMA_OWNER_CPU ||
        buffer_dma.owner != DMA_OWNER_CPU) {
        result = DEVICE_SUBSTRATE_STATUS_OWNERSHIP_FAILURE;
        goto cleanup;
    }

    proof->interrupt_count = interrupt_context.count;
    proof->used_after = read_u16(queue, used_offset + 2U);
    proof->used_length = read_u32(queue, used_offset + 8U);
    if (proof->used_after != (uint16_t)(proof->used_before + 1U) ||
        read_u32(queue, used_offset + 4U) != 0U ||
        proof->used_length != VIRTIO_RNG_BYTES) {
        result = DEVICE_SUBSTRATE_STATUS_USED_RING_FAILURE;
        goto cleanup;
    }
    if ((*(volatile uint8_t *)(void *)isr.base & UINT8_C(1)) == 0U) {
        result = DEVICE_SUBSTRATE_STATUS_WRONG_INTERRUPT;
        goto cleanup;
    }
    proof->random_bytes = VIRTIO_RNG_BYTES;
    for (size_t index = 0U; index < VIRTIO_RNG_BYTES; ++index) {
        if (buffer[index] != 0U) {
            ++proof->nonzero_bytes;
        }
    }
    if (proof->nonzero_bytes == 0U) {
        result = DEVICE_SUBSTRATE_STATUS_DEVICE_DMA_FAILURE;
        goto cleanup;
    }
    proof->dma_device_written = true;
    proof->msix_delivered = true;
    proof->ownership_round_trip = true;

cleanup:
    if (cpu_interrupts_enabled()) {
        cpu_interrupt_disable();
    }
    if (device_reset && common.base != NULL && !reset_device(common.base)) {
        result = DEVICE_SUBSTRATE_STATUS_TEARDOWN_FAILURE;
    }
    if (bus_master_enabled) {
        if (pci_claim_disable_bus_master(&claim) != PCI_RESOURCE_STATUS_OK) {
            result = DEVICE_SUBSTRATE_STATUS_TEARDOWN_FAILURE;
        }
        bus_master_enabled = false;
    }
    if (queue_dma.active && queue_dma.owner == DMA_OWNER_DEVICE &&
        dma_transfer_to_cpu(&queue_dma) != DMA_STATUS_OK) {
        result = DEVICE_SUBSTRATE_STATUS_TEARDOWN_FAILURE;
    }
    if (buffer_dma.active && buffer_dma.owner == DMA_OWNER_DEVICE &&
        dma_transfer_to_cpu(&buffer_dma) != DMA_STATUS_OK) {
        result = DEVICE_SUBSTRATE_STATUS_TEARDOWN_FAILURE;
    }
    if (binding.active && msix_unbind(&binding) != MSIX_STATUS_OK) {
        result = DEVICE_SUBSTRATE_STATUS_TEARDOWN_FAILURE;
    }
    if (buffer_dma.active && dma_release(&buffer_dma) != DMA_STATUS_OK) {
        result = DEVICE_SUBSTRATE_STATUS_TEARDOWN_FAILURE;
    }
    if (queue_dma.active && dma_release(&queue_dma) != DMA_STATUS_OK) {
        result = DEVICE_SUBSTRATE_STATUS_TEARDOWN_FAILURE;
    }
    if (claim.active && pci_release_device(&claim) !=
            PCI_RESOURCE_STATUS_OK) {
        result = DEVICE_SUBSTRATE_STATUS_TEARDOWN_FAILURE;
    }

    if (result == DEVICE_SUBSTRATE_STATUS_OK &&
        (pci_resource_get_state().active_claims != 0U ||
            pci_resource_get_state().active_mappings != 0U ||
            pci_resource_get_state().bus_masters != 0U ||
            interrupt_vector_get_state().allocated != 0U ||
            dma_get_state().active_allocations != 0U ||
            msix_get_state().active_bindings != 0U)) {
        result = DEVICE_SUBSTRATE_STATUS_TEARDOWN_FAILURE;
    }
    proof->teardown_complete = result == DEVICE_SUBSTRATE_STATUS_OK;
    if (result == DEVICE_SUBSTRATE_STATUS_OK) {
        installed_proof = *proof;
    }
    return result;
}

struct device_substrate_proof device_substrate_get_proof(void)
{
    return installed_proof;
}

const char *device_substrate_status_string(
    enum device_substrate_status status
)
{
    static const char *const messages[DEVICE_SUBSTRATE_STATUS_COUNT] = {
        "ok", "VirtIO RNG proof fixture is absent",
        "VirtIO RNG PCI claim failed", "VirtIO PCI capability is malformed",
        "VirtIO capability BAR mapping failed", "VirtIO reset timed out",
        "VirtIO 1.0 feature negotiation failed",
        "VirtIO request queue setup failed", "VirtIO DMA allocation failed",
        "MSI-X halfway-failure rollback control failed",
        "VirtIO MSI-X binding failed",
        "bus mastering was accepted before DMA preparation",
        "VirtIO bus mastering could not be enabled",
        "VirtIO RNG interrupt timed out", "interrupt was not the bound MSI-X",
        "VirtIO used ring did not advance exactly once",
        "VirtIO RNG did not DMA bytes into the receive buffer",
        "DMA ownership transition violated its state machine",
        "device-substrate teardown leaked or failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        DEVICE_SUBSTRATE_STATUS_COUNT,
        "device-substrate status messages are out of sync");
    if (status < DEVICE_SUBSTRATE_STATUS_OK ||
        status >= DEVICE_SUBSTRATE_STATUS_COUNT) {
        return "unknown device-substrate status";
    }
    return messages[status];
}
