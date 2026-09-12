/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/package_upload.h>

#include <phipia/runtime.h>
#include <string.h>

long phipia_package_upload_open(void)
{
    return phipia_syscall0(PHIPIA_SYS_PACKAGE_UPLOAD_OPEN);
}

long phipia_package_upload_write(
    phipia_handle_t upload,
    const void *bytes,
    size_t byte_count
)
{
    const struct phipia_package_upload_write_request request = {
        sizeof(request), PHIPIA_ABI_VERSION, upload,
        (uint64_t)(uintptr_t)bytes, (uint32_t)byte_count, 0U
    };

    if ((bytes == NULL && byte_count != 0U) ||
        byte_count > PHIPIA_PACKAGE_UPLOAD_WRITE_MAX) {
        return -PHIPIA_EINVAL;
    }
    return phipia_syscall1(PHIPIA_SYS_PACKAGE_UPLOAD_WRITE,
        (uint64_t)(uintptr_t)&request);
}

long phipia_package_upload_seal(
    phipia_handle_t upload,
    uint64_t expected_bytes,
    const uint8_t expected_sha256[PHIPIA_PACKAGE_UPLOAD_SHA256_BYTES],
    struct phipia_package_upload_report *report
)
{
    struct phipia_package_upload_seal_request request;

    if (expected_sha256 == NULL || report == NULL || expected_bytes == 0U ||
        expected_bytes > PHIPIA_PACKAGE_UPLOAD_MAX_BYTES) {
        return -PHIPIA_EINVAL;
    }
    (void)memset(&request, 0, sizeof(request));
    request.size = sizeof(request);
    request.version = PHIPIA_ABI_VERSION;
    request.handle = upload;
    request.expected_bytes = expected_bytes;
    (void)memcpy(request.expected_sha256, expected_sha256,
        sizeof(request.expected_sha256));
    long status = phipia_syscall1(PHIPIA_SYS_PACKAGE_UPLOAD_SEAL,
        (uint64_t)(uintptr_t)&request);

    report->actual_bytes = request.actual_bytes;
    (void)memcpy(report->actual_sha256, request.actual_sha256,
        sizeof(report->actual_sha256));
    report->result_flags = request.result_flags;
    return status;
}

long phipia_package_upload_close(phipia_handle_t upload)
{
    return phipia_handle_close(upload);
}
