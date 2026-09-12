/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PACKAGE_GENERATION_H
#define PHIPIA_PACKAGE_GENERATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/package_state.h>

struct package_generation_package {
    struct package_state_text identifier;
    struct package_state_text version;
    const uint8_t *package_sha256;
    const uint8_t *publisher_key_id;
    bool explicit_root;
    uint32_t dependency_start;
    uint32_t dependency_count;
    uint32_t file_count;
};

struct package_generation_dependency {
    struct package_state_text requested;
    struct package_state_text constraint;
    struct package_state_text provider;
};

struct package_generation_file {
    struct package_state_text path;
    uint32_t owner_index;
    uint16_t kind;
    uint32_t mode;
    uint64_t length;
    const uint8_t *sha256;
    struct package_state_text soname;
};

struct package_generation_spec {
    uint64_t generation;
    uint32_t abi;
    const struct package_generation_package *packages;
    uint32_t package_count;
    const struct package_generation_dependency *dependencies;
    uint32_t dependency_count;
    const struct package_generation_file *files;
    uint32_t file_count;
};

enum package_state_status package_generation_size(
    const struct package_generation_spec *spec,
    size_t *result
);

/*
 * Encodes one complete immutable installed database. Input arrays and output
 * must not overlap. A bounded output span and the view are cleared on refusal;
 * overlarge spans are rejected before access. Success is reported only after
 * package_state_database_parse() accepts the bytes.
 */
enum package_state_status package_generation_encode(
    const struct package_generation_spec *spec,
    uint8_t *result,
    size_t result_bytes,
    struct package_state_database_view *view
);

/*
 * Re-parses a canonical encoded database and requires every header, package,
 * provider edge, and owned-file field to match spec exactly.  This lets a
 * filesystem stager bind its source table to the bytes it will persist without
 * allocating a second encoded database.  The view is cleared on refusal.
 */
enum package_state_status package_generation_verify(
    const struct package_generation_spec *spec,
    const uint8_t *database,
    size_t database_bytes,
    struct package_state_database_view *view
);

#endif
