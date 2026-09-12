/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_XHCI_H
#define PHIPIA_XHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/dma.h>
#include <phipia/msix.h>
#include <phipia/pci_resource.h>

#define XHCI_PCI_CLASS_SERIAL_BUS UINT8_C(0x0C)
#define XHCI_PCI_SUBCLASS_USB UINT8_C(0x03)
#define XHCI_PCI_PROGRAMMING_INTERFACE UINT8_C(0x30)

#define XHCI_DEVICE_DESCRIPTOR_BYTES 18U
#define XHCI_CONTROLLED_ROBUSTNESS_TESTS 19U
#define XHCI_FOUNDATION_ROBUSTNESS_TESTS 17U
#define XHCI_MAX_SCRATCHPADS 32U
#define XHCI_RING_TRB_COUNT 64U

enum xhci_controller_state {
    XHCI_CONTROLLER_UNINITIALIZED = 0,
    XHCI_CONTROLLER_DISCOVERED,
    XHCI_CONTROLLER_CLAIMED,
    XHCI_CONTROLLER_PREPARED,
    XHCI_CONTROLLER_RUNNING,
    XHCI_CONTROLLER_STOPPING,
    XHCI_CONTROLLER_RELEASED,
    XHCI_CONTROLLER_STATE_COUNT
};

enum xhci_dma_object_state {
    XHCI_DMA_OBJECT_EMPTY = 0,
    XHCI_DMA_OBJECT_CPU_OWNED,
    XHCI_DMA_OBJECT_CONTROLLER_OWNED,
    XHCI_DMA_OBJECT_RECLAIMED,
    XHCI_DMA_OBJECT_STATE_COUNT
};

enum xhci_ring_kind {
    XHCI_RING_COMMAND = 0,
    XHCI_RING_EVENT,
    XHCI_RING_CONTROL,
    XHCI_RING_KIND_COUNT
};

enum xhci_status {
    XHCI_STATUS_OK = 0,
    XHCI_STATUS_ABSENT,
    XHCI_STATUS_NULL_ARGUMENT,
    XHCI_STATUS_MULTIPLE_CONTROLLERS,
    XHCI_STATUS_BAD_CLASS_TUPLE,
    XHCI_STATUS_CLAIM_FAILURE,
    XHCI_STATUS_MAPPING_FAILURE,
    XHCI_STATUS_REGISTER_OUTSIDE_BAR,
    XHCI_STATUS_REGISTER_OVERFLOW,
    XHCI_STATUS_REGISTER_ALIGNMENT,
    XHCI_STATUS_REGISTER_OVERLAP,
    XHCI_STATUS_EXTENDED_CAPABILITY_RANGE,
    XHCI_STATUS_EXTENDED_CAPABILITY_LOOP,
    XHCI_STATUS_EXTENDED_CAPABILITY_NON_PROGRESS,
    XHCI_STATUS_LEGACY_OWNERSHIP_TIMEOUT,
    XHCI_STATUS_CONTROLLER_NOT_READY_TIMEOUT,
    XHCI_STATUS_HOST_CONTROLLER_ERROR,
    XHCI_STATUS_HALT_TIMEOUT,
    XHCI_STATUS_RESET_TIMEOUT,
    XHCI_STATUS_UNSUPPORTED_VERSION,
    XHCI_STATUS_UNSUPPORTED_SLOTS,
    XHCI_STATUS_UNSUPPORTED_PORTS,
    XHCI_STATUS_UNSUPPORTED_INTERRUPTERS,
    XHCI_STATUS_UNSUPPORTED_PAGE_SIZE,
    XHCI_STATUS_UNSUPPORTED_CONTEXT_SIZE,
    XHCI_STATUS_UNSUPPORTED_ADDRESS_WIDTH,
    XHCI_STATUS_SCRATCHPAD_OVERFLOW,
    XHCI_STATUS_DMA_ALLOCATION_FAILURE,
    XHCI_STATUS_DMA_LAYOUT_FAILURE,
    XHCI_STATUS_DMA_OWNERSHIP,
    XHCI_STATUS_RING_ALIGNMENT,
    XHCI_STATUS_RING_CONTAINMENT,
    XHCI_STATUS_RING_RESERVED_FIELD,
    XHCI_STATUS_RING_TYPE,
    XHCI_STATUS_RING_CYCLE,
    XHCI_STATUS_RING_OWNERSHIP,
    XHCI_STATUS_RING_POINTER,
    XHCI_STATUS_RING_FULL,
    XHCI_STATUS_ERST_INVALID,
    XHCI_STATUS_CONTEXT_INVALID,
    XHCI_STATUS_NO_CONNECTED_PORT,
    XHCI_STATUS_MULTIPLE_CONNECTED_PORTS,
    XHCI_STATUS_PORT_UNSUPPORTED,
    XHCI_STATUS_PORT_RESET_TIMEOUT,
    XHCI_STATUS_PORT_NOT_ENABLED,
    XHCI_STATUS_INTERRUPT_NOT_READY,
    XHCI_STATUS_MSIX_FAILURE,
    XHCI_STATUS_MSIX_ROLLBACK_FAILURE,
    XHCI_STATUS_BUS_MASTER_PREMATURE,
    XHCI_STATUS_BUS_MASTER_FAILURE,
    XHCI_STATUS_COMMAND_PRECONDITION,
    XHCI_STATUS_COMMAND_TIMEOUT,
    XHCI_STATUS_COMMAND_EVENT_MISMATCH,
    XHCI_STATUS_COMMAND_COMPLETION,
    XHCI_STATUS_ADDRESS_DEVICE_PRECONDITION,
    XHCI_STATUS_DOORBELL_PREMATURE,
    XHCI_STATUS_TRANSFER_TIMEOUT,
    XHCI_STATUS_TRANSFER_EVENT_MISMATCH,
    XHCI_STATUS_TRANSFER_COMPLETION,
    XHCI_STATUS_TRANSFER_LENGTH,
    XHCI_STATUS_DESCRIPTOR_SHORT,
    XHCI_STATUS_DESCRIPTOR_OVERSIZED,
    XHCI_STATUS_DESCRIPTOR_TYPE,
    XHCI_STATUS_DESCRIPTOR_INCONSISTENT,
    XHCI_STATUS_SENTINEL_FAILURE,
    XHCI_STATUS_INTERRUPT_COUNT,
    XHCI_STATUS_TRANSITION_REPEATED,
    XHCI_STATUS_TRANSITION_REVERSED,
    XHCI_STATUS_TRANSITION_INVALID,
    XHCI_STATUS_TEARDOWN_RACE,
    XHCI_STATUS_TEARDOWN_FAILURE,
    XHCI_STATUS_COUNT
};

struct xhci_controller_claim {
    struct pci_device_claim pci;
    struct pci_address address;
    uint64_t generation;
    enum xhci_controller_state state;
};

struct xhci_register_span {
    uint64_t offset;
    uint64_t length;
    volatile uint8_t *base;
    bool valid;
};

struct xhci_register_regions {
    struct pci_mmio_region *mapping;
    uint64_t bar_size;
    struct xhci_register_span capability;
    struct xhci_register_span operational;
    struct xhci_register_span doorbells;
    struct xhci_register_span runtime;
    struct xhci_register_span interrupter;
    size_t extended_capability_count;
    bool valid;
};

struct xhci_dma_ring {
    struct dma_allocation dma;
    enum xhci_dma_object_state state;
    enum xhci_ring_kind kind;
    size_t trb_count;
    size_t enqueue_index;
    size_t dequeue_index;
    uint8_t producer_cycle;
    uint8_t consumer_cycle;
    bool active;
};

struct xhci_erst {
    struct dma_allocation *backing;
    uint64_t offset;
    size_t segment_count;
    bool active;
};

struct xhci_dcbaa {
    struct dma_allocation *backing;
    uint64_t offset;
    size_t entry_count;
    bool active;
};

struct xhci_scratchpads {
    struct dma_allocation dma;
    struct dma_allocation *array_backing;
    uint64_t array_offset;
    size_t count;
    enum xhci_dma_object_state state;
    bool active;
};

struct xhci_device_contexts {
    struct dma_allocation input;
    struct dma_allocation output;
    size_t context_bytes;
    enum xhci_dma_object_state input_state;
    enum xhci_dma_object_state output_state;
    bool active;
};

struct xhci_root_port {
    uint8_t identifier;
    uint8_t speed;
    uint8_t protocol_major;
    uint8_t slot_type;
    bool connected;
    bool enabled;
    bool reset_complete;
};

struct xhci_slot {
    uint8_t identifier;
    enum xhci_dma_object_state context_state;
    bool enabled;
    bool addressed;
};

struct xhci_endpoint_zero_transfer {
    struct xhci_dma_ring ring;
    struct dma_allocation receive;
    enum xhci_dma_object_state receive_state;
    uint64_t status_trb_physical;
    size_t requested_length;
    size_t returned_length;
    bool submitted;
    bool complete;
};

struct xhci_interrupt_binding {
    struct msix_binding msix;
    uint8_t vector;
    uint64_t count_before_transfer;
    uint64_t count_after_transfer;
    bool handler_ready;
    bool event_ring_ready;
    bool active;
};

struct xhci_descriptor_proof {
    uint16_t controller_version;
    size_t descriptor_bytes;
    uint64_t msix_completion_count;
    size_t ignored_events;
    size_t robustness_tests;
    uint8_t root_port;
    uint8_t slot;
    uint8_t vector;
    uint8_t trb_type;
    bool controller_ready;
    bool descriptor_valid;
    bool sentinel_changed_while_controller_owned;
    bool ownership_complete;
    bool teardown_complete;
};

/* Run only synthetic guest-local foundation controls. A non-null output is
 * reset to zero first and receives the full completed count only on success. */
bool xhci_foundation_self_test(size_t *completed_tests);

/* Run through the installed Boot Ledger proof stage after paging, allocation,
 * PCI-resource, interrupt/MSI-X, deadline, scheduler, and DMA foundations.
 * The function owns its controller resources and fully tears them down; the
 * caller owns the proof snapshot passed here. */
enum xhci_status xhci_descriptor_prove(
    struct xhci_descriptor_proof *proof
);

/* Return the last successful proof by value; an all-zero snapshot means no
 * successful installed proof has been published. */
struct xhci_descriptor_proof xhci_get_descriptor_proof(void);

/* Return static storage for a known status; callers must not modify or free it. */
const char *xhci_status_string(enum xhci_status status);

#endif
