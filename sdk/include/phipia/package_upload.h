/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_USER_PACKAGE_UPLOAD_H
#define PHIPIA_USER_PACKAGE_UPLOAD_H

#include <stddef.h>
#include <stdint.h>

#include <phipia/abi.h>

struct phipia_package_upload_report {
    uint64_t actual_bytes;
    uint8_t actual_sha256[PHIPIA_PACKAGE_UPLOAD_SHA256_BYTES];
    uint32_t result_flags;
};

long phipia_package_upload_open(void);
long phipia_package_upload_write(
    phipia_handle_t upload,
    const void *bytes,
    size_t byte_count
);
long phipia_package_upload_seal(
    phipia_handle_t upload,
    uint64_t expected_bytes,
    const uint8_t expected_sha256[PHIPIA_PACKAGE_UPLOAD_SHA256_BYTES],
    struct phipia_package_upload_report *report
);
long phipia_package_upload_close(phipia_handle_t upload);

#endif
