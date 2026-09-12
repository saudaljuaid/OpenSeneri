/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PACKAGE_CONTROL_H
#define PHIPIA_PACKAGE_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/package_manager.h>
#include <phipia/package_service.h>
#include <phipia/package_upload.h>

#define PACKAGE_CONTROL_SESSION_LIMIT 1U
#define PACKAGE_CONTROL_PLAN_MAX_PACKAGES 8U
#define PACKAGE_CONTROL_REPOSITORY_MAX_BYTES (512U * 1024U)
#define PACKAGE_CONTROL_DATABASE_MAX_BYTES (1024U * 1024U)
#define PACKAGE_CONTROL_PAYLOAD_MAX_BYTES (4U * 1024U * 1024U)
#define PACKAGE_CONTROL_TEXT_BYTES 128U
#define PACKAGE_CONTROL_PATH_BYTES 256U

typedef uint64_t package_control_token;

enum package_control_status {
    PACKAGE_CONTROL_STATUS_OK = 0,
    PACKAGE_CONTROL_STATUS_NULL_ARGUMENT,
    PACKAGE_CONTROL_STATUS_BUSY,
    PACKAGE_CONTROL_STATUS_NO_SLOT,
    PACKAGE_CONTROL_STATUS_STALE,
    PACKAGE_CONTROL_STATUS_STATE,
    PACKAGE_CONTROL_STATUS_RANGE,
    PACKAGE_CONTROL_STATUS_RESOURCE,
    PACKAGE_CONTROL_STATUS_CLOCK,
    PACKAGE_CONTROL_STATUS_TRUST,
    PACKAGE_CONTROL_STATUS_UPLOAD,
    PACKAGE_CONTROL_STATUS_MANAGER,
    PACKAGE_CONTROL_STATUS_SERVICE,
    PACKAGE_CONTROL_STATUS_COUNT
};

struct package_control_report {
    enum package_control_status status;
    enum package_upload_status upload_status;
    enum package_manager_status manager_status;
    enum package_service_status service_status;
    package_control_token token;
    uint64_t repository_version;
    uint64_t generation;
    uint32_t plan_count;
    uint32_t attached_count;
    bool prepared;
    bool committed;
};

struct package_control_item {
    uint32_t index;
    uint32_t identifier_bytes;
    uint32_t version_bytes;
    uint32_t path_bytes;
    uint64_t package_bytes;
    uint8_t package_sha256[PACKAGE_MANAGER_SHA256_BYTES];
    char identifier[PACKAGE_CONTROL_TEXT_BYTES];
    char version[PACKAGE_CONTROL_TEXT_BYTES];
    char download_path[PACKAGE_CONTROL_PATH_BYTES];
};

/*
 * Authenticates a sealed repository through immutable platform trust, snapshots
 * recovered installed state, and produces an install/update plan. This first
 * controller profile is deliberately bounded below the parser format maxima so
 * controller and package-service working sets coexist in the 16 MiB heap.
 */
enum package_control_status package_control_open_install(
    uint64_t owner,
    package_upload_token repository_upload,
    const uint8_t *identifier,
    size_t identifier_bytes,
    struct package_control_report *report
);

/*
 * Snapshots recovered installed state and produces a dependency-safe removal
 * plan.  Removal needs no repository upload because every selected identity,
 * version, ownership edge, and file digest comes from the authenticated
 * installed database.
 */
enum package_control_status package_control_open_remove(
    uint64_t owner,
    const uint8_t *identifier,
    size_t identifier_bytes,
    struct package_control_report *report
);

/*
 * Authenticates a repository and creates an all-owned-file repair plan for the
 * authority-selected generation. Every installed package must have one exact
 * repository record; commit accepts only those signed payloads.
 */
enum package_control_status package_control_open_repair(
    uint64_t owner,
    package_upload_token repository_upload,
    struct package_control_report *report
);

enum package_control_status package_control_item(
    uint64_t owner,
    package_control_token token,
    uint32_t index,
    struct package_control_item *item,
    struct package_control_report *report
);

/* Copies and authenticates one exact sealed plan payload into the session. */
enum package_control_status package_control_attach(
    uint64_t owner,
    package_control_token token,
    uint32_t index,
    package_upload_token package_upload,
    struct package_control_report *report
);

/*
 * Rebuilds the authenticated plan, encodes its canonical generation, then
 * bootstraps or prepares and commits it through package_service.
 */
enum package_control_status package_control_commit(
    uint64_t owner,
    package_control_token token,
    struct package_control_report *report
);

enum package_control_status package_control_close(
    uint64_t owner,
    package_control_token token,
    struct package_control_report *report
);

bool package_control_resources_released(void);
const char *package_control_status_string(enum package_control_status status);

#endif
