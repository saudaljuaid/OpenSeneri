/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PACKAGE_UPLOAD_H
#define PHIPIA_PACKAGE_UPLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/fat32_fs.h>
#include <phipia/package_state.h>

/* Kernel-private, 8.3-safe staging names on the writable data volume. */
#define PACKAGE_UPLOAD_DIRECTORY "pkgstage"
#define PACKAGE_UPLOAD_SLOT_LIMIT 4U
#define PACKAGE_UPLOAD_WRITE_MAX 4096U
#define PACKAGE_UPLOAD_MAX_BYTES PHIPFS_MAX_FILE_BYTES
/* Private scratch cleanup stays below ext4's journal revoke ceiling. */
#define PACKAGE_UPLOAD_CLEANUP_CHUNK (32U * 4096U)

typedef uint64_t package_upload_token;

enum package_upload_status {
    PACKAGE_UPLOAD_STATUS_OK = 0,
    PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT,
    PACKAGE_UPLOAD_STATUS_NOT_INITIALIZED,
    PACKAGE_UPLOAD_STATUS_BUSY,
    PACKAGE_UPLOAD_STATUS_NO_SLOT,
    PACKAGE_UPLOAD_STATUS_STALE,
    PACKAGE_UPLOAD_STATUS_STATE,
    PACKAGE_UPLOAD_STATUS_RANGE,
    PACKAGE_UPLOAD_STATUS_LENGTH,
    PACKAGE_UPLOAD_STATUS_DIGEST,
    PACKAGE_UPLOAD_STATUS_FILESYSTEM,
    PACKAGE_UPLOAD_STATUS_DURABILITY,
    PACKAGE_UPLOAD_STATUS_COUNT
};

struct package_upload_report {
    enum package_upload_status status;
    enum phipfs_status filesystem_status;
    package_upload_token token;
    uint64_t byte_count;
    uint8_t sha256[PACKAGE_STATE_SHA256_BYTES];
    bool sealed;
    bool durable;
};

/*
 * Removes bounded crash leftovers and establishes the private staging
 * directory. Call only after the data volume is mounted.
 */
enum package_upload_status package_upload_initialize(
    struct package_upload_report *report
);

/* owner is a nonzero kernel process generation, never a userspace value. */
enum package_upload_status package_upload_open(
    uint64_t owner,
    struct package_upload_report *report
);

/* Writes are bounded so one syscall cannot monopolize kernel execution. */
enum package_upload_status package_upload_write(
    uint64_t owner,
    package_upload_token token,
    const uint8_t *bytes,
    size_t byte_count,
    size_t *written_bytes,
    struct package_upload_report *report
);

/*
 * A package is readable only after the privileged caller's expected length
 * and digest match and a data-volume flush succeeds. The transaction
 * controller remains responsible for binding those values to admitted signed
 * repository metadata.
 */
enum package_upload_status package_upload_seal(
    uint64_t owner,
    package_upload_token token,
    uint64_t expected_bytes,
    const uint8_t expected_sha256[PACKAGE_STATE_SHA256_BYTES],
    struct package_upload_report *report
);

enum package_upload_status package_upload_read(
    uint64_t owner,
    package_upload_token token,
    uint64_t offset,
    uint8_t *bytes,
    size_t capacity,
    size_t *read_bytes,
    struct package_upload_report *report
);

/* Reports only a sealed, durable upload and performs no filesystem access. */
enum package_upload_status package_upload_inspect(
    uint64_t owner,
    package_upload_token token,
    struct package_upload_report *report
);

/* Last native-handle close calls this and durably removes the private file. */
enum package_upload_status package_upload_close(
    uint64_t owner,
    package_upload_token token,
    struct package_upload_report *report
);

bool package_upload_resources_released(void);

const char *package_upload_status_string(enum package_upload_status status);

#endif
