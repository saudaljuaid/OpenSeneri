/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PACKAGE_TRUST_WASM
#include <stdio.h>
#endif

#include <phipia/package_trust.h>
#include <phipia/package_platform_trust.h>

/*
 * Python cryptography Ed25519 key from seed 00..1f, signing
 * "prefix" || 64 zero bytes || "suffix". The independent provider fixes the
 * expected wire bytes; the guest verifies them without host crypto callbacks.
 */
static const uint8_t public_key[32] = {
    0x03U, 0xa1U, 0x07U, 0xbfU, 0xf3U, 0xceU, 0x10U, 0xbeU,
    0x1dU, 0x70U, 0xddU, 0x18U, 0xe7U, 0x4bU, 0xc0U, 0x99U,
    0x67U, 0xe4U, 0xd6U, 0x30U, 0x9bU, 0xa5U, 0x0dU, 0x5fU,
    0x1dU, 0xdcU, 0x86U, 0x64U, 0x12U, 0x55U, 0x31U, 0xb8U
};

static const uint8_t key_id[32] = {
    0x56U, 0x47U, 0x5aU, 0xa7U, 0x54U, 0x63U, 0x47U, 0x4cU,
    0x02U, 0x85U, 0xdfU, 0x5dU, 0xbfU, 0x2bU, 0xcaU, 0xb7U,
    0x3dU, 0xa6U, 0x51U, 0x35U, 0x88U, 0x39U, 0xe9U, 0xb7U,
    0x74U, 0x81U, 0xb2U, 0xeaU, 0xb1U, 0x07U, 0x70U, 0x8cU
};

static const uint8_t publisher_key_id[32] = {
    0x24U, 0xf6U, 0xedU, 0x6aU, 0xcbU, 0xfeU, 0x10U, 0x09U,
    0xc0U, 0x30U, 0xd7U, 0xcaU, 0x56U, 0x7cU, 0x33U, 0xcaU,
    0x48U, 0x30U, 0x91U, 0x14U, 0x98U, 0x23U, 0x6bU, 0x55U,
    0x61U, 0xa6U, 0xc8U, 0x2aU, 0xbeU, 0xc5U, 0xdeU, 0x28U
};

static const uint8_t publisher_public_key[32] = {
    0x29U, 0xacU, 0xbaU, 0xe1U, 0x41U, 0xbcU, 0xcaU, 0xf0U,
    0xb2U, 0x2eU, 0x1aU, 0x94U, 0xd3U, 0x4dU, 0x0bU, 0xc7U,
    0x36U, 0x1eU, 0x52U, 0x6dU, 0x0bU, 0xfeU, 0x12U, 0xc8U,
    0x97U, 0x94U, 0xbcU, 0x93U, 0x22U, 0x96U, 0x6dU, 0xd7U
};

static const uint8_t signature[64] = {
    0xeaU, 0xa8U, 0xdfU, 0xc7U, 0x2cU, 0xccU, 0x7bU, 0x18U,
    0x2eU, 0xc4U, 0x3bU, 0x51U, 0x8bU, 0x42U, 0x09U, 0x83U,
    0x38U, 0xacU, 0x74U, 0x60U, 0x68U, 0xe1U, 0x4cU, 0x96U,
    0x54U, 0x72U, 0x7fU, 0x51U, 0xacU, 0xc2U, 0x90U, 0x70U,
    0x97U, 0xc9U, 0x23U, 0x1cU, 0x0cU, 0xa8U, 0x4fU, 0x13U,
    0xc2U, 0xf7U, 0x07U, 0x7fU, 0xedU, 0x90U, 0x14U, 0x90U,
    0x0bU, 0x47U, 0x34U, 0x93U, 0xaeU, 0x53U, 0x86U, 0x66U,
    0xdeU, 0x69U, 0x60U, 0x5cU, 0xbdU, 0x0bU, 0x03U, 0x0dU
};

static const uint8_t message[76] = {
    'p', 'r', 'e', 'f', 'i', 'x',
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
    's', 'u', 'f', 'f', 'i', 'x'
};

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        destination[index] = source[index];
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

static void clear_bytes(uint8_t *destination, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        destination[index] = 0U;
    }
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

static bool cleared_table(const struct package_trust_table *table)
{
    const uint8_t *bytes = (const uint8_t *)table;
    for (size_t index = 0U; index < sizeof(*table); ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool make_table(uint8_t *table, size_t byte_count)
{
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    if (byte_count != PACKAGE_TRUST_TABLE_HEADER_BYTES +
        PACKAGE_TRUST_TABLE_RECORD_BYTES) {
        return false;
    }
    clear_bytes(table, byte_count);
    copy_bytes(table, (const uint8_t *)"PHIPKEY1", 8U);
    write_u16(table + 8U, 1U);
    write_u16(table + 10U, PACKAGE_TRUST_TABLE_HEADER_BYTES);
    write_u64(table + 16U, byte_count);
    write_u32(table + 24U, 1U);
    write_u32(table + 28U, PACKAGE_TRUST_TABLE_RECORD_BYTES);
    copy_bytes(table + PACKAGE_TRUST_TABLE_HEADER_BYTES, key_id, sizeof(key_id));
    copy_bytes(table + PACKAGE_TRUST_TABLE_HEADER_BYTES + 32U,
        public_key, sizeof(public_key));
    write_u16(table + PACKAGE_TRUST_TABLE_HEADER_BYTES + 64U, 1U);
    if (package_state_sha256(table + PACKAGE_TRUST_TABLE_HEADER_BYTES,
        PACKAGE_TRUST_TABLE_RECORD_BYTES, digest) != PACKAGE_STATE_STATUS_OK) {
        return false;
    }
    copy_bytes(table + 32U, digest, sizeof(digest));
    return true;
}

static int package_trust_test(void)
{
    static const uint8_t scalar_order[32] = {
        0xedU, 0xd3U, 0xf5U, 0x5cU, 0x1aU, 0x63U, 0x12U, 0x58U,
        0xd6U, 0x9cU, 0xf7U, 0xa2U, 0xdeU, 0xf9U, 0xdeU, 0x14U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x10U
    };
    struct package_trust_key key;
    struct package_trust_key duplicate_keys[2];
    struct package_trust_store store;
    struct package_trust_table table_result;
    struct package_manager_trust trust;
    uint8_t table[PACKAGE_TRUST_TABLE_HEADER_BYTES +
        PACKAGE_TRUST_TABLE_RECORD_BYTES];
    uint8_t changed_signature[64];
    uint8_t looked_up[32];
    uint8_t unknown_id[32] = { 0U };
    if (package_platform_trust_key_count() != 0U ||
        package_platform_trust_manager(&trust) || trust.lookup != NULL ||
        trust.verify != NULL || trust.context != NULL) {
        return 22;
    }
    if (package_platform_trust_initialize() != PACKAGE_TRUST_STATUS_OK ||
        package_platform_trust_initialize() != PACKAGE_TRUST_STATUS_OK ||
        package_platform_trust_key_count() != 2U ||
        !package_platform_trust_manager(&trust) || trust.lookup == NULL ||
        trust.verify == NULL || trust.context == NULL ||
        trust.lookup(trust.context, key_id, looked_up) !=
            PACKAGE_MANAGER_KEY_TRUSTED ||
        !equal_bytes(looked_up, public_key, sizeof(looked_up)) ||
        trust.lookup(trust.context, publisher_key_id, looked_up) !=
            PACKAGE_MANAGER_KEY_TRUSTED ||
        !equal_bytes(looked_up, publisher_public_key, sizeof(looked_up))) {
        return 23;
    }
    if (package_trust_open(NULL, 0U, &store) != PACKAGE_TRUST_STATUS_OK) {
        return 1;
    }
    package_trust_manager(&store, &trust);
    if (trust.verify(trust.context, public_key, signature, message,
        sizeof(message), 6U, 64U)) {
        return 2;
    }
    copy_bytes(key.key_id, key_id, sizeof(key.key_id));
    copy_bytes(key.public_key, public_key, sizeof(key.public_key));
    key.status = PACKAGE_MANAGER_KEY_TRUSTED;
    if (package_trust_open(&key, 1U, &store) != PACKAGE_TRUST_STATUS_OK) {
        return 3;
    }
    package_trust_manager(&store, &trust);
    if (trust.lookup == NULL || trust.verify == NULL || trust.context != &store ||
        trust.lookup(trust.context, key_id, looked_up) != PACKAGE_MANAGER_KEY_TRUSTED ||
        !equal_bytes(looked_up, public_key, sizeof(looked_up)) ||
        trust.lookup(trust.context, unknown_id, looked_up) !=
            PACKAGE_MANAGER_KEY_UNKNOWN ||
        !trust.verify(trust.context, public_key, signature, message,
            sizeof(message), 6U, 64U)) {
        return 4;
    }
    if (trust.verify(trust.context, public_key, signature, message,
        sizeof(message), 5U, 64U)) {
        return 5;
    }
    copy_bytes(changed_signature, signature, sizeof(changed_signature));
    changed_signature[0] ^= UINT8_C(1);
    if (trust.verify(trust.context, public_key, changed_signature, message,
        sizeof(message), 6U, 64U)) {
        return 6;
    }
    for (size_t index = 0U; index < 32U; ++index) {
        changed_signature[index] = 0U;
    }
    if (trust.verify(trust.context, public_key, changed_signature, message,
        sizeof(message), 6U, 64U)) {
        return 7;
    }
    copy_bytes(changed_signature, signature, sizeof(changed_signature));
    changed_signature[0] = UINT8_C(0xed);
    for (size_t index = 1U; index < 31U; ++index) {
        changed_signature[index] = UINT8_C(0xff);
    }
    changed_signature[31] = UINT8_C(0x7f);
    if (trust.verify(trust.context, public_key, changed_signature, message,
        sizeof(message), 6U, 64U)) {
        return 8;
    }
    copy_bytes(changed_signature, signature, sizeof(changed_signature));
    copy_bytes(changed_signature + 32U, scalar_order, sizeof(scalar_order));
    if (trust.verify(trust.context, public_key, changed_signature, message,
        sizeof(message), 6U, 64U)) {
        return 9;
    }
    key.status = PACKAGE_MANAGER_KEY_REVOKED;
    if (package_trust_open(&key, 1U, &store) != PACKAGE_TRUST_STATUS_OK ||
        trust.lookup(trust.context, key_id, looked_up) !=
            PACKAGE_MANAGER_KEY_REVOKED ||
        trust.verify(trust.context, public_key, signature, message,
            sizeof(message), 6U, 64U)) {
        return 10;
    }
    key.status = PACKAGE_MANAGER_KEY_TRUSTED;
    key.key_id[0] ^= UINT8_C(1);
    if (package_trust_open(&key, 1U, &store) != PACKAGE_TRUST_STATUS_KEY_ID ||
        trust.verify(trust.context, public_key, signature, message,
            sizeof(message), 6U, 64U)) {
        return 11;
    }
    key.key_id[0] ^= UINT8_C(1);
    duplicate_keys[0] = key;
    duplicate_keys[1] = key;
    if (package_trust_open(duplicate_keys, 2U, &store) !=
        PACKAGE_TRUST_STATUS_ORDER) {
        return 12;
    }
    for (size_t index = 0U; index < sizeof(key.public_key); ++index) {
        key.public_key[index] = 0U;
    }
    if (package_state_sha256(key.public_key, sizeof(key.public_key), key.key_id) !=
            PACKAGE_STATE_STATUS_OK ||
        package_trust_open(&key, 1U, &store) !=
            PACKAGE_TRUST_STATUS_PUBLIC_KEY) {
        return 13;
    }
    if (!make_table(table, sizeof(table)) ||
        package_trust_table_open(table, sizeof(table), &table_result) !=
            PACKAGE_TRUST_STATUS_OK || table_result.store.key_count != 1U ||
        table_result.store.keys != table_result.keys ||
        package_trust_lookup(&table_result.store, key_id, looked_up) !=
            PACKAGE_MANAGER_KEY_TRUSTED ||
        !equal_bytes(looked_up, public_key, sizeof(looked_up))) {
        return 14;
    }
    table[0] ^= UINT8_C(1);
    if (package_trust_table_open(table, sizeof(table), &table_result) !=
            PACKAGE_TRUST_STATUS_MAGIC || !cleared_table(&table_result)) {
        return 15;
    }
    table[0] ^= UINT8_C(1);
    table[12U] = 1U;
    if (package_trust_table_open(table, sizeof(table), &table_result) !=
            PACKAGE_TRUST_STATUS_HEADER || !cleared_table(&table_result)) {
        return 16;
    }
    table[12U] = 0U;
    table[64U] = 1U;
    if (package_trust_table_open(table, sizeof(table), &table_result) !=
            PACKAGE_TRUST_STATUS_RESERVED || !cleared_table(&table_result)) {
        return 17;
    }
    table[64U] = 0U;
    table[PACKAGE_TRUST_TABLE_HEADER_BYTES + 32U] ^= UINT8_C(1);
    if (package_trust_table_open(table, sizeof(table), &table_result) !=
            PACKAGE_TRUST_STATUS_DIGEST || !cleared_table(&table_result)) {
        return 18;
    }
    if (!make_table(table, sizeof(table))) {
        return 19;
    }
    write_u16(table + PACKAGE_TRUST_TABLE_HEADER_BYTES + 64U, 3U);
    if (package_state_sha256(table + PACKAGE_TRUST_TABLE_HEADER_BYTES,
        PACKAGE_TRUST_TABLE_RECORD_BYTES, table + 32U) !=
            PACKAGE_STATE_STATUS_OK ||
        package_trust_table_open(table, sizeof(table), &table_result) !=
            PACKAGE_TRUST_STATUS_TABLE || !cleared_table(&table_result)) {
        return 20;
    }
    if (package_trust_table_open(NULL, sizeof(table), &table_result) !=
            PACKAGE_TRUST_STATUS_NULL_ARGUMENT || !cleared_table(&table_result) ||
        package_trust_table_open(table, sizeof(table) - 1U, &table_result) !=
            PACKAGE_TRUST_STATUS_HEADER || !cleared_table(&table_result)) {
        return 21;
    }
    return 0;
}

#ifdef PHIPIA_PACKAGE_TRUST_WASM
int package_trust_wasm_test(void);

int package_trust_wasm_test(void)
{
    return package_trust_test();
}
#else
int main(void)
{
    int status = package_trust_test();
    if (status != 0) {
        (void)fprintf(stderr, "package trust test failed: %d\n", status);
        return status;
    }
    (void)puts("Phipia Ed25519 package trust tests passed");
    return 0;
}
#endif
