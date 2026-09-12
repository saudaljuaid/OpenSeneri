/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_USER_PACKAGE_CONTROL_H
#define PHIPIA_USER_PACKAGE_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#include <phipia/abi.h>

struct phipia_package_control_report {
    uint64_t repository_version;
    uint64_t generation;
    uint32_t plan_count;
    uint32_t attached_count;
    uint32_t result_flags;
};

struct phipia_package_control_item {
    uint32_t index;
    uint32_t identifier_bytes;
    uint32_t version_bytes;
    uint32_t path_bytes;
    uint64_t package_bytes;
    uint8_t package_sha256[PHIPIA_PACKAGE_UPLOAD_SHA256_BYTES];
    char identifier[PHIPIA_PACKAGE_CONTROL_TEXT_BYTES];
    char version[PHIPIA_PACKAGE_CONTROL_TEXT_BYTES];
    char download_path[PHIPIA_PACKAGE_CONTROL_PATH_BYTES];
};

long phipia_package_control_open_install(
    phipia_handle_t repository_upload,
    const char *identifier,
    size_t identifier_bytes,
    struct phipia_package_control_report *report
);

long phipia_package_control_open_remove(
    const char *identifier,
    size_t identifier_bytes,
    struct phipia_package_control_report *report
);

long phipia_package_control_open_repair(
    phipia_handle_t repository_upload,
    struct phipia_package_control_report *report
);

long phipia_package_control_item(
    phipia_handle_t control,
    uint32_t index,
    struct phipia_package_control_item *item
);

long phipia_package_control_attach(
    phipia_handle_t control,
    uint32_t index,
    phipia_handle_t package_upload,
    struct phipia_package_control_report *report
);

long phipia_package_control_commit(
    phipia_handle_t control,
    struct phipia_package_control_report *report
);

long phipia_package_control_close(phipia_handle_t control);

#endif
