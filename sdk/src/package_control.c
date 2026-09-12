/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/package_control.h>

#include <phipia/runtime.h>
#include <string.h>

static void report_clear(struct phipia_package_control_report *report)
{
    if (report != NULL) {
        (void)memset(report, 0, sizeof(*report));
    }
}

static void report_open(struct phipia_package_control_report *report,
    const struct phipia_package_control_open_request *request)
{
    if (report != NULL) {
        report->repository_version = request->repository_version;
        report->generation = request->generation;
        report->plan_count = request->plan_count;
        report->result_flags = request->result_flags;
    }
}

long phipia_package_control_open_install(
    phipia_handle_t repository_upload,
    const char *identifier,
    size_t identifier_bytes,
    struct phipia_package_control_report *report
)
{
    struct phipia_package_control_open_request request;

    report_clear(report);
    if (repository_upload == PHIPIA_HANDLE_INVALID || identifier == NULL ||
        identifier_bytes == 0U ||
        identifier_bytes >= PHIPIA_PACKAGE_CONTROL_TEXT_BYTES ||
        report == NULL) {
        return -PHIPIA_EINVAL;
    }
    (void)memset(&request, 0, sizeof(request));
    request.size = sizeof(request);
    request.version = PHIPIA_ABI_VERSION;
    request.repository_upload = repository_upload;
    request.identifier = (uint64_t)(uintptr_t)identifier;
    request.identifier_bytes = (uint32_t)identifier_bytes;
    long status = phipia_syscall1(PHIPIA_SYS_PACKAGE_CONTROL_OPEN_INSTALL,
        (uint64_t)(uintptr_t)&request);

    report_open(report, &request);
    return status;
}

long phipia_package_control_open_remove(
    const char *identifier,
    size_t identifier_bytes,
    struct phipia_package_control_report *report
)
{
    struct phipia_package_control_open_request request;

    report_clear(report);
    if (identifier == NULL || identifier_bytes == 0U ||
        identifier_bytes >= PHIPIA_PACKAGE_CONTROL_TEXT_BYTES ||
        report == NULL) {
        return -PHIPIA_EINVAL;
    }
    (void)memset(&request, 0, sizeof(request));
    request.size = sizeof(request);
    request.version = PHIPIA_ABI_VERSION;
    request.identifier = (uint64_t)(uintptr_t)identifier;
    request.identifier_bytes = (uint32_t)identifier_bytes;
    request.flags = PHIPIA_PACKAGE_CONTROL_OPEN_REMOVE;
    long status = phipia_syscall1(PHIPIA_SYS_PACKAGE_CONTROL_OPEN_INSTALL,
        (uint64_t)(uintptr_t)&request);

    report_open(report, &request);
    return status;
}

long phipia_package_control_open_repair(
    phipia_handle_t repository_upload,
    struct phipia_package_control_report *report
)
{
    struct phipia_package_control_open_request request;

    report_clear(report);
    if (repository_upload == PHIPIA_HANDLE_INVALID || report == NULL) {
        return -PHIPIA_EINVAL;
    }
    (void)memset(&request, 0, sizeof(request));
    request.size = sizeof(request);
    request.version = PHIPIA_ABI_VERSION;
    request.repository_upload = repository_upload;
    request.flags = PHIPIA_PACKAGE_CONTROL_OPEN_REPAIR;
    long status = phipia_syscall1(PHIPIA_SYS_PACKAGE_CONTROL_OPEN_INSTALL,
        (uint64_t)(uintptr_t)&request);

    report_open(report, &request);
    return status;
}

long phipia_package_control_item(
    phipia_handle_t control,
    uint32_t index,
    struct phipia_package_control_item *item
)
{
    struct phipia_package_control_item_request request;

    if (control == PHIPIA_HANDLE_INVALID || item == NULL ||
        index >= PHIPIA_PACKAGE_CONTROL_PLAN_MAX) {
        return -PHIPIA_EINVAL;
    }
    (void)memset(item, 0, sizeof(*item));
    (void)memset(&request, 0, sizeof(request));
    request.size = sizeof(request);
    request.version = PHIPIA_ABI_VERSION;
    request.control = control;
    request.index = index;
    long status = phipia_syscall1(PHIPIA_SYS_PACKAGE_CONTROL_ITEM,
        (uint64_t)(uintptr_t)&request);

    if (status != 0) {
        return status;
    }
    item->index = index;
    item->identifier_bytes = request.identifier_bytes;
    item->version_bytes = request.version_bytes;
    item->path_bytes = request.path_bytes;
    item->package_bytes = request.package_bytes;
    (void)memcpy(item->package_sha256, request.package_sha256,
        sizeof(item->package_sha256));
    (void)memcpy(item->identifier, request.identifier,
        sizeof(item->identifier));
    (void)memcpy(item->version, request.package_version,
        sizeof(item->version));
    (void)memcpy(item->download_path, request.download_path,
        sizeof(item->download_path));
    return 0;
}

long phipia_package_control_attach(
    phipia_handle_t control,
    uint32_t index,
    phipia_handle_t package_upload,
    struct phipia_package_control_report *report
)
{
    struct phipia_package_control_attach_request request;

    report_clear(report);
    if (control == PHIPIA_HANDLE_INVALID ||
        package_upload == PHIPIA_HANDLE_INVALID ||
        index >= PHIPIA_PACKAGE_CONTROL_PLAN_MAX || report == NULL) {
        return -PHIPIA_EINVAL;
    }
    (void)memset(&request, 0, sizeof(request));
    request.size = sizeof(request);
    request.version = PHIPIA_ABI_VERSION;
    request.control = control;
    request.index = index;
    request.package_upload = package_upload;
    long status = phipia_syscall1(PHIPIA_SYS_PACKAGE_CONTROL_ATTACH,
        (uint64_t)(uintptr_t)&request);

    report->attached_count = request.attached_count;
    report->result_flags = request.result_flags;
    return status;
}

long phipia_package_control_commit(
    phipia_handle_t control,
    struct phipia_package_control_report *report
)
{
    struct phipia_package_control_commit_request request;

    report_clear(report);
    if (control == PHIPIA_HANDLE_INVALID || report == NULL) {
        return -PHIPIA_EINVAL;
    }
    (void)memset(&request, 0, sizeof(request));
    request.size = sizeof(request);
    request.version = PHIPIA_ABI_VERSION;
    request.control = control;
    long status = phipia_syscall1(PHIPIA_SYS_PACKAGE_CONTROL_COMMIT,
        (uint64_t)(uintptr_t)&request);

    report->generation = request.generation;
    report->plan_count = request.plan_count;
    report->attached_count = request.attached_count;
    report->result_flags = request.result_flags;
    return status;
}

long phipia_package_control_close(phipia_handle_t control)
{
    return phipia_handle_close(control);
}
