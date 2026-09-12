/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/package_state.h>

#define DATABASE_FLAG_EXPLICIT UINT32_C(1)
#define PACKAGE_MAX_DEPENDENCIES 64U
#define PACKAGE_MAX_FILES 256U
#define PACKAGE_MAX_FILE_BYTES (64U * 1024U * 1024U)
#define JOURNAL_PHASE_PREPARED UINT16_C(1)

_Static_assert(sizeof(size_t) <= sizeof(uint64_t),
    "package-state byte counts must fit the journal uint64 fields");
_Static_assert(PACKAGE_MAX_FILES <= UINT16_MAX,
    "per-package owned-file counts must fit fixed parser storage");

struct byte_field {
    const uint8_t *bytes;
    size_t length;
};

static const uint32_t sha256_initial[8] = {
    UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
    UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
    UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
    UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
};

static const uint32_t sha256_constants[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2)
};

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] |
        (uint16_t)((uint16_t)bytes[1] << 8U));
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        (uint32_t)bytes[1] << 8U |
        (uint32_t)bytes[2] << 16U |
        (uint32_t)bytes[3] << 24U;
}

static uint64_t read_u64(const uint8_t *bytes)
{
    return (uint64_t)read_u32(bytes) |
        (uint64_t)read_u32(bytes + 4U) << 32U;
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void write_u64(uint8_t *bytes, uint64_t value)
{
    for (size_t index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static bool equal_bytes(const uint8_t *left, const uint8_t *right, size_t count)
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

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static void clear_bytes(uint8_t *destination, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        destination[index] = 0U;
    }
}

static bool checked_add(size_t left, size_t right, size_t *result)
{
    if (result == NULL || right > SIZE_MAX - left) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool checked_multiply(size_t left, size_t right, size_t *result)
{
    if (result == NULL || (left != 0U && right > SIZE_MAX / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

static uint32_t rotate_right(uint32_t value, uint32_t amount)
{
    return value >> amount | value << (32U - amount);
}

static void sha256_transform(
    struct package_state_sha256_context *context,
    const uint8_t *block
)
{
    uint32_t words[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;

    for (size_t index = 0U; index < 16U; ++index) {
        const size_t offset = index * 4U;
        words[index] = (uint32_t)block[offset] << 24U |
            (uint32_t)block[offset + 1U] << 16U |
            (uint32_t)block[offset + 2U] << 8U |
            (uint32_t)block[offset + 3U];
    }
    for (size_t index = 16U; index < 64U; ++index) {
        const uint32_t first = rotate_right(words[index - 15U], 7U) ^
            rotate_right(words[index - 15U], 18U) ^
            (words[index - 15U] >> 3U);
        const uint32_t second = rotate_right(words[index - 2U], 17U) ^
            rotate_right(words[index - 2U], 19U) ^
            (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + first +
            words[index - 7U] + second;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (size_t index = 0U; index < 64U; ++index) {
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t big_e = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
            rotate_right(e, 25U);
        const uint32_t big_a = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
            rotate_right(a, 22U);
        const uint32_t temporary_one = h + big_e + choose +
            sha256_constants[index] + words[index];
        const uint32_t temporary_two = big_a + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary_one;
        d = c;
        c = b;
        b = a;
        a = temporary_one + temporary_two;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

enum package_state_status package_state_sha256_initialize(
    struct package_state_sha256_context *context
)
{
    if (context == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    for (size_t index = 0U; index < 8U; ++index) {
        context->state[index] = sha256_initial[index];
    }
    context->byte_count = 0U;
    context->block_bytes = 0U;
    context->finished = false;
    return PACKAGE_STATE_STATUS_OK;
}

enum package_state_status package_state_sha256_update(
    struct package_state_sha256_context *context,
    const uint8_t *bytes,
    size_t byte_count
)
{
    size_t count = byte_count;

    if (context == NULL || (bytes == NULL && count != 0U) ||
        (context != NULL &&
            (context->byte_count > UINT64_MAX / UINT64_C(8) ||
                (uint64_t)count > UINT64_MAX / UINT64_C(8) -
                    context->byte_count))) {
        return context == NULL || (bytes == NULL && count != 0U) ?
            PACKAGE_STATE_STATUS_NULL_ARGUMENT : PACKAGE_STATE_STATUS_OVERFLOW;
    }
    if (context->finished) {
        return PACKAGE_STATE_STATUS_SEQUENCE;
    }
    context->byte_count += count;
    while (count != 0U) {
        size_t available = sizeof(context->block) - context->block_bytes;
        size_t copied = count < available ? count : available;
        copy_bytes(context->block + context->block_bytes, bytes, copied);
        context->block_bytes += copied;
        bytes += copied;
        count -= copied;
        if (context->block_bytes == sizeof(context->block)) {
            sha256_transform(context, context->block);
            context->block_bytes = 0U;
        }
    }
    return PACKAGE_STATE_STATUS_OK;
}

enum package_state_status package_state_sha256_finish(
    struct package_state_sha256_context *context,
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES]
)
{
    if (context == NULL || digest == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    if (context->finished) {
        return PACKAGE_STATE_STATUS_SEQUENCE;
    }
    const uint64_t bit_count = context->byte_count * UINT64_C(8);

    context->block[context->block_bytes++] = UINT8_C(0x80);
    if (context->block_bytes > 56U) {
        while (context->block_bytes < sizeof(context->block)) {
            context->block[context->block_bytes++] = 0U;
        }
        sha256_transform(context, context->block);
        context->block_bytes = 0U;
    }
    while (context->block_bytes < 56U) {
        context->block[context->block_bytes++] = 0U;
    }
    for (size_t index = 0U; index < 8U; ++index) {
        context->block[63U - index] = (uint8_t)(bit_count >> (index * 8U));
    }
    sha256_transform(context, context->block);
    for (size_t index = 0U; index < 8U; ++index) {
        digest[index * 4U] = (uint8_t)(context->state[index] >> 24U);
        digest[index * 4U + 1U] = (uint8_t)(context->state[index] >> 16U);
        digest[index * 4U + 2U] = (uint8_t)(context->state[index] >> 8U);
        digest[index * 4U + 3U] = (uint8_t)context->state[index];
    }
    context->finished = true;
    return PACKAGE_STATE_STATUS_OK;
}

enum package_state_status package_state_sha256(
    const uint8_t *bytes,
    size_t byte_count,
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES]
)
{
    struct package_state_sha256_context context;

    if (digest == NULL || (bytes == NULL && byte_count != 0U)) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    if ((uint64_t)byte_count > UINT64_MAX / UINT64_C(8)) {
        return PACKAGE_STATE_STATUS_OVERFLOW;
    }
    enum package_state_status status = package_state_sha256_initialize(&context);
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = package_state_sha256_update(&context, bytes, byte_count);
    }
    return status == PACKAGE_STATE_STATUS_OK ?
        package_state_sha256_finish(&context, digest) : status;
}

static enum package_state_status fixed_text(
    const uint8_t *bytes,
    size_t width,
    bool allow_empty,
    struct byte_field *result
)
{
    size_t length = 0U;

    if (bytes == NULL || result == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    while (length < width && bytes[length] != 0U) {
        if (bytes[length] < UINT8_C(0x20) || bytes[length] > UINT8_C(0x7e)) {
            return PACKAGE_STATE_STATUS_TEXT;
        }
        ++length;
    }
    if (length == width || (!allow_empty && length == 0U) ||
        !zero_bytes(bytes + length, width - length)) {
        return PACKAGE_STATE_STATUS_TEXT;
    }
    result->bytes = bytes;
    result->length = length;
    return PACKAGE_STATE_STATUS_OK;
}

static int compare_fields(const struct byte_field *left, const struct byte_field *right)
{
    size_t count = left->length < right->length ? left->length : right->length;

    for (size_t index = 0U; index < count; ++index) {
        if (left->bytes[index] != right->bytes[index]) {
            return left->bytes[index] < right->bytes[index] ? -1 : 1;
        }
    }
    if (left->length == right->length) {
        return 0;
    }
    return left->length < right->length ? -1 : 1;
}

static bool path_ancestor(
    const struct byte_field *ancestor,
    const struct byte_field *descendant
)
{
    return ancestor->length < descendant->length &&
        descendant->bytes[ancestor->length] == (uint8_t)'/' &&
        equal_bytes(ancestor->bytes, descendant->bytes, ancestor->length);
}

static bool ascii_alphanumeric(uint8_t value)
{
    return (value >= (uint8_t)'0' && value <= (uint8_t)'9') ||
        (value >= (uint8_t)'A' && value <= (uint8_t)'Z') ||
        (value >= (uint8_t)'a' && value <= (uint8_t)'z');
}

static bool package_identifier(const struct byte_field *field)
{
    bool previous_separator = true;

    if (field == NULL || field->length == 0U) {
        return false;
    }
    for (size_t index = 0U; index < field->length; ++index) {
        uint8_t value = field->bytes[index];
        bool alphanumeric = (value >= (uint8_t)'a' && value <= (uint8_t)'z') ||
            (value >= (uint8_t)'0' && value <= (uint8_t)'9');
        bool separator = value == (uint8_t)'.' || value == (uint8_t)'-';
        if (!alphanumeric && !separator) {
            return false;
        }
        if (separator && previous_separator) {
            return false;
        }
        previous_separator = separator;
    }
    return !previous_separator;
}

static bool numeric_identifier(
    const uint8_t *bytes,
    size_t start,
    size_t end,
    bool reject_leading_zero
)
{
    if (start == end || (reject_leading_zero && end - start > 1U &&
        bytes[start] == (uint8_t)'0')) {
        return false;
    }
    for (size_t index = start; index < end; ++index) {
        if (bytes[index] < (uint8_t)'0' || bytes[index] > (uint8_t)'9') {
            return false;
        }
    }
    return true;
}

static bool semantic_version(const uint8_t *bytes, size_t length)
{
    size_t cursor = 0U;

    for (size_t part = 0U; part < 3U; ++part) {
        size_t start = cursor;
        while (cursor < length && bytes[cursor] >= (uint8_t)'0' &&
            bytes[cursor] <= (uint8_t)'9') {
            ++cursor;
        }
        if (!numeric_identifier(bytes, start, cursor, true)) {
            return false;
        }
        if (part != 2U) {
            if (cursor == length || bytes[cursor] != (uint8_t)'.') {
                return false;
            }
            ++cursor;
        }
    }
    if (cursor < length && bytes[cursor] == (uint8_t)'-') {
        ++cursor;
        for (;;) {
            size_t start = cursor;
            bool numeric = true;
            while (cursor < length && bytes[cursor] != (uint8_t)'.' &&
                bytes[cursor] != (uint8_t)'+') {
                uint8_t value = bytes[cursor];
                if (!ascii_alphanumeric(value) && value != (uint8_t)'-') {
                    return false;
                }
                if (value < (uint8_t)'0' || value > (uint8_t)'9') {
                    numeric = false;
                }
                ++cursor;
            }
            if (start == cursor || (numeric && !numeric_identifier(
                bytes, start, cursor, true))) {
                return false;
            }
            if (cursor == length || bytes[cursor] == (uint8_t)'+') {
                break;
            }
            ++cursor;
        }
    }
    if (cursor < length && bytes[cursor] == (uint8_t)'+') {
        ++cursor;
        for (;;) {
            size_t start = cursor;
            while (cursor < length && bytes[cursor] != (uint8_t)'.') {
                uint8_t value = bytes[cursor];
                if (!ascii_alphanumeric(value) && value != (uint8_t)'-') {
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
    return cursor == length;
}

static bool version_constraint(const struct byte_field *field)
{
    size_t cursor = 0U;

    if (field->length == 1U && field->bytes[0] == (uint8_t)'*') {
        return true;
    }
    while (cursor < field->length) {
        size_t version_start;
        size_t version_end;
        uint8_t operation = field->bytes[cursor++];

        if (operation == (uint8_t)'>' || operation == (uint8_t)'<') {
            if (cursor < field->length && field->bytes[cursor] == (uint8_t)'=') {
                ++cursor;
            }
        } else if (operation != (uint8_t)'=' && operation != (uint8_t)'^' &&
            operation != (uint8_t)'~') {
            return false;
        }
        version_start = cursor;
        while (cursor < field->length && field->bytes[cursor] != (uint8_t)',') {
            ++cursor;
        }
        version_end = cursor;
        if (!semantic_version(field->bytes + version_start,
            version_end - version_start)) {
            return false;
        }
        if (cursor < field->length) {
            ++cursor;
            if (cursor == field->length) {
                return false;
            }
        }
    }
    return true;
}

static bool package_path(const struct byte_field *field)
{
    size_t component_start = 0U;

    if (field->length == 0U || field->bytes[0] == (uint8_t)'/' ||
        field->bytes[field->length - 1U] == (uint8_t)'/') {
        return false;
    }
    for (size_t index = 0U; index <= field->length; ++index) {
        if (index == field->length || field->bytes[index] == (uint8_t)'/') {
            size_t component_length = index - component_start;
            if (component_length == 0U ||
                (component_length == 1U &&
                    field->bytes[component_start] == (uint8_t)'.') ||
                (component_length == 2U &&
                    field->bytes[component_start] == (uint8_t)'.' &&
                    field->bytes[component_start + 1U] == (uint8_t)'.')) {
                return false;
            }
            component_start = index + 1U;
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

static bool library_name(const struct byte_field *field)
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

static const uint8_t *package_record(
    const struct package_state_database_view *view,
    uint32_t index
)
{
    return view->bytes + view->package_offset +
        (size_t)index * PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES;
}

static const uint8_t *edge_record(
    const struct package_state_database_view *view,
    uint32_t index
)
{
    return view->bytes + view->edge_offset +
        (size_t)index * PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES;
}

static bool package_index_for(
    const struct package_state_database_view *view,
    const struct byte_field *identifier,
    uint32_t *result
)
{
    for (uint32_t index = 0U; index < view->package_count; ++index) {
        struct byte_field candidate;
        if (fixed_text(package_record(view, index), 64U, false, &candidate) !=
            PACKAGE_STATE_STATUS_OK) {
            return false;
        }
        if (compare_fields(&candidate, identifier) == 0) {
            *result = index;
            return true;
        }
    }
    return false;
}

static enum package_state_status validate_packages(
    const struct package_state_database_view *view,
    uint16_t owned_file_counts[PACKAGE_STATE_DATABASE_MAX_PACKAGES]
)
{
    struct byte_field previous = { NULL, 0U };
    uint32_t edge_cursor = 0U;

    for (uint32_t index = 0U; index < view->package_count; ++index) {
        const uint8_t *record = package_record(view, index);
        struct byte_field identifier;
        struct byte_field version;
        enum package_state_status status;
        uint32_t flags;
        uint32_t edge_start;
        uint32_t edge_count;
        uint32_t file_count;

        status = fixed_text(record, 64U, false, &identifier);
        if (status != PACKAGE_STATE_STATUS_OK || !package_identifier(&identifier)) {
            return PACKAGE_STATE_STATUS_PACKAGE;
        }
        status = fixed_text(record + 64U, 64U, false, &version);
        if (status != PACKAGE_STATE_STATUS_OK ||
            !semantic_version(version.bytes, version.length)) {
            return PACKAGE_STATE_STATUS_PACKAGE;
        }
        if (index != 0U && compare_fields(&previous, &identifier) >= 0) {
            return PACKAGE_STATE_STATUS_PACKAGE;
        }
        previous = identifier;
        flags = read_u32(record + 192U);
        edge_start = read_u32(record + 196U);
        edge_count = read_u32(record + 200U);
        file_count = read_u32(record + 204U);
        if ((flags & ~DATABASE_FLAG_EXPLICIT) != 0U ||
            edge_start != edge_cursor || edge_count > PACKAGE_MAX_DEPENDENCIES ||
            edge_count > view->edge_count - edge_cursor ||
            file_count > PACKAGE_MAX_FILES || !zero_bytes(record + 208U, 48U)) {
            return PACKAGE_STATE_STATUS_PACKAGE;
        }
        owned_file_counts[index] = (uint16_t)file_count;
        edge_cursor += edge_count;
    }
    return edge_cursor == view->edge_count ? PACKAGE_STATE_STATUS_OK :
        PACKAGE_STATE_STATUS_DEPENDENCY;
}

static enum package_state_status validate_dependencies(
    const struct package_state_database_view *view
)
{
    uint16_t incoming[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { 0U };
    bool removed[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { false };
    bool reachable[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { false };
    uint16_t queue[PACKAGE_STATE_DATABASE_MAX_PACKAGES];
    size_t queue_head = 0U;
    size_t queue_tail = 0U;
    size_t removed_count = 0U;

    for (uint32_t owner = 0U; owner < view->package_count; ++owner) {
        const uint8_t *package = package_record(view, owner);
        uint32_t first = read_u32(package + 196U);
        uint32_t count = read_u32(package + 200U);
        struct byte_field previous_identifier = { NULL, 0U };

        for (uint32_t offset = 0U; offset < count; ++offset) {
            const uint8_t *record = edge_record(view, first + offset);
            struct byte_field identifier;
            struct byte_field constraint;
            struct byte_field provider;
            uint32_t provider_index;
            enum package_state_status status;

            status = fixed_text(record, 64U, false, &identifier);
            if (status != PACKAGE_STATE_STATUS_OK ||
                !package_identifier(&identifier)) {
                return PACKAGE_STATE_STATUS_DEPENDENCY;
            }
            status = fixed_text(record + 64U, 56U, false, &constraint);
            if (status != PACKAGE_STATE_STATUS_OK || !version_constraint(&constraint)) {
                return PACKAGE_STATE_STATUS_DEPENDENCY;
            }
            status = fixed_text(record + 120U, 64U, false, &provider);
            if (status != PACKAGE_STATE_STATUS_OK ||
                !package_identifier(&provider) || !zero_bytes(record + 184U, 8U) ||
                !package_index_for(view, &provider, &provider_index)) {
                return PACKAGE_STATE_STATUS_DEPENDENCY;
            }
            if (offset != 0U &&
                compare_fields(&previous_identifier, &identifier) >= 0) {
                return PACKAGE_STATE_STATUS_DEPENDENCY;
            }
            previous_identifier = identifier;
            if (incoming[provider_index] == UINT16_MAX) {
                return PACKAGE_STATE_STATUS_OVERFLOW;
            }
            ++incoming[provider_index];
        }
    }

    for (uint32_t index = 0U; index < view->package_count; ++index) {
        if (incoming[index] == 0U) {
            queue[queue_tail++] = (uint16_t)index;
        }
    }
    while (queue_head < queue_tail) {
        uint32_t owner = queue[queue_head++];
        const uint8_t *package = package_record(view, owner);
        uint32_t first = read_u32(package + 196U);
        uint32_t count = read_u32(package + 200U);
        if (removed[owner]) {
            return PACKAGE_STATE_STATUS_DEPENDENCY;
        }
        removed[owner] = true;
        ++removed_count;
        for (uint32_t offset = 0U; offset < count; ++offset) {
            struct byte_field provider;
            uint32_t provider_index;
            const uint8_t *record = edge_record(view, first + offset);
            if (fixed_text(record + 120U, 64U, false, &provider) !=
                PACKAGE_STATE_STATUS_OK ||
                !package_index_for(view, &provider, &provider_index) ||
                incoming[provider_index] == 0U) {
                return PACKAGE_STATE_STATUS_DEPENDENCY;
            }
            --incoming[provider_index];
            if (incoming[provider_index] == 0U) {
                queue[queue_tail++] = (uint16_t)provider_index;
            }
        }
    }
    if (removed_count != view->package_count) {
        return PACKAGE_STATE_STATUS_DEPENDENCY;
    }

    queue_head = 0U;
    queue_tail = 0U;
    for (uint32_t index = 0U; index < view->package_count; ++index) {
        if ((read_u32(package_record(view, index) + 192U) &
            DATABASE_FLAG_EXPLICIT) != 0U) {
            reachable[index] = true;
            queue[queue_tail++] = (uint16_t)index;
        }
    }
    while (queue_head < queue_tail) {
        uint32_t owner = queue[queue_head++];
        const uint8_t *package = package_record(view, owner);
        uint32_t first = read_u32(package + 196U);
        uint32_t count = read_u32(package + 200U);
        for (uint32_t offset = 0U; offset < count; ++offset) {
            struct byte_field provider;
            uint32_t provider_index;
            const uint8_t *record = edge_record(view, first + offset);
            if (fixed_text(record + 120U, 64U, false, &provider) !=
                PACKAGE_STATE_STATUS_OK ||
                !package_index_for(view, &provider, &provider_index)) {
                return PACKAGE_STATE_STATUS_DEPENDENCY;
            }
            if (!reachable[provider_index]) {
                reachable[provider_index] = true;
                queue[queue_tail++] = (uint16_t)provider_index;
            }
        }
    }
    for (uint32_t index = 0U; index < view->package_count; ++index) {
        if (!reachable[index]) {
            return PACKAGE_STATE_STATUS_DEPENDENCY;
        }
    }
    return PACKAGE_STATE_STATUS_OK;
}

static enum package_state_status validate_files(
    const struct package_state_database_view *view,
    const uint16_t owned_file_counts[PACKAGE_STATE_DATABASE_MAX_PACKAGES]
)
{
    uint16_t actual_counts[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { 0U };
    struct byte_field previous = { NULL, 0U };

    for (uint32_t index = 0U; index < view->file_count; ++index) {
        const uint8_t *record = view->bytes + view->file_offset +
            (size_t)index * PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES;
        struct byte_field path;
        struct byte_field soname;
        uint32_t owner;
        uint16_t kind;
        uint16_t flags;
        uint32_t mode;
        uint64_t length;
        enum package_state_status status;

        status = fixed_text(record, 128U, false, &path);
        if (status != PACKAGE_STATE_STATUS_OK || !package_path(&path) ||
            (index != 0U && (compare_fields(&previous, &path) >= 0 ||
                path_ancestor(&previous, &path)))) {
            return PACKAGE_STATE_STATUS_FILE;
        }
        previous = path;
        owner = read_u32(record + 128U);
        kind = read_u16(record + 132U);
        flags = read_u16(record + 134U);
        mode = read_u32(record + 136U);
        length = read_u64(record + 144U);
        if (owner >= view->package_count || kind == 0U || kind > 4U || flags != 0U ||
            read_u32(record + 140U) != 0U ||
            (mode != UINT32_C(0444) && mode != UINT32_C(0555)) ||
            (kind == 1U && mode != UINT32_C(0555)) ||
            length == 0U || length > PACKAGE_MAX_FILE_BYTES ||
            !zero_bytes(record + 248U, 8U) ||
            actual_counts[owner] == UINT16_MAX) {
            return PACKAGE_STATE_STATUS_FILE;
        }
        status = fixed_text(record + 184U, 64U, kind != 2U, &soname);
        if (status != PACKAGE_STATE_STATUS_OK ||
            (kind == 2U && !library_name(&soname)) ||
            (kind != 2U && soname.length != 0U)) {
            return PACKAGE_STATE_STATUS_FILE;
        }
        ++actual_counts[owner];
    }
    for (uint32_t index = 0U; index < view->package_count; ++index) {
        if (actual_counts[index] != owned_file_counts[index]) {
            return PACKAGE_STATE_STATUS_FILE;
        }
    }
    return PACKAGE_STATE_STATUS_OK;
}

enum package_state_status package_state_database_parse(
    const uint8_t *bytes,
    size_t byte_count,
    struct package_state_database_view *result
)
{
    static const uint8_t magic[8] = {
        'P', 'H', 'I', 'P', 'D', 'B', '0', '1'
    };
    static const uint8_t architecture[16] = {
        'x', '8', '6', '_', '6', '4', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    struct package_state_database_view parsed;
    uint16_t owned_file_counts[PACKAGE_STATE_DATABASE_MAX_PACKAGES] = { 0U };
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    uint64_t total;
    uint64_t package_offset;
    uint64_t edge_offset;
    uint64_t file_offset;
    size_t package_table_bytes;
    size_t edge_table_bytes;
    size_t file_table_bytes;
    size_t expected_edge;
    size_t expected_file;
    size_t expected_total;
    enum package_state_status status;

    if (bytes == NULL || result == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    if (byte_count < PACKAGE_STATE_DATABASE_HEADER_BYTES ||
        byte_count > PACKAGE_STATE_DATABASE_MAX_BYTES) {
        return PACKAGE_STATE_STATUS_LENGTH;
    }
    if (!equal_bytes(bytes, magic, sizeof(magic))) {
        return PACKAGE_STATE_STATUS_MAGIC;
    }
    total = read_u64(bytes + 16U);
    if (read_u16(bytes + 8U) != 1U ||
        read_u16(bytes + 10U) != PACKAGE_STATE_DATABASE_HEADER_BYTES ||
        read_u32(bytes + 12U) != 0U || total != (uint64_t)byte_count ||
        read_u64(bytes + 24U) == 0U || read_u32(bytes + 32U) == 0U ||
        read_u32(bytes + 36U) != 0U) {
        return PACKAGE_STATE_STATUS_HEADER;
    }
    if (!equal_bytes(bytes + 40U, architecture, sizeof(architecture))) {
        return PACKAGE_STATE_STATUS_ARCHITECTURE;
    }
    if (!zero_bytes(bytes + 136U, PACKAGE_STATE_DATABASE_HEADER_BYTES - 136U)) {
        return PACKAGE_STATE_STATUS_RESERVED;
    }

    package_offset = read_u64(bytes + 56U);
    parsed.package_count = read_u32(bytes + 64U);
    edge_offset = read_u64(bytes + 72U);
    parsed.edge_count = read_u32(bytes + 80U);
    file_offset = read_u64(bytes + 88U);
    parsed.file_count = read_u32(bytes + 96U);
    if (parsed.package_count > PACKAGE_STATE_DATABASE_MAX_PACKAGES ||
        parsed.edge_count > PACKAGE_STATE_DATABASE_MAX_EDGES ||
        parsed.file_count > PACKAGE_STATE_DATABASE_MAX_FILES ||
        read_u32(bytes + 68U) != PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES ||
        read_u32(bytes + 84U) != PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES ||
        read_u32(bytes + 100U) != PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES) {
        return PACKAGE_STATE_STATUS_TABLE;
    }
    if (!checked_multiply(parsed.package_count,
            PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES, &package_table_bytes) ||
        !checked_multiply(parsed.edge_count,
            PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES, &edge_table_bytes) ||
        !checked_multiply(parsed.file_count,
            PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES, &file_table_bytes) ||
        !checked_add(PACKAGE_STATE_DATABASE_HEADER_BYTES,
            package_table_bytes, &expected_edge) ||
        !checked_add(expected_edge, edge_table_bytes, &expected_file) ||
        !checked_add(expected_file, file_table_bytes, &expected_total)) {
        return PACKAGE_STATE_STATUS_OVERFLOW;
    }
    if (package_offset != PACKAGE_STATE_DATABASE_HEADER_BYTES ||
        edge_offset != (uint64_t)expected_edge || file_offset != (uint64_t)expected_file ||
        expected_total != byte_count) {
        return PACKAGE_STATE_STATUS_TABLE;
    }
    status = package_state_sha256(bytes + PACKAGE_STATE_DATABASE_HEADER_BYTES,
        byte_count - PACKAGE_STATE_DATABASE_HEADER_BYTES, digest);
    if (status != PACKAGE_STATE_STATUS_OK) {
        return status;
    }
    if (!equal_bytes(digest, bytes + 104U, sizeof(digest))) {
        return PACKAGE_STATE_STATUS_DIGEST;
    }

    parsed.bytes = bytes;
    parsed.byte_count = byte_count;
    parsed.generation = read_u64(bytes + 24U);
    parsed.abi = read_u32(bytes + 32U);
    parsed.package_offset = PACKAGE_STATE_DATABASE_HEADER_BYTES;
    parsed.edge_offset = expected_edge;
    parsed.file_offset = expected_file;
    status = validate_packages(&parsed, owned_file_counts);
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = validate_dependencies(&parsed);
    }
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = validate_files(&parsed, owned_file_counts);
    }
    if (status == PACKAGE_STATE_STATUS_OK) {
        *result = parsed;
    }
    return status;
}

static bool database_view_shape(
    const struct package_state_database_view *database
)
{
    size_t package_bytes;
    size_t edge_bytes;
    size_t file_bytes;
    size_t expected_edge;
    size_t expected_file;
    size_t expected_total;

    return database != NULL && database->bytes != NULL &&
        database->byte_count >= PACKAGE_STATE_DATABASE_HEADER_BYTES &&
        database->byte_count <= PACKAGE_STATE_DATABASE_MAX_BYTES &&
        database->package_count <= PACKAGE_STATE_DATABASE_MAX_PACKAGES &&
        database->edge_count <= PACKAGE_STATE_DATABASE_MAX_EDGES &&
        database->file_count <= PACKAGE_STATE_DATABASE_MAX_FILES &&
        checked_multiply(database->package_count,
            PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES, &package_bytes) &&
        checked_multiply(database->edge_count,
            PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES, &edge_bytes) &&
        checked_multiply(database->file_count,
            PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES, &file_bytes) &&
        checked_add(PACKAGE_STATE_DATABASE_HEADER_BYTES, package_bytes,
            &expected_edge) &&
        checked_add(expected_edge, edge_bytes, &expected_file) &&
        checked_add(expected_file, file_bytes, &expected_total) &&
        database->package_offset == PACKAGE_STATE_DATABASE_HEADER_BYTES &&
        database->edge_offset == expected_edge &&
        database->file_offset == expected_file &&
        database->byte_count == expected_total;
}

enum package_state_status package_state_database_package(
    const struct package_state_database_view *database,
    uint32_t index,
    struct package_state_package_view *result
)
{
    struct byte_field identifier;
    struct byte_field version;
    const uint8_t *record;
    uint32_t flags;
    uint32_t dependency_start;
    uint32_t dependency_count;
    uint32_t file_count;

    if (database == NULL || result == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    *result = (struct package_state_package_view){ 0 };
    if (!database_view_shape(database) || index >= database->package_count) {
        return PACKAGE_STATE_STATUS_TABLE;
    }
    record = package_record(database, index);
    flags = read_u32(record + 192U);
    dependency_start = read_u32(record + 196U);
    dependency_count = read_u32(record + 200U);
    file_count = read_u32(record + 204U);
    if (fixed_text(record, 64U, false, &identifier) !=
            PACKAGE_STATE_STATUS_OK || !package_identifier(&identifier) ||
        fixed_text(record + 64U, 64U, false, &version) !=
            PACKAGE_STATE_STATUS_OK ||
        !semantic_version(version.bytes, version.length) ||
        flags > DATABASE_FLAG_EXPLICIT ||
        dependency_start > database->edge_count ||
        dependency_count > database->edge_count - dependency_start ||
        dependency_count > PACKAGE_MAX_DEPENDENCIES ||
        file_count > PACKAGE_MAX_FILES || !zero_bytes(record + 208U, 48U)) {
        return PACKAGE_STATE_STATUS_PACKAGE;
    }
    result->database = database;
    result->package_index = index;
    result->identifier = (struct package_state_text){
        identifier.bytes, identifier.length
    };
    result->version = (struct package_state_text){
        version.bytes, version.length
    };
    result->package_sha256 = record + 128U;
    result->publisher_key_id = record + 160U;
    result->explicit_root = flags == DATABASE_FLAG_EXPLICIT;
    result->dependency_start = dependency_start;
    result->dependency_count = dependency_count;
    result->file_count = file_count;
    return PACKAGE_STATE_STATUS_OK;
}

enum package_state_status package_state_database_dependency(
    const struct package_state_database_view *database,
    uint32_t index,
    struct package_state_dependency_view *result
)
{
    struct byte_field requested;
    struct byte_field constraint;
    struct byte_field provider;
    const uint8_t *record;

    if (database == NULL || result == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    *result = (struct package_state_dependency_view){ 0 };
    if (!database_view_shape(database) || index >= database->edge_count) {
        return PACKAGE_STATE_STATUS_TABLE;
    }
    record = edge_record(database, index);
    if (fixed_text(record, 64U, false, &requested) !=
            PACKAGE_STATE_STATUS_OK || !package_identifier(&requested) ||
        fixed_text(record + 64U, 56U, false, &constraint) !=
            PACKAGE_STATE_STATUS_OK || !version_constraint(&constraint) ||
        fixed_text(record + 120U, 64U, false, &provider) !=
            PACKAGE_STATE_STATUS_OK || !package_identifier(&provider) ||
        !zero_bytes(record + 184U, 8U)) {
        return PACKAGE_STATE_STATUS_DEPENDENCY;
    }
    result->database = database;
    result->dependency_index = index;
    result->requested = (struct package_state_text){
        requested.bytes, requested.length
    };
    result->constraint = (struct package_state_text){
        constraint.bytes, constraint.length
    };
    result->provider = (struct package_state_text){
        provider.bytes, provider.length
    };
    return PACKAGE_STATE_STATUS_OK;
}

enum package_state_status package_state_database_file(
    const struct package_state_database_view *database,
    uint32_t index,
    struct package_state_file_view *result
)
{
    struct byte_field path;
    struct byte_field soname;
    const uint8_t *record;
    uint32_t owner;
    uint16_t kind;
    uint16_t flags;
    uint32_t mode;
    uint64_t length;

    if (database == NULL || result == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    *result = (struct package_state_file_view){ 0 };
    if (!database_view_shape(database) || index >= database->file_count) {
        return PACKAGE_STATE_STATUS_TABLE;
    }
    record = database->bytes + database->file_offset +
        (size_t)index * PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES;
    owner = read_u32(record + 128U);
    kind = read_u16(record + 132U);
    flags = read_u16(record + 134U);
    mode = read_u32(record + 136U);
    length = read_u64(record + 144U);
    if (fixed_text(record, 128U, false, &path) != PACKAGE_STATE_STATUS_OK ||
        !package_path(&path) || owner >= database->package_count || kind == 0U ||
        kind > 4U || flags != 0U || read_u32(record + 140U) != 0U ||
        (mode != UINT32_C(0444) && mode != UINT32_C(0555)) ||
        (kind == 1U && mode != UINT32_C(0555)) || length == 0U ||
        length > PACKAGE_MAX_FILE_BYTES ||
        fixed_text(record + 184U, 64U, kind != 2U, &soname) !=
            PACKAGE_STATE_STATUS_OK ||
        (kind == 2U && !library_name(&soname)) ||
        (kind != 2U && soname.length != 0U) ||
        !zero_bytes(record + 248U, 8U)) {
        return PACKAGE_STATE_STATUS_FILE;
    }
    result->database = database;
    result->file_index = index;
    result->path = (struct package_state_text){ path.bytes, path.length };
    result->owner_index = owner;
    result->kind = kind;
    result->mode = mode;
    result->length = length;
    result->sha256 = record + 152U;
    result->soname = (struct package_state_text){ soname.bytes, soname.length };
    return PACKAGE_STATE_STATUS_OK;
}

enum package_state_status package_state_authority_parse(
    const uint8_t *bytes,
    size_t byte_count,
    struct package_state_authority_view *result
)
{
    static const uint8_t magic[8] = {
        'P', 'H', 'I', 'P', 'G', 'N', '0', '1'
    };
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    enum package_state_status status;
    uint64_t database_bytes;

    if (bytes == NULL || result == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    if (byte_count != PACKAGE_STATE_AUTHORITY_BYTES) {
        return PACKAGE_STATE_STATUS_LENGTH;
    }
    if (!equal_bytes(bytes, magic, sizeof(magic))) {
        return PACKAGE_STATE_STATUS_MAGIC;
    }
    database_bytes = read_u64(bytes + 24U);
    if (read_u16(bytes + 8U) != 1U ||
        read_u16(bytes + 10U) != PACKAGE_STATE_AUTHORITY_BYTES ||
        read_u32(bytes + 12U) != 0U || read_u64(bytes + 16U) == 0U ||
        database_bytes < PACKAGE_STATE_DATABASE_HEADER_BYTES ||
        database_bytes > PACKAGE_STATE_DATABASE_MAX_BYTES) {
        return PACKAGE_STATE_STATUS_AUTHORITY;
    }
    if (!zero_bytes(bytes + 96U, PACKAGE_STATE_AUTHORITY_BYTES - 96U)) {
        return PACKAGE_STATE_STATUS_RESERVED;
    }
    status = package_state_sha256(bytes, 64U, digest);
    if (status != PACKAGE_STATE_STATUS_OK) {
        return status;
    }
    if (!equal_bytes(bytes + 64U, digest, sizeof(digest))) {
        return PACKAGE_STATE_STATUS_DIGEST;
    }
    result->generation = read_u64(bytes + 16U);
    result->database_bytes = database_bytes;
    copy_bytes(result->database_sha256, bytes + 32U,
        sizeof(result->database_sha256));
    return PACKAGE_STATE_STATUS_OK;
}

enum package_state_status package_state_journal_parse(
    const uint8_t *bytes,
    size_t byte_count,
    struct package_state_journal_view *result
)
{
    static const uint8_t magic[8] = {
        'P', 'H', 'I', 'P', 'T', 'X', '0', '1'
    };
    static const uint8_t zero_digest[PACKAGE_STATE_SHA256_BYTES] = { 0U };
    struct package_state_sha256_context context;
    struct byte_field target;
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    uint16_t operation;
    uint64_t base_generation;
    uint64_t target_generation;

    if (bytes == NULL || result == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    if (byte_count != PACKAGE_STATE_JOURNAL_BYTES) {
        return PACKAGE_STATE_STATUS_LENGTH;
    }
    if (!equal_bytes(bytes, magic, sizeof(magic))) {
        return PACKAGE_STATE_STATUS_MAGIC;
    }
    operation = read_u16(bytes + 16U);
    base_generation = read_u64(bytes + 24U);
    target_generation = read_u64(bytes + 32U);
    if (read_u16(bytes + 8U) != 1U ||
        read_u16(bytes + 10U) != PACKAGE_STATE_JOURNAL_BYTES ||
        read_u32(bytes + 12U) != 0U || operation == 0U || operation > 4U ||
        read_u16(bytes + 18U) != JOURNAL_PHASE_PREPARED ||
        read_u32(bytes + 20U) != 0U || base_generation == 0U ||
        base_generation == UINT64_MAX || target_generation != base_generation + 1U ||
        read_u64(bytes + 40U) == 0U ||
        read_u64(bytes + 48U) < PACKAGE_STATE_DATABASE_HEADER_BYTES ||
        read_u64(bytes + 48U) > PACKAGE_STATE_DATABASE_MAX_BYTES ||
        read_u64(bytes + 56U) < PACKAGE_STATE_DATABASE_HEADER_BYTES ||
        read_u64(bytes + 56U) > PACKAGE_STATE_DATABASE_MAX_BYTES) {
        return PACKAGE_STATE_STATUS_JOURNAL;
    }
    if (!zero_bytes(bytes + 224U, PACKAGE_STATE_JOURNAL_BYTES - 224U)) {
        return PACKAGE_STATE_STATUS_RESERVED;
    }
    if (fixed_text(bytes + 160U, 64U, true, &target) != PACKAGE_STATE_STATUS_OK ||
        (target.length != 0U && !package_identifier(&target))) {
        return PACKAGE_STATE_STATUS_TEXT;
    }
    enum package_state_status status = package_state_sha256_initialize(&context);
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = package_state_sha256_update(&context, bytes, 128U);
    }
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = package_state_sha256_update(&context, zero_digest,
            sizeof(zero_digest));
    }
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = package_state_sha256_update(&context, bytes + 160U,
            PACKAGE_STATE_JOURNAL_BYTES - 160U);
    }
    if (status != PACKAGE_STATE_STATUS_OK) {
        return status;
    }
    status = package_state_sha256_finish(&context, digest);
    if (status != PACKAGE_STATE_STATUS_OK) {
        return status;
    }
    if (!equal_bytes(digest, bytes + 128U, sizeof(digest))) {
        return PACKAGE_STATE_STATUS_DIGEST;
    }
    result->operation = (enum package_state_operation)operation;
    result->base_generation = base_generation;
    result->target_generation = target_generation;
    result->required_space = read_u64(bytes + 40U);
    result->base_database_bytes = read_u64(bytes + 48U);
    result->target_database_bytes = read_u64(bytes + 56U);
    copy_bytes(result->base_database_sha256, bytes + 64U,
        sizeof(result->base_database_sha256));
    copy_bytes(result->target_database_sha256, bytes + 96U,
        sizeof(result->target_database_sha256));
    copy_bytes(result->transaction_id, bytes + 128U,
        sizeof(result->transaction_id));
    return PACKAGE_STATE_STATUS_OK;
}

enum package_state_status package_state_authority_encode(
    const struct package_state_database_view *database,
    uint8_t result[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    static const uint8_t magic[8] = {
        'P', 'H', 'I', 'P', 'G', 'N', '0', '1'
    };
    struct package_state_database_view validated;
    enum package_state_status status;

    if (result == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    clear_bytes(result, PACKAGE_STATE_AUTHORITY_BYTES);
    if (database == NULL || database->bytes == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    status = package_state_database_parse(database->bytes,
        database->byte_count, &validated);
    if (status != PACKAGE_STATE_STATUS_OK) {
        return status;
    }
    copy_bytes(result, magic, sizeof(magic));
    write_u16(result + 8U, UINT16_C(1));
    write_u16(result + 10U, PACKAGE_STATE_AUTHORITY_BYTES);
    write_u64(result + 16U, validated.generation);
    write_u64(result + 24U, validated.byte_count);
    status = package_state_sha256(validated.bytes, validated.byte_count,
        result + 32U);
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = package_state_sha256(result, 64U, result + 64U);
    }
    if (status != PACKAGE_STATE_STATUS_OK) {
        clear_bytes(result, PACKAGE_STATE_AUTHORITY_BYTES);
    }
    return status;
}

enum package_state_status package_state_journal_encode(
    const struct package_state_journal_spec *spec,
    uint8_t result[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    static const uint8_t magic[8] = {
        'P', 'H', 'I', 'P', 'T', 'X', '0', '1'
    };
    struct package_state_database_view base;
    struct package_state_database_view target;
    struct package_state_journal_view validated;
    enum package_state_status status;

    if (result == NULL) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    clear_bytes(result, PACKAGE_STATE_JOURNAL_BYTES);
    if (spec == NULL || spec->base == NULL || spec->target == NULL ||
        spec->base->bytes == NULL || spec->target->bytes == NULL ||
        (spec->target_identifier == NULL && spec->target_identifier_bytes != 0U)) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    status = package_state_database_parse(spec->base->bytes,
        spec->base->byte_count, &base);
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = package_state_database_parse(spec->target->bytes,
            spec->target->byte_count, &target);
    }
    if (status != PACKAGE_STATE_STATUS_OK) {
        return status;
    }
    if (spec->operation <= PACKAGE_STATE_OPERATION_INVALID ||
        spec->operation > PACKAGE_STATE_OPERATION_REPAIR ||
        base.generation == UINT64_MAX ||
        target.generation != base.generation + 1U ||
        spec->required_space == 0U || spec->target_identifier_bytes >= 64U) {
        return PACKAGE_STATE_STATUS_JOURNAL;
    }
    copy_bytes(result, magic, sizeof(magic));
    write_u16(result + 8U, UINT16_C(1));
    write_u16(result + 10U, PACKAGE_STATE_JOURNAL_BYTES);
    write_u16(result + 16U, (uint16_t)spec->operation);
    write_u16(result + 18U, JOURNAL_PHASE_PREPARED);
    write_u64(result + 24U, base.generation);
    write_u64(result + 32U, target.generation);
    write_u64(result + 40U, spec->required_space);
    write_u64(result + 48U, base.byte_count);
    write_u64(result + 56U, target.byte_count);
    status = package_state_sha256(base.bytes, base.byte_count, result + 64U);
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = package_state_sha256(target.bytes, target.byte_count,
            result + 96U);
    }
    if (status == PACKAGE_STATE_STATUS_OK &&
            spec->target_identifier_bytes != 0U) {
        copy_bytes(result + 160U, spec->target_identifier,
            spec->target_identifier_bytes);
    }
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = package_state_sha256(result, PACKAGE_STATE_JOURNAL_BYTES,
            result + 128U);
    }
    if (status == PACKAGE_STATE_STATUS_OK) {
        status = package_state_journal_parse(result,
            PACKAGE_STATE_JOURNAL_BYTES, &validated);
    }
    if (status != PACKAGE_STATE_STATUS_OK) {
        clear_bytes(result, PACKAGE_STATE_JOURNAL_BYTES);
    }
    return status;
}

static bool generation_matches(
    const struct package_state_generation *generation,
    uint64_t expected_generation,
    uint64_t expected_bytes,
    const uint8_t expected_sha256[PACKAGE_STATE_SHA256_BYTES],
    struct package_state_database_view *view
)
{
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];

    if (generation == NULL || generation->database == NULL ||
        !generation->owned_files_complete ||
        expected_bytes != (uint64_t)generation->database_bytes ||
        package_state_database_parse(generation->database,
            generation->database_bytes, view) != PACKAGE_STATE_STATUS_OK ||
        view->generation != expected_generation ||
        package_state_sha256(generation->database,
            generation->database_bytes, digest) != PACKAGE_STATE_STATUS_OK) {
        return false;
    }
    return equal_bytes(digest, expected_sha256, sizeof(digest));
}

static void select_generation(
    enum package_state_recovery_choice choice,
    const struct package_state_database_view *view,
    struct package_state_recovery_result *result
)
{
    result->choice = choice;
    result->generation = view->generation;
    result->database = *view;
}

enum package_state_status package_state_recovery_decide(
    const uint8_t *authority,
    size_t authority_bytes,
    const uint8_t *journal,
    size_t journal_bytes,
    const struct package_state_generation *old_generation,
    const struct package_state_generation *new_generation,
    struct package_state_recovery_result *result
)
{
    struct package_state_authority_view authority_view;
    struct package_state_journal_view journal_view;
    struct package_state_database_view old_view;
    struct package_state_database_view new_view;
    enum package_state_status authority_status;
    enum package_state_status journal_status;
    bool old_complete;
    bool new_complete;

    if (authority == NULL || result == NULL ||
        (journal == NULL && journal_bytes != 0U) ||
        (journal != NULL && journal_bytes == 0U)) {
        return PACKAGE_STATE_STATUS_NULL_ARGUMENT;
    }
    result->choice = PACKAGE_STATE_RECOVERY_NONE;
    result->generation = 0U;
    authority_status = package_state_authority_parse(
        authority, authority_bytes, &authority_view);

    if (journal == NULL) {
        if (authority_status != PACKAGE_STATE_STATUS_OK) {
            return authority_status;
        }
        old_complete = generation_matches(old_generation,
            authority_view.generation, authority_view.database_bytes,
            authority_view.database_sha256, &old_view);
        new_complete = generation_matches(new_generation,
            authority_view.generation, authority_view.database_bytes,
            authority_view.database_sha256, &new_view);
        if (old_complete && new_complete) {
            return PACKAGE_STATE_STATUS_MISMATCH;
        }
        if (old_complete) {
            select_generation(PACKAGE_STATE_RECOVERY_OLD, &old_view, result);
            return PACKAGE_STATE_STATUS_OK;
        }
        if (new_complete) {
            select_generation(PACKAGE_STATE_RECOVERY_NEW, &new_view, result);
            return PACKAGE_STATE_STATUS_OK;
        }
        return PACKAGE_STATE_STATUS_INCOMPLETE;
    }

    journal_status = package_state_journal_parse(
        journal, journal_bytes, &journal_view);
    if (journal_status != PACKAGE_STATE_STATUS_OK) {
        return journal_status;
    }
    old_complete = generation_matches(old_generation,
        journal_view.base_generation, journal_view.base_database_bytes,
        journal_view.base_database_sha256, &old_view);
    new_complete = generation_matches(new_generation,
        journal_view.target_generation, journal_view.target_database_bytes,
        journal_view.target_database_sha256, &new_view);

    if (authority_status == PACKAGE_STATE_STATUS_OK) {
        if (authority_view.generation != journal_view.base_generation &&
            authority_view.generation != journal_view.target_generation) {
            return PACKAGE_STATE_STATUS_MISMATCH;
        }
        if (authority_view.generation == journal_view.base_generation) {
            if (authority_view.database_bytes != journal_view.base_database_bytes ||
                !equal_bytes(authority_view.database_sha256,
                    journal_view.base_database_sha256,
                    PACKAGE_STATE_SHA256_BYTES)) {
                return PACKAGE_STATE_STATUS_MISMATCH;
            }
        } else {
            if (authority_view.database_bytes != journal_view.target_database_bytes ||
                !equal_bytes(authority_view.database_sha256,
                    journal_view.target_database_sha256,
                    PACKAGE_STATE_SHA256_BYTES)) {
                return PACKAGE_STATE_STATUS_MISMATCH;
            }
            if (new_complete) {
                select_generation(PACKAGE_STATE_RECOVERY_NEW, &new_view, result);
                return PACKAGE_STATE_STATUS_OK;
            }
        }
        if (old_complete) {
            select_generation(PACKAGE_STATE_RECOVERY_OLD, &old_view, result);
            return PACKAGE_STATE_STATUS_OK;
        }
        if (journal_view.operation == PACKAGE_STATE_OPERATION_REPAIR &&
            new_complete) {
            select_generation(PACKAGE_STATE_RECOVERY_NEW, &new_view, result);
            return PACKAGE_STATE_STATUS_OK;
        }
        return PACKAGE_STATE_STATUS_INCOMPLETE;
    }
    if (old_complete) {
        select_generation(PACKAGE_STATE_RECOVERY_OLD, &old_view, result);
        return PACKAGE_STATE_STATUS_OK;
    }
    return PACKAGE_STATE_STATUS_INCOMPLETE;
}

const char *package_state_status_string(enum package_state_status status)
{
    static const char *const names[] = {
        "ok",
        "null argument",
        "invalid length",
        "invalid magic",
        "invalid header",
        "nonzero reserved bytes",
        "integer overflow",
        "digest mismatch",
        "invalid canonical text",
        "unsupported architecture",
        "invalid table layout",
        "invalid package record",
        "invalid dependency graph",
        "invalid file record",
        "invalid authority record",
        "invalid journal record",
        "state mismatch",
        "no complete generation",
        "invalid hash sequence"
    };

    _Static_assert(sizeof(names) / sizeof(names[0]) ==
        PACKAGE_STATE_STATUS_COUNT, "package-state status table is incomplete");

    if (status < PACKAGE_STATE_STATUS_OK || status >= PACKAGE_STATE_STATUS_COUNT) {
        return "unknown package-state status";
    }
    return names[status];
}
