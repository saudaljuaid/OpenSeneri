/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/package_fetch.h>
#include <phipia/runtime.h>

#include <stdio.h>

#include "trust_anchor.h"

#define HTTPS_PORT 443U
#define HTTPS_DEADLINE_NS UINT64_C(15000000000)
#define EXPECTED_BODY_BYTES 33U

static const uint8_t expected_sha256[32] = {
    0x52U, 0xe0U, 0xf1U, 0x82U, 0x26U, 0xbeU, 0xf9U, 0xf5U,
    0x15U, 0x18U, 0x5eU, 0x04U, 0x3aU, 0x94U, 0x62U, 0x36U,
    0x7eU, 0x98U, 0x03U, 0xa7U, 0x7dU, 0x26U, 0x03U, 0x29U,
    0x9bU, 0x36U, 0xccU, 0xa6U, 0xcfU, 0x5eU, 0xddU, 0xb1U
};

static uint64_t deadline(void)
{
    const uint64_t now = phipia_monotonic_ns();

    return now > UINT64_MAX - HTTPS_DEADLINE_NS ? UINT64_MAX :
        now + HTTPS_DEADLINE_NS;
}

int main(void)
{
    struct phipia_package_fetch_report report;
    const struct phipia_package_fetch_request request = {
        "repo.phipia.test", HTTPS_PORT, 0U, "/artifact.bin",
        phipia_https_test_anchors,
        sizeof(phipia_https_test_anchors) /
            sizeof(phipia_https_test_anchors[0]),
        deadline(), 128U, EXPECTED_BODY_BYTES, expected_sha256,
        "HTTPS.NEW", "HTTPS.TXT"
    };
    enum phipia_package_fetch_status status;

    puts("PHIPIA HTTPSAPP PHASE start");
    status = phipia_package_fetch_stage(&request, &report);
    if (status != PHIPIA_PACKAGE_FETCH_OK) {
        printf("PHIPIA HTTPSAPP REFUSED %s https=%s tls=%d transport=%ld "
            "storage=%ld cleanup=%ld\n",
            phipia_package_fetch_status_string(status),
            phipia_https_status_string(report.https_status),
            report.bearssl_error, report.transport_error,
            report.storage_error, report.cleanup_error);
        return 20;
    }
    if (report.bytes_received != EXPECTED_BODY_BYTES ||
        !report.published || !report.durable) {
        return 21;
    }
    puts("PHIPIA HTTPSAPP PHASE authenticated-download PASS");
    puts("PHIPIA HTTPSAPP PHASE durable-output PASS");
    const struct phipia_package_fetch_upload_request upload_request = {
        "repo.phipia.test", HTTPS_PORT, 0U, "/artifact.bin",
        phipia_https_test_anchors,
        sizeof(phipia_https_test_anchors) /
            sizeof(phipia_https_test_anchors[0]),
        deadline(), EXPECTED_BODY_BYTES, expected_sha256
    };

    status = phipia_package_fetch_upload(&upload_request, &report);
    if (status != PHIPIA_PACKAGE_FETCH_OK || !report.durable ||
        report.upload == PHIPIA_HANDLE_INVALID ||
        report.upload_flags != (PHIPIA_PACKAGE_UPLOAD_SEALED |
            PHIPIA_PACKAGE_UPLOAD_DURABLE)) {
        printf("PHIPIA HTTPSAPP UPLOAD REFUSED %s https=%s storage=%ld "
            "cleanup=%ld flags=%u\n",
            phipia_package_fetch_status_string(status),
            phipia_https_status_string(report.https_status),
            report.storage_error, report.cleanup_error, report.upload_flags);
        if (report.upload != PHIPIA_HANDLE_INVALID) {
            (void)phipia_package_upload_close(report.upload);
        }
        return 22;
    }
    if (phipia_package_upload_close(report.upload) < 0) {
        return 23;
    }
    puts("PHIPIA HTTPSAPP PHASE kernel-upload PASS");
    puts("PHIPIA HTTPSAPP PASS hostname time trust length close upload");
    return 0;
}
