/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_VIRTIO_NET_H
#define PHIPIA_VIRTIO_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIRTIO_NET_MAX_NICS 1U
#define VIRTIO_NET_QUEUE_LENGTH 16U
#define VIRTIO_NET_RX_RESERVE 32U
#define VIRTIO_NET_TX_RESERVE 16U
#define VIRTIO_NET_PACKET_COUNT \
    (VIRTIO_NET_RX_RESERVE + VIRTIO_NET_TX_RESERVE)
#define VIRTIO_NET_MAX_FRAME_SIZE 1514U
#define VIRTIO_NET_PACKET_BYTES 2048U

enum virtio_net_packet_owner {
    VIRTIO_NET_PACKET_FREE = 0,
    VIRTIO_NET_PACKET_DEVICE_RX,
    VIRTIO_NET_PACKET_KERNEL_RX,
    VIRTIO_NET_PACKET_PROTOCOL,
    VIRTIO_NET_PACKET_SOCKET,
    VIRTIO_NET_PACKET_DEVICE_TX,
    VIRTIO_NET_PACKET_TX_COMPLETE,
    VIRTIO_NET_PACKET_RELEASED,
    VIRTIO_NET_PACKET_OWNER_COUNT
};

enum virtio_net_status {
    VIRTIO_NET_STATUS_OK = 0,
    VIRTIO_NET_STATUS_ABSENT,
    VIRTIO_NET_STATUS_NULL_ARGUMENT,
    VIRTIO_NET_STATUS_ALREADY_INITIALIZED,
    VIRTIO_NET_STATUS_NOT_INITIALIZED,
    VIRTIO_NET_STATUS_UNSUPPORTED_DEVICE,
    VIRTIO_NET_STATUS_CLAIM_FAILURE,
    VIRTIO_NET_STATUS_CAPABILITY_FAILURE,
    VIRTIO_NET_STATUS_MAPPING_FAILURE,
    VIRTIO_NET_STATUS_RESET_FAILURE,
    VIRTIO_NET_STATUS_FEATURE_FAILURE,
    VIRTIO_NET_STATUS_QUEUE_FAILURE,
    VIRTIO_NET_STATUS_DMA_FAILURE,
    VIRTIO_NET_STATUS_MSIX_FAILURE,
    VIRTIO_NET_STATUS_BUS_MASTER_FAILURE,
    VIRTIO_NET_STATUS_LINK_DOWN,
    VIRTIO_NET_STATUS_FRAME_TOO_LARGE,
    VIRTIO_NET_STATUS_TX_EXHAUSTED,
    VIRTIO_NET_STATUS_RX_EMPTY,
    VIRTIO_NET_STATUS_BAD_COMPLETION,
    VIRTIO_NET_STATUS_OWNERSHIP_FAILURE,
    VIRTIO_NET_STATUS_RESET,
    VIRTIO_NET_STATUS_TEARDOWN_FAILURE,
    VIRTIO_NET_STATUS_COUNT
};

struct virtio_net_statistics {
    uint64_t rx_frames;
    uint64_t tx_frames;
    uint64_t dropped_frames;
    uint64_t malformed_frames;
    uint64_t exhausted_frames;
    uint64_t reset_frames;
    uint64_t interrupts;
    uint64_t polling_passes;
    uint64_t interrupt_processing_ns;
    uint64_t polling_processing_ns;
};

struct virtio_net_state {
    struct virtio_net_statistics statistics;
    uint64_t device_generation;
    uint64_t negotiated_features;
    uint8_t mac[6];
    uint16_t rx_queue_size;
    uint16_t tx_queue_size;
    bool present;
    bool active;
    bool link_up;
    bool polling_fallback;
};

enum virtio_net_status virtio_net_initialize(void);
enum virtio_net_status virtio_net_shutdown(void);
enum virtio_net_status virtio_net_reset(void);
enum virtio_net_status virtio_net_service(void);
enum virtio_net_status virtio_net_transmit(
    const uint8_t *frame,
    size_t length
);
enum virtio_net_status virtio_net_receive(
    uint8_t *frame,
    size_t capacity,
    size_t *length
);
struct virtio_net_state virtio_net_get_state(void);
bool virtio_net_self_test(size_t *completed_tests);
const char *virtio_net_status_string(enum virtio_net_status status);

#endif
