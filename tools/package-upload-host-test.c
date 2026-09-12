/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <phipia/fat32_fs.h>
#include <phipia/package_state.h>
#include <phipia/package_upload.h>

#define MOCK_FILE_BYTES (256U * 1024U)
#define NO_WRITE_FAILURE SIZE_MAX
#define CHECK(condition, code) do { if (!(condition)) return (code); } while (0)

struct mock_file {
    uint8_t bytes[MOCK_FILE_BYTES];
    size_t size;
    size_t offset;
    bool present;
    bool open;
};

static struct mock_file files[PACKAGE_UPLOAD_SLOT_LIMIT];
static bool directory_present;
static bool fail_next_sync;
static bool fail_next_open;
static bool fail_next_unlink;
static size_t write_failure_at = NO_WRITE_FAILURE;
static uint32_t sync_count;
static uint32_t unlink_count;
static uint32_t truncate_count;

static int path_index(const char *path)
{
    static const char prefix[] = PACKAGE_UPLOAD_DIRECTORY "/u";

    if (path == NULL) {
        return -1;
    }
    for (size_t index = 0U; index < sizeof(prefix) - 1U; ++index) {
        if (path[index] != prefix[index]) {
            return -1;
        }
    }
    size_t at = sizeof(prefix) - 1U;
    if (path[at] < '0' || path[at] >=
            (char)('0' + PACKAGE_UPLOAD_SLOT_LIMIT) ||
        path[at + 1U] != '.' || path[at + 2U] != 's' ||
        path[at + 3U] != 'p' || path[at + 4U] != 'k' ||
        path[at + 5U] != '\0') {
        return -1;
    }
    return path[at] - '0';
}

enum phipfs_status phipfs_mkdir(enum phipfs_volume volume, const char *path)
{
    if (volume != PHIPFS_VOLUME_DATA || path == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    if (path[0] != 'p' || path[1] != 'k' || path[2] != 'g') {
        return PHIPFS_STATUS_PATH;
    }
    if (directory_present) {
        return PHIPFS_STATUS_EXISTS;
    }
    directory_present = true;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_unlink(enum phipfs_volume volume, const char *path)
{
    int index = path_index(path);

    if (volume != PHIPFS_VOLUME_DATA || index < 0) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    if (!files[index].present) {
        return PHIPFS_STATUS_NOT_FOUND;
    }
    if (fail_next_unlink) {
        fail_next_unlink = false;
        return PHIPFS_STATUS_IO;
    }
    if (files[index].open) {
        return PHIPFS_STATUS_BUSY;
    }
    files[index].present = false;
    files[index].size = 0U;
    files[index].offset = 0U;
    ++unlink_count;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_stat_path(enum phipfs_volume volume,
    const char *path, struct phipfs_stat *stat)
{
    int index = path_index(path);

    if (volume != PHIPFS_VOLUME_DATA || index < 0 || stat == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    if (!files[index].present) {
        return PHIPFS_STATUS_NOT_FOUND;
    }
    *stat = (struct phipfs_stat){
        .size = files[index].size,
        .directory = false
    };
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_truncate(enum phipfs_volume volume,
    const char *path, uint64_t size)
{
    int index = path_index(path);

    if (volume != PHIPFS_VOLUME_DATA || index < 0 ||
            size > MOCK_FILE_BYTES) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    if (!files[index].present) {
        return PHIPFS_STATUS_NOT_FOUND;
    }
    if (files[index].open) {
        return PHIPFS_STATUS_BUSY;
    }
    files[index].size = (size_t)size;
    if (files[index].offset > files[index].size) {
        files[index].offset = files[index].size;
    }
    ++truncate_count;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_sync(enum phipfs_volume volume)
{
    if (volume != PHIPFS_VOLUME_DATA) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    ++sync_count;
    if (fail_next_sync) {
        fail_next_sync = false;
        return PHIPFS_STATUS_IO;
    }
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_create(enum phipfs_volume volume, const char *path)
{
    int index = path_index(path);

    if (volume != PHIPFS_VOLUME_DATA || index < 0 || !directory_present) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    if (files[index].present) {
        return PHIPFS_STATUS_EXISTS;
    }
    files[index] = (struct mock_file){0};
    files[index].present = true;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_open(
    enum phipfs_volume volume,
    const char *path,
    enum phipfs_access access,
    phipfs_handle *handle
)
{
    int index = path_index(path);

    if (volume != PHIPFS_VOLUME_DATA || index < 0 || handle == NULL ||
        (access != PHIPFS_ACCESS_READ && access != PHIPFS_ACCESS_WRITE)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *handle = 0U;
    if (fail_next_open) {
        fail_next_open = false;
        return PHIPFS_STATUS_IO;
    }
    if (!files[index].present) {
        return PHIPFS_STATUS_NOT_FOUND;
    }
    if (files[index].open) {
        return PHIPFS_STATUS_BUSY;
    }
    files[index].open = true;
    files[index].offset = access == PHIPFS_ACCESS_WRITE ? files[index].size : 0U;
    *handle = (phipfs_handle)(index + 1);
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_close(phipfs_handle handle)
{
    if (handle == 0U || handle > PACKAGE_UPLOAD_SLOT_LIMIT ||
        !files[handle - 1U].open) {
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    files[handle - 1U].open = false;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_write(
    phipfs_handle handle,
    const uint8_t *source,
    size_t source_bytes,
    size_t *written_bytes
)
{
    if (written_bytes == NULL || handle == 0U ||
        handle > PACKAGE_UPLOAD_SLOT_LIMIT || !files[handle - 1U].open ||
        (source == NULL && source_bytes != 0U)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    struct mock_file *file = &files[handle - 1U];
    size_t allowed = source_bytes;

    *written_bytes = 0U;
    if (file->offset >= write_failure_at) {
        return PHIPFS_STATUS_IO;
    }
    if (allowed > write_failure_at - file->offset) {
        allowed = write_failure_at - file->offset;
    }
    if (allowed > MOCK_FILE_BYTES - file->offset) {
        return PHIPFS_STATUS_FULL;
    }
    for (size_t index = 0U; index < allowed; ++index) {
        file->bytes[file->offset + index] = source[index];
    }
    file->offset += allowed;
    if (file->offset > file->size) {
        file->size = file->offset;
    }
    *written_bytes = allowed;
    return allowed == source_bytes ? PHIPFS_STATUS_OK : PHIPFS_STATUS_IO;
}

enum phipfs_status phipfs_pread(
    phipfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    uint64_t offset,
    size_t *read_bytes
)
{
    if (read_bytes == NULL || handle == 0U ||
        handle > PACKAGE_UPLOAD_SLOT_LIMIT || !files[handle - 1U].open ||
        (destination == NULL && capacity != 0U)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    struct mock_file *file = &files[handle - 1U];
    size_t available = offset < file->size ? file->size - (size_t)offset : 0U;

    if (capacity > available) {
        capacity = available;
    }
    for (size_t index = 0U; index < capacity; ++index) {
        destination[index] = file->bytes[(size_t)offset + index];
    }
    *read_bytes = capacity;
    return PHIPFS_STATUS_OK;
}

static int initialize_test(void)
{
    struct package_upload_report report;

    files[0].present = true;
    files[1].present = true;
    CHECK(package_upload_initialize(&report) == PACKAGE_UPLOAD_STATUS_OK, 1);
    CHECK(directory_present && !files[0].present && !files[1].present &&
        unlink_count == 2U && sync_count == 1U, 2);
    CHECK(package_upload_initialize(&report) == PACKAGE_UPLOAD_STATUS_OK &&
        sync_count == 1U && package_upload_resources_released(), 3);
    return 0;
}

static int exact_upload_test(void)
{
    static const uint8_t abc[] = {'a', 'b', 'c'};
    static const uint8_t abc_sha256[PACKAGE_STATE_SHA256_BYTES] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    struct package_upload_report report;
    package_upload_token token;
    size_t completed;
    uint8_t copy[4] = {0};

    CHECK(package_upload_open(41U, &report) == PACKAGE_UPLOAD_STATUS_OK &&
        report.token != 0U && !package_upload_resources_released(), 10);
    token = report.token;
    CHECK(package_upload_write(42U, token, abc, sizeof(abc), &completed,
        &report) == PACKAGE_UPLOAD_STATUS_STALE && completed == 0U, 11);
    CHECK(package_upload_write(41U, token, abc, sizeof(abc), &completed,
        &report) == PACKAGE_UPLOAD_STATUS_OK && completed == sizeof(abc) &&
        report.byte_count == sizeof(abc), 12);
    CHECK(package_upload_read(41U, token, 0U, copy, sizeof(copy), &completed,
        &report) == PACKAGE_UPLOAD_STATUS_STATE, 13);
    CHECK(package_upload_seal(41U, token, sizeof(abc), abc_sha256, &report) ==
        PACKAGE_UPLOAD_STATUS_OK && report.sealed && report.durable, 14);
    CHECK(package_upload_inspect(41U, token, &report) ==
            PACKAGE_UPLOAD_STATUS_OK && report.byte_count == sizeof(abc) &&
        memcmp(report.sha256, abc_sha256, sizeof(abc_sha256)) == 0, 141);
    CHECK(package_upload_write(41U, token, abc, sizeof(abc), &completed,
        &report) == PACKAGE_UPLOAD_STATUS_STATE, 15);
    CHECK(package_upload_read(41U, token, 0U, copy, sizeof(copy), &completed,
        &report) == PACKAGE_UPLOAD_STATUS_OK && completed == sizeof(abc) &&
        copy[0] == 'a' && copy[1] == 'b' && copy[2] == 'c', 16);
    CHECK(package_upload_close(41U, token, &report) == PACKAGE_UPLOAD_STATUS_OK,
        17);
    CHECK(package_upload_close(41U, token, &report) ==
        PACKAGE_UPLOAD_STATUS_STALE && package_upload_resources_released(), 18);
    return 0;
}

static int bounded_large_cleanup_test(void)
{
    static uint8_t payload[PACKAGE_UPLOAD_CLEANUP_CHUNK + 4096U];
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    struct package_upload_report report;
    package_upload_token token;
    size_t total = 0U;
    const uint32_t before = truncate_count;

    for (size_t index = 0U; index < sizeof(payload); ++index) {
        payload[index] = (uint8_t)(index * 17U + 3U);
    }
    CHECK(package_state_sha256(payload, sizeof(payload), digest) ==
        PACKAGE_STATE_STATUS_OK, 60);
    CHECK(package_upload_open(80U, &report) == PACKAGE_UPLOAD_STATUS_OK, 61);
    token = report.token;
    while (total < sizeof(payload)) {
        size_t chunk = sizeof(payload) - total;
        size_t written = 0U;

        if (chunk > PACKAGE_UPLOAD_WRITE_MAX) {
            chunk = PACKAGE_UPLOAD_WRITE_MAX;
        }
        CHECK(package_upload_write(80U, token, payload + total, chunk,
            &written, &report) == PACKAGE_UPLOAD_STATUS_OK &&
            written == chunk, 62);
        total += written;
    }
    CHECK(package_upload_seal(80U, token, sizeof(payload), digest, &report) ==
        PACKAGE_UPLOAD_STATUS_OK, 63);
    CHECK(package_upload_close(80U, token, &report) ==
            PACKAGE_UPLOAD_STATUS_OK &&
        truncate_count == before + 2U && package_upload_resources_released(),
        64);
    return 0;
}

static int bounded_slots_test(void)
{
    struct package_upload_report report;
    package_upload_token tokens[PACKAGE_UPLOAD_SLOT_LIMIT];
    size_t written;
    uint8_t byte = 0U;

    for (size_t index = 0U; index < PACKAGE_UPLOAD_SLOT_LIMIT; ++index) {
        CHECK(package_upload_open(50U + index, &report) ==
            PACKAGE_UPLOAD_STATUS_OK, 20);
        tokens[index] = report.token;
    }
    CHECK(package_upload_open(99U, &report) == PACKAGE_UPLOAD_STATUS_NO_SLOT,
        21);
    CHECK(package_upload_write(50U, tokens[0], &byte,
        PACKAGE_UPLOAD_WRITE_MAX + 1U, &written, &report) ==
        PACKAGE_UPLOAD_STATUS_RANGE && written == 0U, 22);
    for (size_t index = 0U; index < PACKAGE_UPLOAD_SLOT_LIMIT; ++index) {
        CHECK(package_upload_close(50U + index, tokens[index], &report) ==
            PACKAGE_UPLOAD_STATUS_OK, 23);
    }
    return 0;
}

static int refusal_cleanup_test(void)
{
    static const uint8_t payload[] = {'x', 'y', 'z'};
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    struct package_upload_report report;
    package_upload_token token;
    size_t written;

    CHECK(package_state_sha256(payload, sizeof(payload), digest) ==
        PACKAGE_STATE_STATUS_OK, 30);
    CHECK(package_upload_open(60U, &report) == PACKAGE_UPLOAD_STATUS_OK, 31);
    token = report.token;
    CHECK(package_upload_write(60U, token, payload, sizeof(payload), &written,
        &report) == PACKAGE_UPLOAD_STATUS_OK, 32);
    CHECK(package_upload_seal(60U, token, sizeof(payload) + 1U, digest,
        &report) == PACKAGE_UPLOAD_STATUS_LENGTH && !report.sealed, 33);
    CHECK(package_upload_read(60U, token, 0U, digest, sizeof(digest), &written,
        &report) == PACKAGE_UPLOAD_STATUS_STATE, 34);
    CHECK(package_upload_close(60U, token, &report) == PACKAGE_UPLOAD_STATUS_OK,
        35);

    digest[0] ^= UINT8_C(0x80);
    CHECK(package_upload_open(61U, &report) == PACKAGE_UPLOAD_STATUS_OK, 36);
    token = report.token;
    CHECK(package_upload_write(61U, token, payload, sizeof(payload), &written,
        &report) == PACKAGE_UPLOAD_STATUS_OK, 37);
    CHECK(package_upload_seal(61U, token, sizeof(payload), digest, &report) ==
        PACKAGE_UPLOAD_STATUS_DIGEST && !report.sealed, 38);
    CHECK(package_upload_close(61U, token, &report) == PACKAGE_UPLOAD_STATUS_OK,
        39);
    return 0;
}

static int failure_recovery_test(void)
{
    static const uint8_t payload[] = {'0', '1', '2', '3'};
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    struct package_upload_report report;
    package_upload_token token;
    size_t written;

    CHECK(package_state_sha256(payload, sizeof(payload), digest) ==
        PACKAGE_STATE_STATUS_OK, 40);
    CHECK(package_upload_open(70U, &report) == PACKAGE_UPLOAD_STATUS_OK, 41);
    token = report.token;
    write_failure_at = 2U;
    CHECK(package_upload_write(70U, token, payload, sizeof(payload), &written,
        &report) == PACKAGE_UPLOAD_STATUS_FILESYSTEM && written == 2U &&
        report.byte_count == 2U, 42);
    write_failure_at = NO_WRITE_FAILURE;
    CHECK(package_upload_write(70U, token, payload, 1U, &written, &report) ==
        PACKAGE_UPLOAD_STATUS_STATE, 43);
    CHECK(package_upload_close(70U, token, &report) == PACKAGE_UPLOAD_STATUS_OK,
        44);

    CHECK(package_upload_open(71U, &report) == PACKAGE_UPLOAD_STATUS_OK, 45);
    token = report.token;
    CHECK(package_upload_write(71U, token, payload, sizeof(payload), &written,
        &report) == PACKAGE_UPLOAD_STATUS_OK, 46);
    fail_next_sync = true;
    CHECK(package_upload_seal(71U, token, sizeof(payload), digest, &report) ==
        PACKAGE_UPLOAD_STATUS_DURABILITY && !report.sealed, 47);
    CHECK(package_upload_close(71U, token, &report) == PACKAGE_UPLOAD_STATUS_OK,
        48);

    CHECK(package_upload_open(72U, &report) == PACKAGE_UPLOAD_STATUS_OK, 49);
    token = report.token;
    CHECK(package_upload_write(72U, token, payload, sizeof(payload), &written,
        &report) == PACKAGE_UPLOAD_STATUS_OK, 50);
    CHECK(package_upload_seal(72U, token, sizeof(payload), digest, &report) ==
        PACKAGE_UPLOAD_STATUS_OK, 51);
    fail_next_sync = true;
    CHECK(package_upload_close(72U, token, &report) ==
        PACKAGE_UPLOAD_STATUS_DURABILITY, 52);
    CHECK(package_upload_close(72U, token, &report) == PACKAGE_UPLOAD_STATUS_OK,
        53);
    CHECK(package_upload_resources_released(), 54);

    fail_next_open = true;
    CHECK(package_upload_open(73U, &report) ==
            PACKAGE_UPLOAD_STATUS_FILESYSTEM &&
        package_upload_resources_released(), 55);

    CHECK(package_upload_open(74U, &report) == PACKAGE_UPLOAD_STATUS_OK, 56);
    token = report.token;
    fail_next_unlink = true;
    CHECK(package_upload_close(74U, token, &report) ==
            PACKAGE_UPLOAD_STATUS_FILESYSTEM &&
        !package_upload_resources_released(), 57);
    CHECK(package_upload_close(74U, token, &report) == PACKAGE_UPLOAD_STATUS_OK,
        58);
    CHECK(package_upload_resources_released(), 59);
    return 0;
}

int main(void)
{
    int result = initialize_test();

    if (result == 0) {
        result = exact_upload_test();
    }
    if (result == 0) {
        result = bounded_slots_test();
    }
    if (result == 0) {
        result = refusal_cleanup_test();
    }
    if (result == 0) {
        result = failure_recovery_test();
    }
    if (result == 0) {
        result = bounded_large_cleanup_test();
    }
    if (result != 0) {
        (void)fprintf(stderr, "package upload host test failed: %d\n", result);
        return result;
    }
    (void)printf("package upload host tests passed: lifecycle/failure matrix, "
        "slots=%u max_bytes=%u syncs=%u unlinks=%u truncates=%u\n",
        PACKAGE_UPLOAD_SLOT_LIMIT, PACKAGE_UPLOAD_MAX_BYTES, sync_count,
        unlink_count, truncate_count);
    return 0;
}
