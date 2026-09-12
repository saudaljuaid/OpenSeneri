/* SPDX-License-Identifier: GPL-3.0-only */
#include <phipia/package_trust.h>

#include <monocypher-ed25519.h>

#define TRUST_TABLE_VERSION UINT16_C(1)
#define TRUST_RECORD_STATUS_TRUSTED UINT16_C(1)
#define TRUST_RECORD_STATUS_REVOKED UINT16_C(2)

static const uint8_t trust_table_magic[8] = {
    'P', 'H', 'I', 'P', 'K', 'E', 'Y', '1'
};

static const uint8_t field_prime[32] = {
    0xedU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
    0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
    0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
    0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x7fU
};

/* Canonical low-order encodings plus the two canonical-y negative-zero forms. */
static const uint8_t invalid_points[10][32] = {
    { 0x01U },
    {
        0x26U, 0xe8U, 0x95U, 0x8fU, 0xc2U, 0xb2U, 0x27U, 0xb0U,
        0x45U, 0xc3U, 0xf4U, 0x89U, 0xf2U, 0xefU, 0x98U, 0xf0U,
        0xd5U, 0xdfU, 0xacU, 0x05U, 0xd3U, 0xc6U, 0x33U, 0x39U,
        0xb1U, 0x38U, 0x02U, 0x88U, 0x6dU, 0x53U, 0xfcU, 0x05U
    },
    { 0x00U },
    {
        0xc7U, 0x17U, 0x6aU, 0x70U, 0x3dU, 0x4dU, 0xd8U, 0x4fU,
        0xbaU, 0x3cU, 0x0bU, 0x76U, 0x0dU, 0x10U, 0x67U, 0x0fU,
        0x2aU, 0x20U, 0x53U, 0xfaU, 0x2cU, 0x39U, 0xccU, 0xc6U,
        0x4eU, 0xc7U, 0xfdU, 0x77U, 0x92U, 0xacU, 0x03U, 0x7aU
    },
    {
        0xecU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x7fU
    },
    {
        0xc7U, 0x17U, 0x6aU, 0x70U, 0x3dU, 0x4dU, 0xd8U, 0x4fU,
        0xbaU, 0x3cU, 0x0bU, 0x76U, 0x0dU, 0x10U, 0x67U, 0x0fU,
        0x2aU, 0x20U, 0x53U, 0xfaU, 0x2cU, 0x39U, 0xccU, 0xc6U,
        0x4eU, 0xc7U, 0xfdU, 0x77U, 0x92U, 0xacU, 0x03U, 0xfaU
    },
    {
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U
    },
    {
        0x26U, 0xe8U, 0x95U, 0x8fU, 0xc2U, 0xb2U, 0x27U, 0xb0U,
        0x45U, 0xc3U, 0xf4U, 0x89U, 0xf2U, 0xefU, 0x98U, 0xf0U,
        0xd5U, 0xdfU, 0xacU, 0x05U, 0xd3U, 0xc6U, 0x33U, 0x39U,
        0xb1U, 0x38U, 0x02U, 0x88U, 0x6dU, 0x53U, 0xfcU, 0x85U
    },
    {
        0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U
    },
    {
        0xecU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
    }
};

static bool equal_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t difference = 0U;
    for (size_t index = 0U; index < count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static int compare_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
    }
    return 0;
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

static bool zero_bytes(const uint8_t *bytes, size_t count)
{
    uint8_t combined = 0U;
    for (size_t index = 0U; index < count; ++index) {
        combined |= bytes[index];
    }
    return combined == 0U;
}

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

enum package_trust_status package_trust_table_open(
    const uint8_t *bytes,
    size_t byte_count,
    struct package_trust_table *result
)
{
    uint32_t key_count;
    size_t records_bytes;
    uint8_t digest[PACKAGE_MANAGER_SHA256_BYTES];
    enum package_trust_status status;
    if (result == NULL) {
        return PACKAGE_TRUST_STATUS_NULL_ARGUMENT;
    }
    clear_bytes((uint8_t *)result, sizeof(*result));
    if (bytes == NULL) {
        return PACKAGE_TRUST_STATUS_NULL_ARGUMENT;
    }
    if (byte_count < PACKAGE_TRUST_TABLE_HEADER_BYTES ||
        byte_count > PACKAGE_TRUST_TABLE_MAX_BYTES) {
        return PACKAGE_TRUST_STATUS_LENGTH;
    }
    if (!equal_bytes(bytes, trust_table_magic, sizeof(trust_table_magic))) {
        return PACKAGE_TRUST_STATUS_MAGIC;
    }
    key_count = read_u32(bytes + 24U);
    if (read_u16(bytes + 8U) != TRUST_TABLE_VERSION ||
        read_u16(bytes + 10U) != PACKAGE_TRUST_TABLE_HEADER_BYTES ||
        read_u32(bytes + 12U) != 0U ||
        read_u64(bytes + 16U) != (uint64_t)byte_count ||
        read_u32(bytes + 28U) != PACKAGE_TRUST_TABLE_RECORD_BYTES) {
        return PACKAGE_TRUST_STATUS_HEADER;
    }
    if (key_count > PACKAGE_TRUST_MAX_KEYS) {
        return PACKAGE_TRUST_STATUS_BOUND;
    }
    records_bytes = (size_t)key_count * PACKAGE_TRUST_TABLE_RECORD_BYTES;
    if (records_bytes != byte_count - PACKAGE_TRUST_TABLE_HEADER_BYTES) {
        return PACKAGE_TRUST_STATUS_TABLE;
    }
    if (!zero_bytes(bytes + 64U, PACKAGE_TRUST_TABLE_HEADER_BYTES - 64U)) {
        return PACKAGE_TRUST_STATUS_RESERVED;
    }
    if (package_state_sha256(bytes + PACKAGE_TRUST_TABLE_HEADER_BYTES,
        records_bytes, digest) != PACKAGE_STATE_STATUS_OK ||
        !equal_bytes(digest, bytes + 32U, sizeof(digest))) {
        return PACKAGE_TRUST_STATUS_DIGEST;
    }
    for (size_t index = 0U; index < key_count; ++index) {
        const uint8_t *record = bytes + PACKAGE_TRUST_TABLE_HEADER_BYTES +
            index * PACKAGE_TRUST_TABLE_RECORD_BYTES;
        uint16_t encoded_status = read_u16(record + 64U);
        if (!zero_bytes(record + 66U, PACKAGE_TRUST_TABLE_RECORD_BYTES - 66U)) {
            clear_bytes((uint8_t *)result, sizeof(*result));
            return PACKAGE_TRUST_STATUS_RESERVED;
        }
        if (encoded_status != TRUST_RECORD_STATUS_TRUSTED &&
            encoded_status != TRUST_RECORD_STATUS_REVOKED) {
            clear_bytes((uint8_t *)result, sizeof(*result));
            return PACKAGE_TRUST_STATUS_TABLE;
        }
        copy_bytes(result->keys[index].key_id, record,
            sizeof(result->keys[index].key_id));
        copy_bytes(result->keys[index].public_key, record + 32U,
            sizeof(result->keys[index].public_key));
        result->keys[index].status = encoded_status == TRUST_RECORD_STATUS_TRUSTED
            ? PACKAGE_MANAGER_KEY_TRUSTED : PACKAGE_MANAGER_KEY_REVOKED;
    }
    status = package_trust_open(result->keys, key_count, &result->store);
    if (status != PACKAGE_TRUST_STATUS_OK) {
        clear_bytes((uint8_t *)result, sizeof(*result));
        return status;
    }
    return PACKAGE_TRUST_STATUS_OK;
}

static bool canonical_non_small_order_point(const uint8_t point[32])
{
    uint8_t y[32];
    bool less_than_prime = false;
    copy_bytes(y, point, sizeof(y));
    y[31] &= UINT8_C(0x7f);
    for (size_t index = sizeof(y); index > 0U; --index) {
        size_t current = index - 1U;
        if (y[current] != field_prime[current]) {
            less_than_prime = y[current] < field_prime[current];
            break;
        }
    }
    if (!less_than_prime) {
        return false;
    }
    for (size_t index = 0U; index <
        sizeof(invalid_points) / sizeof(invalid_points[0]); ++index) {
        if (equal_bytes(point, invalid_points[index], sizeof(invalid_points[index]))) {
            return false;
        }
    }
    return true;
}

enum package_trust_status package_trust_open(
    const struct package_trust_key *keys,
    size_t key_count,
    struct package_trust_store *result
)
{
    if (result == NULL) {
        return PACKAGE_TRUST_STATUS_NULL_ARGUMENT;
    }
    result->keys = NULL;
    result->key_count = 0U;
    if (keys == NULL && key_count != 0U) {
        return PACKAGE_TRUST_STATUS_NULL_ARGUMENT;
    }
    if (key_count > PACKAGE_TRUST_MAX_KEYS) {
        return PACKAGE_TRUST_STATUS_BOUND;
    }
    for (size_t index = 0U; index < key_count; ++index) {
        uint8_t key_id[PACKAGE_MANAGER_SHA256_BYTES];
        if (keys[index].status != PACKAGE_MANAGER_KEY_TRUSTED &&
            keys[index].status != PACKAGE_MANAGER_KEY_REVOKED) {
            return PACKAGE_TRUST_STATUS_PUBLIC_KEY;
        }
        if (package_state_sha256(keys[index].public_key,
            sizeof(keys[index].public_key), key_id) != PACKAGE_STATE_STATUS_OK ||
            !equal_bytes(key_id, keys[index].key_id, sizeof(key_id))) {
            return PACKAGE_TRUST_STATUS_KEY_ID;
        }
        if (!canonical_non_small_order_point(keys[index].public_key)) {
            return PACKAGE_TRUST_STATUS_PUBLIC_KEY;
        }
        if (index != 0U && compare_bytes(keys[index - 1U].key_id,
            keys[index].key_id, sizeof(keys[index].key_id)) >= 0) {
            return PACKAGE_TRUST_STATUS_ORDER;
        }
    }
    result->keys = keys;
    result->key_count = key_count;
    return PACKAGE_TRUST_STATUS_OK;
}

enum package_manager_key_status package_trust_lookup(
    void *context,
    const uint8_t key_id[PACKAGE_MANAGER_SHA256_BYTES],
    uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES]
)
{
    const struct package_trust_store *store = context;
    if (store == NULL || key_id == NULL || public_key == NULL ||
        (store->keys == NULL && store->key_count != 0U) ||
        store->key_count > PACKAGE_TRUST_MAX_KEYS) {
        return PACKAGE_MANAGER_KEY_UNKNOWN;
    }
    for (size_t index = 0U; index < store->key_count; ++index) {
        if (equal_bytes(key_id, store->keys[index].key_id,
            PACKAGE_MANAGER_SHA256_BYTES)) {
            copy_bytes(public_key, store->keys[index].public_key,
                PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES);
            return store->keys[index].status;
        }
    }
    return PACKAGE_MANAGER_KEY_UNKNOWN;
}

bool package_trust_verify(
    void *context,
    const uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t signature[PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES],
    const uint8_t *message,
    size_t message_bytes,
    size_t zero_offset,
    size_t zero_bytes
)
{
    static const uint8_t zeros[PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES] = { 0U };
    const struct package_trust_store *store = context;
    crypto_sha512_ctx hash_context;
    uint8_t expanded_hash[64];
    uint8_t reduced_hash[32];
    bool trusted = false;
    if (store == NULL || public_key == NULL || signature == NULL ||
        message == NULL || store->keys == NULL ||
        store->key_count == 0U || store->key_count > PACKAGE_TRUST_MAX_KEYS ||
        zero_bytes != PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES ||
        zero_offset > message_bytes || zero_bytes > message_bytes - zero_offset ||
        !canonical_non_small_order_point(public_key) ||
        !canonical_non_small_order_point(signature)) {
        return false;
    }
    for (size_t index = 0U; index < store->key_count; ++index) {
        if (store->keys[index].status == PACKAGE_MANAGER_KEY_TRUSTED &&
            equal_bytes(public_key, store->keys[index].public_key,
                PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES)) {
            trusted = true;
        }
    }
    if (!trusted) {
        return false;
    }
    crypto_sha512_init(&hash_context);
    crypto_sha512_update(&hash_context, signature, 32U);
    crypto_sha512_update(&hash_context, public_key,
        PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES);
    crypto_sha512_update(&hash_context, message, zero_offset);
    crypto_sha512_update(&hash_context, zeros, zero_bytes);
    crypto_sha512_update(&hash_context, message + zero_offset + zero_bytes,
        message_bytes - zero_offset - zero_bytes);
    crypto_sha512_final(&hash_context, expanded_hash);
    crypto_eddsa_reduce(reduced_hash, expanded_hash);
    return crypto_eddsa_check_equation(signature, public_key, reduced_hash) == 0;
}

void package_trust_manager(
    struct package_trust_store *store,
    struct package_manager_trust *result
)
{
    if (result == NULL) {
        return;
    }
    result->lookup = package_trust_lookup;
    result->verify = package_trust_verify;
    result->context = store;
}
