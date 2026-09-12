/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ABI_NETWORK_H
#define PHIPIA_ABI_NETWORK_H

#include <phipia/abi/base.h>

#define PHIPIA_NETWORK_IO_MAX_BYTES 4096U

struct phipia_ipv4_endpoint {
    uint32_t address;
    uint16_t port;
    uint16_t reserved;
} __attribute__((packed));

struct phipia_network_io {
    uint32_t size;
    uint32_t version;
    phipia_handle_t handle;
    uint64_t buffer;
    uint64_t deadline_ns;
    struct phipia_ipv4_endpoint endpoint;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed));

enum phipia_stream_shutdown {
    PHIPIA_SHUTDOWN_WRITE = UINT32_C(1) << 0,
    PHIPIA_SHUTDOWN_READ = UINT32_C(1) << 1
};

_Static_assert(sizeof(struct phipia_ipv4_endpoint) == 8U,
    "Phipia IPv4 endpoint ABI changed");
_Static_assert(sizeof(struct phipia_network_io) == 48U,
    "Phipia network-I/O ABI changed");

#endif
