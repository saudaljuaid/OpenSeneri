/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_PACKAGE_TRUST_H
#define PHIPIA_PACKAGE_TRUST_H

#include <stddef.h>
#include <stdint.h>

#include <phipia/package_manager.h>

#define PACKAGE_TRUST_MAX_KEYS 64U
#define PACKAGE_TRUST_TABLE_HEADER_BYTES 128U
#define PACKAGE_TRUST_TABLE_RECORD_BYTES 96U
#define PACKAGE_TRUST_TABLE_MAX_BYTES \
    (PACKAGE_TRUST_TABLE_HEADER_BYTES + \
        PACKAGE_TRUST_MAX_KEYS * PACKAGE_TRUST_TABLE_RECORD_BYTES)

enum package_trust_status {
    PACKAGE_TRUST_STATUS_OK = 0,
    PACKAGE_TRUST_STATUS_NULL_ARGUMENT,
    PACKAGE_TRUST_STATUS_BOUND,
    PACKAGE_TRUST_STATUS_LENGTH,
    PACKAGE_TRUST_STATUS_MAGIC,
    PACKAGE_TRUST_STATUS_HEADER,
    PACKAGE_TRUST_STATUS_RESERVED,
    PACKAGE_TRUST_STATUS_DIGEST,
    PACKAGE_TRUST_STATUS_TABLE,
    PACKAGE_TRUST_STATUS_ORDER,
    PACKAGE_TRUST_STATUS_KEY_ID,
    PACKAGE_TRUST_STATUS_PUBLIC_KEY,
    PACKAGE_TRUST_STATUS_COUNT
};

struct package_trust_key {
    uint8_t key_id[PACKAGE_MANAGER_SHA256_BYTES];
    uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES];
    enum package_manager_key_status status;
};

/* The admitted key array must remain live and immutable with the store. */
struct package_trust_store {
    const struct package_trust_key *keys;
    size_t key_count;
};

/* Caller-owned storage for one admitted immutable platform trust table. */
struct package_trust_table {
    struct package_trust_key keys[PACKAGE_TRUST_MAX_KEYS];
    struct package_trust_store store;
};

/*
 * Decode a canonical platform-provisioned table. The input may be released
 * after this returns; the admitted keys live in result. Result is cleared on
 * every refusal so malformed platform bytes cannot leave a usable store.
 */
enum package_trust_status package_trust_table_open(
    const uint8_t *bytes,
    size_t byte_count,
    struct package_trust_table *result
);

enum package_trust_status package_trust_open(
    const struct package_trust_key *keys,
    size_t key_count,
    struct package_trust_store *result
);

void package_trust_manager(
    struct package_trust_store *store,
    struct package_manager_trust *result
);

enum package_manager_key_status package_trust_lookup(
    void *context,
    const uint8_t key_id[PACKAGE_MANAGER_SHA256_BYTES],
    uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES]
);

bool package_trust_verify(
    void *context,
    const uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t signature[PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES],
    const uint8_t *message,
    size_t message_bytes,
    size_t zero_offset,
    size_t zero_bytes
);

#endif
