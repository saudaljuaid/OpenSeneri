/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PACKAGE_STATE_H
#define PHIPIA_PACKAGE_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PACKAGE_STATE_SHA256_BYTES 32U

#define PACKAGE_STATE_DATABASE_HEADER_BYTES 512U
#define PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES 256U
#define PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES 192U
#define PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES 256U
#define PACKAGE_STATE_DATABASE_MAX_BYTES (32U * 1024U * 1024U)
#define PACKAGE_STATE_DATABASE_MAX_PACKAGES 256U
#define PACKAGE_STATE_DATABASE_MAX_EDGES 4096U
#define PACKAGE_STATE_DATABASE_MAX_FILES 4096U

#define PACKAGE_STATE_AUTHORITY_BYTES 128U
#define PACKAGE_STATE_JOURNAL_BYTES 512U

enum package_state_status {
    PACKAGE_STATE_STATUS_OK = 0,
    PACKAGE_STATE_STATUS_NULL_ARGUMENT,
    PACKAGE_STATE_STATUS_LENGTH,
    PACKAGE_STATE_STATUS_MAGIC,
    PACKAGE_STATE_STATUS_HEADER,
    PACKAGE_STATE_STATUS_RESERVED,
    PACKAGE_STATE_STATUS_OVERFLOW,
    PACKAGE_STATE_STATUS_DIGEST,
    PACKAGE_STATE_STATUS_TEXT,
    PACKAGE_STATE_STATUS_ARCHITECTURE,
    PACKAGE_STATE_STATUS_TABLE,
    PACKAGE_STATE_STATUS_PACKAGE,
    PACKAGE_STATE_STATUS_DEPENDENCY,
    PACKAGE_STATE_STATUS_FILE,
    PACKAGE_STATE_STATUS_AUTHORITY,
    PACKAGE_STATE_STATUS_JOURNAL,
    PACKAGE_STATE_STATUS_MISMATCH,
    PACKAGE_STATE_STATUS_INCOMPLETE,
    PACKAGE_STATE_STATUS_SEQUENCE,
    PACKAGE_STATE_STATUS_COUNT
};

struct package_state_sha256_context {
    uint32_t state[8];
    uint64_t byte_count;
    uint8_t block[64];
    size_t block_bytes;
    bool finished;
};

struct package_state_database_view {
    const uint8_t *bytes;
    size_t byte_count;
    uint64_t generation;
    uint32_t abi;
    uint32_t package_count;
    uint32_t edge_count;
    uint32_t file_count;
    size_t package_offset;
    size_t edge_offset;
    size_t file_offset;
};

struct package_state_text {
    const uint8_t *bytes;
    size_t length;
};

struct package_state_package_view {
    const struct package_state_database_view *database;
    uint32_t package_index;
    struct package_state_text identifier;
    struct package_state_text version;
    const uint8_t *package_sha256;
    const uint8_t *publisher_key_id;
    bool explicit_root;
    uint32_t dependency_start;
    uint32_t dependency_count;
    uint32_t file_count;
};

struct package_state_dependency_view {
    const struct package_state_database_view *database;
    uint32_t dependency_index;
    struct package_state_text requested;
    struct package_state_text constraint;
    struct package_state_text provider;
};

struct package_state_file_view {
    const struct package_state_database_view *database;
    uint32_t file_index;
    struct package_state_text path;
    uint32_t owner_index;
    uint16_t kind;
    uint32_t mode;
    uint64_t length;
    const uint8_t *sha256;
    struct package_state_text soname;
};

struct package_state_authority_view {
    uint64_t generation;
    uint64_t database_bytes;
    uint8_t database_sha256[PACKAGE_STATE_SHA256_BYTES];
};

enum package_state_operation {
    PACKAGE_STATE_OPERATION_INVALID = 0,
    PACKAGE_STATE_OPERATION_INSTALL = 1,
    PACKAGE_STATE_OPERATION_UPDATE = 2,
    PACKAGE_STATE_OPERATION_REMOVE = 3,
    PACKAGE_STATE_OPERATION_REPAIR = 4
};

struct package_state_journal_view {
    enum package_state_operation operation;
    uint64_t base_generation;
    uint64_t target_generation;
    uint64_t required_space;
    uint64_t base_database_bytes;
    uint64_t target_database_bytes;
    uint8_t base_database_sha256[PACKAGE_STATE_SHA256_BYTES];
    uint8_t target_database_sha256[PACKAGE_STATE_SHA256_BYTES];
    uint8_t transaction_id[PACKAGE_STATE_SHA256_BYTES];
};

struct package_state_journal_spec {
    enum package_state_operation operation;
    const struct package_state_database_view *base;
    const struct package_state_database_view *target;
    uint64_t required_space;
    const uint8_t *target_identifier;
    size_t target_identifier_bytes;
};

struct package_state_generation {
    const uint8_t *database;
    size_t database_bytes;
    /* Set only after the caller verifies every owned immutable file. */
    bool owned_files_complete;
};

enum package_state_recovery_choice {
    PACKAGE_STATE_RECOVERY_NONE = 0,
    PACKAGE_STATE_RECOVERY_OLD,
    PACKAGE_STATE_RECOVERY_NEW
};

struct package_state_recovery_result {
    enum package_state_recovery_choice choice;
    uint64_t generation;
    struct package_state_database_view database;
};

enum package_state_status package_state_sha256(
    const uint8_t *bytes,
    size_t byte_count,
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES]
);

enum package_state_status package_state_sha256_initialize(
    struct package_state_sha256_context *context
);

enum package_state_status package_state_sha256_update(
    struct package_state_sha256_context *context,
    const uint8_t *bytes,
    size_t byte_count
);

enum package_state_status package_state_sha256_finish(
    struct package_state_sha256_context *context,
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES]
);

enum package_state_status package_state_database_parse(
    const uint8_t *bytes,
    size_t byte_count,
    struct package_state_database_view *result
);

/* Immutable record views over a database returned by the parser. */
enum package_state_status package_state_database_package(
    const struct package_state_database_view *database,
    uint32_t index,
    struct package_state_package_view *result
);

enum package_state_status package_state_database_dependency(
    const struct package_state_database_view *database,
    uint32_t index,
    struct package_state_dependency_view *result
);

enum package_state_status package_state_database_file(
    const struct package_state_database_view *database,
    uint32_t index,
    struct package_state_file_view *result
);

enum package_state_status package_state_authority_parse(
    const uint8_t *bytes,
    size_t byte_count,
    struct package_state_authority_view *result
);

enum package_state_status package_state_journal_parse(
    const uint8_t *bytes,
    size_t byte_count,
    struct package_state_journal_view *result
);

/*
 * Canonical encoders for the transaction records written by a privileged
 * generation service. Inputs are parsed again and output is cleared on every
 * refusal so a partial record cannot be persisted accidentally.
 */
enum package_state_status package_state_authority_encode(
    const struct package_state_database_view *database,
    uint8_t result[PACKAGE_STATE_AUTHORITY_BYTES]
);

enum package_state_status package_state_journal_encode(
    const struct package_state_journal_spec *spec,
    uint8_t result[PACKAGE_STATE_JOURNAL_BYTES]
);

/*
 * With a journal, old/new mean its base/target generation. Without a journal
 * (journal bytes NULL and length zero), either candidate may satisfy authority.
 * The core never chooses a candidate unless its database is canonical, its
 * whole-database digest matches, and owned_files_complete is true.
 */
enum package_state_status package_state_recovery_decide(
    const uint8_t *authority,
    size_t authority_bytes,
    const uint8_t *journal,
    size_t journal_bytes,
    const struct package_state_generation *old_generation,
    const struct package_state_generation *new_generation,
    struct package_state_recovery_result *result
);

const char *package_state_status_string(enum package_state_status status);

#endif
