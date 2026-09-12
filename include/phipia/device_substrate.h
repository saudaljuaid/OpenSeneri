/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_DEVICE_SUBSTRATE_H
#define PHIPIA_DEVICE_SUBSTRATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DEVICE_SUBSTRATE_DMA_BYTES UINT32_C(64)

enum device_substrate_status {
    DEVICE_SUBSTRATE_STATUS_OK = 0,
    DEVICE_SUBSTRATE_STATUS_ABSENT,
    DEVICE_SUBSTRATE_STATUS_CLAIM_FAILURE,
    DEVICE_SUBSTRATE_STATUS_CAPABILITY_FAILURE,
    DEVICE_SUBSTRATE_STATUS_MAPPING_FAILURE,
    DEVICE_SUBSTRATE_STATUS_DEVICE_RESET_FAILURE,
    DEVICE_SUBSTRATE_STATUS_FEATURE_NEGOTIATION_FAILURE,
    DEVICE_SUBSTRATE_STATUS_QUEUE_FAILURE,
    DEVICE_SUBSTRATE_STATUS_DMA_FAILURE,
    DEVICE_SUBSTRATE_STATUS_MSIX_NEGATIVE_CONTROL_FAILURE,
    DEVICE_SUBSTRATE_STATUS_MSIX_FAILURE,
    DEVICE_SUBSTRATE_STATUS_BUS_MASTER_GUARD_FAILURE,
    DEVICE_SUBSTRATE_STATUS_BUS_MASTER_FAILURE,
    DEVICE_SUBSTRATE_STATUS_INTERRUPT_TIMEOUT,
    DEVICE_SUBSTRATE_STATUS_WRONG_INTERRUPT,
    DEVICE_SUBSTRATE_STATUS_USED_RING_FAILURE,
    DEVICE_SUBSTRATE_STATUS_DEVICE_DMA_FAILURE,
    DEVICE_SUBSTRATE_STATUS_OWNERSHIP_FAILURE,
    DEVICE_SUBSTRATE_STATUS_TEARDOWN_FAILURE,
    DEVICE_SUBSTRATE_STATUS_COUNT
};

struct device_substrate_proof {
    uint16_t queue_size;
    uint16_t used_before;
    uint16_t used_after;
    uint32_t used_length;
    uint64_t interrupt_count;
    size_t random_bytes;
    size_t nonzero_bytes;
    size_t negative_controls;
    bool dma_device_written;
    bool msix_delivered;
    bool ownership_round_trip;
    bool teardown_complete;
};

enum device_substrate_status device_substrate_prove(
    struct device_substrate_proof *proof
);
struct device_substrate_proof device_substrate_get_proof(void);
const char *device_substrate_status_string(
    enum device_substrate_status status
);

#endif
