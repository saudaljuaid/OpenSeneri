/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the production Notes save path against storage boundary failures. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/ui.c"

enum save_fault {
    SAVE_OK, CREATE_FAIL, CREATE_LOST, STAT_FAIL, WRITE_FAIL, SHORT_WRITE,
    FIRST_SYNC_FAIL, PUBLISH_FAIL, PUBLISH_LOST, PUBLISH_WRONG_INODE,
    SOURCE_REPLACED, SECOND_SYNC_FAIL, CLOSE_FAIL
};

static enum save_fault fault;
static unsigned occupied_names;
static unsigned opens;
static unsigned writes;
static unsigned syncs;
static unsigned publications;
static unsigned closes;
static unsigned live_handles;
static uint64_t target_inode;
static uint64_t scratch_inode;
static size_t target_length;
static size_t scratch_length;
static uint16_t expected_mode;
static bool pending_publication;
static char scratch_name[PHIPFS_MAX_PATH + 1U];
static uint8_t target_bytes[8192];
static uint8_t scratch_bytes[8192];
static uint8_t expected_bytes[8192];
static size_t expected_length;
static const char *expected_destination;
static const char *expected_scratch;
static unsigned fail_write_at;
static bool fail_paint_row;
static bool paint_saved;
static unsigned cleanup_calls;
static bool cleanup_sync_fails;
static bool cleanup_unlink_fails;
static bool cleanup_lost;
static bool pending_cleanup;
static bool replace_before_cleanup;

static void assert_handle(phipfs_handle handle)
{
    assert(handle == 77U && live_handles == 1U);
}

bool phipfs_has_atomic_replace(enum phipfs_volume volume)
{
    assert(volume == PHIPFS_VOLUME_DATA);
    return true;
}

const char *phipfs_status_string(enum phipfs_status status)
{
    return status == PHIPFS_STATUS_OK ? "ok" : "error";
}

enum phipfs_status phipfs_stat_path(enum phipfs_volume volume, const char *path,
    struct phipfs_stat *result)
{
    assert(volume == PHIPFS_VOLUME_DATA && strcmp(path, expected_destination) == 0);
    if (target_inode == 0U) return PHIPFS_STATUS_NOT_FOUND;
    memset(result, 0, sizeof(*result));
    result->object_id = target_inode;
    result->size = target_length;
    result->mode = 0100600U;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_lstat_path(enum phipfs_volume volume, const char *path,
    struct phipfs_stat *result)
{
    assert(volume == PHIPFS_VOLUME_DATA && strcmp(path, scratch_name) == 0);
    if (scratch_inode == 0U) return PHIPFS_STATUS_NOT_FOUND;
    memset(result, 0, sizeof(*result));
    result->object_id = scratch_inode;
    result->size = scratch_length;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_open_options(enum phipfs_volume volume, const char *path,
    enum phipfs_access access, uint8_t flags, uint16_t mode, phipfs_handle *handle)
{
    assert(volume == PHIPFS_VOLUME_DATA && access == PHIPFS_ACCESS_READ_WRITE);
    assert(flags == (PHIPFS_OPEN_CREATE | PHIPFS_OPEN_EXCLUSIVE));
    assert(mode == expected_mode && live_handles == 0U);
    ++opens;
    char expected[PHIPFS_MAX_PATH + 1U];
    strcpy(expected, expected_scratch);
    char *digit = strchr(expected, '0');
    assert(digit != NULL);
    *digit = (char)('0' + opens);
    assert(strcmp(path, expected) == 0);
    if (opens <= occupied_names) return PHIPFS_STATUS_EXISTS;
    assert(scratch_inode == 0U); /* Successful cleanup makes the same name reusable. */
    strcpy(scratch_name, path);
    if (fault == CREATE_FAIL) return PHIPFS_STATUS_IO;
    scratch_inode = 20U;
    if (fault == CREATE_LOST) return PHIPFS_STATUS_IO;
    *handle = 77U;
    live_handles = 1U;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_fstat(phipfs_handle handle, struct phipfs_stat *result)
{
    assert_handle(handle);
    if (fault == STAT_FAIL) return PHIPFS_STATUS_IO;
    memset(result, 0, sizeof(*result));
    result->object_id = 20U;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_write(phipfs_handle handle, const uint8_t *bytes,
    size_t length, size_t *written)
{
    assert_handle(handle);
    assert(syncs == 0U && publications == 0U && scratch_length + length <= expected_length);
    ++writes;
    if (fault == WRITE_FAIL || writes == fail_write_at) return PHIPFS_STATUS_IO;
    *written = fault == SHORT_WRITE ? length / 2U : length;
    memcpy(scratch_bytes + scratch_length, bytes, *written);
    scratch_length += *written;
    return PHIPFS_STATUS_OK;
}

static void finish_publication(void)
{
    target_inode = scratch_inode;
    target_length = scratch_length;
    memcpy(target_bytes, scratch_bytes, scratch_length);
    scratch_inode = 0U;
    pending_publication = false;
}

enum phipfs_status phipfs_fsync(phipfs_handle handle)
{
    assert_handle(handle);
    ++syncs;
    if (cleanup_sync_fails) return PHIPFS_STATUS_IO;
    if (pending_cleanup) { scratch_inode = 0U; pending_cleanup = false; }
    if (syncs == 1U) {
        assert(publications == 0U && target_inode != 20U);
        if (fault == FIRST_SYNC_FAIL) return PHIPFS_STATUS_IO;
    } else if (publications != 0U) {
        if (syncs == 2U && fault == SECOND_SYNC_FAIL) return PHIPFS_STATUS_IO;
        if (pending_publication) finish_publication();
    }
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_unlink_held_file(phipfs_handle handle, const char *path)
{
    assert_handle(handle);
    assert(strcmp(path, scratch_name) == 0 && !pending_publication);
    ++cleanup_calls;
    if (replace_before_cleanup) scratch_inode = 30U;
    if (scratch_inode == 0U) return PHIPFS_STATUS_NOT_FOUND;
    if (scratch_inode != 20U) return PHIPFS_STATUS_STALE_HANDLE;
    if (cleanup_unlink_fails) return PHIPFS_STATUS_IO;
    if (cleanup_lost) { pending_cleanup = true; return PHIPFS_STATUS_IO; }
    scratch_inode = 0U;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_publish_file(phipfs_handle handle, const char *source,
    const char *destination)
{
    assert_handle(handle);
    assert(strcmp(source, scratch_name) == 0 && strcmp(destination, expected_destination) == 0);
    assert(syncs == 1U && scratch_length == expected_length);
    assert(memcmp(scratch_bytes, expected_bytes, expected_length) == 0);
    ++publications;
    if (fault == PUBLISH_FAIL) return PHIPFS_STATUS_IO;
    if (fault == SOURCE_REPLACED) {
        scratch_inode = 30U;
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    if (fault == PUBLISH_LOST) {
        pending_publication = true;
        return PHIPFS_STATUS_IO;
    }
    finish_publication();
    if (fault == PUBLISH_WRONG_INODE) target_inode = 30U;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_close(phipfs_handle handle)
{
    assert_handle(handle);
    ++closes;
    live_handles = 0U;
    return fault == CLOSE_FAIL ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
}

static void reset_save(enum save_fault next_fault)
{
    assert(live_handles == 0U);
    fault = next_fault;
    occupied_names = opens = writes = syncs = publications = closes = 0U;
    target_inode = 10U;
    target_length = 3U;
    scratch_inode = 0U;
    scratch_length = 0U;
    expected_mode = 0600U;
    pending_publication = false;
    memcpy(target_bytes, "old", 3U);
    memset(scratch_bytes, 0, sizeof(scratch_bytes));
    strcpy(note_path, "folder/NOTES.TXT");
    strcpy(note_buffer, "complete new note\n");
    note_length = strlen(note_buffer);
    expected_length = note_length;
    memcpy(expected_bytes, note_buffer, note_length);
    expected_destination = note_path;
    expected_scratch = "folder/SNTMP0.TMP";
    fail_write_at = 0U;
    fail_paint_row = paint_saved = false;
    cleanup_calls = 0U;
    cleanup_sync_fails = cleanup_unlink_fails = cleanup_lost = pending_cleanup = false;
    replace_before_cleanup = false;
    note_dirty = true;
    note_savable = true;
}

static void assert_saved(void)
{
    assert(!note_dirty && live_handles == 0U && closes == 1U);
    assert(target_inode == 20U && scratch_inode == 0U && target_length == expected_length);
    assert(memcmp(target_bytes, expected_bytes, expected_length) == 0);
    assert(syncs == 2U && publications == 1U);
}

const struct editor_item *editor_item(size_t index)
{
    assert(index < EDITOR_MAX_ITEMS);
    return NULL;
}

struct paint_image_info paint_image(void)
{
    return (struct paint_image_info){ .width = 2U, .height = 2U, .row_stride = 8U, .dirty = true };
}

enum paint_status paint_copy_bgr24_row(uint32_t row, uint8_t *destination,
    size_t capacity, size_t *written)
{
    assert(row < 2U && capacity >= 8U);
    if (fail_paint_row && row == 0U) return PAINT_STATUS_SURFACE_FAILURE;
    memset(destination, row == 0U ? 0x11 : 0x22, 6U);
    destination[6] = destination[7] = 0U;
    *written = 8U;
    return PAINT_STATUS_OK;
}

void paint_mark_saved(void) { paint_saved = true; }
void console_serial_write(const char *message) { (void)message; }

enum phipfs_status phipfs_list(enum phipfs_volume volume, const char *path,
    struct phipfs_list_entry *entries, size_t capacity, size_t *entry_count)
{
    (void)path; (void)entries; (void)capacity;
    assert(volume == PHIPFS_VOLUME_DATA);
    *entry_count = 0U;
    return PHIPFS_STATUS_IO; /* Rendering and directory enumeration are separate tests. */
}

static void reset_app(unsigned app, enum save_fault next_fault)
{
    reset_save(next_fault);
    if (app == 0U) {
        expected_destination = "MEDIAEDT.PHI";
        expected_scratch = "MSTMP0.PHI";
        expected_length = UI_MEDIA_SOURCE_PROJECT_BYTES;
        media_source_encode_project(expected_bytes);
        media_source_dirty = true;
    } else if (app == 1U) {
        expected_destination = "PHIPMED.PHI";
        expected_scratch = "METMP0.PHI";
        expected_length = UI_MEDIA_PROJECT_BYTES;
        media_editor_encode(expected_bytes);
        media_editor_dirty = true;
    } else {
        expected_destination = "PAINT.BMP";
        expected_scratch = "PNTMP0.BMP";
        expected_length = 70U;
        memset(expected_bytes, 0, expected_length);
        expected_bytes[0] = 'B'; expected_bytes[1] = 'M';
        expected_bytes[2] = 70U; expected_bytes[10] = 54U; expected_bytes[14] = 40U;
        expected_bytes[18] = expected_bytes[22] = 2U;
        expected_bytes[26] = 1U; expected_bytes[28] = 24U; expected_bytes[34] = 16U;
        memset(expected_bytes + 54U, 0x22, 6U);
        memset(expected_bytes + 62U, 0x11, 6U);
    }
}

static enum phipfs_status save_app(unsigned app)
{
    return app == 0U ? media_source_save() : app == 1U ? media_editor_save_timeline() : paint_save();
}

int main(void)
{
    reset_save(SAVE_OK);
    occupied_names = 2U; /* Existing files and dangling symlinks both refuse O_EXCL. */
    assert(note_save() == PHIPFS_STATUS_OK && opens == 3U);
    assert_saved();
    reset_save(SAVE_OK);
    target_inode = 0U;
    expected_mode = 0644U;
    note_length = 0U;
    expected_length = 0U;
    assert(note_save() == PHIPFS_STATUS_OK && writes == 0U);
    assert_saved();
    reset_save(PUBLISH_LOST);
    assert(note_save() == PHIPFS_STATUS_OK);
    assert_saved();

    const enum save_fault faults[] = { CREATE_FAIL, CREATE_LOST, STAT_FAIL,
        WRITE_FAIL, SHORT_WRITE, FIRST_SYNC_FAIL, PUBLISH_FAIL, SOURCE_REPLACED,
        PUBLISH_WRONG_INODE, SECOND_SYNC_FAIL, CLOSE_FAIL };
    for (size_t index = 0U; index < sizeof(faults) / sizeof(faults[0]); ++index) {
        reset_save(faults[index]);
        const enum phipfs_status status = note_save();
        assert(status != PHIPFS_STATUS_OK && note_dirty && live_handles == 0U);
        assert(closes == (fault == CREATE_FAIL || fault == CREATE_LOST ? 0U : 1U));
        assert(strstr(app_status, "save note failed") != NULL);
        if (fault <= FIRST_SYNC_FAIL) assert(publications == 0U);
        if (fault <= PUBLISH_FAIL || fault == SOURCE_REPLACED) {
            assert(target_inode == 10U && target_length == 3U);
            assert(memcmp(target_bytes, "old", 3U) == 0);
        }
        if (fault == CREATE_LOST) assert(scratch_inode == 20U && cleanup_calls == 0U);
        if (fault == STAT_FAIL || fault == WRITE_FAIL || fault == SHORT_WRITE ||
            fault == FIRST_SYNC_FAIL || fault == PUBLISH_FAIL) assert(scratch_inode == 0U && cleanup_calls == 1U);
        if (fault == SOURCE_REPLACED) assert(scratch_inode == 30U);
        if (fault == PUBLISH_FAIL || fault == SOURCE_REPLACED) assert(syncs == 4U);
    }
    reset_save(SAVE_OK);
    occupied_names = 9U;
    assert(note_save() == PHIPFS_STATUS_EXISTS && opens == 9U);
    assert(note_dirty && live_handles == 0U && closes == 0U && writes == 0U);
    reset_save(SAVE_OK);
    note_savable = false;
    assert(note_save() == PHIPFS_STATUS_RANGE && opens == 0U && note_dirty);
    for (unsigned app = 0U; app < 3U; ++app) {
        const enum save_fault app_faults[] = { SAVE_OK, PUBLISH_LOST, CREATE_LOST, WRITE_FAIL,
            FIRST_SYNC_FAIL, PUBLISH_FAIL, SOURCE_REPLACED, SECOND_SYNC_FAIL, CLOSE_FAIL };
        for (size_t index = 0U; index < sizeof(app_faults) / sizeof(app_faults[0]); ++index) {
            reset_app(app, app_faults[index]);
            const bool success = fault == SAVE_OK || fault == PUBLISH_LOST;
            assert((save_app(app) == PHIPFS_STATUS_OK) == success);
            assert(live_handles == 0U);
            assert((app == 0U ? !media_source_dirty : app == 1U ? !media_editor_dirty : paint_saved) == success);
            if (success) {
                assert(target_inode == 20U && scratch_inode == 0U && target_length == expected_length);
                assert(memcmp(target_bytes, expected_bytes, expected_length) == 0);
            }
        }
    }
    for (unsigned boundary = 1U; boundary <= 3U; ++boundary) {
        reset_app(2U, SAVE_OK);
        fail_write_at = boundary;
        assert(paint_save() == PHIPFS_STATUS_IO);
        assert(!paint_saved && publications == 0U && closes == 1U && target_inode == 10U);
    }
    reset_app(2U, SAVE_OK);
    fail_paint_row = true;
    assert(paint_save() == PHIPFS_STATUS_IO && publications == 0U && live_handles == 0U);
    /* Recovery must not promote either format's shared, incomplete legacy scratch. */
    reset_app(0U, SAVE_OK);
    assert(media_source_recover_project() == PHIPFS_STATUS_OK);
    assert(media_editor_recover() == PHIPFS_STATUS_OK);
    assert(paint_recover_save() == PHIPFS_STATUS_OK);
    assert(opens == 0U && publications == 0U && syncs == 0U);
    for (unsigned failure = 0U; failure < 4U; ++failure) {
        reset_save(WRITE_FAIL);
        cleanup_sync_fails = failure == 0U;
        cleanup_unlink_fails = failure == 1U;
        cleanup_lost = failure == 2U;
        replace_before_cleanup = failure == 3U;
        assert(note_save() == PHIPFS_STATUS_IO && note_dirty && live_handles == 0U);
        assert(target_inode == 10U && target_length == 3U);
        assert(cleanup_calls == (failure == 0U ? 0U : 1U));
        assert(scratch_inode == (failure == 2U ? 0U : failure == 3U ? 30U : 20U));
    }
    reset_save(WRITE_FAIL);
    for (unsigned attempt = 0U; attempt < 12U; ++attempt) {
        // Reset observations only, retaining namespace/content between saves.
        opens = writes = syncs = publications = closes = cleanup_calls = 0U;
        assert(note_save() == PHIPFS_STATUS_IO && note_dirty && live_handles == 0U);
        assert(opens == 1U && scratch_inode == 0U && target_inode == 10U && target_length == 3U);
    }
    puts("Notes, Paint, Media ext4 save: ownership, complete publication, failure handling PASS");
    return 0;
}
