/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PACKAGE_BUILDER_H
#define PHIPIA_PACKAGE_BUILDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/package_generation.h>
#include <phipia/package_manager.h>

struct package_builder_package_bytes {
    const uint8_t *bytes;
    size_t byte_count;
};

struct package_builder_repair_file {
    struct package_state_text path;
    const uint8_t *payload;
    size_t payload_bytes;
};

enum package_builder_file_source_kind {
    PACKAGE_BUILDER_FILE_SOURCE_INVALID = 0,
    PACKAGE_BUILDER_FILE_SOURCE_INSTALLED = 1,
    PACKAGE_BUILDER_FILE_SOURCE_PAYLOAD = 2
};

/*
 * Exact immutable source for one canonically sorted output file.  Installed
 * sources name an old database package/file index and carry no byte slice;
 * payload sources name a verified plan/package file and expose its signed
 * payload slice.  Input buffers must remain live and unchanged while used.
 */
struct package_builder_file_source {
    enum package_builder_file_source_kind kind;
    uint32_t package_index;
    uint32_t file_index;
    const uint8_t *payload;
    size_t payload_bytes;
};

/*
 * Caller-owned bounded workspace.  This object is intentionally large and
 * must be allocated from the privileged service heap, never a syscall stack.
 * It must not overlap any input and must not be copied after success because
 * spec points into its own arrays.
 */
struct package_builder_workspace {
    bool has_installed;
    struct package_manager_repository_view repository;
    struct package_state_database_view installed;
    struct package_manager_plan verified_plan;
    struct package_manager_package_view
        admitted[PACKAGE_MANAGER_PLAN_MAX_PACKAGES];
    struct package_generation_spec spec;
    struct package_generation_package
        packages[PACKAGE_STATE_DATABASE_MAX_PACKAGES];
    struct package_generation_dependency
        dependencies[PACKAGE_STATE_DATABASE_MAX_EDGES];
    struct package_generation_file files[PACKAGE_STATE_DATABASE_MAX_FILES];
    struct package_builder_file_source
        file_sources[PACKAGE_STATE_DATABASE_MAX_FILES];
};

/*
 * Re-authenticates every supplied byte object and recomputes the plan before
 * producing a complete next-generation metadata specification.  Install and
 * update sources are aligned with the dependency-first plan items; removal
 * accepts no repository or package bytes.  Success performs no filesystem I/O
 * and must be followed by package_generation_encode() before persistence.
 */
enum package_manager_status package_builder_build(
    const struct package_manager_repository_view *repository,
    const struct package_state_database_view *installed,
    const struct package_manager_plan *plan,
    const struct package_builder_package_bytes *packages,
    uint32_t package_count,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust,
    struct package_builder_workspace *workspace
);

/*
 * Rebuilds the next generation from authoritative installed metadata.  The
 * sorted replacement list supplies already authenticated bytes for damaged
 * owned paths; every other path remains an individually verified old-generation
 * copy.  Success performs no filesystem I/O.
 */
enum package_manager_status package_builder_repair(
    const struct package_state_database_view *installed,
    const struct package_builder_repair_file *replacements,
    uint32_t replacement_count,
    struct package_builder_workspace *workspace
);

#endif
