/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/package_generation.h>
#include <phipia/package_state.h>

#define OLD_DATABASE_BYTES PACKAGE_STATE_DATABASE_HEADER_BYTES
#define NEW_PACKAGE_COUNT 2U
#define NEW_EDGE_COUNT 1U
#define NEW_FILE_COUNT 2U
#define NEW_EDGE_OFFSET (PACKAGE_STATE_DATABASE_HEADER_BYTES + \
    NEW_PACKAGE_COUNT * PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES)
#define NEW_FILE_OFFSET (NEW_EDGE_OFFSET + \
    NEW_EDGE_COUNT * PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES)
#define NEW_DATABASE_BYTES (NEW_FILE_OFFSET + \
    NEW_FILE_COUNT * PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES)

#define CHECK(condition, code) do { if (!(condition)) { return (code); } } while (0)

static void clear_bytes(uint8_t *bytes, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static void put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static void put_u64(uint8_t *bytes, uint64_t value)
{
    put_u32(bytes, (uint32_t)value);
    put_u32(bytes + 4U, (uint32_t)(value >> 32U));
}

static uint64_t get_u64(const uint8_t *bytes)
{
    return (uint64_t)bytes[0] |
        (uint64_t)bytes[1] << 8U |
        (uint64_t)bytes[2] << 16U |
        (uint64_t)bytes[3] << 24U |
        (uint64_t)bytes[4] << 32U |
        (uint64_t)bytes[5] << 40U |
        (uint64_t)bytes[6] << 48U |
        (uint64_t)bytes[7] << 56U;
}

static void put_text(uint8_t *field, size_t width, const char *text)
{
    size_t length = 0U;
    clear_bytes(field, width);
    while (text[length] != '\0') {
        field[length] = (uint8_t)text[length];
        ++length;
    }
}

static void put_magic(uint8_t *field, const char *magic)
{
    for (size_t index = 0U; index < 8U; ++index) {
        field[index] = (uint8_t)magic[index];
    }
}

static bool same_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t different = 0U;
    for (size_t index = 0U; index < count; ++index) {
        different |= left[index] ^ right[index];
    }
    return different == 0U;
}

static bool all_zero(const uint8_t *bytes, size_t count)
{
    uint8_t combined = 0U;

    for (size_t index = 0U; index < count; ++index) {
        combined |= bytes[index];
    }
    return combined == 0U;
}

static bool state_text_is(
    const struct package_state_text *text,
    const char *expected
)
{
    size_t length = 0U;

    while (expected[length] != '\0') {
        ++length;
    }
    return text->length == length &&
        same_bytes(text->bytes, (const uint8_t *)expected, length);
}

static struct package_state_text state_text(const char *text)
{
    struct package_state_text result = {
        (const uint8_t *)text, 0U
    };

    while (text[result.length] != '\0') {
        ++result.length;
    }
    return result;
}

static void digest_text(const char *text, uint8_t output[PACKAGE_STATE_SHA256_BYTES])
{
    size_t length = 0U;
    while (text[length] != '\0') {
        ++length;
    }
    if (package_state_sha256((const uint8_t *)text, length, output) !=
        PACKAGE_STATE_STATUS_OK) {
        output[0] = 1U;
    }
}

static void finalize_database(uint8_t *database, size_t byte_count)
{
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    if (package_state_sha256(database + PACKAGE_STATE_DATABASE_HEADER_BYTES,
        byte_count - PACKAGE_STATE_DATABASE_HEADER_BYTES, digest) ==
        PACKAGE_STATE_STATUS_OK) {
        copy_bytes(database + 104U, digest, sizeof(digest));
    }
}

static void database_header(
    uint8_t *database,
    size_t byte_count,
    uint64_t generation,
    uint32_t package_count,
    uint32_t edge_count,
    uint32_t file_count
)
{
    put_magic(database, "PHIPDB01");
    put_u16(database + 8U, 1U);
    put_u16(database + 10U, PACKAGE_STATE_DATABASE_HEADER_BYTES);
    put_u64(database + 16U, byte_count);
    put_u64(database + 24U, generation);
    put_u32(database + 32U, 1U);
    put_text(database + 40U, 16U, "x86_64");
    put_u64(database + 56U, PACKAGE_STATE_DATABASE_HEADER_BYTES);
    put_u32(database + 64U, package_count);
    put_u32(database + 68U, PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES);
    put_u64(database + 72U, PACKAGE_STATE_DATABASE_HEADER_BYTES +
        (uint64_t)package_count * PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES);
    put_u32(database + 80U, edge_count);
    put_u32(database + 84U, PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES);
    put_u64(database + 88U, PACKAGE_STATE_DATABASE_HEADER_BYTES +
        (uint64_t)package_count * PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES +
        (uint64_t)edge_count * PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES);
    put_u32(database + 96U, file_count);
    put_u32(database + 100U, PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES);
}

static void build_old_database(uint8_t database[OLD_DATABASE_BYTES])
{
    clear_bytes(database, OLD_DATABASE_BYTES);
    database_header(database, OLD_DATABASE_BYTES, 1U, 0U, 0U, 0U);
    finalize_database(database, OLD_DATABASE_BYTES);
}

static void build_new_database(uint8_t database[NEW_DATABASE_BYTES])
{
    uint8_t *application;
    uint8_t *library;
    uint8_t *edge;
    uint8_t *application_file;
    uint8_t *library_file;

    clear_bytes(database, NEW_DATABASE_BYTES);
    database_header(database, NEW_DATABASE_BYTES, 2U,
        NEW_PACKAGE_COUNT, NEW_EDGE_COUNT, NEW_FILE_COUNT);
    application = database + PACKAGE_STATE_DATABASE_HEADER_BYTES;
    library = application + PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES;
    edge = database + NEW_EDGE_OFFSET;
    application_file = database + NEW_FILE_OFFSET;
    library_file = application_file + PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES;

    put_text(application, 64U, "org.phipia.app");
    put_text(application + 64U, 64U, "1.0.0");
    digest_text("application package", application + 128U);
    digest_text("publisher public key", application + 160U);
    put_u32(application + 192U, 1U);
    put_u32(application + 196U, 0U);
    put_u32(application + 200U, 1U);
    put_u32(application + 204U, 1U);

    put_text(library, 64U, "org.phipia.lib");
    put_text(library + 64U, 64U, "1.0.0");
    digest_text("library package", library + 128U);
    digest_text("publisher public key", library + 160U);
    put_u32(library + 192U, 0U);
    put_u32(library + 196U, 1U);
    put_u32(library + 200U, 0U);
    put_u32(library + 204U, 1U);

    put_text(edge, 64U, "org.phipia.lib");
    put_text(edge + 64U, 56U, "^1.0.0");
    put_text(edge + 120U, 64U, "org.phipia.lib");

    put_text(application_file, 128U, "bin/app");
    put_u32(application_file + 128U, 0U);
    put_u16(application_file + 132U, 1U);
    put_u32(application_file + 136U, 0555U);
    put_u64(application_file + 144U, 3U);
    digest_text("app", application_file + 152U);

    put_text(library_file, 128U, "lib/libx.so.1");
    put_u32(library_file + 128U, 1U);
    put_u16(library_file + 132U, 2U);
    put_u32(library_file + 136U, 0444U);
    put_u64(library_file + 144U, 3U);
    digest_text("lib", library_file + 152U);
    put_text(library_file + 184U, 64U, "libx.so.1");
    finalize_database(database, NEW_DATABASE_BYTES);
}

static void build_authority(
    uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t *database,
    size_t database_bytes
)
{
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    clear_bytes(authority, PACKAGE_STATE_AUTHORITY_BYTES);
    put_magic(authority, "PHIPGN01");
    put_u16(authority + 8U, 1U);
    put_u16(authority + 10U, PACKAGE_STATE_AUTHORITY_BYTES);
    put_u64(authority + 16U, get_u64(database + 24U));
    put_u64(authority + 24U, database_bytes);
    (void)package_state_sha256(database, database_bytes, authority + 32U);
    (void)package_state_sha256(authority, 64U, digest);
    copy_bytes(authority + 64U, digest, sizeof(digest));
}

static void rehash_authority(uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES])
{
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    (void)package_state_sha256(authority, 64U, digest);
    copy_bytes(authority + 64U, digest, sizeof(digest));
}

static void rehash_journal(uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES])
{
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    clear_bytes(journal + 128U, PACKAGE_STATE_SHA256_BYTES);
    (void)package_state_sha256(journal, PACKAGE_STATE_JOURNAL_BYTES, digest);
    copy_bytes(journal + 128U, digest, sizeof(digest));
}

static void build_journal(
    uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES],
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES]
)
{
    clear_bytes(journal, PACKAGE_STATE_JOURNAL_BYTES);
    put_magic(journal, "PHIPTX01");
    put_u16(journal + 8U, 1U);
    put_u16(journal + 10U, PACKAGE_STATE_JOURNAL_BYTES);
    put_u16(journal + 16U, PACKAGE_STATE_OPERATION_INSTALL);
    put_u16(journal + 18U, 1U);
    put_u64(journal + 24U, 1U);
    put_u64(journal + 32U, 2U);
    put_u64(journal + 40U, 4096U);
    put_u64(journal + 48U, OLD_DATABASE_BYTES);
    put_u64(journal + 56U, NEW_DATABASE_BYTES);
    (void)package_state_sha256(
        old_database, OLD_DATABASE_BYTES, journal + 64U);
    (void)package_state_sha256(
        new_database, NEW_DATABASE_BYTES, journal + 96U);
    put_text(journal + 160U, 64U, "org.phipia.app");
    rehash_journal(journal);
}

static int test_sha256(void)
{
    static const uint8_t expected[PACKAGE_STATE_SHA256_BYTES] = {
        0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
        0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
        0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
        0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU
    };
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    struct package_state_sha256_context context;

    CHECK(package_state_sha256((const uint8_t *)"abc", 3U, digest) ==
        PACKAGE_STATE_STATUS_OK, 1);
    CHECK(same_bytes(digest, expected, sizeof(expected)), 2);
    CHECK(package_state_sha256_initialize(&context) ==
        PACKAGE_STATE_STATUS_OK &&
        package_state_sha256_update(&context, (const uint8_t *)"a", 1U) ==
            PACKAGE_STATE_STATUS_OK &&
        package_state_sha256_update(&context, (const uint8_t *)"bc", 2U) ==
            PACKAGE_STATE_STATUS_OK &&
        package_state_sha256_finish(&context, digest) ==
            PACKAGE_STATE_STATUS_OK &&
        same_bytes(digest, expected, sizeof(expected)), 3);
    CHECK(package_state_sha256_update(&context, NULL, 0U) ==
        PACKAGE_STATE_STATUS_SEQUENCE, 4);
    CHECK(package_state_sha256_finish(&context, digest) ==
        PACKAGE_STATE_STATUS_SEQUENCE, 5);
    return 0;
}

static int test_parsers_and_mutations(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    /* Independently emitted by phipia-transaction.py for this exact state. */
    static const uint8_t canonical_database_sha256[PACKAGE_STATE_SHA256_BYTES] = {
        0xa8U, 0x85U, 0xe2U, 0xfeU, 0x09U, 0xbfU, 0x1bU, 0xe9U,
        0xe7U, 0x82U, 0x31U, 0x67U, 0x27U, 0x6dU, 0xb1U, 0xfaU,
        0xdbU, 0xf7U, 0x3dU, 0x0cU, 0xbdU, 0xa0U, 0x05U, 0xbbU,
        0x82U, 0xcfU, 0x6eU, 0xaeU, 0x0eU, 0xbaU, 0x0cU, 0xc4U
    };
    struct package_state_database_view database_view;
    struct package_state_package_view package_view;
    struct package_state_dependency_view dependency_view;
    struct package_state_file_view file_view;
    struct package_state_authority_view authority_view;
    struct package_state_journal_view journal_view;
    uint8_t changed[NEW_DATABASE_BYTES];
    uint8_t authority_changed[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t journal_changed[PACKAGE_STATE_JOURNAL_BYTES];
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];

    CHECK(package_state_database_parse(old_database, OLD_DATABASE_BYTES,
        &database_view) == PACKAGE_STATE_STATUS_OK &&
        database_view.generation == 1U && database_view.package_count == 0U, 10);
    CHECK(package_state_database_parse(new_database, NEW_DATABASE_BYTES,
        &database_view) == PACKAGE_STATE_STATUS_OK &&
        database_view.generation == 2U && database_view.package_count == 2U &&
        database_view.edge_count == 1U && database_view.file_count == 2U, 11);
    CHECK(package_state_database_package(&database_view, 0U, &package_view) ==
            PACKAGE_STATE_STATUS_OK && package_view.database == &database_view &&
        package_view.package_index == 0U && package_view.explicit_root &&
        package_view.dependency_start == 0U && package_view.dependency_count == 1U &&
        package_view.file_count == 1U &&
        state_text_is(&package_view.identifier, "org.phipia.app") &&
        state_text_is(&package_view.version, "1.0.0") &&
        package_state_database_package(&database_view, 2U, &package_view) ==
            PACKAGE_STATE_STATUS_TABLE && package_view.database == NULL, 46);
    CHECK(package_state_database_dependency(&database_view, 0U,
            &dependency_view) == PACKAGE_STATE_STATUS_OK &&
        dependency_view.database == &database_view &&
        dependency_view.dependency_index == 0U &&
        state_text_is(&dependency_view.requested, "org.phipia.lib") &&
        state_text_is(&dependency_view.constraint, "^1.0.0") &&
        state_text_is(&dependency_view.provider, "org.phipia.lib") &&
        package_state_database_dependency(&database_view, 1U,
            &dependency_view) == PACKAGE_STATE_STATUS_TABLE &&
        dependency_view.database == NULL, 47);
    CHECK(package_state_database_file(&database_view, 0U, &file_view) ==
            PACKAGE_STATE_STATUS_OK && file_view.database == &database_view &&
        file_view.file_index == 0U && file_view.owner_index == 0U &&
        file_view.kind == 1U && file_view.mode == 0555U && file_view.length == 3U &&
        state_text_is(&file_view.path, "bin/app") && file_view.soname.length == 0U &&
        package_state_database_file(&database_view, 1U, &file_view) ==
            PACKAGE_STATE_STATUS_OK && file_view.owner_index == 1U &&
        file_view.kind == 2U && file_view.mode == 0444U &&
        state_text_is(&file_view.path, "lib/libx.so.1") &&
        state_text_is(&file_view.soname, "libx.so.1") &&
        package_state_database_file(&database_view, 2U, &file_view) ==
            PACKAGE_STATE_STATUS_TABLE && file_view.database == NULL, 48);
    CHECK(package_state_sha256(new_database, NEW_DATABASE_BYTES, digest) ==
        PACKAGE_STATE_STATUS_OK && same_bytes(digest,
            canonical_database_sha256, sizeof(digest)), 25);
    CHECK(package_state_authority_parse(authority, PACKAGE_STATE_AUTHORITY_BYTES,
        &authority_view) == PACKAGE_STATE_STATUS_OK &&
        authority_view.generation == 2U, 12);
    CHECK(package_state_journal_parse(journal, PACKAGE_STATE_JOURNAL_BYTES,
        &journal_view) == PACKAGE_STATE_STATUS_OK &&
        journal_view.operation == PACKAGE_STATE_OPERATION_INSTALL &&
        journal_view.base_generation == 1U &&
        journal_view.target_generation == 2U, 13);

    copy_bytes(changed, new_database, sizeof(changed));
    changed[136U] = 1U;
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_RESERVED, 14);

    copy_bytes(changed, new_database, sizeof(changed));
    changed[PACKAGE_STATE_DATABASE_HEADER_BYTES] = (uint8_t)'O';
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_PACKAGE, 15);

    copy_bytes(changed, new_database, sizeof(changed));
    changed[NEW_DATABASE_BYTES - 1U] = 1U;
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_FILE, 16);

    copy_bytes(changed, new_database, sizeof(changed));
    put_text(changed + NEW_EDGE_OFFSET + 120U, 64U, "org.phipia.missing");
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_DEPENDENCY, 17);

    copy_bytes(changed, new_database, sizeof(changed));
    put_text(changed + NEW_EDGE_OFFSET + 120U, 64U, "org.phipia.app");
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_DEPENDENCY, 26);

    copy_bytes(changed, new_database, sizeof(changed));
    put_u32(changed + PACKAGE_STATE_DATABASE_HEADER_BYTES + 192U, 0U);
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_DEPENDENCY, 27);

    copy_bytes(changed, new_database, sizeof(changed));
    put_text(changed + NEW_EDGE_OFFSET + 64U, 56U, "1.0.0");
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_DEPENDENCY, 28);

    copy_bytes(changed, new_database, sizeof(changed));
    put_text(changed + NEW_FILE_OFFSET +
        PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES, 128U, "bin/app");
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_FILE, 18);

    copy_bytes(changed, new_database, sizeof(changed));
    clear_bytes(changed + NEW_FILE_OFFSET +
        PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES + 184U, 64U);
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_FILE, 29);

    copy_bytes(changed, new_database, sizeof(changed));
    changed[NEW_DATABASE_BYTES - 9U] ^= 1U;
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_DIGEST, 19);

    copy_bytes(changed, new_database, sizeof(changed));
    put_u32(changed + 64U, PACKAGE_STATE_DATABASE_MAX_PACKAGES + 1U);
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_TABLE, 20);

    copy_bytes(authority_changed, authority, sizeof(authority_changed));
    authority_changed[64U] ^= 1U;
    CHECK(package_state_authority_parse(authority_changed,
        sizeof(authority_changed), &authority_view) ==
        PACKAGE_STATE_STATUS_DIGEST, 21);

    copy_bytes(journal_changed, journal, sizeof(journal_changed));
    journal_changed[224U] = 1U;
    CHECK(package_state_journal_parse(journal_changed,
        sizeof(journal_changed), &journal_view) ==
        PACKAGE_STATE_STATUS_RESERVED, 22);

    copy_bytes(journal_changed, journal, sizeof(journal_changed));
    journal_changed[128U] ^= 1U;
    CHECK(package_state_journal_parse(journal_changed,
        sizeof(journal_changed), &journal_view) ==
        PACKAGE_STATE_STATUS_DIGEST, 23);

    copy_bytes(journal_changed, journal, sizeof(journal_changed));
    put_u64(journal_changed + 24U, UINT64_MAX);
    put_u64(journal_changed + 32U, 0U);
    rehash_journal(journal_changed);
    CHECK(package_state_journal_parse(journal_changed,
        sizeof(journal_changed), &journal_view) ==
        PACKAGE_STATE_STATUS_JOURNAL, 24);
    return 0;
}

static int test_encoders(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    static const uint8_t target[] = "org.phipia.app";
    struct package_state_database_view old_view;
    struct package_state_database_view new_view;
    struct package_state_database_view invalid_view;
    struct package_state_journal_spec spec;
    uint8_t encoded_authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t encoded_journal[PACKAGE_STATE_JOURNAL_BYTES];

    CHECK(package_state_database_parse(old_database, OLD_DATABASE_BYTES,
            &old_view) == PACKAGE_STATE_STATUS_OK &&
        package_state_database_parse(new_database, NEW_DATABASE_BYTES,
            &new_view) == PACKAGE_STATE_STATUS_OK, 40);
    CHECK(package_state_authority_encode(&new_view, encoded_authority) ==
            PACKAGE_STATE_STATUS_OK &&
        same_bytes(encoded_authority, authority, sizeof(encoded_authority)), 41);
    spec = (struct package_state_journal_spec){
        PACKAGE_STATE_OPERATION_INSTALL,
        &old_view,
        &new_view,
        4096U,
        target,
        sizeof(target) - 1U
    };
    CHECK(package_state_journal_encode(&spec, encoded_journal) ==
            PACKAGE_STATE_STATUS_OK &&
        same_bytes(encoded_journal, journal, sizeof(encoded_journal)), 42);

    invalid_view = new_view;
    invalid_view.byte_count = 1U;
    encoded_authority[0] = 1U;
    CHECK(package_state_authority_encode(&invalid_view, encoded_authority) ==
            PACKAGE_STATE_STATUS_LENGTH &&
        all_zero(encoded_authority, sizeof(encoded_authority)), 43);
    spec.target = &old_view;
    encoded_journal[0] = 1U;
    CHECK(package_state_journal_encode(&spec, encoded_journal) ==
            PACKAGE_STATE_STATUS_JOURNAL &&
        all_zero(encoded_journal, sizeof(encoded_journal)), 44);
    spec.target = &new_view;
    spec.target_identifier = (const uint8_t *)"Bad/Identifier";
    spec.target_identifier_bytes = sizeof("Bad/Identifier") - 1U;
    encoded_journal[0] = 1U;
    CHECK(package_state_journal_encode(&spec, encoded_journal) ==
            PACKAGE_STATE_STATUS_TEXT &&
        all_zero(encoded_journal, sizeof(encoded_journal)), 45);
    return 0;
}

static int test_generation_encoder(
    const uint8_t expected[NEW_DATABASE_BYTES]
)
{
    struct package_generation_package packages[NEW_PACKAGE_COUNT];
    struct package_generation_dependency dependencies[NEW_EDGE_COUNT];
    struct package_generation_file files[NEW_FILE_COUNT];
    struct package_generation_spec spec;
    struct package_state_database_view view;
    uint8_t application_package[PACKAGE_STATE_SHA256_BYTES];
    uint8_t library_package[PACKAGE_STATE_SHA256_BYTES];
    uint8_t publisher[PACKAGE_STATE_SHA256_BYTES];
    uint8_t application_file[PACKAGE_STATE_SHA256_BYTES];
    uint8_t library_file[PACKAGE_STATE_SHA256_BYTES];
    uint8_t encoded[NEW_DATABASE_BYTES];
    size_t encoded_bytes = 0U;

    digest_text("application package", application_package);
    digest_text("library package", library_package);
    digest_text("publisher public key", publisher);
    digest_text("app", application_file);
    digest_text("lib", library_file);
    packages[0] = (struct package_generation_package){
        state_text("org.phipia.app"), state_text("1.0.0"),
        application_package, publisher, true, 0U, 1U, 1U
    };
    packages[1] = (struct package_generation_package){
        state_text("org.phipia.lib"), state_text("1.0.0"),
        library_package, publisher, false, 1U, 0U, 1U
    };
    dependencies[0] = (struct package_generation_dependency){
        state_text("org.phipia.lib"), state_text("^1.0.0"),
        state_text("org.phipia.lib")
    };
    files[0] = (struct package_generation_file){
        state_text("bin/app"), 0U, 1U, 0555U, 3U, application_file,
        state_text("")
    };
    files[1] = (struct package_generation_file){
        state_text("lib/libx.so.1"), 1U, 2U, 0444U, 3U, library_file,
        state_text("libx.so.1")
    };
    spec = (struct package_generation_spec){
        2U, 1U, packages, NEW_PACKAGE_COUNT, dependencies, NEW_EDGE_COUNT,
        files, NEW_FILE_COUNT
    };
    CHECK(package_generation_size(&spec, &encoded_bytes) ==
            PACKAGE_STATE_STATUS_OK && encoded_bytes == NEW_DATABASE_BYTES, 50);
    CHECK(package_generation_encode(&spec, encoded, sizeof(encoded), &view) ==
            PACKAGE_STATE_STATUS_OK && view.generation == 2U &&
        same_bytes(encoded, expected, sizeof(encoded)), 51);
    CHECK(package_generation_verify(&spec, encoded, sizeof(encoded), &view) ==
            PACKAGE_STATE_STATUS_OK && view.generation == 2U, 56);

    packages[0].explicit_root = false;
    CHECK(package_generation_verify(&spec, encoded, sizeof(encoded), &view) ==
            PACKAGE_STATE_STATUS_MISMATCH && view.bytes == NULL, 57);
    packages[0].explicit_root = true;
    files[0].length = 4U;
    CHECK(package_generation_verify(&spec, encoded, sizeof(encoded), &view) ==
            PACKAGE_STATE_STATUS_MISMATCH && view.bytes == NULL, 58);
    files[0].length = 3U;

    packages[0].identifier = state_text("org.phipia.lib");
    encoded[0] = 1U;
    CHECK(package_generation_encode(&spec, encoded, sizeof(encoded), &view) ==
            PACKAGE_STATE_STATUS_PACKAGE && all_zero(encoded, sizeof(encoded)) &&
        view.bytes == NULL, 52);
    packages[0].identifier = state_text("org.phipia.app");
    files[1].path = files[0].path;
    encoded[0] = 1U;
    CHECK(package_generation_encode(&spec, encoded, sizeof(encoded), &view) ==
            PACKAGE_STATE_STATUS_FILE && all_zero(encoded, sizeof(encoded)) &&
        view.bytes == NULL, 53);
    files[1].path = state_text("bin/app/child");
    encoded[0] = 1U;
    CHECK(package_generation_encode(&spec, encoded, sizeof(encoded), &view) ==
            PACKAGE_STATE_STATUS_FILE && all_zero(encoded, sizeof(encoded)) &&
        view.bytes == NULL, 60);
    files[1].path = state_text("lib/libx.so.1");
    packages[0].package_sha256 = NULL;
    encoded[0] = 1U;
    CHECK(package_generation_encode(&spec, encoded, sizeof(encoded), &view) ==
            PACKAGE_STATE_STATUS_NULL_ARGUMENT &&
        all_zero(encoded, sizeof(encoded)) && view.bytes == NULL, 54);
    packages[0].package_sha256 = application_package;
    encoded[0] = 1U;
    CHECK(package_generation_encode(&spec, encoded, sizeof(encoded) - 1U,
            &view) == PACKAGE_STATE_STATUS_LENGTH &&
        all_zero(encoded, sizeof(encoded) - 1U) && view.bytes == NULL, 55);
    CHECK(package_generation_verify(&spec, encoded, sizeof(encoded) - 1U,
            &view) == PACKAGE_STATE_STATUS_MISMATCH && view.bytes == NULL, 59);
    return 0;
}

static int test_recovery(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_state_generation old_generation = {
        old_database, OLD_DATABASE_BYTES, true
    };
    struct package_state_generation new_generation = {
        new_database, NEW_DATABASE_BYTES, true
    };
    struct package_state_recovery_result result;
    uint8_t changed_authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t repair_journal[PACKAGE_STATE_JOURNAL_BYTES];

    CHECK(package_state_recovery_decide(old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_OLD && result.generation == 1U, 30);

    CHECK(package_state_recovery_decide(new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_NEW && result.generation == 2U, 31);

    copy_bytes(repair_journal, journal, sizeof(repair_journal));
    put_u16(repair_journal + 16U, PACKAGE_STATE_OPERATION_REPAIR);
    clear_bytes(repair_journal + 160U, 64U);
    rehash_journal(repair_journal);
    old_generation.owned_files_complete = false;
    CHECK(package_state_recovery_decide(old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, repair_journal,
        PACKAGE_STATE_JOURNAL_BYTES, &old_generation, &new_generation,
        &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_NEW && result.generation == 2U,
        61);
    old_generation.owned_files_complete = true;
    CHECK(package_state_recovery_decide(old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, repair_journal,
        PACKAGE_STATE_JOURNAL_BYTES, &old_generation, &new_generation,
        &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_OLD, 62);

    new_generation.owned_files_complete = false;
    CHECK(package_state_recovery_decide(new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_OLD, 32);

    old_generation.owned_files_complete = false;
    CHECK(package_state_recovery_decide(new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) ==
        PACKAGE_STATE_STATUS_INCOMPLETE, 33);

    old_generation.owned_files_complete = true;
    new_generation.owned_files_complete = true;
    CHECK(package_state_recovery_decide(new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, NULL, 0U,
        &old_generation, &new_generation, &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_NEW, 34);

    copy_bytes(changed_authority, new_authority, sizeof(changed_authority));
    changed_authority[64U] ^= 1U;
    CHECK(package_state_recovery_decide(changed_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_OLD, 35);

    copy_bytes(changed_authority, new_authority, sizeof(changed_authority));
    put_u64(changed_authority + 16U, 99U);
    rehash_authority(changed_authority);
    CHECK(package_state_recovery_decide(changed_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) ==
        PACKAGE_STATE_STATUS_MISMATCH, 36);

    copy_bytes(changed_authority, old_authority, sizeof(changed_authority));
    changed_authority[32U] ^= 1U;
    rehash_authority(changed_authority);
    CHECK(package_state_recovery_decide(changed_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) ==
        PACKAGE_STATE_STATUS_MISMATCH, 37);
    return 0;
}

int main(void)
{
    uint8_t old_database[OLD_DATABASE_BYTES];
    uint8_t new_database[NEW_DATABASE_BYTES];
    uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES];
    int result;

    result = test_sha256();
    if (result != 0) {
        return result;
    }
    build_old_database(old_database);
    build_new_database(new_database);
    build_authority(old_authority, old_database, OLD_DATABASE_BYTES);
    build_authority(new_authority, new_database, NEW_DATABASE_BYTES);
    build_journal(journal, old_database, new_database);
    result = test_parsers_and_mutations(
        old_database, new_database, new_authority, journal);
    if (result != 0) {
        return result;
    }
    result = test_encoders(old_database, new_database, new_authority, journal);
    if (result != 0) {
        return result;
    }
    result = test_generation_encoder(new_database);
    if (result != 0) {
        return result;
    }
    return test_recovery(old_database, new_database,
        old_authority, new_authority, journal);
}
