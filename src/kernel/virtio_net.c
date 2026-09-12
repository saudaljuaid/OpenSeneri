/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/cpu.h>
#include <phipia/clock.h>
#include <phipia/console.h>
#include <phipia/dma.h>
#include <phipia/interrupts.h>
#include <phipia/msix.h>
#include <phipia/pci.h>
#include <phipia/pci_resource.h>
#include <phipia/virtio_net.h>

#define VIRTIO_VENDOR_ID UINT16_C(0x1AF4)
#define VIRTIO_NET_MODERN_DEVICE_ID UINT16_C(0x1041)

#define VIRTIO_PCI_CAP_COMMON UINT8_C(1)
#define VIRTIO_PCI_CAP_NOTIFY UINT8_C(2)
#define VIRTIO_PCI_CAP_ISR UINT8_C(3)
#define VIRTIO_PCI_CAP_DEVICE UINT8_C(4)
#define VIRTIO_PCI_CAP_MIN_LENGTH UINT8_C(16)
#define VIRTIO_PCI_NOTIFY_CAP_LENGTH UINT8_C(20)

#define VIRTIO_STATUS_ACKNOWLEDGE UINT8_C(1)
#define VIRTIO_STATUS_DRIVER UINT8_C(2)
#define VIRTIO_STATUS_DRIVER_OK UINT8_C(4)
#define VIRTIO_STATUS_FEATURES_OK UINT8_C(8)
#define VIRTIO_STATUS_NEEDS_RESET UINT8_C(64)
#define VIRTIO_STATUS_FAILED UINT8_C(128)

#define VIRTIO_NET_F_MAC (UINT64_C(1) << 5U)
#define VIRTIO_NET_F_STATUS (UINT64_C(1) << 16U)
#define VIRTIO_F_VERSION_1 (UINT64_C(1) << 32U)
#define VIRTIO_NET_REQUIRED_FEATURES \
    (VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS | VIRTIO_F_VERSION_1)

#define VIRTIO_COMMON_DEVICE_FEATURE_SELECT UINT64_C(0)
#define VIRTIO_COMMON_DEVICE_FEATURE UINT64_C(4)
#define VIRTIO_COMMON_DRIVER_FEATURE_SELECT UINT64_C(8)
#define VIRTIO_COMMON_DRIVER_FEATURE UINT64_C(12)
#define VIRTIO_COMMON_CONFIG_MSIX_VECTOR UINT64_C(16)
#define VIRTIO_COMMON_NUM_QUEUES UINT64_C(18)
#define VIRTIO_COMMON_DEVICE_STATUS UINT64_C(20)
#define VIRTIO_COMMON_CONFIG_GENERATION UINT64_C(21)
#define VIRTIO_COMMON_QUEUE_SELECT UINT64_C(22)
#define VIRTIO_COMMON_QUEUE_SIZE UINT64_C(24)
#define VIRTIO_COMMON_QUEUE_MSIX_VECTOR UINT64_C(26)
#define VIRTIO_COMMON_QUEUE_ENABLE UINT64_C(28)
#define VIRTIO_COMMON_QUEUE_NOTIFY_OFF UINT64_C(30)
#define VIRTIO_COMMON_QUEUE_DESC UINT64_C(32)
#define VIRTIO_COMMON_QUEUE_DRIVER UINT64_C(40)
#define VIRTIO_COMMON_QUEUE_DEVICE UINT64_C(48)
#define VIRTIO_COMMON_MIN_LENGTH UINT64_C(56)

#define VIRTIO_NET_DEVICE_MAC UINT64_C(0)
#define VIRTIO_NET_DEVICE_STATUS UINT64_C(6)
#define VIRTIO_NET_DEVICE_MIN_LENGTH UINT64_C(8)
#define VIRTIO_NET_STATUS_LINK_UP UINT16_C(1)

#define VIRTIO_NET_RX_QUEUE UINT16_C(0)
#define VIRTIO_NET_TX_QUEUE UINT16_C(1)
#define VIRTIO_QUEUE_NO_VECTOR UINT16_C(0xFFFF)
#define VIRTIO_MSIX_ENTRY UINT16_C(0)

#define VIRTQ_DESC_BYTES UINT64_C(16)
#define VIRTQ_DESC_FLAG_WRITE UINT16_C(2)
/* VERSION_1 devices use the 12-byte modern header layout even when the
 * mergeable receive-buffer feature is not negotiated.  The final field is
 * reserved/zero on transmit and contains one receive buffer on our RX path.
 */
#define VIRTIO_NET_HEADER_BYTES 12U
#define VIRTIO_NET_ARENA_BYTES \
    ((uint64_t)VIRTIO_NET_PACKET_COUNT * VIRTIO_NET_PACKET_BYTES)
#define VIRTIO_NET_ARENA_PAGES \
    ((VIRTIO_NET_ARENA_BYTES + PHIPIA_PAGE_SIZE - 1U) / PHIPIA_PAGE_SIZE)

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

struct virtio_queue {
    struct dma_allocation dma;
    uint8_t *memory;
    uint64_t available_offset;
    uint64_t used_offset;
    uint64_t notify_displacement;
    uint16_t size;
    uint16_t available_index;
    uint16_t used_index;
    uint16_t packet_for_descriptor[VIRTIO_NET_QUEUE_LENGTH];
    bool descriptor_inflight[VIRTIO_NET_QUEUE_LENGTH];
};

struct packet_record {
    enum virtio_net_packet_owner owner;
    uint16_t length;
    uint16_t descriptor;
};

struct virtio_net_runtime {
    struct virtio_net_state public;
    struct pci_device_claim claim;
    struct msix_binding msix;
    struct dma_allocation arena;
    struct virtio_queue rx;
    struct virtio_queue tx;
    struct virtio_capability_region common;
    struct virtio_capability_region notify;
    struct virtio_capability_region isr;
    struct virtio_capability_region device;
    struct packet_record packets[VIRTIO_NET_PACKET_COUNT];
    uint16_t ready[VIRTIO_NET_RX_RESERVE];
    size_t ready_head;
    size_t ready_count;
    volatile bool deferred_work;
    bool bus_master_enabled;
};

static struct virtio_net_runtime runtime;
static uint64_t next_device_generation = UINT64_C(1);

_Static_assert(VIRTIO_NET_ARENA_BYTES % PHIPIA_PAGE_SIZE == 0U,
    "network packet arena must use complete DMA pages");
_Static_assert(VIRTIO_NET_HEADER_BYTES + VIRTIO_NET_MAX_FRAME_SIZE <=
    VIRTIO_NET_PACKET_BYTES, "network packet buffer is too small");
_Static_assert(VIRTIO_NET_PACKET_COUNT <= UINT16_MAX,
    "network packet indices must fit in descriptors");

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool add_checked(uint64_t left, uint64_t right, uint64_t *sum)
{
    if (sum == NULL || left > UINT64_MAX - right) {
        return false;
    }
    *sum = left + right;
    return true;
}

static bool multiply_checked(uint64_t left, uint64_t right, uint64_t *product)
{
    if (product == NULL || (right != 0U && left > UINT64_MAX / right)) {
        return false;
    }
    *product = left * right;
    return true;
}

static bool align_up_checked(uint64_t value, uint64_t alignment, uint64_t *result)
{
    uint64_t rounded;

    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U ||
        !add_checked(value, alignment - 1U, &rounded)) {
        return false;
    }
    *result = rounded & ~(alignment - 1U);
    return true;
}

static bool queue_layout(
    uint16_t size,
    uint64_t capacity,
    uint64_t *available_offset,
    uint64_t *used_offset
)
{
    uint64_t descriptors;
    uint64_t available_bytes;
    uint64_t used_bytes;
    uint64_t available_end;
    uint64_t used_end;

    if (size == 0U || size > VIRTIO_NET_QUEUE_LENGTH ||
        (size & (uint16_t)(size - 1U)) != 0U ||
        !multiply_checked(VIRTQ_DESC_BYTES, size, &descriptors) ||
        !multiply_checked(UINT64_C(2), size, &available_bytes) ||
        !add_checked(available_bytes, UINT64_C(6), &available_bytes) ||
        !add_checked(descriptors, available_bytes, &available_end) ||
        !align_up_checked(available_end, UINT64_C(4), used_offset) ||
        !multiply_checked(UINT64_C(8), size, &used_bytes) ||
        !add_checked(used_bytes, UINT64_C(6), &used_bytes) ||
        !add_checked(*used_offset, used_bytes, &used_end) ||
        used_end > capacity) {
        return false;
    }
    *available_offset = descriptors;
    return true;
}

static uint32_t config_dword(
    const struct pci_function *function,
    uint16_t offset,
    bool *ok
)
{
    uint32_t value = 0U;

    if (ok == NULL || pci_config_read_port(function->address,
            (uint16_t)(offset & ~UINT16_C(3)), &value) != PCI_STATUS_OK) {
        if (ok != NULL) {
            *ok = false;
        }
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

static enum virtio_net_status collect_capabilities(
    const struct pci_function *function
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
            return VIRTIO_NET_STATUS_CAPABILITY_FAILURE;
        }
        switch (type) {
        case VIRTIO_PCI_CAP_COMMON: region = &runtime.common; break;
        case VIRTIO_PCI_CAP_NOTIFY: region = &runtime.notify; break;
        case VIRTIO_PCI_CAP_ISR: region = &runtime.isr; break;
        case VIRTIO_PCI_CAP_DEVICE: region = &runtime.device; break;
        default: continue;
        }
        if (region->present) {
            return VIRTIO_NET_STATUS_CAPABILITY_FAILURE;
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
                return VIRTIO_NET_STATUS_CAPABILITY_FAILURE;
            }
            region->notify_multiplier = config_u32(function,
                (uint16_t)(capability->offset + 16U), &ok);
        }
        if (!ok || region->bar >= PCI_BAR_COUNT || region->length == 0U) {
            return VIRTIO_NET_STATUS_CAPABILITY_FAILURE;
        }
        region->present = true;
    }
    if (!runtime.common.present || !runtime.notify.present ||
        !runtime.isr.present || !runtime.device.present ||
        runtime.common.length < VIRTIO_COMMON_MIN_LENGTH ||
        runtime.device.length < VIRTIO_NET_DEVICE_MIN_LENGTH) {
        return VIRTIO_NET_STATUS_CAPABILITY_FAILURE;
    }
    return VIRTIO_NET_STATUS_OK;
}

static enum virtio_net_status map_capability(
    struct virtio_capability_region *capability
)
{
    volatile void *pointer = NULL;

    capability->mapping = pci_claim_mapped_bar(&runtime.claim,
        capability->bar);
    if (capability->mapping == NULL &&
        pci_claim_map_bar(&runtime.claim, capability->bar,
            &capability->mapping) != PCI_RESOURCE_STATUS_OK) {
        return VIRTIO_NET_STATUS_MAPPING_FAILURE;
    }
    if (pci_mmio_subregion(capability->mapping, capability->offset,
            capability->length, &pointer) != PCI_RESOURCE_STATUS_OK) {
        return VIRTIO_NET_STATUS_MAPPING_FAILURE;
    }
    capability->base = (volatile uint8_t *)pointer;
    return VIRTIO_NET_STATUS_OK;
}

static uint8_t mmio_read8(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint8_t *)(void *)(base + offset);
}

static uint16_t mmio_read16(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint16_t *)(void *)(base + offset);
}

static uint32_t mmio_read32(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint32_t *)(void *)(base + offset);
}

static void mmio_write8(volatile uint8_t *base, uint64_t offset, uint8_t value)
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

static bool reset_device(void)
{
    if (runtime.common.base == NULL) {
        return false;
    }
    mmio_write8(runtime.common.base, VIRTIO_COMMON_DEVICE_STATUS, 0U);
    for (uint64_t spin = 0U; spin < UINT64_C(10000000); ++spin) {
        if (mmio_read8(runtime.common.base,
                VIRTIO_COMMON_DEVICE_STATUS) == 0U) {
            return true;
        }
    }
    return false;
}

static uint8_t *packet_bytes(uint16_t packet)
{
    if (packet >= VIRTIO_NET_PACKET_COUNT || runtime.arena.cpu_address == NULL) {
        return NULL;
    }
    return (uint8_t *)runtime.arena.cpu_address +
        (size_t)packet * VIRTIO_NET_PACKET_BYTES;
}

static uint64_t packet_physical(uint16_t packet)
{
    return (uint64_t)runtime.arena.frames.physical_base +
        (uint64_t)packet * VIRTIO_NET_PACKET_BYTES;
}

static void descriptor_write(
    struct virtio_queue *queue,
    uint16_t descriptor,
    uint64_t address,
    uint32_t length,
    uint16_t flags
)
{
    uint8_t *entry = queue->memory + (size_t)descriptor * VIRTQ_DESC_BYTES;

    *(uint64_t *)(void *)(entry + 0U) = address;
    *(uint32_t *)(void *)(entry + 8U) = length;
    *(uint16_t *)(void *)(entry + 12U) = flags;
    *(uint16_t *)(void *)(entry + 14U) = 0U;
}

static void queue_publish(struct virtio_queue *queue, uint16_t descriptor)
{
    const uint16_t slot = (uint16_t)(queue->available_index &
        (uint16_t)(queue->size - 1U));

    *(uint16_t *)(void *)(queue->memory + queue->available_offset + 4U +
        (uint64_t)slot * 2U) = descriptor;
    cpu_store_fence();
    ++queue->available_index;
    *(volatile uint16_t *)(void *)(queue->memory +
        queue->available_offset + 2U) = queue->available_index;
}

static bool queue_used(
    struct virtio_queue *queue,
    uint32_t *identifier,
    uint32_t *length
)
{
    const uint16_t device_index = *(volatile uint16_t *)(void *)(
        queue->memory + queue->used_offset + 2U);
    uint16_t slot;
    uint8_t *element;

    if (device_index == queue->used_index) {
        return false;
    }
    slot = (uint16_t)(queue->used_index & (uint16_t)(queue->size - 1U));
    element = queue->memory + queue->used_offset + 4U + (uint64_t)slot * 8U;
    __asm__ volatile ("lfence" : : : "memory");
    *identifier = *(volatile uint32_t *)(void *)(element + 0U);
    *length = *(volatile uint32_t *)(void *)(element + 4U);
    ++queue->used_index;
    return true;
}

static void notify_queue(const struct virtio_queue *queue, uint16_t index)
{
    cpu_store_fence();
    *(volatile uint16_t *)(void *)(runtime.notify.base +
        queue->notify_displacement) = index;
}

static uint16_t free_rx_packet(void)
{
    for (uint16_t packet = 0U; packet < VIRTIO_NET_RX_RESERVE; ++packet) {
        if (runtime.packets[packet].owner == VIRTIO_NET_PACKET_FREE ||
            runtime.packets[packet].owner == VIRTIO_NET_PACKET_RELEASED) {
            return packet;
        }
    }
    return UINT16_MAX;
}

static bool post_rx_descriptor(uint16_t descriptor)
{
    const uint16_t packet = free_rx_packet();

    if (descriptor >= runtime.rx.size || packet == UINT16_MAX ||
        runtime.rx.descriptor_inflight[descriptor]) {
        return false;
    }
    zero_bytes(packet_bytes(packet), VIRTIO_NET_PACKET_BYTES);
    runtime.packets[packet].owner = VIRTIO_NET_PACKET_DEVICE_RX;
    runtime.packets[packet].length = 0U;
    runtime.packets[packet].descriptor = descriptor;
    runtime.rx.packet_for_descriptor[descriptor] = packet;
    runtime.rx.descriptor_inflight[descriptor] = true;
    descriptor_write(&runtime.rx, descriptor, packet_physical(packet),
        VIRTIO_NET_PACKET_BYTES, VIRTQ_DESC_FLAG_WRITE);
    queue_publish(&runtime.rx, descriptor);
    return true;
}

static void replenish_rx(void)
{
    bool posted = false;

    for (uint16_t descriptor = 0U; descriptor < runtime.rx.size; ++descriptor) {
        if (!runtime.rx.descriptor_inflight[descriptor] &&
            post_rx_descriptor(descriptor)) {
            posted = true;
        }
    }
    if (posted) {
        notify_queue(&runtime.rx, VIRTIO_NET_RX_QUEUE);
    }
}

static bool ready_push(uint16_t packet)
{
    size_t slot;

    if (runtime.ready_count >= VIRTIO_NET_RX_RESERVE) {
        return false;
    }
    slot = (runtime.ready_head + runtime.ready_count) %
        VIRTIO_NET_RX_RESERVE;
    runtime.ready[slot] = packet;
    ++runtime.ready_count;
    return true;
}

static bool ready_pop(uint16_t *packet)
{
    if (packet == NULL || runtime.ready_count == 0U) {
        return false;
    }
    *packet = runtime.ready[runtime.ready_head];
    runtime.ready_head = (runtime.ready_head + 1U) %
        VIRTIO_NET_RX_RESERVE;
    --runtime.ready_count;
    return true;
}

static enum virtio_net_status service_rx(void)
{
    uint32_t identifier;
    uint32_t used_length;
    size_t completed = 0U;

    while (completed < runtime.rx.size &&
        queue_used(&runtime.rx, &identifier, &used_length)) {
        uint16_t packet;
        struct packet_record *record;

        ++completed;
        if (identifier >= runtime.rx.size ||
            !runtime.rx.descriptor_inflight[identifier]) {
            ++runtime.public.statistics.malformed_frames;
            return VIRTIO_NET_STATUS_BAD_COMPLETION;
        }
        runtime.rx.descriptor_inflight[identifier] = false;
        packet = runtime.rx.packet_for_descriptor[identifier];
        if (packet >= VIRTIO_NET_RX_RESERVE) {
            ++runtime.public.statistics.malformed_frames;
            return VIRTIO_NET_STATUS_BAD_COMPLETION;
        }
        record = &runtime.packets[packet];
        if (record->owner != VIRTIO_NET_PACKET_DEVICE_RX ||
            record->descriptor != identifier) {
            ++runtime.public.statistics.malformed_frames;
            return VIRTIO_NET_STATUS_OWNERSHIP_FAILURE;
        }
        if (used_length < VIRTIO_NET_HEADER_BYTES ||
            used_length > VIRTIO_NET_HEADER_BYTES +
                VIRTIO_NET_MAX_FRAME_SIZE) {
            record->owner = VIRTIO_NET_PACKET_RELEASED;
            ++runtime.public.statistics.malformed_frames;
            ++runtime.public.statistics.dropped_frames;
        } else {
            record->length = (uint16_t)(used_length -
                VIRTIO_NET_HEADER_BYTES);
            record->owner = VIRTIO_NET_PACKET_KERNEL_RX;
            if (!ready_push(packet)) {
                record->owner = VIRTIO_NET_PACKET_RELEASED;
                ++runtime.public.statistics.exhausted_frames;
                ++runtime.public.statistics.dropped_frames;
            } else {
                ++runtime.public.statistics.rx_frames;
            }
        }
        if (!post_rx_descriptor((uint16_t)identifier)) {
            ++runtime.public.statistics.exhausted_frames;
        }
    }
    return VIRTIO_NET_STATUS_OK;
}

static enum virtio_net_status service_tx(void)
{
    uint32_t identifier;
    uint32_t used_length;
    size_t completed = 0U;

    while (completed < runtime.tx.size &&
        queue_used(&runtime.tx, &identifier, &used_length)) {
        uint16_t packet;
        struct packet_record *record;

        (void)used_length;
        ++completed;
        if (identifier >= runtime.tx.size ||
            !runtime.tx.descriptor_inflight[identifier]) {
            return VIRTIO_NET_STATUS_BAD_COMPLETION;
        }
        runtime.tx.descriptor_inflight[identifier] = false;
        packet = runtime.tx.packet_for_descriptor[identifier];
        if (packet < VIRTIO_NET_RX_RESERVE ||
            packet >= VIRTIO_NET_PACKET_COUNT) {
            return VIRTIO_NET_STATUS_BAD_COMPLETION;
        }
        record = &runtime.packets[packet];
        if (record->owner != VIRTIO_NET_PACKET_DEVICE_TX ||
            record->descriptor != identifier) {
            return VIRTIO_NET_STATUS_OWNERSHIP_FAILURE;
        }
        record->owner = VIRTIO_NET_PACKET_TX_COMPLETE;
        record->length = 0U;
        record->owner = VIRTIO_NET_PACKET_RELEASED;
        record->owner = VIRTIO_NET_PACKET_FREE;
        ++runtime.public.statistics.tx_frames;
    }
    return VIRTIO_NET_STATUS_OK;
}

static void network_interrupt(struct interrupt_frame *frame, void *context)
{
    struct virtio_net_runtime *owner = context;
    uint64_t started;

    if (frame == NULL || owner != &runtime ||
        frame->vector != runtime.msix.vector.vector) {
        return;
    }
    started = clock_monotonic_ns();
    /* Reading the ISR byte acknowledges the VirtIO interrupt cause. */
    (void)mmio_read8(runtime.isr.base, 0U);
    ++runtime.public.statistics.interrupts;
    runtime.deferred_work = true;
    runtime.public.statistics.interrupt_processing_ns +=
        clock_monotonic_ns() - started;
}

static enum virtio_net_status allocate_dma(void)
{
    struct dma_request queue_request = {
        .page_count = 1U,
        .alignment = PHIPIA_PAGE_SIZE,
        .maximum_physical_address = UINT32_MAX
    };
    struct dma_request arena_request = {
        .page_count = VIRTIO_NET_ARENA_PAGES,
        .alignment = PHIPIA_PAGE_SIZE,
        .maximum_physical_address = UINT32_MAX
    };

    if (dma_allocate(&queue_request, &runtime.rx.dma) != DMA_STATUS_OK ||
        dma_allocate(&queue_request, &runtime.tx.dma) != DMA_STATUS_OK ||
        dma_allocate(&arena_request, &runtime.arena) != DMA_STATUS_OK) {
        return VIRTIO_NET_STATUS_DMA_FAILURE;
    }
    runtime.rx.memory = runtime.rx.dma.cpu_address;
    runtime.tx.memory = runtime.tx.dma.cpu_address;
    zero_bytes(runtime.rx.memory, runtime.rx.dma.byte_length);
    zero_bytes(runtime.tx.memory, runtime.tx.dma.byte_length);
    zero_bytes(runtime.arena.cpu_address, runtime.arena.byte_length);
    if (!queue_layout(VIRTIO_NET_QUEUE_LENGTH, runtime.rx.dma.byte_length,
            &runtime.rx.available_offset, &runtime.rx.used_offset) ||
        !queue_layout(VIRTIO_NET_QUEUE_LENGTH, runtime.tx.dma.byte_length,
            &runtime.tx.available_offset, &runtime.tx.used_offset)) {
        return VIRTIO_NET_STATUS_QUEUE_FAILURE;
    }
    runtime.rx.size = VIRTIO_NET_QUEUE_LENGTH;
    runtime.tx.size = VIRTIO_NET_QUEUE_LENGTH;
    for (size_t index = 0U; index < VIRTIO_NET_PACKET_COUNT; ++index) {
        runtime.packets[index].owner = VIRTIO_NET_PACKET_FREE;
    }
    return VIRTIO_NET_STATUS_OK;
}

static enum virtio_net_status negotiate_features(void)
{
    uint64_t offered;
    uint8_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

    mmio_write8(runtime.common.base, VIRTIO_COMMON_DEVICE_STATUS, status);
    mmio_write32(runtime.common.base, VIRTIO_COMMON_DEVICE_FEATURE_SELECT, 0U);
    offered = mmio_read32(runtime.common.base,
        VIRTIO_COMMON_DEVICE_FEATURE);
    mmio_write32(runtime.common.base, VIRTIO_COMMON_DEVICE_FEATURE_SELECT, 1U);
    offered |= (uint64_t)mmio_read32(runtime.common.base,
        VIRTIO_COMMON_DEVICE_FEATURE) << 32U;
    if ((offered & VIRTIO_NET_REQUIRED_FEATURES) !=
            VIRTIO_NET_REQUIRED_FEATURES) {
        return VIRTIO_NET_STATUS_FEATURE_FAILURE;
    }
    runtime.public.negotiated_features = VIRTIO_NET_REQUIRED_FEATURES;
    mmio_write32(runtime.common.base, VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 0U);
    mmio_write32(runtime.common.base, VIRTIO_COMMON_DRIVER_FEATURE,
        (uint32_t)VIRTIO_NET_REQUIRED_FEATURES);
    mmio_write32(runtime.common.base, VIRTIO_COMMON_DRIVER_FEATURE_SELECT, 1U);
    mmio_write32(runtime.common.base, VIRTIO_COMMON_DRIVER_FEATURE,
        (uint32_t)(VIRTIO_NET_REQUIRED_FEATURES >> 32U));
    status |= VIRTIO_STATUS_FEATURES_OK;
    mmio_write8(runtime.common.base, VIRTIO_COMMON_DEVICE_STATUS, status);
    if ((mmio_read8(runtime.common.base, VIRTIO_COMMON_DEVICE_STATUS) &
            VIRTIO_STATUS_FEATURES_OK) == 0U) {
        return VIRTIO_NET_STATUS_FEATURE_FAILURE;
    }
    return VIRTIO_NET_STATUS_OK;
}

static enum virtio_net_status read_device_configuration(void)
{
    for (size_t attempt = 0U; attempt < 8U; ++attempt) {
        const uint8_t before = mmio_read8(runtime.common.base,
            VIRTIO_COMMON_CONFIG_GENERATION);
        uint8_t mac[6];
        uint16_t status;
        uint8_t after;

        for (size_t index = 0U; index < sizeof(mac); ++index) {
            mac[index] = mmio_read8(runtime.device.base,
                VIRTIO_NET_DEVICE_MAC + index);
        }
        status = mmio_read16(runtime.device.base, VIRTIO_NET_DEVICE_STATUS);
        after = mmio_read8(runtime.common.base,
            VIRTIO_COMMON_CONFIG_GENERATION);
        if (before != after) {
            continue;
        }
        if ((mac[0] & UINT8_C(1)) != 0U ||
            (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0U) {
            return VIRTIO_NET_STATUS_FEATURE_FAILURE;
        }
        copy_bytes(runtime.public.mac, mac, sizeof(mac));
        runtime.public.link_up = (status & VIRTIO_NET_STATUS_LINK_UP) != 0U;
        return runtime.public.link_up ? VIRTIO_NET_STATUS_OK :
            VIRTIO_NET_STATUS_LINK_DOWN;
    }
    return VIRTIO_NET_STATUS_FEATURE_FAILURE;
}

static enum virtio_net_status configure_queue(
    struct virtio_queue *queue,
    uint16_t index
)
{
    uint16_t maximum;
    uint16_t notify_offset;
    uint64_t displacement;

    mmio_write16(runtime.common.base, VIRTIO_COMMON_QUEUE_SELECT, index);
    maximum = mmio_read16(runtime.common.base, VIRTIO_COMMON_QUEUE_SIZE);
    if (maximum < VIRTIO_NET_QUEUE_LENGTH ||
        mmio_read16(runtime.common.base, VIRTIO_COMMON_QUEUE_ENABLE) != 0U) {
        return VIRTIO_NET_STATUS_QUEUE_FAILURE;
    }
    mmio_write16(runtime.common.base, VIRTIO_COMMON_QUEUE_SIZE, queue->size);
    mmio_write16(runtime.common.base, VIRTIO_COMMON_QUEUE_MSIX_VECTOR,
        VIRTIO_MSIX_ENTRY);
    if (mmio_read16(runtime.common.base,
            VIRTIO_COMMON_QUEUE_MSIX_VECTOR) == VIRTIO_QUEUE_NO_VECTOR) {
        return VIRTIO_NET_STATUS_MSIX_FAILURE;
    }
    mmio_write64(runtime.common.base, VIRTIO_COMMON_QUEUE_DESC,
        (uint64_t)queue->dma.frames.physical_base);
    mmio_write64(runtime.common.base, VIRTIO_COMMON_QUEUE_DRIVER,
        (uint64_t)queue->dma.frames.physical_base + queue->available_offset);
    mmio_write64(runtime.common.base, VIRTIO_COMMON_QUEUE_DEVICE,
        (uint64_t)queue->dma.frames.physical_base + queue->used_offset);
    notify_offset = mmio_read16(runtime.common.base,
        VIRTIO_COMMON_QUEUE_NOTIFY_OFF);
    if (!multiply_checked(notify_offset, runtime.notify.notify_multiplier,
            &displacement) || displacement > runtime.notify.length ||
        sizeof(uint16_t) > runtime.notify.length - displacement) {
        return VIRTIO_NET_STATUS_QUEUE_FAILURE;
    }
    queue->notify_displacement = displacement;
    mmio_write16(runtime.common.base, VIRTIO_COMMON_QUEUE_ENABLE, 1U);
    if (mmio_read16(runtime.common.base, VIRTIO_COMMON_QUEUE_ENABLE) != 1U) {
        return VIRTIO_NET_STATUS_QUEUE_FAILURE;
    }
    return VIRTIO_NET_STATUS_OK;
}

static enum virtio_net_status prepare_bus_master(void)
{
    struct pci_bus_master_request request = {0};

    if (dma_mark_initialized(&runtime.rx.dma) != DMA_STATUS_OK ||
        dma_mark_initialized(&runtime.tx.dma) != DMA_STATUS_OK ||
        dma_mark_initialized(&runtime.arena) != DMA_STATUS_OK ||
        dma_transfer_to_device(&runtime.rx.dma) != DMA_STATUS_OK ||
        dma_transfer_to_device(&runtime.tx.dma) != DMA_STATUS_OK ||
        dma_transfer_to_device(&runtime.arena) != DMA_STATUS_OK) {
        return VIRTIO_NET_STATUS_OWNERSHIP_FAILURE;
    }
    request.allocations[0] = &runtime.rx.dma;
    request.allocations[1] = &runtime.tx.dma;
    request.allocations[2] = &runtime.arena;
    request.allocation_count = 3U;
    if (pci_claim_enable_bus_master(&runtime.claim, &request) !=
            PCI_RESOURCE_STATUS_OK) {
        return VIRTIO_NET_STATUS_BUS_MASTER_FAILURE;
    }
    runtime.bus_master_enabled = true;
    return VIRTIO_NET_STATUS_OK;
}

static enum virtio_net_status cleanup(bool reset)
{
    enum virtio_net_status result = VIRTIO_NET_STATUS_OK;
    const bool restore_interrupts = cpu_interrupts_enabled();

    if (restore_interrupts) {
        cpu_interrupt_disable();
    }
    if (reset && runtime.common.base != NULL && !reset_device()) {
        result = VIRTIO_NET_STATUS_TEARDOWN_FAILURE;
    }
    if (runtime.bus_master_enabled &&
        pci_claim_disable_bus_master(&runtime.claim) !=
            PCI_RESOURCE_STATUS_OK) {
        result = VIRTIO_NET_STATUS_TEARDOWN_FAILURE;
    }
    runtime.bus_master_enabled = false;
    if (runtime.msix.active && msix_unbind(&runtime.msix) != MSIX_STATUS_OK) {
        result = VIRTIO_NET_STATUS_TEARDOWN_FAILURE;
    }
    struct dma_allocation *const allocations[3] = {
        &runtime.arena, &runtime.tx.dma, &runtime.rx.dma
    };
    for (size_t index = 0U; index < 3U; ++index) {
        struct dma_allocation *allocation = allocations[index];

        if (!allocation->active) {
            continue;
        }
        if (allocation->owner == DMA_OWNER_DEVICE &&
            dma_transfer_to_cpu(allocation) != DMA_STATUS_OK) {
            result = VIRTIO_NET_STATUS_TEARDOWN_FAILURE;
            continue;
        }
        if (allocation->owner == DMA_OWNER_CPU &&
            dma_release(allocation) != DMA_STATUS_OK) {
            result = VIRTIO_NET_STATUS_TEARDOWN_FAILURE;
        }
    }
    if (runtime.claim.active && pci_release_device(&runtime.claim) !=
            PCI_RESOURCE_STATUS_OK) {
        result = VIRTIO_NET_STATUS_TEARDOWN_FAILURE;
    }
    if (restore_interrupts) {
        cpu_interrupt_enable();
    }
    return result;
}

enum virtio_net_status virtio_net_initialize(void)
{
    const struct pci_function *function;
    enum virtio_net_status status;
    enum pci_resource_status resource_status;

    if (runtime.public.active) {
        return VIRTIO_NET_STATUS_ALREADY_INITIALIZED;
    }
    zero_bytes(&runtime, sizeof(runtime));
    function = pci_find_device(VIRTIO_VENDOR_ID,
        VIRTIO_NET_MODERN_DEVICE_ID);
    if (function == NULL) {
        return VIRTIO_NET_STATUS_ABSENT;
    }
    runtime.public.present = true;
    if (function->class_code != PCI_CLASS_NETWORK ||
        function->msi_x_offset == 0U) {
        return VIRTIO_NET_STATUS_UNSUPPORTED_DEVICE;
    }
    resource_status = pci_claim_device(function, &runtime.claim);
    if (resource_status != PCI_RESOURCE_STATUS_OK) {
        console_write("Phipia: virtio-net PCI claim failed: ");
        console_write(pci_resource_status_string(resource_status));
        console_putc('\n');
        return VIRTIO_NET_STATUS_CLAIM_FAILURE;
    }
    status = collect_capabilities(function);
    if (status != VIRTIO_NET_STATUS_OK ||
        map_capability(&runtime.common) != VIRTIO_NET_STATUS_OK ||
        map_capability(&runtime.notify) != VIRTIO_NET_STATUS_OK ||
        map_capability(&runtime.isr) != VIRTIO_NET_STATUS_OK ||
        map_capability(&runtime.device) != VIRTIO_NET_STATUS_OK) {
        status = status == VIRTIO_NET_STATUS_OK ?
            VIRTIO_NET_STATUS_MAPPING_FAILURE : status;
        (void)cleanup(false);
        return status;
    }
    if (!reset_device()) {
        (void)cleanup(false);
        return VIRTIO_NET_STATUS_RESET_FAILURE;
    }
    status = negotiate_features();
    if (status != VIRTIO_NET_STATUS_OK) {
        mmio_write8(runtime.common.base, VIRTIO_COMMON_DEVICE_STATUS,
            VIRTIO_STATUS_FAILED);
        (void)cleanup(true);
        return status;
    }
    status = read_device_configuration();
    if (status != VIRTIO_NET_STATUS_OK &&
        status != VIRTIO_NET_STATUS_LINK_DOWN) {
        (void)cleanup(true);
        return status;
    }
    if (mmio_read16(runtime.common.base, VIRTIO_COMMON_NUM_QUEUES) < 2U) {
        (void)cleanup(true);
        return VIRTIO_NET_STATUS_QUEUE_FAILURE;
    }
    status = allocate_dma();
    if (status != VIRTIO_NET_STATUS_OK) {
        (void)cleanup(true);
        return status;
    }
    if (msix_bind(&runtime.claim, VIRTIO_MSIX_ENTRY, network_interrupt,
            &runtime, &runtime.msix) != MSIX_STATUS_OK) {
        (void)cleanup(true);
        return VIRTIO_NET_STATUS_MSIX_FAILURE;
    }
    mmio_write16(runtime.common.base, VIRTIO_COMMON_CONFIG_MSIX_VECTOR,
        VIRTIO_MSIX_ENTRY);
    if (mmio_read16(runtime.common.base,
            VIRTIO_COMMON_CONFIG_MSIX_VECTOR) == VIRTIO_QUEUE_NO_VECTOR) {
        (void)cleanup(true);
        return VIRTIO_NET_STATUS_MSIX_FAILURE;
    }
    status = configure_queue(&runtime.rx, VIRTIO_NET_RX_QUEUE);
    if (status == VIRTIO_NET_STATUS_OK) {
        status = configure_queue(&runtime.tx, VIRTIO_NET_TX_QUEUE);
    }
    if (status != VIRTIO_NET_STATUS_OK) {
        (void)cleanup(true);
        return status;
    }
    for (uint16_t descriptor = 0U; descriptor < runtime.rx.size;
         ++descriptor) {
        if (!post_rx_descriptor(descriptor)) {
            (void)cleanup(true);
            return VIRTIO_NET_STATUS_QUEUE_FAILURE;
        }
    }
    status = prepare_bus_master();
    if (status != VIRTIO_NET_STATUS_OK) {
        (void)cleanup(true);
        return status;
    }
    runtime.public.rx_queue_size = runtime.rx.size;
    runtime.public.tx_queue_size = runtime.tx.size;
    runtime.public.device_generation = next_device_generation++;
    if (next_device_generation == 0U) {
        next_device_generation = 1U;
    }
    runtime.public.polling_fallback = true;
    runtime.public.active = true;
    mmio_write8(runtime.common.base, VIRTIO_COMMON_DEVICE_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
        VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    notify_queue(&runtime.rx, VIRTIO_NET_RX_QUEUE);
    return runtime.public.link_up ? VIRTIO_NET_STATUS_OK :
        VIRTIO_NET_STATUS_LINK_DOWN;
}

enum virtio_net_status virtio_net_service(void)
{
    enum virtio_net_status status;
    uint8_t device_status;
    uint64_t started;

    if (!runtime.public.active) {
        return VIRTIO_NET_STATUS_NOT_INITIALIZED;
    }
    started = clock_monotonic_ns();
    device_status = mmio_read8(runtime.common.base,
        VIRTIO_COMMON_DEVICE_STATUS);
    if (device_status == 0U ||
        (device_status & (VIRTIO_STATUS_NEEDS_RESET |
            VIRTIO_STATUS_FAILED)) != 0U) {
        return VIRTIO_NET_STATUS_RESET;
    }
    status = read_device_configuration();
    if (status != VIRTIO_NET_STATUS_OK &&
        status != VIRTIO_NET_STATUS_LINK_DOWN) {
        return status;
    }
    if (status == VIRTIO_NET_STATUS_LINK_DOWN) {
        return status;
    }
    ++runtime.public.statistics.polling_passes;
    runtime.deferred_work = false;
    status = service_tx();
    if (status == VIRTIO_NET_STATUS_OK) {
        status = service_rx();
    }
    replenish_rx();
    runtime.public.statistics.polling_processing_ns +=
        clock_monotonic_ns() - started;
    return status;
}

enum virtio_net_status virtio_net_transmit(
    const uint8_t *frame,
    size_t length
)
{
    uint16_t descriptor = UINT16_MAX;
    uint16_t packet = UINT16_MAX;
    uint8_t *buffer;

    if (frame == NULL) {
        return VIRTIO_NET_STATUS_NULL_ARGUMENT;
    }
    if (!runtime.public.active) {
        return VIRTIO_NET_STATUS_NOT_INITIALIZED;
    }
    if (!runtime.public.link_up) {
        return VIRTIO_NET_STATUS_LINK_DOWN;
    }
    if (length < 14U || length > VIRTIO_NET_MAX_FRAME_SIZE) {
        return VIRTIO_NET_STATUS_FRAME_TOO_LARGE;
    }
    (void)virtio_net_service();
    for (uint16_t index = 0U; index < runtime.tx.size; ++index) {
        const uint16_t candidate = (uint16_t)(VIRTIO_NET_RX_RESERVE + index);

        if (!runtime.tx.descriptor_inflight[index] &&
            runtime.packets[candidate].owner == VIRTIO_NET_PACKET_FREE) {
            descriptor = index;
            packet = candidate;
            break;
        }
    }
    if (descriptor == UINT16_MAX) {
        ++runtime.public.statistics.exhausted_frames;
        return VIRTIO_NET_STATUS_TX_EXHAUSTED;
    }
    runtime.packets[packet].owner = VIRTIO_NET_PACKET_PROTOCOL;
    runtime.packets[packet].length = (uint16_t)length;
    runtime.packets[packet].descriptor = descriptor;
    buffer = packet_bytes(packet);
    zero_bytes(buffer, VIRTIO_NET_HEADER_BYTES);
    copy_bytes(buffer + VIRTIO_NET_HEADER_BYTES, frame, length);
    runtime.packets[packet].owner = VIRTIO_NET_PACKET_DEVICE_TX;
    runtime.tx.packet_for_descriptor[descriptor] = packet;
    runtime.tx.descriptor_inflight[descriptor] = true;
    descriptor_write(&runtime.tx, descriptor, packet_physical(packet),
        (uint32_t)(VIRTIO_NET_HEADER_BYTES + length), 0U);
    queue_publish(&runtime.tx, descriptor);
    notify_queue(&runtime.tx, VIRTIO_NET_TX_QUEUE);
    return VIRTIO_NET_STATUS_OK;
}

enum virtio_net_status virtio_net_receive(
    uint8_t *frame,
    size_t capacity,
    size_t *length
)
{
    uint16_t packet;
    struct packet_record *record;

    if (frame == NULL || length == NULL) {
        return VIRTIO_NET_STATUS_NULL_ARGUMENT;
    }
    *length = 0U;
    if (!runtime.public.active) {
        return VIRTIO_NET_STATUS_NOT_INITIALIZED;
    }
    (void)virtio_net_service();
    if (!ready_pop(&packet)) {
        return VIRTIO_NET_STATUS_RX_EMPTY;
    }
    record = &runtime.packets[packet];
    if (record->owner != VIRTIO_NET_PACKET_KERNEL_RX) {
        return VIRTIO_NET_STATUS_OWNERSHIP_FAILURE;
    }
    if (record->length > capacity) {
        record->owner = VIRTIO_NET_PACKET_RELEASED;
        record->owner = VIRTIO_NET_PACKET_FREE;
        ++runtime.public.statistics.dropped_frames;
        replenish_rx();
        return VIRTIO_NET_STATUS_FRAME_TOO_LARGE;
    }
    record->owner = VIRTIO_NET_PACKET_PROTOCOL;
    copy_bytes(frame, packet_bytes(packet) + VIRTIO_NET_HEADER_BYTES,
        record->length);
    *length = record->length;
    record->owner = VIRTIO_NET_PACKET_RELEASED;
    record->length = 0U;
    record->owner = VIRTIO_NET_PACKET_FREE;
    replenish_rx();
    return VIRTIO_NET_STATUS_OK;
}

enum virtio_net_status virtio_net_shutdown(void)
{
    enum virtio_net_status status;

    if (!runtime.public.present && !runtime.claim.active) {
        return VIRTIO_NET_STATUS_NOT_INITIALIZED;
    }
    for (size_t index = 0U; index < VIRTIO_NET_PACKET_COUNT; ++index) {
        if (runtime.packets[index].owner != VIRTIO_NET_PACKET_FREE &&
            runtime.packets[index].owner != VIRTIO_NET_PACKET_RELEASED) {
            ++runtime.public.statistics.reset_frames;
        }
    }
    status = cleanup(true);
    runtime.public.active = false;
    runtime.public.link_up = false;
    return status;
}

enum virtio_net_status virtio_net_reset(void)
{
    if (!runtime.public.active) {
        return VIRTIO_NET_STATUS_NOT_INITIALIZED;
    }
    if (virtio_net_shutdown() != VIRTIO_NET_STATUS_OK) {
        return VIRTIO_NET_STATUS_TEARDOWN_FAILURE;
    }
    return virtio_net_initialize();
}

struct virtio_net_state virtio_net_get_state(void)
{
    return runtime.public;
}

bool virtio_net_self_test(size_t *completed_tests)
{
    uint64_t available;
    uint64_t used;
    uint64_t result;
    size_t completed = 0U;

    if (completed_tests == NULL) {
        return false;
    }
    if (!queue_layout(VIRTIO_NET_QUEUE_LENGTH, PHIPIA_PAGE_SIZE,
            &available, &used) || available != 256U || used != 296U) {
        return false;
    }
    ++completed;
    if (queue_layout(0U, PHIPIA_PAGE_SIZE, &available, &used) ||
        queue_layout(3U, PHIPIA_PAGE_SIZE, &available, &used) ||
        queue_layout(VIRTIO_NET_QUEUE_LENGTH + 1U, PHIPIA_PAGE_SIZE,
            &available, &used)) {
        return false;
    }
    completed += 3U;
    if (!add_checked(1U, 2U, &result) || result != 3U ||
        add_checked(UINT64_MAX, 1U, &result)) {
        return false;
    }
    completed += 2U;
    if (!multiply_checked(16U, 16U, &result) || result != 256U ||
        multiply_checked(UINT64_MAX, 2U, &result)) {
        return false;
    }
    completed += 2U;
    if (!align_up_checked(293U, 4U, &result) || result != 296U ||
        align_up_checked(UINT64_MAX, 4U, &result)) {
        return false;
    }
    completed += 2U;
    if (VIRTIO_NET_PACKET_COUNT != 48U || VIRTIO_NET_RX_RESERVE != 32U ||
        VIRTIO_NET_TX_RESERVE != 16U ||
        VIRTIO_NET_MAX_FRAME_SIZE != 1514U) {
        return false;
    }
    completed += 4U;
    *completed_tests = completed;
    return completed == 14U;
}

const char *virtio_net_status_string(enum virtio_net_status status)
{
    static const char *const messages[VIRTIO_NET_STATUS_COUNT] = {
        "ok", "no supported network device", "null network argument",
        "network device was initialized twice", "network is not initialized",
        "unsupported network device", "network device claim failed",
        "malformed VirtIO capability", "network BAR mapping failed",
        "network device reset failed", "network feature negotiation failed",
        "invalid VirtIO network queue", "network DMA allocation failed",
        "network MSI-X setup failed", "network bus-master setup failed",
        "network link is down", "network frame length is outside bounds",
        "network transmit queue is exhausted", "no received frame is ready",
        "invalid VirtIO completion", "network ownership transition failed",
        "network device generation was reset", "network teardown failed"
    };

    _Static_assert(sizeof(messages) / sizeof(messages[0]) ==
        VIRTIO_NET_STATUS_COUNT, "VirtIO network status messages drifted");
    if (status < VIRTIO_NET_STATUS_OK || status >= VIRTIO_NET_STATUS_COUNT) {
        return "unknown VirtIO network status";
    }
    return messages[status];
}
