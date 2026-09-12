/* SPDX-License-Identifier: GPL-3.0-only */
#include <stddef.h>
#include <stdint.h>

#include <phipia/package_generation.h>
#include <phipia/package_state.h>

static void clear_bytes(uint8_t *destination, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        destination[index] = 0U;
    }
}

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool equal_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t difference = 0U;

    if (left == NULL || right == NULL) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static bool equal_text(
    const struct package_state_text *left,
    const struct package_state_text *right
)
{
    return left->length == right->length &&
        (left->length == 0U || equal_bytes(left->bytes, right->bytes,
            left->length));
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    for (size_t index = 0U; index < 4U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void write_u64(uint8_t *bytes, uint64_t value)
{
    for (size_t index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static bool checked_add(size_t left, size_t right, size_t *result)
{
    if (right > SIZE_MAX - left) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool checked_multiply(size_t left, size_t right, size_t *result)
{
    if (left != 0U && right > SIZE_MAX / left) {
        return false;
    }
    *result = left * right;
    return true;
}

static bool copy_text(
    uint8_t *destination,
    size_t width,
    const struct package_state_text *text,
    bool allow_empty
)
{
    if (text == NULL || text->length >= width ||
        (!allow_empty && text->length == 0U) ||
        (text->bytes == NULL && text->length != 0U)) {
        return false;
    }
    if (text->length != 0U) {
        copy_bytes(destination, text->bytes, text->length);
    }
    return true;
}

enum package_state_status package_generation_size(
    const struct package_generation_spec *spec,
    size_t *result
)
{
    size_t package_bytes;
    size_t dependency_bytes;
    size_t file_bytes;
    size_t total;

    if (spec == NULL || result == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    *result = 0U;
    if (spec->package_count > PACKAGE_STATE_DATABASE_MAX_PACKAGES ||
        spec->dependency_count > PACKAGE_STATE_DATABASE_MAX_EDGES ||
        spec->file_count > PACKAGE_STATE_DATABASE_MAX_FILES) {
        return PACKAGE_STATE_STATUS_TABLE;
    }
    if (!checked_multiply(spec->package_count,
            PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES, &package_bytes) ||
        !checked_multiply(spec->dependency_count,
            PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES, &dependency_bytes) ||
        !checked_multiply(spec->file_count,
            PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES, &file_bytes) ||
        !checked_add(PACKAGE_STATE_DATABASE_HEADER_BYTES, package_bytes,
            &total) ||
        !checked_add(total, dependency_bytes, &total) ||
        !checked_add(total, file_bytes, &total)) {
        return PACKAGE_STATE_STATUS_OVERFLOW;
    }
    if (total > PACKAGE_STATE_DATABASE_MAX_BYTES) {
        return PACKAGE_STATE_STATUS_LENGTH;
    }
    *result = total;
    return PACKAGE_STATE_STATUS_OK;
}

static enum package_state_status encode_packages(
    const struct package_generation_spec *spec,
    uint8_t *bytes
)
{
    for (uint32_t index = 0U; index < spec->package_count; ++index) {
        const struct package_generation_package *package =
            &spec->packages[index];
        uint8_t *record = bytes +
            (size_t)index * PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES;

        if (package->package_sha256 == NULL ||
            package->publisher_key_id == NULL) {
            return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
        }
        if (!copy_text(record, 64U, &package->identifier, false) ||
            !copy_text(record + 64U, 64U, &package->version, false)) {
            return PACKAGE_STATE_STATUS_PACKAGE;
        }
        copy_bytes(record + 128U, package->package_sha256,
            PACKAGE_STATE_SHA256_BYTES);
        copy_bytes(record + 160U, package->publisher_key_id,
            PACKAGE_STATE_SHA256_BYTES);
        write_u32(record + 192U, package->explicit_root ? UINT32_C(1) : 0U);
        write_u32(record + 196U, package->dependency_start);
        write_u32(record + 200U, package->dependency_count);
        write_u32(record + 204U, package->file_count);
    }
    return PACKAGE_STATE_STATUS_OK;
}

static enum package_state_status encode_dependencies(
    const struct package_generation_spec *spec,
    uint8_t *bytes
)
{
    for (uint32_t index = 0U; index < spec->dependency_count; ++index) {
        const struct package_generation_dependency *dependency =
            &spec->dependencies[index];
        uint8_t *record = bytes +
            (size_t)index * PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES;

        if (!copy_text(record, 64U, &dependency->requested, false) ||
            !copy_text(record + 64U, 56U, &dependency->constraint, false) ||
            !copy_text(record + 120U, 64U, &dependency->provider, false)) {
            return PACKAGE_STATE_STATUS_DEPENDENCY;
        }
    }
    return PACKAGE_STATE_STATUS_OK;
}

static enum package_state_status encode_files(
    const struct package_generation_spec *spec,
    uint8_t *bytes
)
{
    for (uint32_t index = 0U; index < spec->file_count; ++index) {
        const struct package_generation_file *file = &spec->files[index];
        uint8_t *record = bytes +
            (size_t)index * PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES;

        if (file->sha256 == NULL) {
            return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
        }
        if (!copy_text(record, 128U, &file->path, false) ||
            !copy_text(record + 184U, 64U, &file->soname, true)) {
            return PACKAGE_STATE_STATUS_FILE;
        }
        write_u32(record + 128U, file->owner_index);
        write_u16(record + 132U, file->kind);
        write_u32(record + 136U, file->mode);
        write_u64(record + 144U, file->length);
        copy_bytes(record + 152U, file->sha256, PACKAGE_STATE_SHA256_BYTES);
    }
    return PACKAGE_STATE_STATUS_OK;
}

enum package_state_status package_generation_encode(
    const struct package_generation_spec *spec,
    uint8_t *result,
    size_t result_bytes,
    struct package_state_database_view *view
)
{
    static const uint8_t magic[8] = {
        'P', 'H', 'I', 'P', 'D', 'B', '0', '1'
    };
    static const uint8_t architecture[16] = {
        'x', '8', '6', '_', '6', '4', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    size_t expected_bytes;
    size_t dependency_offset;
    size_t file_offset;
    enum package_state_status status;

    if (result == NULL || view == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    *view = (struct package_state_database_view){ 0 };
    if (result_bytes > PACKAGE_STATE_DATABASE_MAX_BYTES) {
        return PACKAGE_STATE_STATUS_LENGTH;
    }
    clear_bytes(result, result_bytes);
    status = package_generation_size(spec, &expected_bytes);
    if (status != PACKAGE_STATE_STATUS_OK) {
        return status;
    }
    if (result_bytes != expected_bytes) {
        return PACKAGE_STATE_STATUS_LENGTH;
    }
    if (spec->generation == 0U || spec->abi == 0U) {
        return PACKAGE_STATE_STATUS_HEADER;
    }
    if ((spec->packages == NULL && spec->package_count != 0U) ||
        (spec->dependencies == NULL && spec->dependency_count != 0U) ||
        (spec->files == NULL && spec->file_count != 0U)) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    dependency_offset = PACKAGE_STATE_DATABASE_HEADER_BYTES +
        (size_t)spec->package_count *
            PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES;
    file_offset = dependency_offset + (size_t)spec->dependency_count *
        PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES;
    copy_bytes(result, magic, sizeof(magic));
    write_u16(result + 8U, UINT16_C(1));
    write_u16(result + 10U, PACKAGE_STATE_DATABASE_HEADER_BYTES);
    write_u64(result + 16U, result_bytes);
    write_u64(result + 24U, spec->generation);
    write_u32(result + 32U, spec->abi);
    copy_bytes(result + 40U, architecture, sizeof(architecture));
    write_u64(result + 56U, PACKAGE_STATE_DATABASE_HEADER_BYTES);
    write_u32(result + 64U, spec->package_count);
    write_u32(result + 68U, PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES);
    write_u64(result + 72U, dependency_offset);
    write_u32(result + 80U, spec->dependency_count);
    write_u32(result + 84U, PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES);
    write_u64(result + 88U, file_offset);
    write_u32(result + 96U, spec->file_count);
    write_u32(result + 100U, PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES);
    status = encode_packages(spec,
        result + PACKAGE_STATE_DATABASE_HEADER_BYTES);
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = encode_dependencies(spec, result + dependency_offset);
    }
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = encode_files(spec, result + file_offset);
    }
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = package_state_sha256(
            result + PACKAGE_STATE_DATABASE_HEADER_BYTES,
            result_bytes - PACKAGE_STATE_DATABASE_HEADER_BYTES,
            result + 104U);
    }
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = package_state_database_parse(result, result_bytes, view);
    }
    if (status != PACKAGE_STATE_STATUS_OK) {
        clear_bytes(result, result_bytes);
        *view = (struct package_state_database_view){ 0 };
    }
    return status;
}

enum package_state_status package_generation_verify(
    const struct package_generation_spec *spec,
    const uint8_t *database,
    size_t database_bytes,
    struct package_state_database_view *view
)
{
    struct package_state_database_view parsed;
    size_t expected_bytes;
    enum package_state_status status;

    if (spec == NULL || database == NULL || view == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    *view = (struct package_state_database_view){ 0 };
    status = package_generation_size(spec, &expected_bytes);
    if (status != PACKAGE_STATE_STATUS_OK) {
        return status;
    }
    if (database_bytes != expected_bytes || spec->generation == 0U ||
        spec->abi == 0U ||
        (spec->packages == NULL && spec->package_count != 0U) ||
        (spec->dependencies == NULL && spec->dependency_count != 0U) ||
        (spec->files == NULL && spec->file_count != 0U)) {
        return PACKAGE_STATE_STATUS_MISMATCH;
    }
    status = package_state_database_parse(database, database_bytes, &parsed);
    if (status != PACKAGE_STATE_STATUS_OK) {
        return status;
    }
    if (parsed.generation != spec->generation || parsed.abi != spec->abi ||
        parsed.package_count != spec->package_count ||
        parsed.edge_count != spec->dependency_count ||
        parsed.file_count != spec->file_count) {
        return PACKAGE_STATE_STATUS_MISMATCH;
    }
    for (uint32_t index = 0U; index < spec->package_count; ++index) {
        const struct package_generation_package *expected =
            &spec->packages[index];
        struct package_state_package_view actual;

        status = package_state_database_package(&parsed, index, &actual);
        if (status != PACKAGE_STATE_STATUS_OK) {
            return status;
        }
        if (!equal_text(&actual.identifier, &expected->identifier) ||
            !equal_text(&actual.version, &expected->version) ||
            !equal_bytes(actual.package_sha256, expected->package_sha256,
                PACKAGE_STATE_SHA256_BYTES) ||
            !equal_bytes(actual.publisher_key_id, expected->publisher_key_id,
                PACKAGE_STATE_SHA256_BYTES) ||
            actual.explicit_root != expected->explicit_root ||
            actual.dependency_start != expected->dependency_start ||
            actual.dependency_count != expected->dependency_count ||
            actual.file_count != expected->file_count) {
            return PACKAGE_STATE_STATUS_MISMATCH;
        }
    }
    for (uint32_t index = 0U; index < spec->dependency_count; ++index) {
        const struct package_generation_dependency *expected =
            &spec->dependencies[index];
        struct package_state_dependency_view actual;

        status = package_state_database_dependency(&parsed, index, &actual);
        if (status != PACKAGE_STATE_STATUS_OK) {
            return status;
        }
        if (!equal_text(&actual.requested, &expected->requested) ||
            !equal_text(&actual.constraint, &expected->constraint) ||
            !equal_text(&actual.provider, &expected->provider)) {
            return PACKAGE_STATE_STATUS_MISMATCH;
        }
    }
    for (uint32_t index = 0U; index < spec->file_count; ++index) {
        const struct package_generation_file *expected = &spec->files[index];
        struct package_state_file_view actual;

        status = package_state_database_file(&parsed, index, &actual);
        if (status != PACKAGE_STATE_STATUS_OK) {
            return status;
        }
        if (!equal_text(&actual.path, &expected->path) ||
            actual.owner_index != expected->owner_index ||
            actual.kind != expected->kind || actual.mode != expected->mode ||
            actual.length != expected->length ||
            !equal_bytes(actual.sha256, expected->sha256,
                PACKAGE_STATE_SHA256_BYTES) ||
            !equal_text(&actual.soname, &expected->soname)) {
            return PACKAGE_STATE_STATUS_MISMATCH;
        }
    }
    *view = parsed;
    return PACKAGE_STATE_STATUS_OK;
}
