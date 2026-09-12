/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PACKAGE_MANAGER_H
#define PHIPIA_PACKAGE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/package_state.h>

#define PACKAGE_MANAGER_SHA256_BYTES 32U
#define PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES 32U
#define PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES 64U
#define PACKAGE_MANAGER_REPOSITORY_MAX_BYTES (32U * 1024U * 1024U)
#define PACKAGE_MANAGER_REPOSITORY_MAX_PACKAGES 1024U
#define PACKAGE_MANAGER_REPOSITORY_MAX_RELATIONS_PER_PACKAGE 64U
#define PACKAGE_MANAGER_PACKAGE_MAX_BYTES (256U * 1024U * 1024U)
#define PACKAGE_MANAGER_PACKAGE_MAX_FILES 256U
#define PACKAGE_MANAGER_PLAN_MAX_PACKAGES 128U
#define PACKAGE_MANAGER_DEPENDENCY_DEPTH_MAX 16U
#define PACKAGE_MANAGER_SEARCH_MAX_RESULTS 64U

enum package_manager_status {
    PACKAGE_MANAGER_STATUS_OK = 0,
    PACKAGE_MANAGER_STATUS_NULL_ARGUMENT,
    PACKAGE_MANAGER_STATUS_LENGTH,
    PACKAGE_MANAGER_STATUS_MAGIC,
    PACKAGE_MANAGER_STATUS_HEADER,
    PACKAGE_MANAGER_STATUS_RESERVED,
    PACKAGE_MANAGER_STATUS_OVERFLOW,
    PACKAGE_MANAGER_STATUS_DIGEST,
    PACKAGE_MANAGER_STATUS_TEXT,
    PACKAGE_MANAGER_STATUS_ARCHITECTURE,
    PACKAGE_MANAGER_STATUS_ABI,
    PACKAGE_MANAGER_STATUS_FRESHNESS,
    PACKAGE_MANAGER_STATUS_ROLLBACK,
    PACKAGE_MANAGER_STATUS_TABLE,
    PACKAGE_MANAGER_STATUS_PACKAGE,
    PACKAGE_MANAGER_STATUS_DEPENDENCY,
    PACKAGE_MANAGER_STATUS_CONFLICT,
    PACKAGE_MANAGER_STATUS_AMBIGUOUS_PROVIDER,
    PACKAGE_MANAGER_STATUS_CYCLE,
    PACKAGE_MANAGER_STATUS_GRAPH_BOUND,
    PACKAGE_MANAGER_STATUS_NOT_FOUND,
    PACKAGE_MANAGER_STATUS_ALREADY_INSTALLED,
    PACKAGE_MANAGER_STATUS_DOWNGRADE,
    PACKAGE_MANAGER_STATUS_KEY_ROTATION,
    PACKAGE_MANAGER_STATUS_UNKNOWN_KEY,
    PACKAGE_MANAGER_STATUS_REVOKED_KEY,
    PACKAGE_MANAGER_STATUS_CRYPTO_UNAVAILABLE,
    PACKAGE_MANAGER_STATUS_SIGNATURE,
    PACKAGE_MANAGER_STATUS_STATE,
    PACKAGE_MANAGER_STATUS_IN_USE,
    PACKAGE_MANAGER_STATUS_COUNT
};

enum package_manager_key_status {
    PACKAGE_MANAGER_KEY_TRUSTED = 0,
    PACKAGE_MANAGER_KEY_UNKNOWN,
    PACKAGE_MANAGER_KEY_REVOKED
};

struct package_manager_text {
    const uint8_t *bytes;
    size_t length;
};

/*
 * Key material is supplied by an immutable privileged trust store.  The
 * verifier must perform real Ed25519 verification over message with the
 * indicated byte range treated as zero.  This avoids copying a 256 MiB
 * package solely to clear its embedded signature.  No verifier means refusal.
 */
typedef enum package_manager_key_status (*package_manager_key_lookup_fn)(
    void *context,
    const uint8_t key_id[PACKAGE_MANAGER_SHA256_BYTES],
    uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES]
);

typedef bool (*package_manager_ed25519_verify_fn)(
    void *context,
    const uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t signature[PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES],
    const uint8_t *message,
    size_t message_bytes,
    size_t zero_offset,
    size_t zero_bytes
);

struct package_manager_trust {
    package_manager_key_lookup_fn lookup;
    package_manager_ed25519_verify_fn verify;
    void *context;
};

struct package_manager_policy {
    uint64_t now;
    uint64_t minimum_repository_version;
    uint32_t abi;
    bool allow_downgrade;
};

struct package_manager_repository_view {
    const uint8_t *bytes;
    size_t byte_count;
    uint64_t repository_version;
    uint64_t generated_at;
    uint64_t expires_at;
    uint32_t abi_min;
    uint32_t abi_max;
    uint32_t package_count;
    uint32_t relation_count;
    size_t package_offset;
    size_t relation_offset;
    struct package_manager_text identifier;
};

struct package_manager_catalog_entry {
    const struct package_manager_repository_view *repository;
    uint32_t repository_index;
    struct package_manager_text identifier;
    struct package_manager_text version;
    struct package_manager_text download_path;
    uint64_t package_bytes;
    const uint8_t *package_sha256;
    const uint8_t *publisher_key_id;
    uint32_t dependency_start;
    uint32_t dependency_count;
    uint32_t conflict_start;
    uint32_t conflict_count;
    uint32_t provide_start;
    uint32_t provide_count;
};

struct package_manager_package_view {
    const uint8_t *bytes;
    size_t byte_count;
    struct package_manager_text identifier;
    struct package_manager_text name;
    struct package_manager_text version;
    struct package_manager_text publisher;
    uint64_t capabilities;
    uint32_t abi_min;
    uint32_t abi_max;
    uint32_t file_count;
    uint32_t dependency_count;
    uint32_t conflict_count;
    size_t file_offset;
    size_t dependency_offset;
    size_t conflict_offset;
    size_t payload_offset;
};

enum package_manager_file_kind {
    PACKAGE_MANAGER_FILE_EXECUTABLE = 1,
    PACKAGE_MANAGER_FILE_LIBRARY = 2,
    PACKAGE_MANAGER_FILE_RESOURCE = 3,
    PACKAGE_MANAGER_FILE_FONT = 4
};

/*
 * Immutable slices into package bytes already admitted by
 * package_manager_package_open().  The package buffer must remain live and
 * unchanged while these views are used.
 */
struct package_manager_file_view {
    const struct package_manager_package_view *package;
    uint32_t package_index;
    struct package_manager_text path;
    struct package_manager_text soname;
    enum package_manager_file_kind kind;
    uint32_t mode;
    const uint8_t *sha256;
    const uint8_t *payload;
    size_t payload_bytes;
};

struct package_manager_relation_view {
    const struct package_manager_package_view *package;
    uint32_t package_index;
    struct package_manager_text identifier;
    struct package_manager_text constraint;
};

enum package_manager_plan_operation {
    PACKAGE_MANAGER_PLAN_INSTALL = 1,
    PACKAGE_MANAGER_PLAN_UPDATE = 2,
    PACKAGE_MANAGER_PLAN_REMOVE = 3,
    PACKAGE_MANAGER_PLAN_REPAIR = 4
};

struct package_manager_plan_item {
    uint32_t source_index;
    struct package_manager_text identifier;
    struct package_manager_text version;
    struct package_manager_text download_path;
    uint64_t package_bytes;
    const uint8_t *package_sha256;
    const uint8_t *publisher_key_id;
};

struct package_manager_plan {
    enum package_manager_plan_operation operation;
    struct package_manager_text target;
    struct package_manager_text root;
    uint32_t count;
    struct package_manager_plan_item items[PACKAGE_MANAGER_PLAN_MAX_PACKAGES];
};

/*
 * One exact dependency edge for an item in an authenticated install/update
 * plan.  The provider is the unique package identity selected for requested;
 * it may already be installed and therefore need no plan item of its own.
 */
struct package_manager_plan_binding {
    const struct package_manager_plan *plan;
    uint32_t plan_index;
    struct package_manager_text requested;
    struct package_manager_text constraint;
    struct package_manager_text provider;
};

struct package_manager_search_results {
    uint32_t count;
    uint32_t repository_indices[PACKAGE_MANAGER_SEARCH_MAX_RESULTS];
};

enum package_manager_status package_manager_repository_open(
    const uint8_t *bytes,
    size_t byte_count,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust,
    struct package_manager_repository_view *result
);

enum package_manager_status package_manager_repository_entry(
    const struct package_manager_repository_view *repository,
    uint32_t index,
    struct package_manager_catalog_entry *result
);

enum package_manager_status package_manager_repository_search(
    const struct package_manager_repository_view *repository,
    const uint8_t *query,
    size_t query_bytes,
    struct package_manager_search_results *result
);

enum package_manager_status package_manager_package_open(
    const uint8_t *bytes,
    size_t byte_count,
    const struct package_manager_catalog_entry *expected,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust,
    struct package_manager_package_view *result
);

/*
 * Revalidates the selected record and file digest before exposing a payload
 * slice.  It does not independently authenticate a fabricated package view;
 * callers must use a view returned by package_manager_package_open().
 */
enum package_manager_status package_manager_package_file(
    const struct package_manager_package_view *package,
    uint32_t index,
    struct package_manager_file_view *result
);

enum package_manager_status package_manager_package_dependency(
    const struct package_manager_package_view *package,
    uint32_t index,
    struct package_manager_relation_view *result
);

enum package_manager_status package_manager_package_conflict(
    const struct package_manager_package_view *package,
    uint32_t index,
    struct package_manager_relation_view *result
);

/*
 * Produces dependency-first download/install order.  The resolver is bounded,
 * deterministic, rejects ambiguous providers and backtracks over versions.
 * An installed database is optional for a fresh install.  A successful
 * zero-item install promotes an already-present automatic dependency to an
 * explicit root without downloading its unchanged bytes.  target is the exact
 * requested identity; root is its uniquely selected provider package.
 */
enum package_manager_status package_manager_plan_install(
    const struct package_manager_repository_view *repository,
    const struct package_state_database_view *installed,
    const uint8_t *identifier,
    size_t identifier_bytes,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust,
    struct package_manager_plan *result
);

/*
 * Revalidates the selected plan item against repository and exposes one
 * dependency-to-provider binding.  Both inputs must be the immutable objects
 * used for and returned by package_manager_plan_install().
 */
enum package_manager_status package_manager_plan_dependency_binding(
    const struct package_manager_repository_view *repository,
    const struct package_manager_plan *plan,
    uint32_t plan_index,
    uint32_t dependency_index,
    struct package_manager_plan_binding *result
);

/*
 * Removes the named explicit root and any dependencies no longer reachable
 * from another explicit root.  A still-reachable target is refused as in use.
 */
enum package_manager_status package_manager_plan_remove(
    const struct package_state_database_view *installed,
    const uint8_t *identifier,
    size_t identifier_bytes,
    struct package_manager_plan *result
);

enum package_manager_status package_manager_installed_search(
    const struct package_state_database_view *installed,
    const uint8_t *query,
    size_t query_bytes,
    struct package_manager_search_results *result
);

const char *package_manager_status_string(enum package_manager_status status);

#endif
