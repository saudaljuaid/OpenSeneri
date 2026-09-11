/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <phipia/native_handle.h>
#include "../src/kernel/vfs.c"

static bool host_interrupts_enabled = true;
bool cpu_interrupts_enabled(void) { return host_interrupts_enabled; }
void cpu_interrupt_disable(void) { host_interrupts_enabled = false; }
void cpu_interrupt_enable(void) { host_interrupts_enabled = true; }

static unsigned calls;
static enum native_resource_close_result next_result;
static enum native_resource_close_result close_file(uint8_t type,
    const struct native_resource *resource, void *context)
{
    assert(type == PHIPIA_HANDLE_FILE && resource->words[0] == 77U && context == &calls);
    ++calls;
    return next_result;
}

static unsigned directory_close_calls;
static enum phipfs_status directory_close_status;

static enum phipfs_status directory_stat(enum phipfs_volume volume,
    const char *path, struct phipfs_stat *stat)
{
    assert(volume == PHIPFS_VOLUME_DATA && path != NULL && stat != NULL);
    assert(host_interrupts_enabled);
    *stat = (struct phipfs_stat){.object_id = 101U, .directory = true};
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status directory_open(enum phipfs_volume volume,
    const char *path, phipfs_handle *handle)
{
    assert(volume == PHIPFS_VOLUME_DATA && path != NULL && handle != NULL);
    assert(host_interrupts_enabled);
    *handle = 91U;
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status directory_read(phipfs_handle handle,
    struct phipfs_list_entry *entry, bool *present)
{
    assert(handle == 91U && entry != NULL && present != NULL);
    *present = false;
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status directory_close(phipfs_handle handle)
{
    assert(handle == 91U);
    ++directory_close_calls;
    return directory_close_status;
}

struct native_directory_state {
    phipfs_directory_handle iterator;
    bool active;
};

static enum native_resource_close_result close_directory(uint8_t type,
    const struct native_resource *resource, void *context)
{
    struct native_directory_state *state = context;
    bool consumed = false;
    const enum phipfs_status status = phipfs_directory_close_report(
        state->iterator, &consumed);

    assert(type == PHIPIA_HANDLE_DIRECTORY && resource->words[0] == 0U);
    if (consumed) state->active = false;
    return status == PHIPFS_STATUS_OK ? NATIVE_RESOURCE_CLOSED :
        consumed ? NATIVE_RESOURCE_CLOSED_WITH_ERROR : NATIVE_RESOURCE_RETAINED;
}

int main(void)
{
    struct native_handle_table table;
    const struct native_resource resource = {{77U, 0U, 0U, 0U}};
    struct native_resource *resolved;
    phipia_handle_t first, duplicate, reused;
    assert(native_handle_table_initialize(&table, 2U) == NATIVE_HANDLE_OK);
    assert(native_handle_install(&table, PHIPIA_HANDLE_FILE, &resource, &first) == NATIVE_HANDLE_OK);
    assert(native_handle_duplicate(&table, first, &duplicate) == NATIVE_HANDLE_OK);
    next_result = NATIVE_RESOURCE_CLOSED_WITH_ERROR;
    assert(native_handle_close(&table, first, close_file, &calls) == NATIVE_HANDLE_OK);
    assert(calls == 0U && table.active_handles == 1U && table.active_objects == 1U);
    // The VFS identity is already consumed. Return its error and retire the
    // native wrapper; neither close nor close_all may call the backend again.
    assert(native_handle_close(&table, duplicate, close_file, &calls) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(calls == 1U && table.active_handles == 0U && table.active_objects == 0U);
    assert(native_handle_resolve(&table, duplicate, PHIPIA_HANDLE_FILE, &resolved) == NATIVE_HANDLE_STALE);
    assert(native_handle_close_all(&table, close_file, &calls) == NATIVE_HANDLE_OK && calls == 1U);
    assert(native_handle_install(&table, PHIPIA_HANDLE_FILE, &resource, &reused) == NATIVE_HANDLE_OK);
    assert(reused != first && reused != duplicate);
    // A refusal before consumption retains exactly one retryable reference.
    next_result = NATIVE_RESOURCE_RETAINED;
    assert(native_handle_close(&table, reused, close_file, &calls) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(table.active_handles == 1U && table.active_objects == 1U);
    assert(native_handle_resolve(&table, reused, PHIPIA_HANDLE_FILE, &resolved) == NATIVE_HANDLE_OK);
    next_result = NATIVE_RESOURCE_CLOSED;
    assert(native_handle_close(&table, reused, close_file, &calls) == NATIVE_HANDLE_OK);
    assert(calls == 3U && table.active_handles == 0U && table.active_objects == 0U);
    assert(native_handle_install(&table, PHIPIA_HANDLE_FILE, &resource, &first) == NATIVE_HANDLE_OK);
    assert(native_handle_install(&table, PHIPIA_HANDLE_FILE, &resource, &duplicate) == NATIVE_HANDLE_OK);
    next_result = NATIVE_RESOURCE_CLOSED_WITH_ERROR;
    assert(native_handle_close_all(&table, close_file, &calls) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(calls == 5U && table.active_handles == 0U && table.active_objects == 0U);
    assert(native_handle_close_all(&table, close_file, &calls) == NATIVE_HANDLE_OK && calls == 5U);

    static const struct vfs_backend_ops directory_backend = {
        .stat_path = directory_stat,
        .directory_open = directory_open,
        .directory_read = directory_read,
        .directory_close = directory_close,
        .case_sensitive = true,
    };
    mounts[PHIPFS_VOLUME_DATA].active = true;
    mounts[PHIPFS_VOLUME_DATA].generation = 1U;
    mounts[PHIPFS_VOLUME_DATA].backend = &directory_backend;
    for (size_t index = 0U; index < VFS_VNODE_BUCKETS; ++index) {
        vnode_buckets[index] = VFS_NO_INDEX;
    }
    phipfs_directory_handle iterator;
    assert(phipfs_directory_open(PHIPFS_VOLUME_DATA, "root", &iterator) ==
        PHIPFS_STATUS_OK);
    struct native_directory_state directory_state = {iterator, true};
    struct native_handle_table directory_table;
    const struct native_resource directory_resource = {{0U, 0U, 0U, 0U}};
    phipia_handle_t directory_handle, directory_duplicate;
    assert(native_handle_table_initialize(&directory_table, 2U) == NATIVE_HANDLE_OK);
    assert(native_handle_install(&directory_table, PHIPIA_HANDLE_DIRECTORY,
        &directory_resource, &directory_handle) == NATIVE_HANDLE_OK);
    assert(native_handle_duplicate(&directory_table, directory_handle,
        &directory_duplicate) == NATIVE_HANDLE_OK);
    directory_close_status = PHIPFS_STATUS_IO;
    assert(native_handle_close(&directory_table, directory_handle,
        close_directory, &directory_state) == NATIVE_HANDLE_OK);
    assert(directory_close_calls == 0U && directory_state.active &&
        directory_table.active_handles == 1U);
    assert(native_handle_close(&directory_table, directory_duplicate,
        close_directory, &directory_state) == NATIVE_HANDLE_CLOSE_FAILED);
    assert(directory_close_calls == 1U && !directory_state.active &&
        directory_table.active_handles == 0U && directory_table.active_objects == 0U);
    bool consumed = true;
    assert(phipfs_directory_close_report(iterator, &consumed) ==
        PHIPFS_STATUS_STALE_HANDLE && !consumed);
    assert(native_handle_close_all(&directory_table, close_directory,
        &directory_state) == NATIVE_HANDLE_OK && directory_close_calls == 1U);
    mounts[PHIPFS_VOLUME_DATA].active = false;
    mounts[PHIPFS_VOLUME_DATA].backend = NULL;
    assert(vnode_resources_released());
    puts("native handle close: consumed I/O errors retire wrappers, pre-consumption refusals retry, duplicate references and shutdown census PASS");
    return 0;
}
