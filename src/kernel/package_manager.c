/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/package_manager.h>

#include <limits.h>

#define REPOSITORY_HEADER_BYTES 512U
#define REPOSITORY_PACKAGE_BYTES 512U
#define REPOSITORY_RELATION_BYTES 128U
#define REPOSITORY_SIGNATURE_OFFSET 232U
#define PACKAGE_HEADER_BYTES 512U
#define PACKAGE_FILE_BYTES 256U
#define PACKAGE_RELATION_BYTES 128U
#define PACKAGE_SIGNATURE_OFFSET 440U
#define PACKAGE_MAX_FILE_BYTES (64U * 1024U * 1024U)
#define PACKAGE_CAPABILITY_MASK UINT64_C(0x7ff)
#define SOLVER_MAX_BINDINGS 256U

struct semver {
    struct package_manager_text core[3];
    const uint8_t *prerelease;
    size_t prerelease_bytes;
};

struct binding {
    struct package_manager_text requested;
    struct package_manager_text provider;
};

struct solver {
    const struct package_manager_repository_view *repository;
    uint32_t selected[PACKAGE_MANAGER_PLAN_MAX_PACKAGES];
    uint32_t selected_count;
    struct binding bindings[SOLVER_MAX_BINDINGS];
    uint32_t binding_count;
};

/*
 * Package planning is a privileged, serialized service.  Keep its bounded
 * graph workspace out of the 16 KiB syscall stack and refuse reentrancy.
 */
static struct solver install_solver;
static uint8_t install_order_states[PACKAGE_MANAGER_PLAN_MAX_PACKAGES];
static uint32_t install_order[PACKAGE_MANAGER_PLAN_MAX_PACKAGES];
static bool install_solver_busy;

static const uint8_t repository_magic[8] = {
    'P', 'H', 'I', 'P', 'I', 'D', 'X', '1'
};
static const uint8_t package_magic[8] = {
    'P', 'H', 'I', 'P', 'P', 'K', 'G', '1'
};
static const uint8_t architecture_x86_64[6] = {
    'x', '8', '6', '_', '6', '4'
};

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8U);
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8U) |
        ((uint32_t)bytes[2] << 16U) |
        ((uint32_t)bytes[3] << 24U);
}

static uint64_t read_u64(const uint8_t *bytes)
{
    uint64_t value = 0U;
    for (size_t index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (index * 8U);
    }
    return value;
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t difference = 0U;
    for (size_t index = 0U; index < count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static bool zero_bytes(const uint8_t *bytes, size_t count)
{
    uint8_t combined = 0U;
    for (size_t index = 0U; index < count; ++index) {
        combined |= bytes[index];
    }
    return combined == 0U;
}

static bool add_size(size_t left, size_t right, size_t *result)
{
    if (left > SIZE_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool multiply_size(size_t left, size_t right, size_t *result)
{
    if (left != 0U && right > SIZE_MAX / left) {
        return false;
    }
    *result = left * right;
    return true;
}

static int compare_bytes(
    const uint8_t *left,
    size_t left_bytes,
    const uint8_t *right,
    size_t right_bytes
)
{
    size_t common = left_bytes < right_bytes ? left_bytes : right_bytes;
    for (size_t index = 0U; index < common; ++index) {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
    }
    if (left_bytes == right_bytes) {
        return 0;
    }
    return left_bytes < right_bytes ? -1 : 1;
}

static int compare_text(
    const struct package_manager_text *left,
    const struct package_manager_text *right
)
{
    return compare_bytes(left->bytes, left->length, right->bytes, right->length);
}

static bool text_equal(
    const struct package_manager_text *left,
    const struct package_manager_text *right
)
{
    return compare_text(left, right) == 0;
}

static bool path_ancestor(
    const struct package_manager_text *ancestor,
    const struct package_manager_text *descendant
)
{
    return ancestor->length < descendant->length &&
        descendant->bytes[ancestor->length] == (uint8_t)'/' &&
        compare_bytes(ancestor->bytes, ancestor->length, descendant->bytes,
            ancestor->length) == 0;
}

static enum package_manager_status fixed_text(
    const uint8_t *bytes,
    size_t width,
    bool allow_empty,
    struct package_manager_text *result
)
{
    size_t length = 0U;
    while (length < width && bytes[length] != 0U) {
        if (bytes[length] < UINT8_C(0x20) || bytes[length] > UINT8_C(0x7e)) {
            return PACKAGE_MANAGER_STATUS_TEXT;
        }
        ++length;
    }
    if (length == width || (!allow_empty && length == 0U) ||
        !zero_bytes(bytes + length, width - length)) {
        return PACKAGE_MANAGER_STATUS_TEXT;
    }
    result->bytes = bytes;
    result->length = length;
    return PACKAGE_MANAGER_STATUS_OK;
}

static bool ascii_alphanumeric(uint8_t value)
{
    return (value >= (uint8_t)'0' && value <= (uint8_t)'9') ||
        (value >= (uint8_t)'A' && value <= (uint8_t)'Z') ||
        (value >= (uint8_t)'a' && value <= (uint8_t)'z');
}

static bool package_identifier(const struct package_manager_text *field)
{
    bool previous_separator = true;
    if (field->length == 0U) {
        return false;
    }
    for (size_t index = 0U; index < field->length; ++index) {
        uint8_t value = field->bytes[index];
        bool alphanumeric = (value >= (uint8_t)'a' && value <= (uint8_t)'z') ||
            (value >= (uint8_t)'0' && value <= (uint8_t)'9');
        bool separator = value == (uint8_t)'.' || value == (uint8_t)'-';
        if ((!alphanumeric && !separator) || (separator && previous_separator)) {
            return false;
        }
        previous_separator = separator;
    }
    return !previous_separator;
}

static bool package_path(const struct package_manager_text *field)
{
    size_t component = 0U;
    if (field->length == 0U || field->bytes[0] == (uint8_t)'/' ||
        field->bytes[field->length - 1U] == (uint8_t)'/') {
        return false;
    }
    for (size_t index = 0U; index <= field->length; ++index) {
        if (index == field->length || field->bytes[index] == (uint8_t)'/') {
            size_t count = index - component;
            if (count == 0U || (count == 1U && field->bytes[component] == (uint8_t)'.') ||
                (count == 2U && field->bytes[component] == (uint8_t)'.' &&
                    field->bytes[component + 1U] == (uint8_t)'.')) {
                return false;
            }
            component = index + 1U;
        } else {
            uint8_t value = field->bytes[index];
            if (!ascii_alphanumeric(value) && value != (uint8_t)'.' &&
                value != (uint8_t)'_' && value != (uint8_t)'+' &&
                value != (uint8_t)'-') {
                return false;
            }
        }
    }
    return true;
}

static bool library_name(const struct package_manager_text *field)
{
    if (field->length == 0U ||
        (field->length == 1U && field->bytes[0] == (uint8_t)'.') ||
        (field->length == 2U && field->bytes[0] == (uint8_t)'.' &&
            field->bytes[1] == (uint8_t)'.')) {
        return false;
    }
    for (size_t index = 0U; index < field->length; ++index) {
        uint8_t value = field->bytes[index];
        if (!ascii_alphanumeric(value) && value != (uint8_t)'.' &&
            value != (uint8_t)'_' && value != (uint8_t)'+' &&
            value != (uint8_t)'-') {
            return false;
        }
    }
    return true;
}

static bool parse_uint_decimal(
    const uint8_t *bytes,
    size_t start,
    size_t end,
    bool reject_leading_zero,
    uint64_t *result
)
{
    uint64_t value = 0U;
    if (start == end || (reject_leading_zero && end - start > 1U &&
        bytes[start] == (uint8_t)'0')) {
        return false;
    }
    for (size_t index = start; index < end; ++index) {
        uint8_t digit;
        if (bytes[index] < (uint8_t)'0' || bytes[index] > (uint8_t)'9') {
            return false;
        }
        if (result == NULL) {
            continue;
        }
        digit = (uint8_t)(bytes[index] - (uint8_t)'0');
        if (value > (UINT64_MAX - digit) / UINT64_C(10)) {
            return false;
        }
        value = value * UINT64_C(10) + digit;
    }
    if (result != NULL) {
        *result = value;
    }
    return true;
}

static bool semantic_version(
    const uint8_t *bytes,
    size_t length,
    struct semver *result
)
{
    struct semver parsed = { { { NULL, 0U }, { NULL, 0U }, { NULL, 0U } },
        NULL, 0U };
    size_t cursor = 0U;
    for (size_t part = 0U; part < 3U; ++part) {
        size_t start = cursor;
        while (cursor < length && bytes[cursor] >= (uint8_t)'0' &&
            bytes[cursor] <= (uint8_t)'9') {
            ++cursor;
        }
        if (!parse_uint_decimal(bytes, start, cursor, true, NULL)) {
            return false;
        }
        parsed.core[part].bytes = bytes + start;
        parsed.core[part].length = cursor - start;
        if (part != 2U) {
            if (cursor == length || bytes[cursor] != (uint8_t)'.') {
                return false;
            }
            ++cursor;
        }
    }
    if (cursor < length && bytes[cursor] == (uint8_t)'-') {
        size_t prerelease_start = ++cursor;
        for (;;) {
            size_t start = cursor;
            bool numeric = true;
            while (cursor < length && bytes[cursor] != (uint8_t)'.' &&
                bytes[cursor] != (uint8_t)'+') {
                if (!ascii_alphanumeric(bytes[cursor]) && bytes[cursor] != (uint8_t)'-') {
                    return false;
                }
                if (bytes[cursor] < (uint8_t)'0' || bytes[cursor] > (uint8_t)'9') {
                    numeric = false;
                }
                ++cursor;
            }
            if (start == cursor || (numeric && !parse_uint_decimal(
                bytes, start, cursor, true, NULL))) {
                return false;
            }
            if (cursor == length || bytes[cursor] == (uint8_t)'+') {
                break;
            }
            ++cursor;
        }
        parsed.prerelease = bytes + prerelease_start;
        parsed.prerelease_bytes = cursor - prerelease_start;
    }
    if (cursor < length && bytes[cursor] == (uint8_t)'+') {
        ++cursor;
        for (;;) {
            size_t start = cursor;
            while (cursor < length && bytes[cursor] != (uint8_t)'.') {
                if (!ascii_alphanumeric(bytes[cursor]) && bytes[cursor] != (uint8_t)'-') {
                    return false;
                }
                ++cursor;
            }
            if (start == cursor) {
                return false;
            }
            if (cursor == length) {
                break;
            }
            ++cursor;
        }
    }
    if (cursor != length) {
        return false;
    }
    if (result != NULL) {
        *result = parsed;
    }
    return true;
}

static bool prerelease_part(
    const struct semver *version,
    size_t *cursor,
    const uint8_t **bytes,
    size_t *length,
    bool *numeric
)
{
    size_t start;
    if (*cursor >= version->prerelease_bytes) {
        return false;
    }
    start = *cursor;
    *numeric = true;
    while (*cursor < version->prerelease_bytes &&
        version->prerelease[*cursor] != (uint8_t)'.') {
        uint8_t value = version->prerelease[*cursor];
        if (value < (uint8_t)'0' || value > (uint8_t)'9') {
            *numeric = false;
        }
        ++*cursor;
    }
    *bytes = version->prerelease + start;
    *length = *cursor - start;
    if (*cursor < version->prerelease_bytes) {
        ++*cursor;
    }
    return true;
}

static int compare_semver(const struct semver *left, const struct semver *right)
{
    for (size_t index = 0U; index < 3U; ++index) {
        int comparison;
        if (left->core[index].length != right->core[index].length) {
            return left->core[index].length < right->core[index].length ? -1 : 1;
        }
        comparison = compare_bytes(left->core[index].bytes,
            left->core[index].length, right->core[index].bytes,
            right->core[index].length);
        if (comparison != 0) {
            return comparison;
        }
    }
    if (left->prerelease_bytes == 0U || right->prerelease_bytes == 0U) {
        if (left->prerelease_bytes == right->prerelease_bytes) {
            return 0;
        }
        return left->prerelease_bytes == 0U ? 1 : -1;
    }
    size_t left_cursor = 0U;
    size_t right_cursor = 0U;
    for (;;) {
        const uint8_t *left_part = NULL;
        const uint8_t *right_part = NULL;
        size_t left_length = 0U;
        size_t right_length = 0U;
        bool left_numeric = false;
        bool right_numeric = false;
        bool has_left = prerelease_part(&*left, &left_cursor, &left_part,
            &left_length, &left_numeric);
        bool has_right = prerelease_part(&*right, &right_cursor, &right_part,
            &right_length, &right_numeric);
        int comparison;
        if (!has_left || !has_right) {
            if (has_left == has_right) {
                return 0;
            }
            return has_left ? 1 : -1;
        }
        if (left_numeric != right_numeric) {
            return left_numeric ? -1 : 1;
        }
        if (left_numeric) {
            comparison = left_length == right_length ?
                compare_bytes(left_part, left_length, right_part, right_length) :
                (left_length < right_length ? -1 : 1);
        } else {
            comparison = compare_bytes(left_part, left_length, right_part, right_length);
        }
        if (comparison != 0) {
            return comparison;
        }
    }
}

static int compare_semver_text(
    const struct package_manager_text *left,
    const struct package_manager_text *right
)
{
    struct semver left_value = { 0 };
    struct semver right_value = { 0 };
    if (!semantic_version(left->bytes, left->length, &left_value) ||
        !semantic_version(right->bytes, right->length, &right_value)) {
        return 0;
    }
    return compare_semver(&left_value, &right_value);
}

static bool version_constraint(const struct package_manager_text *field)
{
    size_t cursor = 0U;
    if (field->length == 1U && field->bytes[0] == (uint8_t)'*') {
        return true;
    }
    while (cursor < field->length) {
        size_t start;
        uint8_t operation = field->bytes[cursor++];
        if (operation == (uint8_t)'>' || operation == (uint8_t)'<') {
            if (cursor < field->length && field->bytes[cursor] == (uint8_t)'=') {
                ++cursor;
            }
        } else if (operation != (uint8_t)'=' && operation != (uint8_t)'^' &&
            operation != (uint8_t)'~') {
            return false;
        }
        start = cursor;
        while (cursor < field->length && field->bytes[cursor] != (uint8_t)',') {
            ++cursor;
        }
        if (!semantic_version(field->bytes + start, cursor - start, NULL)) {
            return false;
        }
        if (cursor < field->length && ++cursor == field->length) {
            return false;
        }
    }
    return true;
}

static bool version_satisfies(
    const struct package_manager_text *version_text,
    const struct package_manager_text *constraint
)
{
    struct semver version = { 0 };
    size_t cursor = 0U;
    if (!semantic_version(version_text->bytes, version_text->length, &version)) {
        return false;
    }
    if (constraint->length == 1U && constraint->bytes[0] == (uint8_t)'*') {
        return true;
    }
    while (cursor < constraint->length) {
        uint8_t operation = constraint->bytes[cursor++];
        bool equals = false;
        size_t start;
        struct semver required = { 0 };
        int comparison;
        if ((operation == (uint8_t)'>' || operation == (uint8_t)'<') &&
            cursor < constraint->length && constraint->bytes[cursor] == (uint8_t)'=') {
            equals = true;
            ++cursor;
        }
        start = cursor;
        while (cursor < constraint->length && constraint->bytes[cursor] != (uint8_t)',') {
            ++cursor;
        }
        if (!semantic_version(constraint->bytes + start, cursor - start, &required)) {
            return false;
        }
        comparison = compare_semver(&version, &required);
        if ((operation == (uint8_t)'=' && comparison != 0) ||
            (operation == (uint8_t)'>' && (comparison < 0 || (!equals && comparison == 0))) ||
            (operation == (uint8_t)'<' && (comparison > 0 || (!equals && comparison == 0)))) {
            return false;
        }
        if (operation == (uint8_t)'^' || operation == (uint8_t)'~') {
            if (comparison < 0) {
                return false;
            }
            bool major_zero = required.core[0].length == 1U &&
                required.core[0].bytes[0] == (uint8_t)'0';
            bool minor_zero = required.core[1].length == 1U &&
                required.core[1].bytes[0] == (uint8_t)'0';
            if (compare_bytes(version.core[0].bytes, version.core[0].length,
                    required.core[0].bytes, required.core[0].length) != 0 ||
                ((operation == (uint8_t)'~' ||
                    (operation == (uint8_t)'^' && major_zero)) &&
                    compare_bytes(version.core[1].bytes, version.core[1].length,
                        required.core[1].bytes, required.core[1].length) != 0) ||
                (operation == (uint8_t)'^' && major_zero && minor_zero &&
                    compare_bytes(version.core[2].bytes, version.core[2].length,
                        required.core[2].bytes, required.core[2].length) != 0)) {
                return false;
            }
        }
        if (cursor < constraint->length) {
            ++cursor;
        }
    }
    return true;
}

static bool contains_casefold(
    const struct package_manager_text *text,
    const uint8_t *query,
    size_t query_bytes
)
{
    if (query_bytes == 0U) {
        return true;
    }
    if (query_bytes > text->length) {
        return false;
    }
    for (size_t start = 0U; start <= text->length - query_bytes; ++start) {
        bool match = true;
        for (size_t offset = 0U; offset < query_bytes; ++offset) {
            uint8_t left = text->bytes[start + offset];
            uint8_t right = query[offset];
            if (left >= (uint8_t)'A' && left <= (uint8_t)'Z') {
                left = (uint8_t)(left + ((uint8_t)'a' - (uint8_t)'A'));
            }
            if (right >= (uint8_t)'A' && right <= (uint8_t)'Z') {
                right = (uint8_t)(right + ((uint8_t)'a' - (uint8_t)'A'));
            }
            if (left != right) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static enum package_manager_status verify_signed_object(
    const uint8_t *bytes,
    size_t byte_count,
    size_t signature_offset,
    const uint8_t *key_id,
    const struct package_manager_trust *trust
)
{
    uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES];
    uint8_t derived[PACKAGE_MANAGER_SHA256_BYTES];
    enum package_manager_key_status key_status;
    if (trust == NULL || trust->lookup == NULL || trust->verify == NULL) {
        return PACKAGE_MANAGER_STATUS_CRYPTO_UNAVAILABLE;
    }
    key_status = trust->lookup(trust->context, key_id, public_key);
    if (key_status == PACKAGE_MANAGER_KEY_UNKNOWN) {
        return PACKAGE_MANAGER_STATUS_UNKNOWN_KEY;
    }
    if (key_status == PACKAGE_MANAGER_KEY_REVOKED) {
        return PACKAGE_MANAGER_STATUS_REVOKED_KEY;
    }
    if (key_status != PACKAGE_MANAGER_KEY_TRUSTED ||
        package_state_sha256(public_key, sizeof(public_key), derived) !=
            PACKAGE_STATE_STATUS_OK ||
        !bytes_equal(derived, key_id, sizeof(derived))) {
        return PACKAGE_MANAGER_STATUS_UNKNOWN_KEY;
    }
    if (!trust->verify(trust->context, public_key, bytes + signature_offset,
        bytes, byte_count, signature_offset, PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES)) {
        return PACKAGE_MANAGER_STATUS_SIGNATURE;
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

static const uint8_t *repository_record(
    const struct package_manager_repository_view *repository,
    uint32_t index
)
{
    return repository->bytes + repository->package_offset +
        (size_t)index * REPOSITORY_PACKAGE_BYTES;
}

static enum package_manager_status relation_at(
    const struct package_manager_repository_view *repository,
    uint32_t index,
    bool provide,
    struct package_manager_text *identifier,
    struct package_manager_text *value
)
{
    const uint8_t *record;
    enum package_manager_status status;
    if (index >= repository->relation_count) {
        return PACKAGE_MANAGER_STATUS_TABLE;
    }
    record = repository->bytes + repository->relation_offset +
        (size_t)index * REPOSITORY_RELATION_BYTES;
    status = fixed_text(record, 64U, false, identifier);
    if (status != PACKAGE_MANAGER_STATUS_OK || !package_identifier(identifier)) {
        return PACKAGE_MANAGER_STATUS_DEPENDENCY;
    }
    status = fixed_text(record + 64U, 56U, false, value);
    if (status != PACKAGE_MANAGER_STATUS_OK ||
        (provide ? !semantic_version(value->bytes, value->length, NULL) :
            !version_constraint(value)) || !zero_bytes(record + 120U, 8U)) {
        return PACKAGE_MANAGER_STATUS_DEPENDENCY;
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

enum package_manager_status package_manager_repository_entry(
    const struct package_manager_repository_view *repository,
    uint32_t index,
    struct package_manager_catalog_entry *result
)
{
    const uint8_t *record;
    enum package_manager_status status;
    if (repository == NULL || result == NULL) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    if (repository->bytes == NULL || index >= repository->package_count) {
        return PACKAGE_MANAGER_STATUS_NOT_FOUND;
    }
    record = repository_record(repository, index);
    status = fixed_text(record, 64U, false, &result->identifier);
    if (status != PACKAGE_MANAGER_STATUS_OK) {
        return status;
    }
    status = fixed_text(record + 64U, 64U, false, &result->version);
    if (status != PACKAGE_MANAGER_STATUS_OK) {
        return status;
    }
    status = fixed_text(record + 128U, 128U, false, &result->download_path);
    if (status != PACKAGE_MANAGER_STATUS_OK) {
        return status;
    }
    result->repository_index = index;
    result->repository = repository;
    result->package_bytes = read_u64(record + 256U);
    result->package_sha256 = record + 264U;
    result->publisher_key_id = record + 296U;
    result->dependency_start = read_u32(record + 328U);
    result->dependency_count = read_u32(record + 332U);
    result->conflict_start = read_u32(record + 336U);
    result->conflict_count = read_u32(record + 340U);
    result->provide_start = read_u32(record + 344U);
    result->provide_count = read_u32(record + 348U);
    return PACKAGE_MANAGER_STATUS_OK;
}

enum package_manager_status package_manager_repository_open(
    const uint8_t *bytes,
    size_t byte_count,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust,
    struct package_manager_repository_view *result
)
{
    struct package_manager_repository_view parsed;
    uint8_t digest[PACKAGE_MANAGER_SHA256_BYTES];
    size_t package_bytes;
    size_t relation_bytes;
    size_t expected_relation;
    size_t expected_end;
    struct package_manager_text architecture;
    struct package_manager_text previous_identifier = { NULL, 0U };
    struct package_manager_text previous_version = { NULL, 0U };
    uint32_t relation_cursor = 0U;
    if (bytes == NULL || policy == NULL || result == NULL) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    if (byte_count < REPOSITORY_HEADER_BYTES ||
        byte_count > PACKAGE_MANAGER_REPOSITORY_MAX_BYTES) {
        return PACKAGE_MANAGER_STATUS_LENGTH;
    }
    if (!bytes_equal(bytes, repository_magic, sizeof(repository_magic))) {
        return PACKAGE_MANAGER_STATUS_MAGIC;
    }
    if (read_u16(bytes + 8U) != 1U || read_u16(bytes + 10U) != REPOSITORY_HEADER_BYTES ||
        read_u32(bytes + 12U) != 0U || read_u64(bytes + 16U) != byte_count) {
        return PACKAGE_MANAGER_STATUS_HEADER;
    }
    parsed.bytes = bytes;
    parsed.byte_count = byte_count;
    parsed.repository_version = read_u64(bytes + 24U);
    parsed.generated_at = read_u64(bytes + 32U);
    parsed.expires_at = read_u64(bytes + 40U);
    parsed.abi_min = read_u32(bytes + 48U);
    parsed.abi_max = read_u32(bytes + 52U);
    parsed.package_offset = (size_t)read_u64(bytes + 136U);
    parsed.package_count = read_u32(bytes + 144U);
    parsed.relation_offset = (size_t)read_u64(bytes + 152U);
    parsed.relation_count = read_u32(bytes + 160U);
    if (parsed.repository_version == 0U || parsed.generated_at == 0U ||
        parsed.expires_at == 0U || parsed.generated_at >= parsed.expires_at ||
        parsed.abi_min == 0U || parsed.abi_min > parsed.abi_max) {
        return PACKAGE_MANAGER_STATUS_HEADER;
    }
    if (policy->minimum_repository_version > parsed.repository_version) {
        return PACKAGE_MANAGER_STATUS_ROLLBACK;
    }
    if (policy->now < parsed.generated_at || policy->now >= parsed.expires_at) {
        return PACKAGE_MANAGER_STATUS_FRESHNESS;
    }
    if (policy->abi < parsed.abi_min || policy->abi > parsed.abi_max) {
        return PACKAGE_MANAGER_STATUS_ABI;
    }
    if (fixed_text(bytes + 56U, 16U, false, &architecture) !=
            PACKAGE_MANAGER_STATUS_OK ||
        architecture.length != sizeof(architecture_x86_64) ||
        !bytes_equal(architecture.bytes, architecture_x86_64,
            sizeof(architecture_x86_64))) {
        return PACKAGE_MANAGER_STATUS_ARCHITECTURE;
    }
    if (fixed_text(bytes + 72U, 64U, false, &parsed.identifier) !=
            PACKAGE_MANAGER_STATUS_OK || !package_identifier(&parsed.identifier)) {
        return PACKAGE_MANAGER_STATUS_TEXT;
    }
    if (parsed.package_count == 0U ||
        parsed.package_count > PACKAGE_MANAGER_REPOSITORY_MAX_PACKAGES ||
        read_u32(bytes + 148U) != REPOSITORY_PACKAGE_BYTES ||
        read_u32(bytes + 164U) != REPOSITORY_RELATION_BYTES ||
        !multiply_size(parsed.package_count, REPOSITORY_PACKAGE_BYTES, &package_bytes) ||
        !add_size(REPOSITORY_HEADER_BYTES, package_bytes, &expected_relation) ||
        parsed.package_offset != REPOSITORY_HEADER_BYTES ||
        parsed.relation_offset != expected_relation ||
        !multiply_size(parsed.relation_count, REPOSITORY_RELATION_BYTES, &relation_bytes) ||
        !add_size(parsed.relation_offset, relation_bytes, &expected_end) ||
        expected_end != byte_count ||
        parsed.relation_count > parsed.package_count *
            (3U * PACKAGE_MANAGER_REPOSITORY_MAX_RELATIONS_PER_PACKAGE)) {
        return PACKAGE_MANAGER_STATUS_TABLE;
    }
    if (read_u16(bytes + 296U) != 1U ||
        read_u16(bytes + 298U) != PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES ||
        !zero_bytes(bytes + 300U, 212U) || zero_bytes(bytes + 232U, 64U)) {
        return PACKAGE_MANAGER_STATUS_RESERVED;
    }
    if (package_state_sha256(bytes + REPOSITORY_HEADER_BYTES,
        byte_count - REPOSITORY_HEADER_BYTES, digest) != PACKAGE_STATE_STATUS_OK ||
        !bytes_equal(digest, bytes + 168U, sizeof(digest))) {
        return PACKAGE_MANAGER_STATUS_DIGEST;
    }
    for (uint32_t index = 0U; index < parsed.package_count; ++index) {
        struct package_manager_catalog_entry entry;
        enum package_manager_status status = package_manager_repository_entry(
            &parsed, index, &entry);
        if (status != PACKAGE_MANAGER_STATUS_OK ||
            !package_identifier(&entry.identifier) ||
            !semantic_version(entry.version.bytes, entry.version.length, NULL) ||
            !package_path(&entry.download_path) || entry.package_bytes == 0U ||
            entry.package_bytes > PACKAGE_MANAGER_PACKAGE_MAX_BYTES ||
            entry.dependency_count > PACKAGE_MANAGER_REPOSITORY_MAX_RELATIONS_PER_PACKAGE ||
            entry.conflict_count > PACKAGE_MANAGER_REPOSITORY_MAX_RELATIONS_PER_PACKAGE ||
            entry.provide_count > PACKAGE_MANAGER_REPOSITORY_MAX_RELATIONS_PER_PACKAGE ||
            entry.dependency_start != relation_cursor ||
            entry.conflict_start != relation_cursor + entry.dependency_count ||
            entry.provide_start != relation_cursor + entry.dependency_count +
                entry.conflict_count || !zero_bytes(repository_record(&parsed, index) + 352U, 160U)) {
            return PACKAGE_MANAGER_STATUS_PACKAGE;
        }
        if (index != 0U) {
            int identity = compare_text(&previous_identifier, &entry.identifier);
            if (identity > 0 || (identity == 0 &&
                compare_text(&previous_version, &entry.version) >= 0)) {
                return PACKAGE_MANAGER_STATUS_PACKAGE;
            }
            if (identity == 0 && compare_semver_text(&previous_version,
                &entry.version) == 0) {
                return PACKAGE_MANAGER_STATUS_AMBIGUOUS_PROVIDER;
            }
        }
        previous_identifier = entry.identifier;
        previous_version = entry.version;
        for (uint32_t group = 0U; group < 3U; ++group) {
            uint32_t first = group == 0U ? entry.dependency_start :
                (group == 1U ? entry.conflict_start : entry.provide_start);
            uint32_t count = group == 0U ? entry.dependency_count :
                (group == 1U ? entry.conflict_count : entry.provide_count);
            struct package_manager_text previous = { NULL, 0U };
            for (uint32_t offset = 0U; offset < count; ++offset) {
                struct package_manager_text identifier;
                struct package_manager_text value;
                status = relation_at(&parsed, first + offset, group == 2U,
                    &identifier, &value);
                if (status != PACKAGE_MANAGER_STATUS_OK ||
                    (offset != 0U && compare_text(&previous, &identifier) >= 0) ||
                    (group == 2U && text_equal(&identifier, &entry.identifier))) {
                    return PACKAGE_MANAGER_STATUS_DEPENDENCY;
                }
                previous = identifier;
                if (group == 0U) {
                    for (uint32_t conflict = 0U; conflict < entry.conflict_count;
                        ++conflict) {
                        struct package_manager_text conflict_id;
                        struct package_manager_text ignored;
                        if (relation_at(&parsed, entry.conflict_start + conflict, false,
                            &conflict_id, &ignored) != PACKAGE_MANAGER_STATUS_OK ||
                            text_equal(&identifier, &conflict_id)) {
                            return PACKAGE_MANAGER_STATUS_DEPENDENCY;
                        }
                    }
                }
            }
        }
        relation_cursor += entry.dependency_count + entry.conflict_count +
            entry.provide_count;
        if (relation_cursor > parsed.relation_count) {
            return PACKAGE_MANAGER_STATUS_TABLE;
        }
        for (uint32_t earlier = 0U; earlier < index; ++earlier) {
            struct package_manager_catalog_entry other;
            (void)package_manager_repository_entry(&parsed, earlier, &other);
            if (text_equal(&entry.download_path, &other.download_path)) {
                return PACKAGE_MANAGER_STATUS_PACKAGE;
            }
        }
    }
    if (relation_cursor != parsed.relation_count) {
        return PACKAGE_MANAGER_STATUS_TABLE;
    }
    enum package_manager_status status = verify_signed_object(bytes, byte_count,
        REPOSITORY_SIGNATURE_OFFSET, bytes + 200U, trust);
    if (status != PACKAGE_MANAGER_STATUS_OK) {
        return status;
    }
    *result = parsed;
    return PACKAGE_MANAGER_STATUS_OK;
}

enum package_manager_status package_manager_repository_search(
    const struct package_manager_repository_view *repository,
    const uint8_t *query,
    size_t query_bytes,
    struct package_manager_search_results *result
)
{
    if (repository == NULL || result == NULL ||
        (query == NULL && query_bytes != 0U)) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    result->count = 0U;
    for (uint32_t index = 0U; index < repository->package_count; ++index) {
        struct package_manager_catalog_entry entry;
        enum package_manager_status status = package_manager_repository_entry(
            repository, index, &entry);
        if (status != PACKAGE_MANAGER_STATUS_OK) {
            return status;
        }
        if (contains_casefold(&entry.identifier, query, query_bytes)) {
            if (result->count == PACKAGE_MANAGER_SEARCH_MAX_RESULTS) {
                return PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
            }
            result->repository_indices[result->count++] = index;
        }
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

static enum package_manager_status package_relation_at(
    const uint8_t *bytes,
    size_t table_offset,
    uint32_t count,
    uint32_t index,
    struct package_manager_text *identifier,
    struct package_manager_text *constraint
)
{
    const uint8_t *record;
    enum package_manager_status status;
    if (index >= count) {
        return PACKAGE_MANAGER_STATUS_TABLE;
    }
    record = bytes + table_offset + (size_t)index * PACKAGE_RELATION_BYTES;
    status = fixed_text(record, 64U, false, identifier);
    if (status != PACKAGE_MANAGER_STATUS_OK || !package_identifier(identifier)) {
        return PACKAGE_MANAGER_STATUS_DEPENDENCY;
    }
    status = fixed_text(record + 64U, 56U, false, constraint);
    if (status != PACKAGE_MANAGER_STATUS_OK || !version_constraint(constraint) ||
        !zero_bytes(record + 120U, 8U)) {
        return PACKAGE_MANAGER_STATUS_DEPENDENCY;
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

static enum package_manager_status package_relation_view(
    const struct package_manager_package_view *package,
    size_t table_offset,
    uint32_t count,
    uint32_t index,
    struct package_manager_relation_view *result
)
{
    struct package_manager_relation_view parsed;
    enum package_manager_status status;
    size_t record_offset;
    if (package == NULL || result == NULL) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    if (package->bytes == NULL || count >
            PACKAGE_MANAGER_REPOSITORY_MAX_RELATIONS_PER_PACKAGE ||
        index >= count || table_offset > package->byte_count ||
        !multiply_size(index, PACKAGE_RELATION_BYTES, &record_offset) ||
        record_offset > package->byte_count - table_offset ||
        PACKAGE_RELATION_BYTES >
            package->byte_count - table_offset - record_offset) {
        return PACKAGE_MANAGER_STATUS_TABLE;
    }
    status = package_relation_at(package->bytes, table_offset, count, index,
        &parsed.identifier, &parsed.constraint);
    if (status != PACKAGE_MANAGER_STATUS_OK) {
        return status;
    }
    parsed.package = package;
    parsed.package_index = index;
    *result = parsed;
    return PACKAGE_MANAGER_STATUS_OK;
}

enum package_manager_status package_manager_package_dependency(
    const struct package_manager_package_view *package,
    uint32_t index,
    struct package_manager_relation_view *result
)
{
    if (package == NULL) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    return package_relation_view(package, package->dependency_offset,
        package->dependency_count, index, result);
}

enum package_manager_status package_manager_package_conflict(
    const struct package_manager_package_view *package,
    uint32_t index,
    struct package_manager_relation_view *result
)
{
    if (package == NULL) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    return package_relation_view(package, package->conflict_offset,
        package->conflict_count, index, result);
}

enum package_manager_status package_manager_package_file(
    const struct package_manager_package_view *package,
    uint32_t index,
    struct package_manager_file_view *result
)
{
    struct package_manager_file_view parsed;
    const uint8_t *record;
    uint8_t digest[PACKAGE_MANAGER_SHA256_BYTES];
    uint16_t kind;
    uint16_t flags;
    uint64_t payload_offset;
    uint64_t payload_bytes;
    size_t record_offset;
    if (package == NULL || result == NULL) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    if (package->bytes == NULL || package->file_count == 0U ||
        package->file_count > PACKAGE_MANAGER_PACKAGE_MAX_FILES ||
        index >= package->file_count || package->file_offset >
            package->byte_count ||
        !multiply_size(index, PACKAGE_FILE_BYTES, &record_offset) ||
        record_offset > package->byte_count - package->file_offset ||
        PACKAGE_FILE_BYTES >
            package->byte_count - package->file_offset - record_offset) {
        return PACKAGE_MANAGER_STATUS_TABLE;
    }
    record = package->bytes + package->file_offset + record_offset;
    kind = read_u16(record + 128U);
    flags = read_u16(record + 130U);
    parsed.mode = read_u32(record + 132U);
    payload_offset = read_u64(record + 136U);
    payload_bytes = read_u64(record + 144U);
    if (fixed_text(record, 128U, false, &parsed.path) !=
            PACKAGE_MANAGER_STATUS_OK ||
        !package_path(&parsed.path) || kind < PACKAGE_MANAGER_FILE_EXECUTABLE ||
        kind > PACKAGE_MANAGER_FILE_FONT || flags != 0U ||
        (parsed.mode != 0444U && parsed.mode != 0555U) ||
        (kind == PACKAGE_MANAGER_FILE_EXECUTABLE && parsed.mode != 0555U) ||
        payload_bytes == 0U || payload_bytes > PACKAGE_MAX_FILE_BYTES ||
        payload_offset > SIZE_MAX || payload_bytes > SIZE_MAX ||
        payload_offset < package->payload_offset ||
        (size_t)payload_offset > package->byte_count ||
        (size_t)payload_bytes > package->byte_count - (size_t)payload_offset ||
        fixed_text(record + 184U, 64U, true, &parsed.soname) !=
            PACKAGE_MANAGER_STATUS_OK ||
        (kind == PACKAGE_MANAGER_FILE_LIBRARY ?
            !library_name(&parsed.soname) : parsed.soname.length != 0U) ||
        !zero_bytes(record + 248U, 8U)) {
        return PACKAGE_MANAGER_STATUS_PACKAGE;
    }
    parsed.package = package;
    parsed.package_index = index;
    parsed.kind = (enum package_manager_file_kind)kind;
    parsed.sha256 = record + 152U;
    parsed.payload = package->bytes + (size_t)payload_offset;
    parsed.payload_bytes = (size_t)payload_bytes;
    if (package_state_sha256(parsed.payload, parsed.payload_bytes, digest) !=
            PACKAGE_STATE_STATUS_OK ||
        !bytes_equal(digest, parsed.sha256, sizeof(digest))) {
        return PACKAGE_MANAGER_STATUS_DIGEST;
    }
    *result = parsed;
    return PACKAGE_MANAGER_STATUS_OK;
}

enum package_manager_status package_manager_package_open(
    const uint8_t *bytes,
    size_t byte_count,
    const struct package_manager_catalog_entry *expected,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust,
    struct package_manager_package_view *result
)
{
    struct package_manager_package_view parsed;
    struct package_manager_text architecture;
    uint8_t digest[PACKAGE_MANAGER_SHA256_BYTES];
    size_t file_table_bytes;
    size_t dependency_table_bytes;
    size_t conflict_table_bytes;
    size_t expected_dependency;
    size_t expected_conflict;
    size_t expected_payload;
    uint64_t payload_bytes;
    uint64_t expected_file_payload;
    if (bytes == NULL || expected == NULL || policy == NULL || result == NULL) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    if (byte_count < PACKAGE_HEADER_BYTES ||
        byte_count > PACKAGE_MANAGER_PACKAGE_MAX_BYTES ||
        expected->package_bytes != byte_count) {
        return PACKAGE_MANAGER_STATUS_LENGTH;
    }
    if (!bytes_equal(bytes, package_magic, sizeof(package_magic))) {
        return PACKAGE_MANAGER_STATUS_MAGIC;
    }
    if (package_state_sha256(bytes, byte_count, digest) != PACKAGE_STATE_STATUS_OK ||
        !bytes_equal(digest, expected->package_sha256, sizeof(digest))) {
        return PACKAGE_MANAGER_STATUS_DIGEST;
    }
    if (read_u16(bytes + 8U) != 3U || read_u16(bytes + 10U) != PACKAGE_HEADER_BYTES ||
        read_u32(bytes + 12U) != 0U || read_u64(bytes + 16U) != byte_count) {
        return PACKAGE_MANAGER_STATUS_HEADER;
    }
    parsed.bytes = bytes;
    parsed.byte_count = byte_count;
    parsed.file_offset = (size_t)read_u64(bytes + 24U);
    parsed.file_count = read_u32(bytes + 32U);
    parsed.dependency_offset = (size_t)read_u64(bytes + 40U);
    parsed.dependency_count = read_u32(bytes + 48U);
    parsed.conflict_offset = (size_t)read_u64(bytes + 56U);
    parsed.conflict_count = read_u32(bytes + 64U);
    parsed.payload_offset = (size_t)read_u64(bytes + 72U);
    payload_bytes = read_u64(bytes + 80U);
    parsed.abi_min = read_u32(bytes + 88U);
    parsed.abi_max = read_u32(bytes + 92U);
    if (parsed.file_count == 0U || parsed.file_count > PACKAGE_MANAGER_PACKAGE_MAX_FILES ||
        parsed.dependency_count > PACKAGE_MANAGER_REPOSITORY_MAX_RELATIONS_PER_PACKAGE ||
        parsed.conflict_count > PACKAGE_MANAGER_REPOSITORY_MAX_RELATIONS_PER_PACKAGE ||
        read_u32(bytes + 36U) != PACKAGE_FILE_BYTES ||
        read_u32(bytes + 52U) != PACKAGE_RELATION_BYTES ||
        read_u32(bytes + 68U) != PACKAGE_RELATION_BYTES ||
        !multiply_size(parsed.file_count, PACKAGE_FILE_BYTES, &file_table_bytes) ||
        !add_size(PACKAGE_HEADER_BYTES, file_table_bytes, &expected_dependency) ||
        !multiply_size(parsed.dependency_count, PACKAGE_RELATION_BYTES,
            &dependency_table_bytes) ||
        !add_size(expected_dependency, dependency_table_bytes, &expected_conflict) ||
        !multiply_size(parsed.conflict_count, PACKAGE_RELATION_BYTES,
            &conflict_table_bytes) ||
        !add_size(expected_conflict, conflict_table_bytes, &expected_payload) ||
        parsed.file_offset != PACKAGE_HEADER_BYTES ||
        parsed.dependency_offset != expected_dependency ||
        parsed.conflict_offset != expected_conflict ||
        parsed.payload_offset != expected_payload ||
        parsed.payload_offset > byte_count || payload_bytes != byte_count - parsed.payload_offset) {
        return PACKAGE_MANAGER_STATUS_TABLE;
    }
    if (parsed.abi_min == 0U || parsed.abi_min > parsed.abi_max) {
        return PACKAGE_MANAGER_STATUS_HEADER;
    }
    if (policy->abi < parsed.abi_min || policy->abi > parsed.abi_max) {
        return PACKAGE_MANAGER_STATUS_ABI;
    }
    if (fixed_text(bytes + 96U, 16U, false, &architecture) !=
            PACKAGE_MANAGER_STATUS_OK ||
        architecture.length != sizeof(architecture_x86_64) ||
        !bytes_equal(architecture.bytes, architecture_x86_64,
            sizeof(architecture_x86_64))) {
        return PACKAGE_MANAGER_STATUS_ARCHITECTURE;
    }
    if (fixed_text(bytes + 112U, 64U, false, &parsed.identifier) !=
            PACKAGE_MANAGER_STATUS_OK || !package_identifier(&parsed.identifier) ||
        fixed_text(bytes + 176U, 64U, false, &parsed.name) !=
            PACKAGE_MANAGER_STATUS_OK ||
        fixed_text(bytes + 240U, 64U, false, &parsed.version) !=
            PACKAGE_MANAGER_STATUS_OK ||
        !semantic_version(parsed.version.bytes, parsed.version.length, NULL) ||
        fixed_text(bytes + 304U, 64U, false, &parsed.publisher) !=
            PACKAGE_MANAGER_STATUS_OK) {
        return PACKAGE_MANAGER_STATUS_TEXT;
    }
    parsed.capabilities = read_u64(bytes + 368U);
    if ((parsed.capabilities & ~PACKAGE_CAPABILITY_MASK) != 0U ||
        !text_equal(&parsed.identifier, &expected->identifier) ||
        !text_equal(&parsed.version, &expected->version) ||
        !bytes_equal(bytes + 408U, expected->publisher_key_id,
            PACKAGE_MANAGER_SHA256_BYTES)) {
        return PACKAGE_MANAGER_STATUS_PACKAGE;
    }
    if (package_state_sha256(bytes + PACKAGE_HEADER_BYTES,
        byte_count - PACKAGE_HEADER_BYTES, digest) != PACKAGE_STATE_STATUS_OK ||
        !bytes_equal(digest, bytes + 376U, sizeof(digest))) {
        return PACKAGE_MANAGER_STATUS_DIGEST;
    }
    if (read_u16(bytes + 504U) != 1U ||
        read_u16(bytes + 506U) != PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES ||
        !zero_bytes(bytes + 508U, 4U) || zero_bytes(bytes + 440U, 64U)) {
        return PACKAGE_MANAGER_STATUS_RESERVED;
    }
    for (uint32_t group = 0U; group < 2U; ++group) {
        uint32_t count = group == 0U ? parsed.dependency_count : parsed.conflict_count;
        uint32_t repository_first = group == 0U ? expected->dependency_start :
            expected->conflict_start;
        uint32_t repository_count = group == 0U ? expected->dependency_count :
            expected->conflict_count;
        struct package_manager_text previous = { NULL, 0U };
        if (count != repository_count) {
            return PACKAGE_MANAGER_STATUS_DEPENDENCY;
        }
        for (uint32_t index = 0U; index < count; ++index) {
            struct package_manager_relation_view relation;
            struct package_manager_text expected_identifier;
            struct package_manager_text expected_constraint;
            enum package_manager_status status = group == 0U ?
                package_manager_package_dependency(&parsed, index, &relation) :
                package_manager_package_conflict(&parsed, index, &relation);
            if (status != PACKAGE_MANAGER_STATUS_OK ||
                (index != 0U && compare_text(&previous, &relation.identifier) >= 0) ||
                expected->repository == NULL ||
                relation_at(expected->repository, repository_first + index, false,
                    &expected_identifier, &expected_constraint) !=
                    PACKAGE_MANAGER_STATUS_OK) {
                return PACKAGE_MANAGER_STATUS_DEPENDENCY;
            }
            previous = relation.identifier;
            if (!text_equal(&relation.identifier, &expected_identifier) ||
                !text_equal(&relation.constraint, &expected_constraint)) {
                return PACKAGE_MANAGER_STATUS_DEPENDENCY;
            }
            if (group == 0U) {
                for (uint32_t conflict = 0U; conflict < parsed.conflict_count;
                    ++conflict) {
                    struct package_manager_relation_view conflict_relation;
                    if (package_manager_package_conflict(&parsed, conflict,
                        &conflict_relation) !=
                            PACKAGE_MANAGER_STATUS_OK ||
                        text_equal(&relation.identifier,
                            &conflict_relation.identifier)) {
                        return PACKAGE_MANAGER_STATUS_DEPENDENCY;
                    }
                }
            }
        }
    }
    expected_file_payload = parsed.payload_offset;
    struct package_manager_text previous_path = { NULL, 0U };
    for (uint32_t index = 0U; index < parsed.file_count; ++index) {
        struct package_manager_file_view file;
        enum package_manager_status status = package_manager_package_file(
            &parsed, index, &file);
        if (status != PACKAGE_MANAGER_STATUS_OK) {
            return status;
        }
        if ((index != 0U && (compare_text(&previous_path, &file.path) >= 0 ||
                path_ancestor(&previous_path, &file.path))) ||
            (size_t)(file.payload - bytes) != expected_file_payload) {
            return PACKAGE_MANAGER_STATUS_PACKAGE;
        }
        previous_path = file.path;
        expected_file_payload += file.payload_bytes;
    }
    if (expected_file_payload != byte_count) {
        return PACKAGE_MANAGER_STATUS_TABLE;
    }
    enum package_manager_status status = verify_signed_object(bytes, byte_count,
        PACKAGE_SIGNATURE_OFFSET, bytes + 408U, trust);
    if (status != PACKAGE_MANAGER_STATUS_OK) {
        return status;
    }
    *result = parsed;
    return PACKAGE_MANAGER_STATUS_OK;
}

static bool entry_provides(
    const struct package_manager_repository_view *repository,
    const struct package_manager_catalog_entry *entry,
    const struct package_manager_text *requested,
    struct package_manager_text *version
)
{
    if (text_equal(&entry->identifier, requested)) {
        *version = entry->version;
        return true;
    }
    for (uint32_t index = 0U; index < entry->provide_count; ++index) {
        struct package_manager_text identifier;
        struct package_manager_text provided_version;
        if (relation_at(repository, entry->provide_start + index, true,
            &identifier, &provided_version) != PACKAGE_MANAGER_STATUS_OK) {
            return false;
        }
        if (text_equal(&identifier, requested)) {
            *version = provided_version;
            return true;
        }
    }
    return false;
}

static bool selected_for_provider(
    const struct solver *solver,
    const struct package_manager_text *provider,
    uint32_t *repository_index
)
{
    for (uint32_t index = 0U; index < solver->selected_count; ++index) {
        struct package_manager_catalog_entry entry;
        (void)package_manager_repository_entry(solver->repository,
            solver->selected[index], &entry);
        if (text_equal(&entry.identifier, provider)) {
            *repository_index = solver->selected[index];
            return true;
        }
    }
    return false;
}

static bool binding_for(
    const struct solver *solver,
    const struct package_manager_text *requested,
    struct package_manager_text *provider
)
{
    for (uint32_t index = 0U; index < solver->binding_count; ++index) {
        if (text_equal(&solver->bindings[index].requested, requested)) {
            *provider = solver->bindings[index].provider;
            return true;
        }
    }
    return false;
}

static enum package_manager_status add_binding(
    struct solver *solver,
    const struct package_manager_text *requested,
    const struct package_manager_text *provider
)
{
    struct package_manager_text existing;
    if (binding_for(solver, requested, &existing)) {
        return text_equal(&existing, provider) ? PACKAGE_MANAGER_STATUS_OK :
            PACKAGE_MANAGER_STATUS_AMBIGUOUS_PROVIDER;
    }
    if (solver->binding_count == SOLVER_MAX_BINDINGS) {
        return PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
    }
    solver->bindings[solver->binding_count].requested = *requested;
    solver->bindings[solver->binding_count].provider = *provider;
    ++solver->binding_count;
    return PACKAGE_MANAGER_STATUS_OK;
}

static enum package_manager_status selected_conflict(const struct solver *solver)
{
    for (uint32_t source_index = 0U; source_index < solver->selected_count;
        ++source_index) {
        struct package_manager_catalog_entry source = { 0 };

        if (package_manager_repository_entry(solver->repository,
                solver->selected[source_index], &source) !=
                PACKAGE_MANAGER_STATUS_OK) {
            return PACKAGE_MANAGER_STATUS_STATE;
        }
        for (uint32_t conflict_index = 0U;
            conflict_index < source.conflict_count; ++conflict_index) {
            struct package_manager_text conflict_id = { NULL, 0U };
            struct package_manager_text constraint = { NULL, 0U };

            if (relation_at(solver->repository,
                    source.conflict_start + conflict_index, false,
                    &conflict_id, &constraint) != PACKAGE_MANAGER_STATUS_OK) {
                return PACKAGE_MANAGER_STATUS_STATE;
            }
            for (uint32_t target_index = 0U;
                target_index < solver->selected_count; ++target_index) {
                struct package_manager_catalog_entry target = { 0 };
                struct package_manager_text provided_version = { NULL, 0U };

                if (package_manager_repository_entry(solver->repository,
                        solver->selected[target_index], &target) !=
                        PACKAGE_MANAGER_STATUS_OK) {
                    return PACKAGE_MANAGER_STATUS_STATE;
                }
                if (entry_provides(solver->repository, &target, &conflict_id,
                    &provided_version) && version_satisfies(&provided_version,
                        &constraint)) {
                    return PACKAGE_MANAGER_STATUS_CONFLICT;
                }
            }
        }
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

static int status_priority(enum package_manager_status status)
{
    switch (status) {
    case PACKAGE_MANAGER_STATUS_GRAPH_BOUND:
        return 6;
    case PACKAGE_MANAGER_STATUS_AMBIGUOUS_PROVIDER:
        return 5;
    case PACKAGE_MANAGER_STATUS_CYCLE:
        return 4;
    case PACKAGE_MANAGER_STATUS_CONFLICT:
        return 3;
    case PACKAGE_MANAGER_STATUS_DEPENDENCY:
        return 2;
    default:
        return 1;
    }
}

static enum package_manager_status solve_requirement(
    struct solver *solver,
    const struct package_manager_text *requested,
    const struct package_manager_text *constraint,
    uint32_t depth
)
{
    uint32_t candidate_count = 0U;
    struct package_manager_text provider = { NULL, 0U };
    struct package_manager_text existing_provider;
    uint32_t existing_index;
    if (depth > PACKAGE_MANAGER_DEPENDENCY_DEPTH_MAX) {
        return PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
    }
    for (uint32_t index = 0U; index < solver->repository->package_count; ++index) {
        struct package_manager_catalog_entry entry;
        struct package_manager_text provided_version;
        (void)package_manager_repository_entry(solver->repository, index, &entry);
        if (!entry_provides(solver->repository, &entry, requested, &provided_version) ||
            !version_satisfies(&provided_version, constraint)) {
            continue;
        }
        if (provider.bytes == NULL) {
            provider = entry.identifier;
        } else if (!text_equal(&provider, &entry.identifier)) {
            return PACKAGE_MANAGER_STATUS_AMBIGUOUS_PROVIDER;
        }
        ++candidate_count;
    }
    if (candidate_count == 0U) {
        return PACKAGE_MANAGER_STATUS_DEPENDENCY;
    }
    if (binding_for(solver, requested, &existing_provider) &&
        !text_equal(&existing_provider, &provider)) {
        return PACKAGE_MANAGER_STATUS_AMBIGUOUS_PROVIDER;
    }
    if (selected_for_provider(solver, &provider, &existing_index)) {
        struct package_manager_catalog_entry entry;
        struct package_manager_text provided_version;
        (void)package_manager_repository_entry(solver->repository, existing_index, &entry);
        if (!entry_provides(solver->repository, &entry, requested, &provided_version) ||
            !version_satisfies(&provided_version, constraint)) {
            return PACKAGE_MANAGER_STATUS_DEPENDENCY;
        }
        return add_binding(solver, requested, &provider);
    }
    {
        enum package_manager_status best = PACKAGE_MANAGER_STATUS_DEPENDENCY;
        struct package_manager_text ceiling = { NULL, 0U };

        /*
         * Re-scan the bounded catalog for each choice instead of putting a
         * 4 KiB candidate array on every recursive syscall-stack frame.
         */
        for (uint32_t attempt = 0U; attempt < candidate_count; ++attempt) {
            uint32_t selected_candidate = UINT32_MAX;
            struct package_manager_catalog_entry selected_entry;

            for (uint32_t index = 0U; index < solver->repository->package_count;
                ++index) {
                struct package_manager_catalog_entry candidate;
                struct package_manager_text provided_version;

                (void)package_manager_repository_entry(solver->repository,
                    index, &candidate);
                if (!entry_provides(solver->repository, &candidate, requested,
                        &provided_version) ||
                    !version_satisfies(&provided_version, constraint) ||
                    (ceiling.bytes != NULL && compare_semver_text(
                        &candidate.version, &ceiling) >= 0)) {
                    continue;
                }
                if (selected_candidate == UINT32_MAX || compare_semver_text(
                        &candidate.version, &selected_entry.version) > 0) {
                    selected_candidate = index;
                    selected_entry = candidate;
                }
            }
            if (selected_candidate == UINT32_MAX) {
                return best;
            }
            ceiling = selected_entry.version;
            {
        uint32_t selected_snapshot = solver->selected_count;
        uint32_t binding_snapshot = solver->binding_count;
        enum package_manager_status status;

        if (solver->selected_count == PACKAGE_MANAGER_PLAN_MAX_PACKAGES) {
            return PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
        }
        solver->selected[solver->selected_count++] = selected_candidate;
        status = add_binding(solver, requested, &selected_entry.identifier);
        if (status == PACKAGE_MANAGER_STATUS_OK) {
            status = add_binding(solver, &selected_entry.identifier,
                &selected_entry.identifier);
        }
        for (uint32_t dependency = 0U;
            status == PACKAGE_MANAGER_STATUS_OK &&
                dependency < selected_entry.dependency_count;
            ++dependency) {
            struct package_manager_text dependency_id;
            struct package_manager_text dependency_constraint;

            (void)relation_at(solver->repository,
                selected_entry.dependency_start + dependency, false,
                &dependency_id, &dependency_constraint);
            status = solve_requirement(solver, &dependency_id,
                &dependency_constraint, depth + 1U);
        }
        if (status == PACKAGE_MANAGER_STATUS_OK) {
            status = selected_conflict(solver);
        }
        if (status == PACKAGE_MANAGER_STATUS_OK) {
            return status;
        }
        if (status_priority(status) > status_priority(best)) {
            best = status;
        }
        solver->selected_count = selected_snapshot;
        solver->binding_count = binding_snapshot;
            }
        }
        return best;
    }
}

static bool selected_position_for_provider(
    const struct solver *solver,
    const struct package_manager_text *provider,
    uint32_t *position
)
{
    for (uint32_t index = 0U; index < solver->selected_count; ++index) {
        struct package_manager_catalog_entry entry;
        (void)package_manager_repository_entry(solver->repository,
            solver->selected[index], &entry);
        if (text_equal(&entry.identifier, provider)) {
            *position = index;
            return true;
        }
    }
    return false;
}

static enum package_manager_status order_packages(
    const struct solver *solver,
    uint8_t states[PACKAGE_MANAGER_PLAN_MAX_PACKAGES],
    uint32_t order[PACKAGE_MANAGER_PLAN_MAX_PACKAGES],
    uint32_t *order_count
)
{
    while (*order_count < solver->selected_count) {
        bool progress = false;

        for (uint32_t position = 0U; position < solver->selected_count;
            ++position) {
            struct package_manager_catalog_entry entry;
            bool ready = true;

            if (states[position] != 0U) {
                continue;
            }
            (void)package_manager_repository_entry(solver->repository,
                solver->selected[position], &entry);
            for (uint32_t dependency = 0U;
                dependency < entry.dependency_count; ++dependency) {
                struct package_manager_text dependency_id;
                struct package_manager_text ignored;
                struct package_manager_text provider;
                uint32_t dependency_position;

                (void)relation_at(solver->repository,
                    entry.dependency_start + dependency, false,
                    &dependency_id, &ignored);
                if (!binding_for(solver, &dependency_id, &provider) ||
                    !selected_position_for_provider(solver, &provider,
                        &dependency_position)) {
                    return PACKAGE_MANAGER_STATUS_DEPENDENCY;
                }
                if (states[dependency_position] == 0U) {
                    ready = false;
                    break;
                }
            }
            if (ready) {
                states[position] = 1U;
                order[(*order_count)++] = position;
                progress = true;
            }
        }
        if (!progress) {
            return PACKAGE_MANAGER_STATUS_CYCLE;
        }
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

static bool installed_package(
    const struct package_state_database_view *installed,
    const struct package_manager_text *identifier,
    uint32_t *index,
    struct package_manager_text *version,
    const uint8_t **publisher_key_id,
    bool *explicit_root
)
{
    if (installed == NULL) {
        return false;
    }
    for (uint32_t current = 0U; current < installed->package_count; ++current) {
        struct package_state_package_view package;
        struct package_manager_text candidate;

        if (package_state_database_package(installed, current, &package) !=
                PACKAGE_STATE_STATUS_OK) {
            return false;
        }
        candidate = (struct package_manager_text){
            package.identifier.bytes, package.identifier.length
        };
        if (text_equal(&candidate, identifier)) {
            *index = current;
            *version = (struct package_manager_text){
                package.version.bytes, package.version.length
            };
            *publisher_key_id = package.publisher_key_id;
            *explicit_root = package.explicit_root;
            return true;
        }
    }
    return false;
}

static enum package_manager_status publisher_key_policy(
    const uint8_t *key_id,
    const struct package_manager_trust *trust
)
{
    uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES];
    uint8_t derived[PACKAGE_MANAGER_SHA256_BYTES];
    enum package_manager_key_status status;
    if (trust == NULL || trust->lookup == NULL) {
        return PACKAGE_MANAGER_STATUS_CRYPTO_UNAVAILABLE;
    }
    status = trust->lookup(trust->context, key_id, public_key);
    if (status == PACKAGE_MANAGER_KEY_REVOKED) {
        return PACKAGE_MANAGER_STATUS_REVOKED_KEY;
    }
    if (status != PACKAGE_MANAGER_KEY_TRUSTED ||
        package_state_sha256(public_key, sizeof(public_key), derived) !=
            PACKAGE_STATE_STATUS_OK ||
        !bytes_equal(derived, key_id, sizeof(derived))) {
        return PACKAGE_MANAGER_STATUS_UNKNOWN_KEY;
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

enum package_manager_status package_manager_plan_install(
    const struct package_manager_repository_view *repository,
    const struct package_state_database_view *installed,
    const uint8_t *identifier,
    size_t identifier_bytes,
    const struct package_manager_policy *policy,
    const struct package_manager_trust *trust,
    struct package_manager_plan *result
)
{
    struct package_manager_text requested = { identifier, identifier_bytes };
    static const uint8_t star = '*';
    struct package_manager_text constraint = { &star, 1U };
    uint32_t order_count = 0U;
    bool target_installed = false;
    bool target_explicit = false;
    enum package_manager_status status;

    if (repository == NULL || policy == NULL || result == NULL ||
        identifier == NULL || !package_identifier(&requested)) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    result->operation = 0;
    result->target = requested;
    result->root = (struct package_manager_text){ NULL, 0U };
    result->count = 0U;
    if (install_solver_busy) {
        return PACKAGE_MANAGER_STATUS_STATE;
    }
    install_solver_busy = true;
    install_solver.repository = repository;
    install_solver.selected_count = 0U;
    install_solver.binding_count = 0U;
    for (uint32_t index = 0U; index < PACKAGE_MANAGER_PLAN_MAX_PACKAGES;
        ++index) {
        install_order_states[index] = 0U;
    }
    status = solve_requirement(&install_solver, &requested,
        &constraint, 0U);
    if (status != PACKAGE_MANAGER_STATUS_OK) {
        goto finish;
    }
    if (!binding_for(&install_solver, &requested, &result->root)) {
        status = PACKAGE_MANAGER_STATUS_STATE;
        goto finish;
    }
    status = order_packages(&install_solver, install_order_states,
        install_order, &order_count);
    if (status != PACKAGE_MANAGER_STATUS_OK) {
        goto finish;
    }
    result->operation = PACKAGE_MANAGER_PLAN_INSTALL;
    for (uint32_t ordered = 0U; ordered < order_count; ++ordered) {
        struct package_manager_catalog_entry entry;
        struct package_manager_text installed_version;
        const uint8_t *installed_key;
        bool installed_explicit;
        uint32_t installed_index;
        bool present;
        (void)package_manager_repository_entry(repository,
            install_solver.selected[install_order[ordered]], &entry);
        status = publisher_key_policy(entry.publisher_key_id, trust);
        if (status != PACKAGE_MANAGER_STATUS_OK) {
            goto finish;
        }
        present = installed_package(installed, &entry.identifier, &installed_index,
            &installed_version, &installed_key, &installed_explicit);
        (void)installed_index;
        if (present) {
            int comparison = compare_semver_text(&entry.version, &installed_version);
            if (!bytes_equal(installed_key, entry.publisher_key_id,
                PACKAGE_MANAGER_SHA256_BYTES)) {
                status = PACKAGE_MANAGER_STATUS_KEY_ROTATION;
                goto finish;
            }
            if (comparison < 0 && !policy->allow_downgrade) {
                status = PACKAGE_MANAGER_STATUS_DOWNGRADE;
                goto finish;
            }
            if (text_equal(&entry.identifier, &result->root)) {
                target_installed = true;
                target_explicit = installed_explicit;
            }
            if (comparison == 0) {
                continue;
            }
        }
        if (result->count == PACKAGE_MANAGER_PLAN_MAX_PACKAGES) {
            status = PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
            goto finish;
        }
        struct package_manager_plan_item *item = &result->items[result->count++];
        item->source_index = entry.repository_index;
        item->identifier = entry.identifier;
        item->version = entry.version;
        item->download_path = entry.download_path;
        item->package_bytes = entry.package_bytes;
        item->package_sha256 = entry.package_sha256;
        item->publisher_key_id = entry.publisher_key_id;
    }
    if (result->count == 0U) {
        if (target_installed && !target_explicit) {
            status = PACKAGE_MANAGER_STATUS_OK;
            goto finish;
        }
        status = PACKAGE_MANAGER_STATUS_ALREADY_INSTALLED;
        goto finish;
    }
    if (target_installed) {
        result->operation = PACKAGE_MANAGER_PLAN_UPDATE;
    }
    status = PACKAGE_MANAGER_STATUS_OK;
finish:
    install_solver.repository = NULL;
    install_solver.selected_count = 0U;
    install_solver.binding_count = 0U;
    install_solver_busy = false;
    return status;
}

static bool plan_item_matches(
    const struct package_manager_plan_item *item,
    const struct package_manager_catalog_entry *entry
)
{
    return item->source_index == entry->repository_index &&
        item->identifier.bytes == entry->identifier.bytes &&
        item->identifier.length == entry->identifier.length &&
        item->version.bytes == entry->version.bytes &&
        item->version.length == entry->version.length &&
        item->download_path.bytes == entry->download_path.bytes &&
        item->download_path.length == entry->download_path.length &&
        item->package_bytes == entry->package_bytes &&
        item->package_sha256 == entry->package_sha256 &&
        item->publisher_key_id == entry->publisher_key_id;
}

enum package_manager_status package_manager_plan_dependency_binding(
    const struct package_manager_repository_view *repository,
    const struct package_manager_plan *plan,
    uint32_t plan_index,
    uint32_t dependency_index,
    struct package_manager_plan_binding *result
)
{
    struct package_manager_catalog_entry consumer;
    struct package_manager_text requested;
    struct package_manager_text constraint;
    struct package_manager_text provider = { NULL, 0U };
    uint32_t candidate_count = 0U;

    if (repository == NULL || plan == NULL || result == NULL) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    *result = (struct package_manager_plan_binding){ 0 };
    if ((plan->operation != PACKAGE_MANAGER_PLAN_INSTALL &&
            plan->operation != PACKAGE_MANAGER_PLAN_UPDATE) ||
        plan->count == 0U || plan->count > PACKAGE_MANAGER_PLAN_MAX_PACKAGES ||
        plan_index >= plan->count) {
        return PACKAGE_MANAGER_STATUS_TABLE;
    }
    if (package_manager_repository_entry(repository,
            plan->items[plan_index].source_index, &consumer) !=
            PACKAGE_MANAGER_STATUS_OK ||
        !plan_item_matches(&plan->items[plan_index], &consumer)) {
        return PACKAGE_MANAGER_STATUS_STATE;
    }
    if (dependency_index >= consumer.dependency_count) {
        return PACKAGE_MANAGER_STATUS_TABLE;
    }
    if (relation_at(repository, consumer.dependency_start + dependency_index,
            false, &requested, &constraint) != PACKAGE_MANAGER_STATUS_OK) {
        return PACKAGE_MANAGER_STATUS_STATE;
    }
    for (uint32_t index = 0U; index < repository->package_count; ++index) {
        struct package_manager_catalog_entry candidate;
        struct package_manager_text provided_version;

        if (package_manager_repository_entry(repository, index, &candidate) !=
                PACKAGE_MANAGER_STATUS_OK) {
            return PACKAGE_MANAGER_STATUS_STATE;
        }
        if (!entry_provides(repository, &candidate, &requested,
                &provided_version) ||
            !version_satisfies(&provided_version, &constraint)) {
            continue;
        }
        if (provider.bytes == NULL) {
            provider = candidate.identifier;
        } else if (!text_equal(&provider, &candidate.identifier)) {
            return PACKAGE_MANAGER_STATUS_AMBIGUOUS_PROVIDER;
        }
        ++candidate_count;
    }
    if (candidate_count == 0U) {
        return PACKAGE_MANAGER_STATUS_DEPENDENCY;
    }
    result->plan = plan;
    result->plan_index = plan_index;
    result->requested = requested;
    result->constraint = constraint;
    result->provider = provider;
    return PACKAGE_MANAGER_STATUS_OK;
}

static bool installed_index_for(
    const struct package_state_database_view *installed,
    const struct package_manager_text *identifier,
    uint32_t *result
)
{
    for (uint32_t index = 0U; index < installed->package_count; ++index) {
        struct package_state_package_view package;
        struct package_manager_text candidate;

        if (package_state_database_package(installed, index, &package) !=
                PACKAGE_STATE_STATUS_OK) {
            return false;
        }
        candidate = (struct package_manager_text){
            package.identifier.bytes, package.identifier.length
        };
        if (text_equal(&candidate, identifier)) {
            *result = index;
            return true;
        }
    }
    return false;
}

enum package_manager_status package_manager_plan_remove(
    const struct package_state_database_view *installed,
    const uint8_t *identifier,
    size_t identifier_bytes,
    struct package_manager_plan *result
)
{
    struct package_manager_text requested = { identifier, identifier_bytes };
    bool reachable[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { false };
    uint16_t queue[PACKAGE_STATE_DATABASE_MAX_PACKAGES];
    size_t head = 0U;
    size_t tail = 0U;
    uint32_t target;
    if (installed == NULL || installed->bytes == NULL || identifier == NULL ||
        result == NULL || !package_identifier(&requested)) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    result->operation = 0;
    result->target = requested;
    result->root = requested;
    result->count = 0U;
    if (!installed_index_for(installed, &requested, &target)) {
        return PACKAGE_MANAGER_STATUS_NOT_FOUND;
    }
    struct package_state_package_view target_package;
    if (package_state_database_package(installed, target, &target_package) !=
            PACKAGE_STATE_STATUS_OK) {
        return PACKAGE_MANAGER_STATUS_STATE;
    }
    if (!target_package.explicit_root) {
        return PACKAGE_MANAGER_STATUS_IN_USE;
    }
    for (uint32_t index = 0U; index < installed->package_count; ++index) {
        struct package_state_package_view package;

        if (package_state_database_package(installed, index, &package) !=
                PACKAGE_STATE_STATUS_OK) {
            return PACKAGE_MANAGER_STATUS_STATE;
        }
        if (index != target && package.explicit_root) {
            reachable[index] = true;
            queue[tail++] = (uint16_t)index;
        }
    }
    while (head < tail) {
        uint32_t owner = queue[head++];
        struct package_state_package_view package;

        if (package_state_database_package(installed, owner, &package) !=
                PACKAGE_STATE_STATUS_OK) {
            return PACKAGE_MANAGER_STATUS_STATE;
        }
        for (uint32_t offset = 0U; offset < package.dependency_count; ++offset) {
            struct package_state_dependency_view dependency;
            struct package_manager_text provider;
            uint32_t provider_index;

            if (package_state_database_dependency(installed,
                    package.dependency_start + offset, &dependency) !=
                    PACKAGE_STATE_STATUS_OK) {
                return PACKAGE_MANAGER_STATUS_STATE;
            }
            provider = (struct package_manager_text){
                dependency.provider.bytes, dependency.provider.length
            };
            if (!installed_index_for(installed, &provider, &provider_index)) {
                return PACKAGE_MANAGER_STATUS_STATE;
            }
            if (!reachable[provider_index]) {
                reachable[provider_index] = true;
                queue[tail++] = (uint16_t)provider_index;
            }
        }
    }
    if (reachable[target]) {
        return PACKAGE_MANAGER_STATUS_IN_USE;
    }
    result->operation = PACKAGE_MANAGER_PLAN_REMOVE;
    bool emitted[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { false };
    for (;;) {
        uint32_t package_index = installed->package_count;
        for (uint32_t candidate = 0U; candidate < installed->package_count;
            ++candidate) {
            bool has_dependent = false;
            if (reachable[candidate] || emitted[candidate]) {
                continue;
            }
            for (uint32_t owner = 0U; owner < installed->package_count &&
                !has_dependent; ++owner) {
                if (reachable[owner] || emitted[owner] || owner == candidate) {
                    continue;
                }
                struct package_state_package_view owner_package;
                if (package_state_database_package(installed, owner,
                        &owner_package) != PACKAGE_STATE_STATUS_OK) {
                    return PACKAGE_MANAGER_STATUS_STATE;
                }
                for (uint32_t offset = 0U;
                    offset < owner_package.dependency_count; ++offset) {
                    struct package_state_dependency_view dependency;
                    struct package_manager_text provider;
                    uint32_t provider_index;

                    if (package_state_database_dependency(installed,
                            owner_package.dependency_start + offset,
                            &dependency) != PACKAGE_STATE_STATUS_OK) {
                        return PACKAGE_MANAGER_STATUS_STATE;
                    }
                    provider = (struct package_manager_text){
                        dependency.provider.bytes, dependency.provider.length
                    };
                    if (!installed_index_for(installed, &provider,
                            &provider_index)) {
                        return PACKAGE_MANAGER_STATUS_STATE;
                    }
                    if (provider_index == candidate) {
                        has_dependent = true;
                        break;
                    }
                }
            }
            if (!has_dependent) {
                package_index = candidate;
                break;
            }
        }
        if (package_index == installed->package_count) {
            break;
        }
        {
            if (result->count == PACKAGE_MANAGER_PLAN_MAX_PACKAGES) {
                return PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
            }
            struct package_state_package_view package;

            if (package_state_database_package(installed, package_index,
                    &package) != PACKAGE_STATE_STATUS_OK) {
                return PACKAGE_MANAGER_STATUS_STATE;
            }
            struct package_manager_plan_item *item =
                &result->items[result->count++];
            item->source_index = package_index;
            item->identifier = (struct package_manager_text){
                package.identifier.bytes, package.identifier.length
            };
            item->version = (struct package_manager_text){
                package.version.bytes, package.version.length
            };
            item->download_path.bytes = NULL;
            item->download_path.length = 0U;
            item->package_bytes = 0U;
            item->package_sha256 = package.package_sha256;
            item->publisher_key_id = package.publisher_key_id;
            emitted[package_index] = true;
        }
    }
    for (uint32_t index = 0U; index < installed->package_count; ++index) {
        if (!reachable[index] && !emitted[index]) {
            return PACKAGE_MANAGER_STATUS_CYCLE;
        }
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

enum package_manager_status package_manager_installed_search(
    const struct package_state_database_view *installed,
    const uint8_t *query,
    size_t query_bytes,
    struct package_manager_search_results *result
)
{
    if (installed == NULL || installed->bytes == NULL || result == NULL ||
        (query == NULL && query_bytes != 0U)) {
        return PACKAGE_MANAGER_STATUS_NULL_ARGUMENT;
    }
    result->count = 0U;
    for (uint32_t index = 0U; index < installed->package_count; ++index) {
        struct package_state_package_view package;
        struct package_manager_text identifier;

        if (package_state_database_package(installed, index, &package) !=
                PACKAGE_STATE_STATUS_OK) {
            return PACKAGE_MANAGER_STATUS_STATE;
        }
        identifier = (struct package_manager_text){
            package.identifier.bytes, package.identifier.length
        };
        if (contains_casefold(&identifier, query, query_bytes)) {
            if (result->count == PACKAGE_MANAGER_SEARCH_MAX_RESULTS) {
                return PACKAGE_MANAGER_STATUS_GRAPH_BOUND;
            }
            result->repository_indices[result->count++] = index;
        }
    }
    return PACKAGE_MANAGER_STATUS_OK;
}

const char *package_manager_status_string(enum package_manager_status status)
{
    static const char *const names[PACKAGE_MANAGER_STATUS_COUNT] = {
        "ok", "null argument", "length", "magic", "header", "reserved",
        "overflow", "digest", "text", "architecture", "ABI", "freshness",
        "repository rollback", "table", "package", "dependency", "conflict",
        "ambiguous provider", "dependency cycle", "graph bound", "not found",
        "already installed", "downgrade", "publisher key rotation", "unknown key",
        "revoked key", "crypto unavailable", "signature", "installed state",
        "package in use"
    };
    if ((unsigned int)status >= (unsigned int)PACKAGE_MANAGER_STATUS_COUNT) {
        return "unknown package-manager status";
    }
    return names[status];
}
