/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_MSIX_H
#define PHIPIA_MSIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/interrupt_vector.h>
#include <phipia/interrupts.h>
#include <phipia/pci_resource.h>

enum msix_status {
    MSIX_STATUS_OK = 0,
    MSIX_STATUS_NULL_ARGUMENT,
    MSIX_STATUS_INTERRUPTS_ENABLED,
    MSIX_STATUS_BAD_CLAIM,
    MSIX_STATUS_CAPABILITY_MISSING,
    MSIX_STATUS_CAPABILITY_MALFORMED,
    MSIX_STATUS_ALREADY_ENABLED,
    MSIX_STATUS_BAD_TABLE_SIZE,
    MSIX_STATUS_BAD_ENTRY_INDEX,
    MSIX_STATUS_BAD_BIR,
    MSIX_STATUS_TABLE_OUTSIDE_BAR,
    MSIX_STATUS_PBA_OUTSIDE_BAR,
    MSIX_STATUS_MAPPING_FAILURE,
    MSIX_STATUS_VECTOR_FAILURE,
    MSIX_STATUS_HANDLER_FAILURE,
    MSIX_STATUS_HANDLER_NOT_INSTALLED,
    MSIX_STATUS_PROGRAMMING_FAILURE,
    MSIX_STATUS_ALREADY_BOUND,
    MSIX_STATUS_NOT_BOUND,
    MSIX_STATUS_ROLLBACK_FAILURE,
    MSIX_STATUS_INJECTED_FAILURE,
    MSIX_STATUS_COUNT
};

struct msix_binding {
    struct pci_device_claim *claim;
    struct interrupt_vector_allocation vector;
    volatile uint32_t *entry;
    uint32_t original_entry[4];
    uint16_t original_control;
    uint16_t table_size;
    uint16_t entry_index;
    uint8_t capability_offset;
    uint8_t table_bar;
    uint8_t pba_bar;
    bool handler_installed;
    bool table_mapped_here;
    bool pba_mapped_here;
    bool delivery_masked;
    bool active;
};

struct msix_state {
    size_t active_bindings;
    size_t rollback_count;
    bool failure_injection_armed;
};

enum msix_status msix_bind(
    struct pci_device_claim *claim,
    uint16_t entry_index,
    interrupt_handler_t handler,
    void *context,
    struct msix_binding *binding
);
enum msix_status msix_bind_masked(
    struct pci_device_claim *claim,
    uint16_t entry_index,
    interrupt_handler_t handler,
    void *context,
    struct msix_binding *binding
);
enum msix_status msix_set_masked(
    struct msix_binding *binding,
    bool masked
);
enum msix_status msix_unbind(struct msix_binding *binding);
void msix_test_inject_failure_once(void);
struct msix_state msix_get_state(void);
bool msix_self_test(void);
const char *msix_status_string(enum msix_status status);

#endif
