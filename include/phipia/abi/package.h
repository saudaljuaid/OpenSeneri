/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ABI_PACKAGE_H
#define PHIPIA_ABI_PACKAGE_H

#include <phipia/abi/base.h>

#define PHIPIA_PACKAGE_UPLOAD_SHA256_BYTES UINT32_C(32)
#define PHIPIA_PACKAGE_UPLOAD_WRITE_MAX UINT32_C(4096)
#define PHIPIA_PACKAGE_UPLOAD_MAX_BYTES UINT32_C(16777216)

#define PHIPIA_PACKAGE_UPLOAD_SEALED UINT32_C(1)
#define PHIPIA_PACKAGE_UPLOAD_DURABLE UINT32_C(2)

#define PHIPIA_PACKAGE_CONTROL_PLAN_MAX UINT32_C(8)
#define PHIPIA_PACKAGE_CONTROL_TEXT_BYTES UINT32_C(128)
#define PHIPIA_PACKAGE_CONTROL_PATH_BYTES UINT32_C(256)

#define PHIPIA_PACKAGE_CONTROL_PREPARED UINT32_C(1)
#define PHIPIA_PACKAGE_CONTROL_COMMITTED UINT32_C(2)

#define PHIPIA_PACKAGE_CONTROL_OPEN_INSTALL UINT32_C(0)
#define PHIPIA_PACKAGE_CONTROL_OPEN_REMOVE UINT32_C(1)
#define PHIPIA_PACKAGE_CONTROL_OPEN_REPAIR UINT32_C(2)

struct phipia_package_upload_write_request {
    uint32_t size;
    uint32_t version;
    phipia_handle_t handle;
    uint64_t buffer;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed));

struct phipia_package_upload_seal_request {
    uint32_t size;
    uint32_t version;
    phipia_handle_t handle;
    uint64_t expected_bytes;
    uint8_t expected_sha256[PHIPIA_PACKAGE_UPLOAD_SHA256_BYTES];
    uint64_t actual_bytes;
    uint8_t actual_sha256[PHIPIA_PACKAGE_UPLOAD_SHA256_BYTES];
    uint32_t result_flags;
    uint32_t reserved;
} __attribute__((packed));

struct phipia_package_control_open_request {
    uint32_t size;
    uint32_t version;
    phipia_handle_t repository_upload;
    uint64_t identifier;
    uint32_t identifier_bytes;
    uint32_t flags;
    uint64_t repository_version;
    uint64_t generation;
    uint32_t plan_count;
    uint32_t result_flags;
} __attribute__((packed));

struct phipia_package_control_item_request {
    uint32_t size;
    uint32_t version;
    phipia_handle_t control;
    uint32_t index;
    uint32_t flags;
    uint64_t package_bytes;
    uint8_t package_sha256[PHIPIA_PACKAGE_UPLOAD_SHA256_BYTES];
    uint32_t identifier_bytes;
    uint32_t version_bytes;
    uint32_t path_bytes;
    uint32_t reserved;
    char identifier[PHIPIA_PACKAGE_CONTROL_TEXT_BYTES];
    char package_version[PHIPIA_PACKAGE_CONTROL_TEXT_BYTES];
    char download_path[PHIPIA_PACKAGE_CONTROL_PATH_BYTES];
} __attribute__((packed));

struct phipia_package_control_attach_request {
    uint32_t size;
    uint32_t version;
    phipia_handle_t control;
    uint32_t index;
    uint32_t flags;
    phipia_handle_t package_upload;
    uint32_t attached_count;
    uint32_t result_flags;
} __attribute__((packed));

struct phipia_package_control_commit_request {
    uint32_t size;
    uint32_t version;
    phipia_handle_t control;
    uint32_t flags;
    uint32_t reserved;
    uint64_t generation;
    uint32_t plan_count;
    uint32_t attached_count;
    uint32_t result_flags;
    uint32_t result_reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct phipia_package_upload_write_request) == 32U,
    "Phipia package-upload write ABI changed");
_Static_assert(sizeof(struct phipia_package_upload_seal_request) == 104U,
    "Phipia package-upload seal ABI changed");
_Static_assert(sizeof(struct phipia_package_control_open_request) == 56U,
    "Phipia package-control open ABI changed");
_Static_assert(sizeof(struct phipia_package_control_item_request) == 592U,
    "Phipia package-control item ABI changed");
_Static_assert(sizeof(struct phipia_package_control_attach_request) == 40U,
    "Phipia package-control attach ABI changed");
_Static_assert(sizeof(struct phipia_package_control_commit_request) == 48U,
    "Phipia package-control commit ABI changed");

#endif
