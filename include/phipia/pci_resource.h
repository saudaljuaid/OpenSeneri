/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PCI_RESOURCE_H
#define PHIPIA_PCI_RESOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/dma.h>
#include <phipia/pci.h>

#define PCI_BAR_COUNT 6U
#define PCI_CLAIM_MAPPING_CAPACITY PCI_BAR_COUNT
#define PCI_ACTIVE_CLAIM_CAPACITY 16U
#define PCI_BUS_MASTER_DMA_CAPACITY 8U

#define PCI_DEVICE_MMIO_ARENA_BASE UINT64_C(0x0000000C00000000)
#define PCI_DEVICE_MMIO_ARENA_SIZE (UINT64_C(64) * 1024U * 1024U)

#define PCI_COMMAND_IO_SPACE UINT16_C(0x0001)
#define PCI_COMMAND_MEMORY_SPACE UINT16_C(0x0002)
#define PCI_COMMAND_BUS_MASTER UINT16_C(0x0004)

enum pci_bar_kind {
    PCI_BAR_UNIMPLEMENTED = 0,
    PCI_BAR_IO,
    PCI_BAR_MEMORY_32,
    PCI_BAR_MEMORY_64,
    PCI_BAR_KIND_COUNT
};

enum pci_resource_status {
    PCI_RESOURCE_STATUS_OK = 0,
    PCI_RESOURCE_STATUS_NULL_ARGUMENT,
    PCI_RESOURCE_STATUS_ALREADY_INITIALIZED,
    PCI_RESOURCE_STATUS_NOT_INITIALIZED,
    PCI_RESOURCE_STATUS_INTERRUPTS_ENABLED,
    PCI_RESOURCE_STATUS_FUNCTION_NOT_ENUMERATED,
    PCI_RESOURCE_STATUS_UNSUPPORTED_HEADER,
    PCI_RESOURCE_STATUS_ALREADY_CLAIMED,
    PCI_RESOURCE_STATUS_CLAIM_TABLE_FULL,
    PCI_RESOURCE_STATUS_CONFIG_ACCESS,
    PCI_RESOURCE_STATUS_DECODE_ENABLED,
    PCI_RESOURCE_STATUS_RESTORE_FAILURE,
    PCI_RESOURCE_STATUS_INJECTED_FAILURE,
    PCI_RESOURCE_STATUS_MALFORMED_64_BIT_PAIR,
    PCI_RESOURCE_STATUS_RESERVED_MEMORY_TYPE,
    PCI_RESOURCE_STATUS_ZERO_BAR_SIZE,
    PCI_RESOURCE_STATUS_NON_POWER_OF_TWO_BAR,
    PCI_RESOURCE_STATUS_UNASSIGNED_BAR,
    PCI_RESOURCE_STATUS_MISALIGNED_BAR,
    PCI_RESOURCE_STATUS_BAR_RANGE_OVERFLOW,
    PCI_RESOURCE_STATUS_OVERLAPPING_BAR,
    PCI_RESOURCE_STATUS_BAD_BAR_INDEX,
    PCI_RESOURCE_STATUS_IO_BAR_NOT_MAPPABLE,
    PCI_RESOURCE_STATUS_BAR_ALREADY_MAPPED,
    PCI_RESOURCE_STATUS_MAPPING_ORDER,
    PCI_RESOURCE_STATUS_MAPPING_CAPACITY,
    PCI_RESOURCE_STATUS_MMIO_RANGE_OVERFLOW,
    PCI_RESOURCE_STATUS_MMIO_RANGE_OVERLAP,
    PCI_RESOURCE_STATUS_MMIO_RAM_OVERLAP,
    PCI_RESOURCE_STATUS_MMIO_ARENA_EXHAUSTED,
    PCI_RESOURCE_STATUS_PAGING_FAILURE,
    PCI_RESOURCE_STATUS_BAD_MMIO_SUBREGION,
    PCI_RESOURCE_STATUS_DMA_NOT_PREPARED,
    PCI_RESOURCE_STATUS_BUS_MASTER_ALREADY_ENABLED,
    PCI_RESOURCE_STATUS_BUS_MASTER_DISABLED,
    PCI_RESOURCE_STATUS_CLAIM_INCONSISTENT,
    PCI_RESOURCE_STATUS_COUNT
};

struct pci_bar_description {
    struct pci_address device;
    uint8_t index;
    enum pci_bar_kind kind;
    uint8_t attribute_bits;
    uint8_t pair_index;
    uint64_t base;
    uint64_t size;
    bool prefetchable;
    bool implemented;
};

struct pci_mmio_region {
    struct pci_address device;
    uint8_t bar_index;
    uint64_t physical_base;
    uint64_t size;
    uint64_t mapping_physical_base;
    uint64_t mapping_virtual_base;
    uint64_t mapping_length;
    uint64_t virtual_base;
    bool active;
};

struct pci_device_claim {
    struct pci_address device;
    uint16_t original_command;
    uint16_t current_command;
    uint64_t identifier;
    size_t bar_count;
    size_t mapping_count;
    struct pci_bar_description bars[PCI_BAR_COUNT];
    struct pci_mmio_region mappings[PCI_CLAIM_MAPPING_CAPACITY];
    bool memory_decode_enabled;
    bool bus_master_enabled;
    bool active;
};

struct pci_bus_master_request {
    const struct dma_allocation *allocations[PCI_BUS_MASTER_DMA_CAPACITY];
    size_t allocation_count;
};

struct pci_resource_state {
    size_t active_claims;
    size_t active_mappings;
    size_t arena_pages;
    size_t mapped_pages;
    size_t bus_masters;
    bool active;
};

enum pci_resource_status pci_resource_initialize(void);
enum pci_resource_status pci_claim_device(
    const struct pci_function *function,
    struct pci_device_claim *claim
);
enum pci_resource_status pci_claim_map_bar(
    struct pci_device_claim *claim,
    uint8_t bar_index,
    struct pci_mmio_region **region
);
enum pci_resource_status pci_mmio_subregion(
    const struct pci_mmio_region *region,
    uint64_t offset,
    uint64_t length,
    volatile void **pointer
);
enum pci_resource_status pci_claim_unmap_last_bar(
    struct pci_device_claim *claim,
    uint8_t bar_index
);
const struct pci_bar_description *pci_claim_bar(
    const struct pci_device_claim *claim,
    uint8_t bar_index
);
struct pci_mmio_region *pci_claim_mapped_bar(
    struct pci_device_claim *claim,
    uint8_t bar_index
);
enum pci_resource_status pci_claim_enable_bus_master(
    struct pci_device_claim *claim,
    const struct pci_bus_master_request *request
);
enum pci_resource_status pci_claim_disable_bus_master(
    struct pci_device_claim *claim
);
enum pci_resource_status pci_release_device(struct pci_device_claim *claim);
struct pci_resource_state pci_resource_get_state(void);
enum pci_resource_status pci_resource_verify(void);
bool pci_resource_self_test(const struct pci_function *probe_function);
const char *pci_resource_status_string(enum pci_resource_status status);

#endif
