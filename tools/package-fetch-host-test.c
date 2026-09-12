/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/package_fetch.h>

#include <phipia/abi.h>
#include <phipia/runtime.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, code) do { if (!(condition)) return (code); } while (0)

static const uint8_t body[] = "hello from the Phipia HTTPS peer\n";
static const uint8_t body_sha256[32] = {
    0x52U, 0xe0U, 0xf1U, 0x82U, 0x26U, 0xbeU, 0xf9U, 0xf5U,
    0x15U, 0x18U, 0x5eU, 0x04U, 0x3aU, 0x94U, 0x62U, 0x36U,
    0x7eU, 0x98U, 0x03U, 0xa7U, 0x7dU, 0x26U, 0x03U, 0x29U,
    0x9bU, 0x36U, 0xccU, 0xa6U, 0xcfU, 0x5eU, 0xddU, 0xb1U
};

static uint8_t temporary[128];
static uint8_t staged[128];
static size_t temporary_bytes;
static size_t staged_bytes;
static bool temporary_present;
static bool staged_present;
static bool handle_live;
static unsigned write_calls;
static unsigned sync_calls;
static unsigned fail_write_call;
static unsigned fail_sync_call;
static bool fail_close;
static bool fail_replace;
static bool upload_live;
static bool fail_upload_open;
static bool fail_upload_write;
static bool fail_upload_seal;
static bool fail_upload_close;
static uint8_t uploaded[128];
static size_t uploaded_bytes;
static enum phipia_https_status forced_https_status;
static bool partial_https_body;

static void reset_fixture(void)
{
    (void)memset(temporary, 0, sizeof(temporary));
    (void)memset(staged, 0, sizeof(staged));
    temporary_bytes = 0U;
    staged_bytes = 0U;
    temporary_present = false;
    staged_present = false;
    handle_live = false;
    write_calls = 0U;
    sync_calls = 0U;
    fail_write_call = 0U;
    fail_sync_call = 0U;
    fail_close = false;
    fail_replace = false;
    upload_live = false;
    fail_upload_open = false;
    fail_upload_write = false;
    fail_upload_seal = false;
    fail_upload_close = false;
    (void)memset(uploaded, 0, sizeof(uploaded));
    uploaded_bytes = 0U;
    forced_https_status = PHIPIA_HTTPS_OK;
    partial_https_body = false;
}

long phipia_package_upload_open(void)
{
    if (fail_upload_open || upload_live) {
        return -(long)PHIPIA_EIO;
    }
    upload_live = true;
    uploaded_bytes = 0U;
    return 19;
}

long phipia_package_upload_write(phipia_handle_t upload, const void *bytes,
    size_t byte_count)
{
    if (!upload_live || upload != 19U || bytes == NULL || fail_upload_write) {
        return -(long)PHIPIA_EIO;
    }
    if (byte_count > sizeof(uploaded) - uploaded_bytes) {
        return -(long)PHIPIA_ENOSPC;
    }
    (void)memcpy(uploaded + uploaded_bytes, bytes, byte_count);
    uploaded_bytes += byte_count;
    return (long)byte_count;
}

long phipia_package_upload_seal(phipia_handle_t upload,
    uint64_t expected_bytes, const uint8_t expected_sha256[32],
    struct phipia_package_upload_report *report)
{
    if (!upload_live || upload != 19U || expected_sha256 == NULL ||
        report == NULL) {
        return -(long)PHIPIA_EINVAL;
    }
    (void)memset(report, 0, sizeof(*report));
    report->actual_bytes = uploaded_bytes;
    if (uploaded_bytes == sizeof(body) - 1U &&
        memcmp(uploaded, body, uploaded_bytes) == 0) {
        (void)memcpy(report->actual_sha256, body_sha256,
            sizeof(body_sha256));
    }
    if (fail_upload_seal) {
        return -(long)PHIPIA_EIO;
    }
    if (expected_bytes != uploaded_bytes) {
        return -(long)PHIPIA_EINVAL;
    }
    if (memcmp(expected_sha256, report->actual_sha256,
            sizeof(report->actual_sha256)) != 0) {
        return -(long)PHIPIA_EACCES;
    }
    report->result_flags = PHIPIA_PACKAGE_UPLOAD_SEALED |
        PHIPIA_PACKAGE_UPLOAD_DURABLE;
    return 0;
}

long phipia_package_upload_close(phipia_handle_t upload)
{
    if (!upload_live || upload != 19U) {
        return -(long)PHIPIA_ESTALE;
    }
    if (fail_upload_close) {
        return -(long)PHIPIA_EIO;
    }
    upload_live = false;
    uploaded_bytes = 0U;
    return 0;
}

long phipia_file_open(uint16_t volume, const char *path, uint32_t flags)
{
    if (volume != PHIPIA_VOLUME_DATA || strcmp(path, "FETCH.NEW") != 0 ||
        flags != (PHIPIA_OPEN_WRITE | PHIPIA_OPEN_CREATE |
            PHIPIA_OPEN_TRUNCATE) || handle_live) {
        return -(long)PHIPIA_EINVAL;
    }
    temporary_present = true;
    temporary_bytes = 0U;
    handle_live = true;
    return 7;
}

long phipia_file_write(phipia_handle_t handle, const void *bytes, size_t count)
{
    size_t accepted = count > 7U ? 7U : count;
    ++write_calls;
    if (!handle_live || handle != 7U || bytes == NULL ||
        (fail_write_call != 0U && write_calls == fail_write_call)) {
        return -(long)PHIPIA_EIO;
    }
    if (accepted > sizeof(temporary) - temporary_bytes) {
        return -(long)PHIPIA_ENOSPC;
    }
    (void)memcpy(temporary + temporary_bytes, bytes, accepted);
    temporary_bytes += accepted;
    return (long)accepted;
}

long phipia_handle_close(phipia_handle_t handle)
{
    if (!handle_live || handle != 7U) {
        return -(long)PHIPIA_ESTALE;
    }
    handle_live = false;
    return fail_close ? -(long)PHIPIA_EIO : 0;
}

long phipia_path_unlink(uint16_t volume, const char *path)
{
    if (volume != PHIPIA_VOLUME_DATA || strcmp(path, "FETCH.NEW") != 0) {
        return -(long)PHIPIA_EINVAL;
    }
    if (!temporary_present) {
        return -(long)PHIPIA_ENOENT;
    }
    temporary_present = false;
    temporary_bytes = 0U;
    return 0;
}

long phipia_volume_sync(uint16_t volume)
{
    ++sync_calls;
    if (volume != PHIPIA_VOLUME_DATA ||
        (fail_sync_call != 0U && sync_calls == fail_sync_call)) {
        return -(long)PHIPIA_EIO;
    }
    return 0;
}

long phipia_path_replace(uint16_t volume, const char *source,
    const char *destination)
{
    if (volume != PHIPIA_VOLUME_DATA || strcmp(source, "FETCH.NEW") != 0 ||
        strcmp(destination, "FETCH.BIN") != 0 || !temporary_present ||
        fail_replace) {
        return -(long)PHIPIA_EIO;
    }
    (void)memcpy(staged, temporary, temporary_bytes);
    staged_bytes = temporary_bytes;
    staged_present = true;
    temporary_present = false;
    temporary_bytes = 0U;
    return 0;
}

enum phipia_https_status phipia_https_get_stream(
    const struct phipia_https_stream_request *request,
    struct phipia_https_response *response)
{
    size_t first = partial_https_body ? 5U : 17U;
    long count;
    if (request == NULL || response == NULL || request->write_body == NULL ||
        request->body_limit < sizeof(body) - 1U) {
        return PHIPIA_HTTPS_ARGUMENT;
    }
    (void)memset(response, 0, sizeof(*response));
    response->status_code = 200U;
    response->content_length = sizeof(body) - 1U;
    count = request->write_body(request->write_context, body, first);
    if (count != (long)first) {
        return PHIPIA_HTTPS_BODY_WRITE;
    }
    response->body_length = first;
    if (forced_https_status != PHIPIA_HTTPS_OK) {
        return forced_https_status;
    }
    count = request->write_body(request->write_context, body + first,
        sizeof(body) - 1U - first);
    if (count != (long)(sizeof(body) - 1U - first)) {
        return PHIPIA_HTTPS_BODY_WRITE;
    }
    response->body_length = sizeof(body) - 1U;
    return PHIPIA_HTTPS_OK;
}

static struct phipia_package_fetch_request request(bool exact)
{
    static const br_x509_trust_anchor anchor = {0};
    const struct phipia_package_fetch_request result = {
        "repo.phipia.test", 443U, 0U, "/index.sri", &anchor, 1U,
        UINT64_MAX, 128U, exact ? sizeof(body) - 1U : 0U,
        exact ? body_sha256 : NULL, "FETCH.NEW", "FETCH.BIN"
    };
    return result;
}

static struct phipia_package_fetch_upload_request upload_request(void)
{
    static const br_x509_trust_anchor anchor = {0};
    const struct phipia_package_fetch_upload_request result = {
        "repo.phipia.test", 443U, 0U, "/package.spk", &anchor, 1U,
        UINT64_MAX, sizeof(body) - 1U, body_sha256
    };

    return result;
}

static int run_tests(void)
{
    struct phipia_package_fetch_request input;
    struct phipia_package_fetch_report report;
    uint8_t wrong_digest[32] = {0U};

    reset_fixture();
    input = request(true);
    CHECK(phipia_package_fetch_stage(&input, &report) ==
            PHIPIA_PACKAGE_FETCH_OK && report.published && report.durable &&
        report.bytes_received == sizeof(body) - 1U &&
        memcmp(report.sha256, body_sha256, sizeof(body_sha256)) == 0 &&
        staged_present && staged_bytes == sizeof(body) - 1U &&
        memcmp(staged, body, staged_bytes) == 0 && !temporary_present &&
        !handle_live && sync_calls == 2U && write_calls > 2U, 1);

    reset_fixture();
    input = request(false);
    CHECK(phipia_package_fetch_stage(&input, &report) ==
            PHIPIA_PACKAGE_FETCH_OK && report.durable && staged_present, 2);

    reset_fixture();
    input = request(true);
    input.expected_sha256 = wrong_digest;
    CHECK(phipia_package_fetch_stage(&input, &report) ==
            PHIPIA_PACKAGE_FETCH_DIGEST && !temporary_present &&
        !staged_present && !handle_live && sync_calls == 1U, 3);

    reset_fixture();
    input = request(true);
    partial_https_body = true;
    forced_https_status = PHIPIA_HTTPS_BODY_TRUNCATED;
    CHECK(phipia_package_fetch_stage(&input, &report) ==
            PHIPIA_PACKAGE_FETCH_HTTPS &&
        report.https_status == PHIPIA_HTTPS_BODY_TRUNCATED &&
        report.bytes_received == 5U && !temporary_present && !handle_live, 4);

    reset_fixture();
    input = request(true);
    fail_write_call = 2U;
    CHECK(phipia_package_fetch_stage(&input, &report) ==
            PHIPIA_PACKAGE_FETCH_WRITE &&
        report.https_status == PHIPIA_HTTPS_BODY_WRITE &&
        report.storage_error == -(long)PHIPIA_EIO && !temporary_present &&
        !handle_live, 5);

    reset_fixture();
    input = request(true);
    fail_close = true;
    CHECK(phipia_package_fetch_stage(&input, &report) ==
            PHIPIA_PACKAGE_FETCH_CLOSE && !temporary_present && !handle_live, 6);

    reset_fixture();
    input = request(true);
    fail_sync_call = 1U;
    CHECK(phipia_package_fetch_stage(&input, &report) ==
            PHIPIA_PACKAGE_FETCH_SYNC && !temporary_present && !staged_present &&
        sync_calls == 2U, 7);

    reset_fixture();
    input = request(true);
    fail_replace = true;
    CHECK(phipia_package_fetch_stage(&input, &report) ==
            PHIPIA_PACKAGE_FETCH_PUBLISH && !temporary_present &&
        !staged_present && sync_calls == 2U, 8);

    reset_fixture();
    input = request(true);
    fail_sync_call = 2U;
    CHECK(phipia_package_fetch_stage(&input, &report) ==
            PHIPIA_PACKAGE_FETCH_SYNC && report.published && !report.durable &&
        staged_present && !temporary_present && sync_calls == 2U, 9);

    reset_fixture();
    input = request(true);
    input.temporary_path = input.staged_path;
    CHECK(phipia_package_fetch_stage(&input, &report) ==
            PHIPIA_PACKAGE_FETCH_ARGUMENT && !temporary_present &&
        !staged_present && !handle_live, 10);

    struct phipia_package_fetch_upload_request upload = upload_request();

    reset_fixture();
    CHECK(phipia_package_fetch_upload(&upload, &report) ==
            PHIPIA_PACKAGE_FETCH_OK && report.durable && !report.published &&
        report.upload == 19U && upload_live &&
        report.bytes_received == sizeof(body) - 1U &&
        memcmp(report.sha256, body_sha256, sizeof(body_sha256)) == 0 &&
        report.upload_flags == (PHIPIA_PACKAGE_UPLOAD_SEALED |
            PHIPIA_PACKAGE_UPLOAD_DURABLE), 11);
    CHECK(phipia_package_upload_close(report.upload) == 0 && !upload_live, 12);

    reset_fixture();
    upload = upload_request();
    upload.expected_sha256 = wrong_digest;
    CHECK(phipia_package_fetch_upload(&upload, &report) ==
            PHIPIA_PACKAGE_FETCH_DIGEST && !upload_live &&
        report.upload == PHIPIA_HANDLE_INVALID, 13);

    reset_fixture();
    upload = upload_request();
    partial_https_body = true;
    forced_https_status = PHIPIA_HTTPS_BODY_TRUNCATED;
    CHECK(phipia_package_fetch_upload(&upload, &report) ==
            PHIPIA_PACKAGE_FETCH_HTTPS && !upload_live &&
        report.upload == PHIPIA_HANDLE_INVALID, 14);

    reset_fixture();
    upload = upload_request();
    fail_upload_write = true;
    CHECK(phipia_package_fetch_upload(&upload, &report) ==
            PHIPIA_PACKAGE_FETCH_WRITE && !upload_live &&
        report.storage_error == -(long)PHIPIA_EIO, 15);

    reset_fixture();
    upload = upload_request();
    fail_upload_open = true;
    CHECK(phipia_package_fetch_upload(&upload, &report) ==
            PHIPIA_PACKAGE_FETCH_UPLOAD_OPEN && !upload_live, 16);

    reset_fixture();
    upload = upload_request();
    fail_upload_seal = true;
    CHECK(phipia_package_fetch_upload(&upload, &report) ==
            PHIPIA_PACKAGE_FETCH_UPLOAD_SEAL && !upload_live &&
        report.storage_error == -(long)PHIPIA_EIO, 17);

    reset_fixture();
    upload = upload_request();
    upload.expected_bytes = 0U;
    CHECK(phipia_package_fetch_upload(&upload, &report) ==
            PHIPIA_PACKAGE_FETCH_ARGUMENT && !upload_live, 18);

    reset_fixture();
    upload = upload_request();
    forced_https_status = PHIPIA_HTTPS_BODY_TRUNCATED;
    partial_https_body = true;
    fail_upload_close = true;
    CHECK(phipia_package_fetch_upload(&upload, &report) ==
            PHIPIA_PACKAGE_FETCH_HTTPS && upload_live && report.upload == 19U &&
        report.cleanup_error == -(long)PHIPIA_EIO, 19);
    fail_upload_close = false;
    CHECK(phipia_package_upload_close(report.upload) == 0 && !upload_live, 20);
    return 0;
}

int main(void)
{
    int status = run_tests();
    if (status != 0) {
        (void)fprintf(stderr, "package fetch host test failed: %d\n", status);
        return status;
    }
    (void)puts("Phipia durable streaming package-fetch tests passed: "
        "path and kernel-owned upload modes");
    return 0;
}
