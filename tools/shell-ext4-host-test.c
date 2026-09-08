/* SPDX-License-Identifier: GPL-3.0-only */
/* Production terminal writes preserve each backend's truncation boundary. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/shell.c"

static bool ext4, live, exists, appended;
static unsigned opened_count, closed_count, truncated_count, sync_count;
static enum phipfs_status truncate_result, write_result;
static char bytes[128];
static size_t length;
static bool interrupts_enabled, queued_keyboard, queued_ui, inject_on_disable;
static unsigned halts;

void cpu_interrupt_disable(void)
{ assert(interrupts_enabled); if (inject_on_disable) queued_keyboard = true; interrupts_enabled = false; }
void cpu_interrupt_enable(void) { assert(!interrupts_enabled); interrupts_enabled = true; }
void cpu_enable_and_halt(void) { assert(!interrupts_enabled); ++halts; interrupts_enabled = true; }
bool keyboard_events_pending(void) { assert(!interrupts_enabled); return queued_keyboard; }
bool ui_events_pending(void) { assert(!interrupts_enabled); return queued_ui; }

bool phipfs_has_atomic_replace(enum phipfs_volume volume)
{ assert(volume == PHIPFS_VOLUME_DATA); return ext4; }
const char *phipfs_status_string(enum phipfs_status status)
{ (void)status; return "failure"; }
void console_write(const char *text) { (void)text; }
void console_putc(char character) { (void)character; }
enum phipfs_status phipfs_stat_path(enum phipfs_volume volume, const char *path,
    struct phipfs_stat *result)
{ (void)result; assert(volume == PHIPFS_VOLUME_DATA && strcmp(path, "notes.txt") == 0);
  return exists ? PHIPFS_STATUS_OK : PHIPFS_STATUS_NOT_FOUND; }
enum phipfs_status phipfs_create(enum phipfs_volume volume, const char *path)
{ (void)volume; (void)path; assert(!exists); exists = true; return PHIPFS_STATUS_OK; }
enum phipfs_status phipfs_open(enum phipfs_volume volume, const char *path,
    enum phipfs_access access, phipfs_handle *handle)
{ (void)volume; (void)path; assert(exists && !live && access == PHIPFS_ACCESS_WRITE);
  live = true; appended = false; ++opened_count; *handle = 77U; return PHIPFS_STATUS_OK; }
static enum phipfs_status truncate_bytes(void)
{ ++truncated_count; if (truncate_result == PHIPFS_STATUS_OK) length = 0U;
  return truncate_result; }
enum phipfs_status phipfs_truncate(enum phipfs_volume volume, const char *path, uint64_t size)
{ (void)volume; (void)path; assert(!ext4 && !live && size == 0U); return truncate_bytes(); }
enum phipfs_status phipfs_ftruncate(phipfs_handle handle, uint64_t size)
{ assert(ext4 && live && handle == 77U && size == 0U); return truncate_bytes(); }
enum phipfs_status phipfs_set_append(phipfs_handle handle, bool append)
{ assert(live && handle == 77U && append); appended = true; return PHIPFS_STATUS_OK; }
enum phipfs_status phipfs_write(phipfs_handle handle, const uint8_t *buffer, size_t count, size_t *written)
{ assert(live && handle == 77U); *written = 0U;
  if (write_result != PHIPFS_STATUS_OK) return write_result;
  size_t offset = appended ? length : 0U; assert(offset + count < sizeof(bytes));
  memcpy(bytes + offset, buffer, count); length = offset + count; *written = count;
  return PHIPFS_STATUS_OK; }
enum phipfs_status phipfs_fsync(phipfs_handle handle)
{ assert(live && handle == 77U); ++sync_count; return PHIPFS_STATUS_OK; }
enum phipfs_status phipfs_close(phipfs_handle handle)
{ assert(live && handle == 77U); live = false; ++closed_count; return PHIPFS_STATUS_OK; }

int main(void)
{
    interrupts_enabled = true;
    shell_idle_if_no_input(true);
    assert(halts == 1U && interrupts_enabled);
    inject_on_disable = true; /* Input arrives after the main loop's last drain. */
    shell_idle_if_no_input(true);
    assert(halts == 1U && interrupts_enabled && queued_keyboard);
    inject_on_disable = queued_keyboard = false;
    queued_ui = true; /* Pointer or application event arrived during storage work. */
    shell_idle_if_no_input(true);
    assert(halts == 1U && interrupts_enabled);
    shell_idle_if_no_input(false); /* Disabled UI must not prevent terminal idle. */
    assert(halts == 2U && interrupts_enabled);
    queued_ui = false;
    puts("queued keyboard and application input after storage work prevents lost wakeup: PASS");
    strcpy(filesystem_cwd, ".");
    for (unsigned backend = 0U; backend < 2U; ++backend) {
        ext4 = backend != 0U; exists = false; length = 0U;
        truncate_result = write_result = PHIPFS_STATUS_OK;
        opened_count = closed_count = truncated_count = sync_count = 0U;
        command_write_line("notes.txt \"first cut\"", false);
        command_write_line("notes.txt \"second line\"", true);
        assert(length == 22U && memcmp(bytes, "first cut\nsecond line\n", length) == 0);
        assert(!live && opened_count == 2U && closed_count == 2U && truncated_count == 1U && sync_count == 2U);
        command_write_line("notes.txt \"short\"", false);
        assert(length == 6U && memcmp(bytes, "short\n", length) == 0);
        truncate_result = PHIPFS_STATUS_IO;
        const unsigned before = opened_count;
        command_write_line("notes.txt \"refused\"", false);
        assert(!live && length == 6U && opened_count == before + (ext4 ? 1U : 0U));
        assert(opened_count == closed_count && sync_count == 3U);
        truncate_result = PHIPFS_STATUS_OK; write_result = PHIPFS_STATUS_IO;
        command_write_line("notes.txt \"refused\"", true);
        assert(!live && length == 6U && opened_count == closed_count && sync_count == 3U);
    }
    puts("terminal ext4/FAT32 write, append, truncate and failure cleanup: PASS");
    return 0;
}
