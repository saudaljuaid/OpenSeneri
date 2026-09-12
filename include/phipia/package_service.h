/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PACKAGE_SERVICE_H
#define PHIPIA_PACKAGE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/fat32_fs.h>
#include <phipia/package_builder.h>
#include <phipia/package_state.h>

/* Deliberately 8.3-safe so recovery has one layout on FAT32 and ext4. */
#define PACKAGE_SERVICE_STATE_DIRECTORY "pkgstate"
#define PACKAGE_SERVICE_AUTHORITY_PATH "pkgstate/auth.bin"
#define PACKAGE_SERVICE_AUTHORITY_NEW_PATH "pkgstate/auth.new"
#define PACKAGE_SERVICE_AUTHORITY_OLD_PATH "pkgstate/auth.old"
#define PACKAGE_SERVICE_JOURNAL_PATH "pkgstate/txn.bin"
#define PACKAGE_SERVICE_JOURNAL_NEW_PATH "pkgstate/txn.new"
#define PACKAGE_SERVICE_REPOSITORY_FLOOR_PATH "pkgstate/repo.bin"
#define PACKAGE_SERVICE_REPOSITORY_FLOOR_NEW_PATH "pkgstate/repo.new"
#define PACKAGE_SERVICE_REPOSITORY_FLOOR_BYTES 128U
#define PACKAGE_SERVICE_TRANSACTION_BYTES (32U * 4096U)
#define PACKAGE_SERVICE_CLEANUP_CHUNK PACKAGE_SERVICE_TRANSACTION_BYTES

/* Two candidates and scratch state must fit Phipia's bounded 16 MiB heap. */
#define PACKAGE_SERVICE_MAX_DATABASE_BYTES (4U * 1024U * 1024U)
#define PACKAGE_SERVICE_IO_BYTES 4096U
#define PACKAGE_SERVICE_MAX_TREE_ENTRIES 8192U

enum package_service_status {
    PACKAGE_SERVICE_STATUS_OK = 0,
    PACKAGE_SERVICE_STATUS_NULL_ARGUMENT,
    PACKAGE_SERVICE_STATUS_BUSY,
    PACKAGE_SERVICE_STATUS_ABSENT,
    PACKAGE_SERVICE_STATUS_UNAVAILABLE,
    PACKAGE_SERVICE_STATUS_FILESYSTEM,
    PACKAGE_SERVICE_STATUS_RESOURCE,
    PACKAGE_SERVICE_STATUS_STATE,
    PACKAGE_SERVICE_STATUS_INCOMPLETE,
    PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE,
    PACKAGE_SERVICE_STATUS_NAMESPACE,
    PACKAGE_SERVICE_STATUS_DURABILITY,
    PACKAGE_SERVICE_STATUS_CLEANUP,
    PACKAGE_SERVICE_STATUS_COUNT
};

struct package_service_report {
    enum package_service_status status;
    enum phipfs_status filesystem_status;
    enum package_state_status state_status;
    enum package_state_recovery_choice choice;
    uint64_t generation;
    uint64_t repository_floor;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint32_t files_verified;
    uint32_t files_staged;
    uint32_t tree_entries;
    uint32_t sync_count;
    uint32_t rename_count;
    uint32_t peak_file_handles;
    uint32_t peak_allocations;
    uint32_t live_file_handles;
    uint32_t live_allocations;
    bool journal_present;
    bool prepared;
    bool committed;
    bool authority_replaced;
    bool cleanup_complete;
};

/*
 * Privileged recovery entry point. The caller must invoke it only after the
 * data volume and heap are online, before package-owned executables are used.
 */
enum package_service_status package_service_recover(
    struct package_service_report *report
);

/*
 * Recovers first, then copies the complete authority-selected canonical
 * database into caller-owned privileged memory. output_bytes remains zero and
 * the copied span is cleared on every refusal.
 */
enum package_service_status package_service_snapshot(
    uint8_t *database,
    size_t capacity,
    size_t *output_bytes,
    struct package_service_report *report
);

/*
 * Copies authenticated authority metadata for a repair transaction. A clean
 * state is returned exactly like package_service_snapshot(); an otherwise
 * quiescent generation whose owned files are incomplete is also admitted so
 * signed replacement bytes can rebuild it. Live or ambiguous transactions
 * still fail closed.
 */
enum package_service_status package_service_repair_snapshot(
    uint8_t *database,
    size_t capacity,
    size_t *output_bytes,
    struct package_service_report *report
);

/*
 * Reads the greatest valid current or crash-leftover repository floor. An
 * absent record is floor zero. Any present malformed candidate fails closed.
 */
enum package_service_status package_service_repository_floor_read(
    uint64_t *repository_floor,
    struct package_service_report *report
);

/*
 * Durably advances, but never lowers, the signed-repository rollback floor.
 * The new candidate is flushed before the old authority is removed, so every
 * write/rename/flush prefix retains at least the previously accepted floor.
 */
enum package_service_status package_service_repository_floor_advance(
    uint64_t repository_version,
    struct package_service_report *report
);

struct package_service_prepare_request {
    const struct package_builder_workspace *builder;
    const uint8_t *database;
    size_t database_bytes;
};

/*
 * Writes and verifies a complete target generation, then durably publishes a
 * prepared journal while leaving the current authority unchanged.  Therefore
 * success is not installation success: reboot recovery rolls this generation
 * back until a separate authority-commit phase exists.  Fresh-store bootstrap
 * and repair are not admitted by this entry point.
 */
enum package_service_status package_service_prepare(
    const struct package_service_prepare_request *request,
    struct package_service_report *report
);

/*
 * Creates generation one only when no authority or transaction exists.  The
 * authenticated builder must describe a fresh install and every file must come
 * from an admitted signed payload.  A durable auth.new receipt makes every
 * bootstrap power-cut prefix recoverable without inventing a generation zero.
 */
enum package_service_status package_service_bootstrap(
    const struct package_service_prepare_request *request,
    struct package_service_report *report
);

/*
 * Commits an already prepared transaction.  The service revalidates the
 * journal, both complete generations, and the current base authority before
 * durably selecting the target.  Once report.committed is true, recovery will
 * select the target even if best-effort cleanup returns an error.
 */
enum package_service_status package_service_commit(
    struct package_service_report *report
);

const char *package_service_status_string(enum package_service_status status);

#endif
