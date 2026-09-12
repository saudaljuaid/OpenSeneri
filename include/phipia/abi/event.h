/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ABI_EVENT_H
#define PHIPIA_ABI_EVENT_H

#include <phipia/abi/base.h>

#define PHIPIA_WAIT_MAX 8U

enum phipia_wait_interest {
    PHIPIA_WAIT_READABLE = UINT32_C(1) << 0,
    PHIPIA_WAIT_WRITABLE = UINT32_C(1) << 1,
    PHIPIA_WAIT_ACCEPTABLE = UINT32_C(1) << 2,
    PHIPIA_WAIT_SIGNALED = UINT32_C(1) << 3,
    PHIPIA_WAIT_CLOSED = UINT32_C(1) << 4
};

#define PHIPIA_WAIT_INTERESTS_V1 ((UINT32_C(1) << 5) - UINT32_C(1))

struct phipia_wait_item {
    phipia_handle_t handle;
    uint32_t interests;
    uint32_t ready;
} __attribute__((packed));

struct phipia_wait_request {
    uint32_t size;
    uint32_t version;
    uint64_t items;
    uint64_t deadline_ns;
    uint32_t count;
    uint32_t flags;
} __attribute__((packed));

struct phipia_timer_set_request {
    uint32_t size;
    uint32_t version;
    phipia_handle_t handle;
    uint64_t deadline_ns;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct phipia_wait_item) == 16U,
    "Phipia wait-item ABI changed");
_Static_assert(sizeof(struct phipia_wait_request) == 32U,
    "Phipia wait-request ABI changed");
_Static_assert(sizeof(struct phipia_timer_set_request) == 32U,
    "Phipia timer-set ABI changed");

#endif
