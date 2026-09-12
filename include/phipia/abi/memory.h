/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ABI_MEMORY_H
#define PHIPIA_ABI_MEMORY_H

#include <phipia/abi/base.h>

enum phipia_memory_flags {
    PHIPIA_MEMORY_READ = UINT32_C(1) << 0,
    PHIPIA_MEMORY_WRITE = UINT32_C(1) << 1,
    PHIPIA_MEMORY_GUARD_BEFORE = UINT32_C(1) << 2,
    PHIPIA_MEMORY_GUARD_AFTER = UINT32_C(1) << 3
};

#define PHIPIA_MEMORY_FLAGS_V1 (PHIPIA_MEMORY_READ | PHIPIA_MEMORY_WRITE | \
    PHIPIA_MEMORY_GUARD_BEFORE | PHIPIA_MEMORY_GUARD_AFTER)

struct phipia_memory_map_request {
    uint32_t size;
    uint32_t version;
    uint64_t length;
    uint64_t address_hint;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

struct phipia_memory_map_response {
    uint32_t size;
    uint32_t version;
    uint64_t address;
    uint64_t length;
} __attribute__((packed));

_Static_assert(sizeof(struct phipia_memory_map_request) == 32U,
    "Phipia memory-map request ABI changed");
_Static_assert(sizeof(struct phipia_memory_map_response) == 24U,
    "Phipia memory-map response ABI changed");

#endif
