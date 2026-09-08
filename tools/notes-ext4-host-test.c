/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the production Notes save path against storage boundary failures. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/ui.c"

enum save_fault {
    SAVE_OK, CREATE_FAIL, CREATE_LOST, STAT_FAIL, WRITE_FAIL, SHORT_WRITE,
    FIRST_SYNC_FAIL, PUBLISH_FAIL, PUBLISH_LOST, PUBLISH_WRONG_INODE,
    SOURCE_REPLACED, SECOND_SYNC_FAIL, CLOSE_FAIL, CREATE_FULL
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
static uint8_t target_bytes[180U * 960U + 54U];
static uint8_t scratch_bytes[180U * 960U + 54U];
static uint8_t expected_bytes[180U * 960U + 54U];
static size_t expected_length;
static const char *expected_destination;
static const char *expected_scratch;
static unsigned fail_write_at;
static unsigned short_write_at;
static bool large_paint;
static bool fail_paint_row;
static bool paint_saved;
static unsigned cleanup_calls;
static bool cleanup_sync_fails;
static bool cleanup_unlink_fails;
static bool cleanup_lost;
static bool pending_cleanup;
static bool replace_before_cleanup;
static uint32_t setting_values[SETTINGS_MAX_TILES][SETTINGS_MAX_ROWS];
static bool settings_read_fails;
static bool settings_close_fails;
static bool data_admitted = true;
static bool restored_show_desktop;
static unsigned applied_settings;
static unsigned storage_error_dialogs;

enum dialog_status dialog_open(const struct dialog_request *request, struct ui_rect *damage)
{
    assert(state.active && request->icon == DIALOG_ICON_ERROR && request->buttons == 1U);
    assert(strcmp(request->title, "Save failed") == 0 || strcmp(request->title, "Export failed") == 0);
    assert(request->detail[0] != '\0' && strstr(request->message, "work remains open") != NULL);
    *damage = (struct ui_rect){ 0U, 0U, 100U, 100U };
    ++storage_error_dialogs;
    return DIALOG_STATUS_OK;
}

uint32_t editor_playhead_ms(void) { return 0U; }
static bool copy_source_live;
static unsigned copy_failure;
static size_t copy_position;
static unsigned copy_reads;
static unsigned copy_stats;

static void assert_handle(phipfs_handle handle)
{
    assert(handle == 77U && live_handles == 1U);
}

bool phipfs_has_atomic_replace(enum phipfs_volume volume)
{
    assert(volume == PHIPFS_VOLUME_DATA);
    return data_admitted;
}

enum taskbar_status taskbar_set_search_mode(enum taskbar_search_mode mode)
{ assert((uint32_t)mode == setting_values[0][1]); ++applied_settings; return TASKBAR_STATUS_OK; }
enum taskbar_status taskbar_set_search_visible(bool visible)
{ assert(visible == (setting_values[0][1] != 0U)); ++applied_settings; return TASKBAR_STATUS_OK; }
enum taskbar_status taskbar_set_show_desktop_button(bool visible)
{ restored_show_desktop = visible; ++applied_settings; return TASKBAR_STATUS_OK; }
enum taskbar_status taskbar_set_theme(enum taskbar_theme theme)
{ assert(theme == (setting_values[1][1] == 0U ? TASKBAR_THEME_DARK : TASKBAR_THEME_LIGHT)); ++applied_settings; return TASKBAR_STATUS_OK; }
enum taskbar_status taskbar_set_alignment(enum taskbar_alignment alignment)
{ assert(alignment == (setting_values[1][2] == 0U ? TASKBAR_ALIGNMENT_CENTER : TASKBAR_ALIGNMENT_LEFT)); ++applied_settings; return TASKBAR_STATUS_OK; }
enum taskbar_status taskbar_set_transparency(bool transparent)
{ assert(transparent == (setting_values[1][3] != 0U)); ++applied_settings; return TASKBAR_STATUS_OK; }

const char *phipfs_status_string(enum phipfs_status status)
{
    return status == PHIPFS_STATUS_OK ? "ok" : "error";
}

/* No legacy publication path may run while Data is unavailable or ext4 is admitted. */
enum phipfs_status phipfs_create(enum phipfs_volume volume, const char *path)
{ (void)volume; (void)path; assert(!"unexpected legacy create"); return PHIPFS_STATUS_IO; }
enum phipfs_status phipfs_unlink(enum phipfs_volume volume, const char *path)
{ (void)volume; (void)path; assert(!"unexpected legacy unlink"); return PHIPFS_STATUS_IO; }
enum phipfs_status phipfs_rename(enum phipfs_volume volume, const char *source, const char *destination)
{ (void)volume; (void)source; (void)destination; assert(!"unexpected legacy rename"); return PHIPFS_STATUS_IO; }
enum phipfs_status phipfs_sync(enum phipfs_volume volume)
{ (void)volume; assert(!"unexpected legacy sync"); return PHIPFS_STATUS_IO; }

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
    if (fault == CREATE_FULL) return PHIPFS_STATUS_FULL;
    scratch_inode = 20U;
    if (fault == CREATE_LOST) return PHIPFS_STATUS_IO;
    *handle = 77U;
    live_handles = 1U;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_fstat(phipfs_handle handle, struct phipfs_stat *result)
{
    if (handle == 89U) {
        assert(copy_source_live);
        if (copy_failure == 2U) return PHIPFS_STATUS_IO;
        memset(result, 0, sizeof(*result));
        result->object_id = 99U;
        result->size = expected_length;
        if (++copy_stats > 1U && copy_failure == 5U) ++result->size;
        return PHIPFS_STATUS_OK;
    }
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
    *written = fault == SHORT_WRITE || writes == short_write_at ? length / 2U : length;
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
    if (handle == 89U) {
        assert(copy_source_live);
        copy_source_live = false;
        return copy_failure == 6U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
    }
    if (handle == 88U) {
        assert(live_handles == 1U);
        live_handles = 0U;
        return settings_close_fails ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
    }
    assert_handle(handle);
    ++closes;
    live_handles = 0U;
    return fault == CLOSE_FAIL ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_open(enum phipfs_volume volume, const char *path,
    enum phipfs_access access, phipfs_handle *handle)
{
    if (strcmp(path, "SOURCE.BIN") == 0 || strcmp(path, "SOURCE.BMP") == 0) {
        assert(volume == PHIPFS_VOLUME_DATA && access == PHIPFS_ACCESS_READ && !copy_source_live);
        if (copy_failure == 1U) return PHIPFS_STATUS_IO;
        copy_source_live = true;
        *handle = 89U;
        return PHIPFS_STATUS_OK;
    }
    assert(volume == PHIPFS_VOLUME_DATA && strcmp(path, "SETTINGS.PHI") == 0 && access == PHIPFS_ACCESS_READ);
    assert(live_handles == 0U);
    if (target_inode == 0U) return PHIPFS_STATUS_NOT_FOUND;
    *handle = 88U;
    live_handles = 1U;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_read(phipfs_handle handle, uint8_t *bytes, size_t capacity, size_t *count)
{
    if (handle == 89U) {
        assert(copy_source_live && capacity <= expected_length - copy_position);
        *count = 0U;
        if (++copy_reads == 2U) {
            if (copy_failure == 3U) return PHIPFS_STATUS_IO;
            if (copy_failure == 4U) return PHIPFS_STATUS_OK;
        }
        memcpy(bytes, expected_bytes + copy_position, capacity);
        copy_position += capacity;
        *count = capacity;
        return PHIPFS_STATUS_OK;
    }
    assert(handle == 88U && live_handles == 1U);
    if (settings_read_fails) return PHIPFS_STATUS_IO;
    *count = target_length < capacity ? target_length : capacity;
    memcpy(bytes, target_bytes, *count);
    return PHIPFS_STATUS_OK;
}

uint32_t settings_row_state(size_t page, size_t row) { return setting_values[page][row]; }
enum phipfs_status phipfs_pread(phipfs_handle handle, uint8_t *bytes,
    size_t capacity, uint64_t offset, size_t *count)
{
    assert(handle == 89U && offset <= expected_length);
    const size_t saved = copy_position;
    copy_position = (size_t)offset;
    const enum phipfs_status status = phipfs_read(handle, bytes, capacity, count);
    copy_position = saved;
    return status;
}
uint32_t framebuffer_pack(uint8_t red, uint8_t green, uint8_t blue)
{
    return ((uint32_t)red << 16U) | ((uint32_t)green << 8U) | blue;
}

enum phipfs_status phipfs_seek(phipfs_handle handle, int64_t offset,
    enum phipfs_seek_origin origin, uint64_t *position)
{
    assert(handle == 89U && copy_source_live && origin == PHIPFS_SEEK_START);
    assert(offset >= 0 && (uint64_t)offset <= expected_length);
    copy_position = (size_t)offset;
    *position = (uint64_t)offset;
    return PHIPFS_STATUS_OK;
}
enum settings_status settings_set_tile(size_t page, const struct settings_tile *tile)
{
    assert(page < SETTINGS_MAX_TILES && tile != NULL);
    return SETTINGS_STATUS_OK;
}
enum settings_status settings_set_row(size_t page, size_t row, const struct settings_row *value)
{
    assert(page < SETTINGS_MAX_TILES && row < SETTINGS_MAX_ROWS && value != NULL);
    setting_values[page][row] = value->state;
    return SETTINGS_STATUS_OK;
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
    short_write_at = 0U;
    large_paint = false;
    fail_paint_row = paint_saved = false;
    cleanup_calls = 0U;
    cleanup_sync_fails = cleanup_unlink_fails = cleanup_lost = pending_cleanup = false;
    replace_before_cleanup = false;
    settings_read_fails = settings_close_fails = false;
    assert(!copy_source_live);
    copy_failure = copy_reads = copy_stats = 0U;
    copy_position = 0U;
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
    if (large_paint)
        return (struct paint_image_info){ .width = 320U, .height = 180U, .row_stride = 960U, .dirty = true };
    return (struct paint_image_info){ .width = 2U, .height = 2U, .row_stride = 8U, .dirty = true };
}

enum paint_status paint_copy_bgr24_row(uint32_t row, uint8_t *destination,
    size_t capacity, size_t *written)
{
    if (large_paint) {
        assert(row < 180U && capacity >= 960U);
        if (fail_paint_row && row == 0U) return PAINT_STATUS_SURFACE_FAILURE;
        memset(destination, (int)(row % 251U + 1U), 960U);
        *written = 960U;
        return PAINT_STATUS_OK;
    }
    assert(row < 2U && capacity >= 8U);
    if (fail_paint_row && row == 0U) return PAINT_STATUS_SURFACE_FAILURE;
    memset(destination, row == 0U ? 0x11 : 0x22, 6U);
    destination[6] = destination[7] = 0U;
    *written = 8U;
    return PAINT_STATUS_OK;
}

void paint_mark_saved(void) { paint_saved = true; }
void console_serial_write(const char *message) { (void)message; }
struct ui_rect editor_stage_rect(void) { return (struct ui_rect){ .width = 2U, .height = 2U }; }
enum surface_status surface_read_pixel(const struct surface *surface, uint32_t x, uint32_t y, uint32_t *pixel)
{
    (void)surface; (void)x; (void)y; (void)pixel;
    return SURFACE_STATUS_NULL_ARGUMENT;
}

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
        if (app == 3U) {
            expected_destination = "EXPORT.BMP";
            expected_scratch = "MEXTP0.BMP";
            media_source_preview_loaded = true;
            media_source_preview_width = media_source_preview_height = 2U;
            media_editor_export_active = false;
            logo_red_shift = 16U; logo_green_shift = 8U; logo_blue_shift = 0U;
            for (unsigned x = 0U; x < 2U; ++x) {
                media_source_preview_pixels[x] = 0x111111U;
                media_source_preview_pixels[UI_MEDIA_SOURCE_PREVIEW_WIDTH + x] = 0x222222U;
            }
        }
    }
}

static void reset_large_bitmap(unsigned app)
{
    reset_app(app, SAVE_OK);
    large_paint = true;
    expected_length = sizeof(expected_bytes);
    memset(expected_bytes, 0, expected_length);
    expected_bytes[0] = 'B'; expected_bytes[1] = 'M';
    const uint32_t fields[][2] = { {2U, 172854U}, {10U, 54U}, {14U, 40U},
        {18U, 320U}, {22U, 180U}, {34U, 172800U} };
    for (size_t index = 0U; index < sizeof(fields) / sizeof(fields[0]); ++index)
        for (unsigned byte = 0U; byte < 4U; ++byte)
            expected_bytes[fields[index][0] + byte] = (uint8_t)(fields[index][1] >> (8U * byte));
    expected_bytes[26] = 1U; expected_bytes[28] = 24U;
    for (unsigned row = 0U; row < 180U; ++row) {
        const uint8_t value = (uint8_t)(row % 251U + 1U);
        memset(expected_bytes + 54U + (179U - row) * 960U, value, 960U);
        for (unsigned x = 0U; x < 320U; ++x)
            media_source_preview_pixels[row * UI_MEDIA_SOURCE_PREVIEW_WIDTH + x] =
                (uint32_t)value * UINT32_C(0x010101);
    }
    media_source_preview_width = 320U;
    media_source_preview_height = 180U;
}

static enum phipfs_status save_app(unsigned app)
{
    return app == 0U ? media_source_save() : app == 1U ? media_editor_save_timeline() :
        app == 2U ? paint_save() : media_source_export();
}

static void storage_errors_keep_application_open(void)
{
    const enum save_fault note_failures[] = { CREATE_FULL, WRITE_FAIL, FIRST_SYNC_FAIL, PUBLISH_FAIL };
    for (size_t index = 0U; index < sizeof(note_failures) / sizeof(note_failures[0]); ++index) {
        reset_save(note_failures[index]);
        storage_error_dialogs = 0U;
        state.active = true;
        state.layout.panel = (struct ui_rect){ 0U, 0U, 200U, 200U };
        struct ui_rect note_damage = { 0U, 0U, 0U, 0U };
        assert(application_storage_action(APPLICATION_NOTES_SAVE, &note_damage) == UI_STATUS_OK);
        assert(state.active && note_dirty && storage_error_dialogs == 1U && note_damage.width != 0U);
        assert(live_handles == 0U && scratch_inode == 0U);
        assert(target_length == 3U && memcmp(target_bytes, "old", 3U) == 0);
        assert(strcmp(note_buffer, "complete new note\n") == 0);
        fault = SAVE_OK;
        assert(application_storage_action(APPLICATION_NOTES_SAVE, &note_damage) == UI_STATUS_OK);
        assert(!note_dirty && storage_error_dialogs == 1U && live_handles == 0U && scratch_inode == 0U);
        assert(target_length == expected_length && memcmp(target_bytes, expected_bytes, expected_length) == 0);
    }
    for (unsigned app = 1U; app <= 3U; ++app) {
        const enum application_storage_action action = app == 1U ? APPLICATION_MEDIA_SAVE :
            (app == 2U ? APPLICATION_PAINT_SAVE : APPLICATION_MEDIA_EXPORT);
        const enum save_fault failures[] = { CREATE_FULL, WRITE_FAIL };
        for (size_t index = 0U; index < sizeof(failures) / sizeof(failures[0]); ++index) {
            reset_app(app, failures[index]);
            media_source_dirty = false; /* Test the second project publication directly. */
            storage_error_dialogs = 0U;
            state.active = true;
            state.layout.panel = (struct ui_rect){ 0U, 0U, 200U, 200U };
            struct ui_rect damage = { 0U, 0U, 0U, 0U };
            assert(application_storage_action(action, &damage) == UI_STATUS_OK);
            assert(state.active && storage_error_dialogs == 1U && damage.width != 0U);
            assert(live_handles == 0U && scratch_inode == 0U && !media_editor_export_active);
            assert(target_length == 3U && memcmp(target_bytes, "old", 3U) == 0);
            if (app == 1U) assert(media_editor_dirty);
            if (app == 2U) assert(!paint_saved);
        }
    }
    reset_app(2U, SAVE_OK);
    storage_error_dialogs = 0U;
    struct ui_rect damage = { 0U, 0U, 0U, 0U };
    assert(application_storage_action(APPLICATION_PAINT_SAVE, &damage) == UI_STATUS_OK);
    assert(paint_saved && storage_error_dialogs == 0U && live_handles == 0U);
    puts("Notes, Paint and Media ENOSPC/IO failures retain documents, close ownership and keep the desktop active: PASS");
}

static void settings_persistence(void)
{
    static const uint8_t initial[16] = { 'P', 'H', 'I', 'P', 'C', 'F', 'G', 1U, 3U, 1U, 0U, 1U, 1U };
    reset_save(SAVE_OK);
    target_length = sizeof(initial);
    memcpy(target_bytes, initial, sizeof(initial));
    target_bytes[9] = 0U; /* Persisted show-desktop differs from the boot default. */
    data_admitted = false;
    phipia_shell_ready = true;
    applied_settings = 0U;
    settings_restore_damage = false;
    phipia_seed_settings(); /* Desktop comes up before Data is mounted. */
    assert(setting_values[0][2] == 1U);
    ui_restore_storage_settings();
    assert(applied_settings == 0U && !settings_restore_damage);
    data_admitted = true;
    ui_restore_storage_settings();
    assert(setting_values[0][2] == 0U && !restored_show_desktop);
    assert(persisted_settings[1] == 0U && applied_settings == 6U && settings_restore_damage);
    assert(live_handles == 0U && writes == 0U && publications == 0U && target_bytes[9] == 0U);
    settings_read_fails = true;
    ui_restore_storage_settings();
    assert(setting_values[0][2] == 1U && restored_show_desktop);
    assert(live_handles == 0U && writes == 0U && target_bytes[9] == 0U);
    phipia_shell_ready = false;
    applied_settings = 0U;
    ui_restore_storage_settings();
    assert(applied_settings == 0U);
    const enum save_fault failures[] = { SAVE_OK, WRITE_FAIL, FIRST_SYNC_FAIL, PUBLISH_FAIL, PUBLISH_LOST, SECOND_SYNC_FAIL, CLOSE_FAIL };
    for (size_t failure = 0U; failure < sizeof(failures) / sizeof(failures[0]); ++failure) {
        reset_save(failures[failure]);
        target_length = sizeof(initial);
        memcpy(target_bytes, initial, sizeof(initial));
        phipia_seed_settings();
        assert(live_handles == 0U && opens == 0U && writes == 0U);
        assert(setting_values[0][1] == 3U && setting_values[1][1] == 0U);
        expected_destination = "SETTINGS.PHI";
        expected_scratch = "STTMP0.PHI";
        expected_length = sizeof(initial);
        memcpy(expected_bytes, initial, sizeof(initial));
        expected_bytes[8] = 1U;
        expected_bytes[10] = 1U;
        setting_values[0][1] = 1U;
        setting_values[1][1] = 1U;
        const bool success = fault == SAVE_OK || fault == PUBLISH_LOST;
        assert((phipia_settings_save() == PHIPFS_STATUS_OK) == success);
        assert(live_handles == 0U);
        assert(persisted_settings[0] == (success ? 1U : 3U));
        if (success) {
            const unsigned before = opens;
            assert(phipia_settings_save() == PHIPFS_STATUS_OK && opens == before);
            assert(target_length == sizeof(initial) && memcmp(target_bytes, expected_bytes, sizeof(initial)) == 0);
            memset(setting_values, 0, sizeof(setting_values));
            phipia_seed_settings(); /* Simulated UI reinitialization uses only disk state. */
            assert(setting_values[0][1] == 1U && setting_values[1][1] == 1U);
        }
    }
    for (unsigned corruption = 0U; corruption < 6U; ++corruption) {
        reset_save(SAVE_OK);
        target_length = sizeof(initial);
        memcpy(target_bytes, initial, sizeof(initial));
        if (corruption == 0U) target_length = 15U;
        if (corruption == 1U) target_length = 17U;
        if (corruption == 2U) target_bytes[7] = 2U;
        if (corruption == 3U) target_bytes[8] = 4U;
        if (corruption == 4U) target_bytes[12] = 2U;
        if (corruption == 5U) target_bytes[15] = 1U;
        uint8_t decoded[5] = { 9U, 9U, 9U, 9U, 9U };
        assert(phipia_settings_read(decoded) == PHIPFS_STATUS_CORRUPT);
        for (size_t index = 0U; index < sizeof(decoded); ++index) assert(decoded[index] == 9U);
        assert(live_handles == 0U && writes == 0U);
    }
    for (unsigned failure = 0U; failure < 3U; ++failure) {
        reset_save(SAVE_OK);
        target_length = sizeof(initial);
        memcpy(target_bytes, initial, sizeof(initial));
        settings_read_fails = failure == 0U;
        settings_close_fails = failure == 1U;
        if (failure == 2U) target_inode = 0U;
        uint8_t decoded[5] = { 9U, 9U, 9U, 9U, 9U };
        assert(phipia_settings_read(decoded) == (failure == 2U ? PHIPFS_STATUS_NOT_FOUND : PHIPFS_STATUS_IO));
        for (size_t index = 0U; index < sizeof(decoded); ++index) assert(decoded[index] == 9U);
        assert(live_handles == 0U && writes == 0U);
    }
}

int main(void)
{
    storage_errors_keep_application_open();
    for (unsigned failure = 0U; failure <= 6U; ++failure) {
        if (failure == 5U) continue; /* Copy's second-fstat growth case is separate. */
        reset_large_bitmap(3U);
        copy_failure = failure;
        const enum phipfs_status status = media_source_load_preview("SOURCE.BMP");
        assert((status == PHIPFS_STATUS_OK) == (failure == 0U));
        assert(media_source_preview_loaded == (failure == 0U));
        assert(!copy_source_live && live_handles == 0U && writes == 0U && publications == 0U);
        if (failure == 0U) {
            assert(copy_stats == 1U && copy_reads == 4U && media_source_preview_width == 320U && media_source_preview_height == 180U);
            for (unsigned y = 0U; y < 180U; ++y)
                for (unsigned x = 0U; x < 320U; ++x)
                    assert(media_source_preview_pixels[y * UI_MEDIA_SOURCE_PREVIEW_WIDTH + x] ==
                        (uint32_t)(y % 251U + 1U) * UINT32_C(0x010101));
        }
    }
    reset_large_bitmap(3U);
    const uint32_t negative_height = (uint32_t)-180;
    for (unsigned byte = 0U; byte < 4U; ++byte) expected_bytes[22U + byte] = (uint8_t)(negative_height >> (8U * byte));
    assert(media_source_load_preview("SOURCE.BMP") == PHIPFS_STATUS_OK && copy_reads == 4U);
    assert(!copy_source_live && live_handles == 0U && copy_position == 54U);
    for (unsigned y = 0U; y < 180U; ++y)
        for (unsigned x = 0U; x < 320U; ++x)
            assert(media_source_preview_pixels[y * UI_MEDIA_SOURCE_PREVIEW_WIDTH + x] ==
                (uint32_t)((179U - y) % 251U + 1U) * UINT32_C(0x010101));
    for (unsigned failure = 0U; failure < 8U; ++failure) {
        reset_save(SAVE_OK);
        copy_failure = failure;
        expected_destination = "COPY.BIN";
        expected_scratch = "CPTMP0.TMP";
        expected_length = failure == 7U ? 0U : 2U * 65536U + 8192U - 3U;
        for (size_t index = 0U; index < expected_length; ++index) expected_bytes[index] = (uint8_t)(index * 17U);
        const enum phipfs_status status = explorer_copy_file("SOURCE.BIN", expected_destination);
        const bool succeeds = failure == 0U || failure == 7U;
        if ((status == PHIPFS_STATUS_OK) != succeeds)
            fprintf(stderr, "copy case %u status %u reads %u stats %u writes %u publications %u\n",
                failure, (unsigned)status, copy_reads, copy_stats, writes, publications);
        assert((status == PHIPFS_STATUS_OK) == succeeds);
        assert(!copy_source_live && live_handles == 0U && scratch_inode == 0U);
        if (succeeds) {
            assert(publications == 1U && target_length == expected_length && copy_reads == (failure == 7U ? 0U : 3U));
            assert(memcmp(target_bytes, expected_bytes, expected_length) == 0);
        } else assert(publications == 0U && target_length == 3U && memcmp(target_bytes, "old", 3U) == 0);
    }
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
    for (unsigned app = 0U; app < 4U; ++app) {
        const enum save_fault app_faults[] = { SAVE_OK, PUBLISH_LOST, CREATE_LOST, WRITE_FAIL,
            FIRST_SYNC_FAIL, PUBLISH_FAIL, SOURCE_REPLACED, SECOND_SYNC_FAIL, CLOSE_FAIL };
        for (size_t index = 0U; index < sizeof(app_faults) / sizeof(app_faults[0]); ++index) {
            reset_app(app, app_faults[index]);
            const bool success = fault == SAVE_OK || fault == PUBLISH_LOST;
            assert((save_app(app) == PHIPFS_STATUS_OK) == success);
            assert(live_handles == 0U);
            if (app < 3U)
                assert((app == 0U ? !media_source_dirty : app == 1U ? !media_editor_dirty : paint_saved) == success);
            if (success) {
                assert(target_inode == 20U && scratch_inode == 0U && target_length == expected_length);
                assert(memcmp(target_bytes, expected_bytes, expected_length) == 0);
            }
        }
    }
    for (unsigned app = 2U; app <= 3U; ++app) {
        reset_large_bitmap(app);
        assert(save_app(app) == PHIPFS_STATUS_OK && writes == 3U);
        assert(target_length == expected_length && memcmp(target_bytes, expected_bytes, expected_length) == 0);
        assert(publications == 1U && scratch_inode == 0U && live_handles == 0U);
        for (unsigned boundary = 1U; boundary <= 3U; ++boundary) {
            for (unsigned short_write = 0U; short_write <= 1U; ++short_write) {
                reset_large_bitmap(app);
                if (short_write) short_write_at = boundary;
                else fail_write_at = boundary;
                assert(save_app(app) == (short_write ? PHIPFS_STATUS_WRITEBACK : PHIPFS_STATUS_IO));
                assert(!paint_saved && publications == 0U && closes == 1U && target_inode == 10U);
                assert(scratch_inode == 0U && live_handles == 0U && target_length == 3U);
                assert(memcmp(target_bytes, "old", 3U) == 0);
            }
        }
    }
    reset_app(3U, SAVE_OK);
    media_editor_export_active = true; /* Fail sampling after writing the BMP header. */
    assert(media_source_export() == PHIPFS_STATUS_IO && publications == 0U && live_handles == 0U);
    assert(target_inode == 10U && scratch_inode == 0U);
    media_editor_export_active = false;
    reset_large_bitmap(2U);
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
    settings_persistence();
    puts("Notes, Paint, Media, Settings ext4 save: ownership, complete publication, failure handling PASS");
    return 0;
}
