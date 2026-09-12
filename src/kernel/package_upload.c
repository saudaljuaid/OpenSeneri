/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded, kernel-owned package ingress with exact digest sealing. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/fat32_fs.h>
#include <phipia/package_state.h>
#include <phipia/package_upload.h>

struct upload_slot {
    struct package_state_sha256_context sha256;
    phipfs_handle file;
    uint64_t owner;
    uint64_t byte_count;
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    uint32_t generation;
    bool active;
    bool file_open;
    bool file_present;
    bool sealed;
    bool durable;
    bool poisoned;
};

static struct upload_slot slots[PACKAGE_UPLOAD_SLOT_LIMIT];
static bool initialized;
static bool servicing;

static void zero_bytes(void *destination, size_t count)
{
    uint8_t *bytes = destination;

    for (size_t index = 0U; index < count; ++index) {
        bytes[index] = 0U;
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

static void slot_path(size_t index, char path[PHIPFS_MAX_PATH])
{
    static const char prefix[] = PACKAGE_UPLOAD_DIRECTORY "/u";
    static const char suffix[] = ".spk";
    size_t cursor = 0U;

    zero_bytes(path, PHIPFS_MAX_PATH);
    for (size_t at = 0U; at < sizeof(prefix) - 1U; ++at) {
        path[cursor++] = prefix[at];
    }
    path[cursor++] = (char)('0' + index);
    for (size_t at = 0U; at < sizeof(suffix); ++at) {
        path[cursor++] = suffix[at];
    }
}

static package_upload_token encode_token(size_t index, uint32_t generation)
{
    return (uint64_t)generation << 32U | (uint64_t)(index + 1U);
}

static void report_clear(struct package_upload_report *report)
{
    if (report != NULL) {
        zero_bytes(report, sizeof(*report));
        report->status = PACKAGE_UPLOAD_STATUS_STATE;
        report->filesystem_status = PHIPFS_STATUS_OK;
    }
}

static enum package_upload_status finish(
    struct package_upload_report *report,
    enum package_upload_status status,
    enum phipfs_status filesystem_status,
    const struct upload_slot *slot,
    size_t index
)
{
    if (report != NULL) {
        report->status = status;
        report->filesystem_status = filesystem_status;
        if (slot != NULL) {
            report->token = encode_token(index, slot->generation);
            report->byte_count = slot->byte_count;
            for (size_t at = 0U; at < sizeof(report->sha256); ++at) {
                report->sha256[at] = slot->digest[at];
            }
            report->sealed = slot->sealed;
            report->durable = slot->durable;
        }
    }
    return status;
}

static enum package_upload_status initialization_failure(
    struct package_upload_report *report,
    enum package_upload_status status,
    enum phipfs_status filesystem_status,
    bool changed
)
{
    if (changed) {
        enum phipfs_status sync_status = phipfs_sync(PHIPFS_VOLUME_DATA);

        if (sync_status != PHIPFS_STATUS_OK) {
            status = PACKAGE_UPLOAD_STATUS_DURABILITY;
            filesystem_status = sync_status;
        }
    }
    servicing = false;
    return finish(report, status, filesystem_status, NULL, 0U);
}

static enum package_upload_status resolve_slot(
    uint64_t owner,
    package_upload_token token,
    struct upload_slot **slot,
    size_t *slot_index
)
{
    uint32_t encoded_index = (uint32_t)token;
    uint32_t generation = (uint32_t)(token >> 32U);
    size_t index;

    if (!initialized) {
        return PACKAGE_UPLOAD_STATUS_NOT_INITIALIZED;
    }
    if (owner == 0U || slot == NULL || slot_index == NULL ||
        encoded_index == 0U || encoded_index > PACKAGE_UPLOAD_SLOT_LIMIT ||
        generation == 0U) {
        return PACKAGE_UPLOAD_STATUS_STALE;
    }
    index = (size_t)(encoded_index - 1U);
    if (!slots[index].active || slots[index].generation != generation ||
        slots[index].owner != owner) {
        return PACKAGE_UPLOAD_STATUS_STALE;
    }
    *slot = &slots[index];
    *slot_index = index;
    return PACKAGE_UPLOAD_STATUS_OK;
}

static void release_slot(struct upload_slot *slot)
{
    uint32_t generation = slot->generation + 1U;

    if (generation == 0U) {
        generation = 1U;
    }
    zero_bytes(slot, sizeof(*slot));
    slot->generation = generation;
}

/*
 * Upload files are private inert staging objects, never package authority.
 * Trimming them through several durable transactions is therefore safe and
 * makes cleanup retryable even when a payload spans more blocks than one
 * bounded ext4 journal transaction may revoke. Public unlink remains atomic.
 */
static enum phipfs_status remove_private_file(
    const char *path,
    bool *changed
)
{
    struct phipfs_stat stat;
    enum phipfs_status status = phipfs_stat_path(PHIPFS_VOLUME_DATA, path,
        &stat);

    if (status == PHIPFS_STATUS_NOT_FOUND) {
        return PHIPFS_STATUS_OK;
    }
    if (status != PHIPFS_STATUS_OK || stat.directory) {
        return status == PHIPFS_STATUS_OK ? PHIPFS_STATUS_IS_DIRECTORY :
            status;
    }
    while (stat.size != 0U) {
        const uint64_t next = stat.size > PACKAGE_UPLOAD_CLEANUP_CHUNK ?
            stat.size - PACKAGE_UPLOAD_CLEANUP_CHUNK : 0U;

        status = phipfs_truncate(PHIPFS_VOLUME_DATA, path, next);
        if (status != PHIPFS_STATUS_OK) {
            return status;
        }
        stat.size = next;
        *changed = true;
    }
    status = phipfs_unlink(PHIPFS_VOLUME_DATA, path);
    if (status == PHIPFS_STATUS_OK) {
        *changed = true;
        return PHIPFS_STATUS_OK;
    }
    return status == PHIPFS_STATUS_NOT_FOUND ? PHIPFS_STATUS_OK : status;
}

enum package_upload_status package_upload_initialize(
    struct package_upload_report *report
)
{
    bool changed = false;

    report_clear(report);
    if (report == NULL) {
        return PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT;
    }
    if (servicing) {
        return finish(report, PACKAGE_UPLOAD_STATUS_BUSY, PHIPFS_STATUS_OK,
            NULL, 0U);
    }
    servicing = true;
    if (initialized) {
        servicing = false;
        return finish(report, PACKAGE_UPLOAD_STATUS_OK, PHIPFS_STATUS_OK,
            NULL, 0U);
    }
    enum phipfs_status fs_status = phipfs_mkdir(PHIPFS_VOLUME_DATA,
        PACKAGE_UPLOAD_DIRECTORY);

    if (fs_status == PHIPFS_STATUS_OK) {
        changed = true;
    } else if (fs_status != PHIPFS_STATUS_EXISTS) {
        return initialization_failure(report,
            PACKAGE_UPLOAD_STATUS_FILESYSTEM, fs_status, changed);
    }
    for (size_t index = 0U; index < PACKAGE_UPLOAD_SLOT_LIMIT; ++index) {
        char path[PHIPFS_MAX_PATH];

        slot_path(index, path);
        fs_status = remove_private_file(path, &changed);
        if (fs_status != PHIPFS_STATUS_OK) {
            return initialization_failure(report,
                PACKAGE_UPLOAD_STATUS_FILESYSTEM, fs_status, changed);
        }
    }
    if (changed) {
        fs_status = phipfs_sync(PHIPFS_VOLUME_DATA);
        if (fs_status != PHIPFS_STATUS_OK) {
            servicing = false;
            return finish(report, PACKAGE_UPLOAD_STATUS_DURABILITY, fs_status,
                NULL, 0U);
        }
    }
    zero_bytes(slots, sizeof(slots));
    for (size_t index = 0U; index < PACKAGE_UPLOAD_SLOT_LIMIT; ++index) {
        slots[index].generation = 1U;
    }
    initialized = true;
    servicing = false;
    return finish(report, PACKAGE_UPLOAD_STATUS_OK, PHIPFS_STATUS_OK, NULL, 0U);
}

enum package_upload_status package_upload_open(
    uint64_t owner,
    struct package_upload_report *report
)
{
    size_t index = PACKAGE_UPLOAD_SLOT_LIMIT;
    char path[PHIPFS_MAX_PATH];
    phipfs_handle file;

    report_clear(report);
    if (report == NULL || owner == 0U) {
        return PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT;
    }
    if (!initialized) {
        return finish(report, PACKAGE_UPLOAD_STATUS_NOT_INITIALIZED,
            PHIPFS_STATUS_OK, NULL, 0U);
    }
    if (servicing) {
        return finish(report, PACKAGE_UPLOAD_STATUS_BUSY, PHIPFS_STATUS_OK,
            NULL, 0U);
    }
    servicing = true;
    for (size_t candidate = 0U; candidate < PACKAGE_UPLOAD_SLOT_LIMIT;
            ++candidate) {
        if (!slots[candidate].active) {
            index = candidate;
            break;
        }
    }
    if (index == PACKAGE_UPLOAD_SLOT_LIMIT) {
        servicing = false;
        return finish(report, PACKAGE_UPLOAD_STATUS_NO_SLOT, PHIPFS_STATUS_OK,
            NULL, 0U);
    }
    slot_path(index, path);
    enum phipfs_status fs_status = phipfs_create(PHIPFS_VOLUME_DATA, path);

    if (fs_status != PHIPFS_STATUS_OK) {
        servicing = false;
        return finish(report, PACKAGE_UPLOAD_STATUS_FILESYSTEM, fs_status,
            NULL, 0U);
    }
    fs_status = phipfs_open(PHIPFS_VOLUME_DATA, path, PHIPFS_ACCESS_WRITE, &file);
    if (fs_status != PHIPFS_STATUS_OK) {
        enum phipfs_status cleanup_status = phipfs_unlink(PHIPFS_VOLUME_DATA,
            path);

        if (cleanup_status == PHIPFS_STATUS_OK) {
            cleanup_status = phipfs_sync(PHIPFS_VOLUME_DATA);
        }
        if (cleanup_status != PHIPFS_STATUS_OK &&
            cleanup_status != PHIPFS_STATUS_NOT_FOUND) {
            initialized = false;
        }
        servicing = false;
        return finish(report, PACKAGE_UPLOAD_STATUS_FILESYSTEM, fs_status,
            NULL, 0U);
    }
    struct upload_slot *slot = &slots[index];
    uint32_t generation = slot->generation;

    zero_bytes(slot, sizeof(*slot));
    slot->generation = generation;
    slot->owner = owner;
    slot->file = file;
    slot->active = true;
    slot->file_open = true;
    slot->file_present = true;
    if (package_state_sha256_initialize(&slot->sha256) !=
            PACKAGE_STATE_STATUS_OK) {
        (void)phipfs_close(file);
        (void)phipfs_unlink(PHIPFS_VOLUME_DATA, path);
        (void)phipfs_sync(PHIPFS_VOLUME_DATA);
        release_slot(slot);
        servicing = false;
        return finish(report, PACKAGE_UPLOAD_STATUS_STATE, PHIPFS_STATUS_OK,
            NULL, 0U);
    }
    servicing = false;
    return finish(report, PACKAGE_UPLOAD_STATUS_OK, PHIPFS_STATUS_OK, slot,
        index);
}

enum package_upload_status package_upload_write(
    uint64_t owner,
    package_upload_token token,
    const uint8_t *bytes,
    size_t byte_count,
    size_t *written_bytes,
    struct package_upload_report *report
)
{
    struct upload_slot *slot;
    size_t index;
    size_t total = 0U;

    report_clear(report);
    if (report == NULL || written_bytes == NULL ||
        (bytes == NULL && byte_count != 0U)) {
        return PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT;
    }
    *written_bytes = 0U;
    enum package_upload_status status = resolve_slot(owner, token, &slot,
        &index);

    if (status != PACKAGE_UPLOAD_STATUS_OK) {
        return finish(report, status, PHIPFS_STATUS_OK, NULL, 0U);
    }
    if (servicing) {
        return finish(report, PACKAGE_UPLOAD_STATUS_BUSY, PHIPFS_STATUS_OK,
            slot, index);
    }
    if (!slot->file_open || slot->sealed || slot->poisoned) {
        return finish(report, PACKAGE_UPLOAD_STATUS_STATE, PHIPFS_STATUS_OK,
            slot, index);
    }
    if (byte_count > PACKAGE_UPLOAD_WRITE_MAX ||
        byte_count > PACKAGE_UPLOAD_MAX_BYTES - slot->byte_count) {
        return finish(report, PACKAGE_UPLOAD_STATUS_RANGE, PHIPFS_STATUS_OK,
            slot, index);
    }
    servicing = true;
    while (total < byte_count) {
        size_t written = 0U;
        enum phipfs_status fs_status = phipfs_write(slot->file, bytes + total,
            byte_count - total, &written);

        if (written > byte_count - total ||
            (fs_status == PHIPFS_STATUS_OK && written == 0U)) {
            slot->poisoned = true;
            *written_bytes = total;
            servicing = false;
            return finish(report, PACKAGE_UPLOAD_STATUS_STATE, fs_status,
                slot, index);
        }
        if (written != 0U) {
            if (package_state_sha256_update(&slot->sha256, bytes + total,
                    written) != PACKAGE_STATE_STATUS_OK) {
                slot->poisoned = true;
                *written_bytes = total;
                servicing = false;
                return finish(report, PACKAGE_UPLOAD_STATUS_STATE,
                    PHIPFS_STATUS_OK, slot, index);
            }
            total += written;
            slot->byte_count += written;
        }
        if (fs_status != PHIPFS_STATUS_OK) {
            slot->poisoned = true;
            *written_bytes = total;
            servicing = false;
            return finish(report, PACKAGE_UPLOAD_STATUS_FILESYSTEM, fs_status,
                slot, index);
        }
    }
    *written_bytes = total;
    servicing = false;
    return finish(report, PACKAGE_UPLOAD_STATUS_OK, PHIPFS_STATUS_OK, slot,
        index);
}

enum package_upload_status package_upload_seal(
    uint64_t owner,
    package_upload_token token,
    uint64_t expected_bytes,
    const uint8_t expected_sha256[PACKAGE_STATE_SHA256_BYTES],
    struct package_upload_report *report
)
{
    struct upload_slot *slot;
    size_t index;
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];

    report_clear(report);
    if (report == NULL || expected_sha256 == NULL) {
        return PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT;
    }
    enum package_upload_status status = resolve_slot(owner, token, &slot,
        &index);

    if (status != PACKAGE_UPLOAD_STATUS_OK) {
        return finish(report, status, PHIPFS_STATUS_OK, NULL, 0U);
    }
    if (servicing) {
        return finish(report, PACKAGE_UPLOAD_STATUS_BUSY, PHIPFS_STATUS_OK,
            slot, index);
    }
    if (!slot->file_open || slot->sealed || slot->poisoned) {
        return finish(report, PACKAGE_UPLOAD_STATUS_STATE, PHIPFS_STATUS_OK,
            slot, index);
    }
    servicing = true;
    enum phipfs_status fs_status = phipfs_close(slot->file);

    slot->file_open = false;
    slot->file = 0U;
    if (fs_status != PHIPFS_STATUS_OK) {
        slot->poisoned = true;
        servicing = false;
        return finish(report, PACKAGE_UPLOAD_STATUS_FILESYSTEM, fs_status,
            slot, index);
    }
    if (package_state_sha256_finish(&slot->sha256, digest) !=
            PACKAGE_STATE_STATUS_OK) {
        slot->poisoned = true;
        servicing = false;
        return finish(report, PACKAGE_UPLOAD_STATUS_STATE, PHIPFS_STATUS_OK,
            slot, index);
    }
    for (size_t at = 0U; at < sizeof(digest); ++at) {
        slot->digest[at] = digest[at];
    }
    if (slot->byte_count != expected_bytes) {
        slot->poisoned = true;
        servicing = false;
        return finish(report, PACKAGE_UPLOAD_STATUS_LENGTH, PHIPFS_STATUS_OK,
            slot, index);
    }
    if (!equal_bytes(digest, expected_sha256, sizeof(digest))) {
        slot->poisoned = true;
        servicing = false;
        return finish(report, PACKAGE_UPLOAD_STATUS_DIGEST, PHIPFS_STATUS_OK,
            slot, index);
    }
    fs_status = phipfs_sync(PHIPFS_VOLUME_DATA);
    if (fs_status != PHIPFS_STATUS_OK) {
        slot->poisoned = true;
        servicing = false;
        return finish(report, PACKAGE_UPLOAD_STATUS_DURABILITY, fs_status,
            slot, index);
    }
    slot->sealed = true;
    slot->durable = true;
    servicing = false;
    return finish(report, PACKAGE_UPLOAD_STATUS_OK, PHIPFS_STATUS_OK, slot,
        index);
}

enum package_upload_status package_upload_read(
    uint64_t owner,
    package_upload_token token,
    uint64_t offset,
    uint8_t *bytes,
    size_t capacity,
    size_t *read_bytes,
    struct package_upload_report *report
)
{
    struct upload_slot *slot;
    size_t index;
    char path[PHIPFS_MAX_PATH];
    phipfs_handle file;

    report_clear(report);
    if (report == NULL || read_bytes == NULL ||
        (bytes == NULL && capacity != 0U)) {
        return PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT;
    }
    *read_bytes = 0U;
    enum package_upload_status status = resolve_slot(owner, token, &slot,
        &index);

    if (status != PACKAGE_UPLOAD_STATUS_OK) {
        return finish(report, status, PHIPFS_STATUS_OK, NULL, 0U);
    }
    if (servicing) {
        return finish(report, PACKAGE_UPLOAD_STATUS_BUSY, PHIPFS_STATUS_OK,
            slot, index);
    }
    if (!slot->sealed || !slot->durable || slot->poisoned || slot->file_open) {
        return finish(report, PACKAGE_UPLOAD_STATUS_STATE, PHIPFS_STATUS_OK,
            slot, index);
    }
    if (capacity > PACKAGE_UPLOAD_WRITE_MAX || offset > slot->byte_count) {
        return finish(report, PACKAGE_UPLOAD_STATUS_RANGE, PHIPFS_STATUS_OK,
            slot, index);
    }
    servicing = true;
    slot_path(index, path);
    enum phipfs_status fs_status = phipfs_open(PHIPFS_VOLUME_DATA, path,
        PHIPFS_ACCESS_READ, &file);

    if (fs_status == PHIPFS_STATUS_OK) {
        fs_status = phipfs_pread(file, bytes, capacity, offset, read_bytes);
        enum phipfs_status close_status = phipfs_close(file);

        if (fs_status == PHIPFS_STATUS_OK && close_status != PHIPFS_STATUS_OK) {
            fs_status = close_status;
        }
    }
    servicing = false;
    return finish(report, fs_status == PHIPFS_STATUS_OK ?
        PACKAGE_UPLOAD_STATUS_OK : PACKAGE_UPLOAD_STATUS_FILESYSTEM,
        fs_status, slot, index);
}

enum package_upload_status package_upload_inspect(
    uint64_t owner,
    package_upload_token token,
    struct package_upload_report *report
)
{
    struct upload_slot *slot;
    size_t index;

    report_clear(report);
    if (report == NULL) {
        return PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT;
    }
    enum package_upload_status status = resolve_slot(owner, token, &slot,
        &index);

    if (status != PACKAGE_UPLOAD_STATUS_OK) {
        return finish(report, status, PHIPFS_STATUS_OK, NULL, 0U);
    }
    if (!slot->sealed || !slot->durable || slot->poisoned || slot->file_open) {
        return finish(report, PACKAGE_UPLOAD_STATUS_STATE, PHIPFS_STATUS_OK,
            slot, index);
    }
    return finish(report, PACKAGE_UPLOAD_STATUS_OK, PHIPFS_STATUS_OK, slot,
        index);
}

enum package_upload_status package_upload_close(
    uint64_t owner,
    package_upload_token token,
    struct package_upload_report *report
)
{
    struct upload_slot *slot;
    size_t index;
    char path[PHIPFS_MAX_PATH];

    report_clear(report);
    if (report == NULL) {
        return PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT;
    }
    enum package_upload_status status = resolve_slot(owner, token, &slot,
        &index);

    if (status != PACKAGE_UPLOAD_STATUS_OK) {
        return finish(report, status, PHIPFS_STATUS_OK, NULL, 0U);
    }
    if (servicing) {
        return finish(report, PACKAGE_UPLOAD_STATUS_BUSY, PHIPFS_STATUS_OK,
            slot, index);
    }
    servicing = true;
    if (slot->file_open) {
        (void)phipfs_close(slot->file);
        slot->file_open = false;
        slot->file = 0U;
    }
    slot_path(index, path);
    if (slot->file_present) {
        bool changed = false;
        enum phipfs_status fs_status = remove_private_file(path, &changed);

        if (fs_status != PHIPFS_STATUS_OK) {
            servicing = false;
            return finish(report, PACKAGE_UPLOAD_STATUS_FILESYSTEM, fs_status,
                slot, index);
        }
        slot->file_present = false;
    }
    enum phipfs_status fs_status = phipfs_sync(PHIPFS_VOLUME_DATA);

    if (fs_status != PHIPFS_STATUS_OK) {
        servicing = false;
        return finish(report, PACKAGE_UPLOAD_STATUS_DURABILITY, fs_status,
            slot, index);
    }
    release_slot(slot);
    servicing = false;
    return finish(report, PACKAGE_UPLOAD_STATUS_OK, PHIPFS_STATUS_OK, NULL, 0U);
}

bool package_upload_resources_released(void)
{
    if (servicing) {
        return false;
    }
    for (size_t index = 0U; index < PACKAGE_UPLOAD_SLOT_LIMIT; ++index) {
        if (slots[index].active) {
            return false;
        }
    }
    return true;
}

const char *package_upload_status_string(enum package_upload_status status)
{
    static const char *const names[PACKAGE_UPLOAD_STATUS_COUNT] = {
        [PACKAGE_UPLOAD_STATUS_OK] = "ok",
        [PACKAGE_UPLOAD_STATUS_NULL_ARGUMENT] = "null argument",
        [PACKAGE_UPLOAD_STATUS_NOT_INITIALIZED] = "not initialized",
        [PACKAGE_UPLOAD_STATUS_BUSY] = "busy",
        [PACKAGE_UPLOAD_STATUS_NO_SLOT] = "no slot",
        [PACKAGE_UPLOAD_STATUS_STALE] = "stale",
        [PACKAGE_UPLOAD_STATUS_STATE] = "state",
        [PACKAGE_UPLOAD_STATUS_RANGE] = "range",
        [PACKAGE_UPLOAD_STATUS_LENGTH] = "length",
        [PACKAGE_UPLOAD_STATUS_DIGEST] = "digest",
        [PACKAGE_UPLOAD_STATUS_FILESYSTEM] = "filesystem",
        [PACKAGE_UPLOAD_STATUS_DURABILITY] = "durability"
    };

    return status < PACKAGE_UPLOAD_STATUS_COUNT && names[status] != NULL ?
        names[status] : "unknown";
}
