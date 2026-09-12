/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ABI_STORAGE_H
#define PHIPIA_ABI_STORAGE_H

#include <phipia/abi/base.h>

#define PHIPIA_PATH_MAX 127U
#define PHIPIA_DIRECTORY_NAME_MAX 12U

enum phipia_volume {
    PHIPIA_VOLUME_SYSTEM = 1,
    PHIPIA_VOLUME_DATA = 2
};

enum phipia_open_flags {
    PHIPIA_OPEN_READ = UINT32_C(1) << 0,
    PHIPIA_OPEN_WRITE = UINT32_C(1) << 1,
    PHIPIA_OPEN_CREATE = UINT32_C(1) << 2,
    PHIPIA_OPEN_TRUNCATE = UINT32_C(1) << 3
};

#define PHIPIA_OPEN_FLAGS_V1 (PHIPIA_OPEN_READ | PHIPIA_OPEN_WRITE | \
    PHIPIA_OPEN_CREATE | PHIPIA_OPEN_TRUNCATE)

enum phipia_seek_origin {
    PHIPIA_SEEK_START = 0,
    PHIPIA_SEEK_CURRENT = 1,
    PHIPIA_SEEK_END = 2
};

struct phipia_path {
    uint64_t address;
    uint32_t length;
    uint16_t volume;
    uint16_t reserved;
} __attribute__((packed));

struct phipia_file_open_request {
    uint32_t size;
    uint32_t version;
    struct phipia_path path;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

struct phipia_io_request {
    uint32_t size;
    uint32_t version;
    phipia_handle_t handle;
    uint64_t buffer;
    uint64_t offset;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed));

struct phipia_seek_request {
    uint32_t size;
    uint32_t version;
    phipia_handle_t handle;
    int64_t offset;
    uint32_t origin;
    uint32_t reserved;
} __attribute__((packed));

struct phipia_path_stat {
    uint32_t size;
    uint32_t version;
    uint64_t byte_length;
    uint32_t attributes;
    uint32_t reserved;
} __attribute__((packed));

enum phipia_path_attributes {
    PHIPIA_PATH_DIRECTORY = UINT32_C(1) << 0,
    PHIPIA_PATH_READ_ONLY = UINT32_C(1) << 1
};

struct phipia_directory_entry {
    uint32_t size;
    uint32_t version;
    uint64_t byte_length;
    uint32_t attributes;
    uint16_t name_length;
    uint16_t reserved;
    uint8_t name[PHIPIA_DIRECTORY_NAME_MAX];
    uint32_t reserved_tail;
} __attribute__((packed));

struct phipia_rename_request {
    uint32_t size;
    uint32_t version;
    struct phipia_path source;
    struct phipia_path destination;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

struct phipia_volume_space {
    uint32_t size;
    uint32_t version;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t cluster_bytes;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct phipia_path) == 16U,
    "Phipia path ABI changed");
_Static_assert(sizeof(struct phipia_file_open_request) == 32U,
    "Phipia file-open ABI changed");
_Static_assert(sizeof(struct phipia_io_request) == 40U,
    "Phipia I/O ABI changed");
_Static_assert(sizeof(struct phipia_seek_request) == 32U,
    "Phipia seek ABI changed");
_Static_assert(sizeof(struct phipia_path_stat) == 24U,
    "Phipia stat ABI changed");
_Static_assert(sizeof(struct phipia_directory_entry) == 40U,
    "Phipia directory-entry ABI changed");
_Static_assert(sizeof(struct phipia_rename_request) == 48U,
    "Phipia rename ABI changed");
_Static_assert(sizeof(struct phipia_volume_space) == 32U,
    "Phipia volume-space ABI changed");

#endif
