/* SPDX-License-Identifier: GPL-3.0-only */
/* The production VFS must let a hidden journal transaction reach its retry. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/vfs.c"

static bool host_interrupts_enabled = true;
bool cpu_interrupts_enabled(void) { return host_interrupts_enabled; }
void cpu_interrupt_disable(void) { host_interrupts_enabled = false; }
void cpu_interrupt_enable(void)
{
    assert(!__atomic_load_n(&vnode_metadata_owned, __ATOMIC_RELAXED));
    host_interrupts_enabled = true;
}

static unsigned int calls;
static unsigned int stats;
static enum phipfs_status mutation_result = PHIPFS_STATUS_IO;
static const char *expected_path = "parent/file";
static bool stat_succeeds;
static unsigned live_backend_handles;
static bool directory_metadata;
static phipfs_handle closing_frontend;
static bool closing_directory;
static uint16_t requested_directory_mode;
static uint16_t expected_file_mode = 0644U;
static unsigned prepared_calls;
static uint8_t expected_prepared_flags = PHIPFS_OPEN_CREATE | PHIPFS_OPEN_TRUNCATE;
static unsigned file_sync_calls;
static unsigned file_stat_calls;
static unsigned publication_calls;
static unsigned held_unlink_calls;
static bool consume_last_vnode;
static unsigned unmount_calls;
static unsigned mount_calls;

static enum phipfs_status reentrant_mount(enum phipfs_volume volume)
{
    assert(volume == PHIPFS_VOLUME_DATA && !mounts[volume].active && mounts[volume].mounting);
    assert(!phipfs_resources_released());
    const unsigned before = ++mount_calls;
    phipfs_handle file = 99U;
    assert(phipfs_open(volume, "new-file", PHIPFS_ACCESS_READ, &file) == PHIPFS_STATUS_NOT_MOUNTED && file == 0U);
    assert(phipfs_mount(volume) == PHIPFS_STATUS_BUSY && mount_calls == before);
    assert(phipfs_unmount(volume) == PHIPFS_STATUS_BUSY);
    return mutation_result;
}

static struct phipfs_drive_info mounted_drive(enum phipfs_volume volume)
{
    assert(volume == PHIPFS_VOLUME_DATA && mounts[volume].mounting && !mounts[volume].active);
    assert(phipfs_mount(volume) == PHIPFS_STATUS_BUSY && phipfs_unmount(volume) == PHIPFS_STATUS_BUSY);
    return (struct phipfs_drive_info){ .mounted = true };
}

static enum phipfs_status reentrant_unmount(enum phipfs_volume volume)
{
    assert(volume == PHIPFS_VOLUME_DATA && !mounts[volume].active && mounts[volume].unmounting);
    assert(!phipfs_resources_released());
    const unsigned before = ++unmount_calls;
    phipfs_handle file = 99U;
    phipfs_directory_handle directory = 99U;
    assert(phipfs_open(volume, "new-file", PHIPFS_ACCESS_READ, &file) == PHIPFS_STATUS_NOT_MOUNTED && file == 0U);
    assert(phipfs_directory_open(volume, ".", &directory) == PHIPFS_STATUS_NOT_MOUNTED && directory == 0U);
    assert(phipfs_mount(volume) == PHIPFS_STATUS_BUSY);
    assert(phipfs_unmount(volume) == PHIPFS_STATUS_BUSY && unmount_calls == before);
    assert(phipfs_sync(volume) == PHIPFS_STATUS_NOT_MOUNTED);
    assert(mounts[volume].references == 0U);
    return mutation_result;
}

static enum phipfs_status partial_write(phipfs_handle handle, const uint8_t *source,
    size_t bytes, size_t *written)
{
    assert(handle == 77U && source != NULL && bytes == 4U && *written == 0U);
    *written = 2U; /* A committed prefix must remain visible on a later failure. */
    return PHIPFS_STATUS_IO;
}

static void stale_io_counts(phipfs_handle handle)
{
    uint8_t bytes[4] = { 1U, 2U, 3U, 4U };
    size_t count = 99U;
    assert(phipfs_read(handle, bytes, sizeof(bytes), &count) == PHIPFS_STATUS_STALE_HANDLE && count == 0U);
    count = 99U;
    assert(phipfs_pread(handle, bytes, sizeof(bytes), 1U, &count) == PHIPFS_STATUS_STALE_HANDLE && count == 0U);
    count = 99U;
    assert(phipfs_write(handle, bytes, sizeof(bytes), &count) == PHIPFS_STATUS_STALE_HANDLE && count == 0U);
    assert(bytes[0] == 1U && bytes[1] == 2U && bytes[2] == 3U && bytes[3] == 4U);
}

static enum phipfs_status read_attribute(enum phipfs_volume volume, const char *path,
    const char *name, uint8_t *output, size_t capacity, size_t *length)
{
    assert(volume == PHIPFS_VOLUME_DATA && strcmp(path, "parent/file") == 0);
    assert(strcmp(name, "user.test") == 0 && output != NULL && capacity == 4U);
    *length = 3U;
    return mutation_result;
}

static enum phipfs_status held_unlink(phipfs_handle handle, const char *path)
{
    assert(handle == 77U && live_backend_handles == 1U && strcmp(path, expected_path) == 0);
    ++held_unlink_calls;
    return mutation_result;
}

static enum phipfs_status publish_file(phipfs_handle handle, const char *source, const char *destination)
{
    assert(handle == 77U && live_backend_handles == 1U);
    assert(strcmp(source, expected_path) == 0 && strcmp(destination, "other/target") == 0);
    ++publication_calls;
    return mutation_result;
}

static enum phipfs_status file_stat(phipfs_handle handle, struct phipfs_stat *result)
{
    assert(handle == 77U && live_backend_handles == 1U);
    ++file_stat_calls;
    if (mutation_result == PHIPFS_STATUS_OK) {
        result->object_id = 500U;
        result->size = 777U;
        result->mode = 0100600U;
    }
    return mutation_result;
}

static enum phipfs_status file_sync(phipfs_handle handle)
{
    assert(handle == 77U && live_backend_handles == 1U);
    ++file_sync_calls;
    return mutation_result;
}

static enum phipfs_status prepared_open(enum phipfs_volume volume, const char *path,
    enum phipfs_access access, uint8_t flags, uint16_t mode,
    phipfs_handle *handle, struct phipfs_stat *result)
{
    assert(volume == PHIPFS_VOLUME_DATA && strcmp(path, expected_path) == 0);
    assert(access == PHIPFS_ACCESS_READ_WRITE && flags == expected_prepared_flags);
    assert(mode == 01720U);
    ++prepared_calls;
    if (consume_last_vnode) {
        struct phipfs_stat other = { .object_id = 99999U };
        size_t index;
        assert(vnode_retain(volume, "callback", &other, &index) == PHIPFS_STATUS_NO_HANDLES);
    }
    if (mutation_result != PHIPFS_STATUS_OK) return mutation_result;
    *handle = 77U;
    memset(result, 0, sizeof(*result));
    result->object_id = 500U;
    ++live_backend_handles;
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status replaced_open(enum phipfs_volume volume, const char *path,
    enum phipfs_access access, phipfs_handle *handle, struct phipfs_stat *stat)
{
    assert(volume == PHIPFS_VOLUME_DATA && strcmp(path, expected_path) == 0);
    assert(access == PHIPFS_ACCESS_READ);
    *handle = 77U;
    memset(stat, 0, sizeof(*stat));
    stat->object_id = 500U; /* The name was replaced after the preliminary stat. */
    ++live_backend_handles;
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status replaced_close(phipfs_handle handle)
{
    assert(handle == 77U && live_backend_handles == 1U);
    for (size_t index = 0U; index < VFS_MAX_VNODES; ++index) {
        assert(!vnodes[index].active || vnodes[index].stat.object_id != 500U);
    }
    if (closing_frontend != 0U) {
        const phipfs_handle retired = closing_frontend;
        closing_frontend = 0U;
        assert((closing_directory ? phipfs_directory_close(retired) : phipfs_close(retired)) == PHIPFS_STATUS_STALE_HANDLE);
    }
    --live_backend_handles;
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status replaced_directory_open(enum phipfs_volume volume, const char *path,
    phipfs_handle *handle, struct phipfs_stat *stat)
{
    const enum phipfs_status status = replaced_open(volume, path, PHIPFS_ACCESS_READ, handle, stat);
    stat->directory = true;
    return status;
}

static enum phipfs_status replaced_directory_read(phipfs_handle handle,
    struct phipfs_list_entry *entry, bool *present)
{
    assert(handle == 77U && live_backend_handles == 1U && entry != NULL);
    *present = false;
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status hidden_stat(enum phipfs_volume volume,
    const char *path, struct phipfs_stat *result)
{
    (void)volume;
    ++stats;
    if (stat_succeeds) {
        assert(strcmp(path, expected_path) == 0);
        memset(result, 0, sizeof(*result));
        result->object_id = 101U;
        result->directory = directory_metadata;
        return PHIPFS_STATUS_OK;
    }
    return PHIPFS_STATUS_IO;
}

static enum phipfs_status mutation(enum phipfs_volume volume, const char *path)
{
    assert(volume == PHIPFS_VOLUME_DATA && strcmp(path, expected_path) == 0);
    ++calls;
    return mutation_result;
}

static enum phipfs_status nofollow_stat(enum phipfs_volume volume, const char *path,
    struct phipfs_stat *result)
{
    const enum phipfs_status status = mutation(volume, path);
    if (status == PHIPFS_STATUS_OK) {
        result->object_id = 84U;
        result->mode = 0120777U;
        result->uid = 70000U;
        result->atime_seconds = -1;
        result->atime_nanos = 123U;
    }
    return status;
}

static enum phipfs_status pair(enum phipfs_volume volume, const char *from, const char *to)
{
    assert(strcmp(to, "other/target") == 0);
    return mutation(volume, from);
}

static enum phipfs_status create(enum phipfs_volume volume, const char *path, uint16_t mode)
{
    assert(mode == expected_file_mode);
    return mutation(volume, path);
}

static enum phipfs_status mkdir_mode(enum phipfs_volume volume, const char *path, uint16_t mode)
{
    requested_directory_mode = mode;
    return mutation(volume, path);
}

static enum phipfs_status truncate_file(enum phipfs_volume volume, const char *path, uint64_t size)
{
    assert(size == 7U);
    return mutation(volume, path);
}

static bool nested_open;
static bool fail_outer_open;
static bool opening_directory;
static phipfs_handle nested_handle;
static unsigned nested_live;

static enum phipfs_status nested_backend_open(enum phipfs_volume volume, const char *path,
    enum phipfs_access access, uint8_t flags, uint16_t mode,
    phipfs_handle *handle, struct phipfs_stat *result)
{
    (void)path; (void)access; (void)flags; (void)mode;
    const bool outer = !nested_open;
    if (outer) {
        nested_open = true;
        assert(phipfs_unmount(volume) == PHIPFS_STATUS_BUSY);
        assert((opening_directory ? phipfs_directory_open(volume, expected_path, &nested_handle) :
            phipfs_open(volume, expected_path, PHIPFS_ACCESS_READ, &nested_handle)) == PHIPFS_STATUS_OK);
        if (fail_outer_open) return PHIPFS_STATUS_IO;
    }
    *handle = outer ? 81U : 82U;
    memset(result, 0, sizeof(*result));
    result->object_id = *handle;
    result->directory = opening_directory;
    ++nested_live;
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status nested_directory_open(enum phipfs_volume volume, const char *path,
    phipfs_handle *handle, struct phipfs_stat *result)
{
    return nested_backend_open(volume, path, PHIPFS_ACCESS_READ, 0U, 0U, handle, result);
}

static enum phipfs_status nested_backend_close(phipfs_handle handle)
{
    assert((handle == 81U || handle == 82U) && nested_live != 0U);
    --nested_live;
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status unexpected_unmount(enum phipfs_volume volume)
{
    (void)volume;
    return PHIPFS_STATUS_IO;
}

static void nested_open_reservations(void)
{
    static const struct vfs_backend_ops backend = {
        .open_options = nested_backend_open, .close = nested_backend_close,
        .unmount = unexpected_unmount,
        .stat_path = hidden_stat, .case_sensitive = true, .validates_mutation_paths = true,
        .directory_open_with_stat = nested_directory_open,
        .directory_read = replaced_directory_read, .directory_close = nested_backend_close,
    };
    mounts[PHIPFS_VOLUME_DATA].backend = &backend;
    stat_succeeds = true;
    for (unsigned kind = 0U; kind < 2U; ++kind) {
        opening_directory = directory_metadata = kind != 0U;
        for (unsigned failure = 0U; failure < 2U; ++failure) {
            nested_open = false;
            fail_outer_open = failure != 0U;
            phipfs_handle outer = 0U;
            const enum phipfs_status expected = failure != 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
            assert((opening_directory ? phipfs_directory_open(PHIPFS_VOLUME_DATA, expected_path, &outer) :
                phipfs_open(PHIPFS_VOLUME_DATA, expected_path, PHIPFS_ACCESS_READ, &outer)) == expected);
            if (failure == 0U) {
                assert((outer & 0xffU) != (nested_handle & 0xffU));
                assert((opening_directory ? phipfs_directory_close(outer) : phipfs_close(outer)) == PHIPFS_STATUS_OK);
            } else assert(outer == 0U);
            assert((opening_directory ? phipfs_directory_close(nested_handle) : phipfs_close(nested_handle)) == PHIPFS_STATUS_OK);
            assert(nested_live == 0U && mounts[PHIPFS_VOLUME_DATA].references == 0U);
            for (size_t index = 0U; index < VFS_MAX_VNODES; ++index) assert(!vnodes[index].active);
            for (size_t index = 0U; index < VFS_MAX_OPEN_FILES; ++index)
                assert(!open_files[index].active && !open_files[index].opening && !open_file_claims[index]);
            for (size_t index = 0U; index < VFS_MAX_DIRECTORY_ITERATORS; ++index)
                assert(!directories[index].active && !directories[index].opening && !directory_claims[index]);
        }
    }
}

int main(void)
{
    for (size_t index = 0U; index < VFS_VNODE_BUCKETS; ++index) vnode_buckets[index] = VFS_NO_INDEX;
    struct vfs_backend_ops backend = {
        .stat_path = hidden_stat, .create = create, .truncate = truncate_file,
        .mkdir = mutation, .unlink = mutation, .rmdir = mutation,
        .link = pair, .rename = pair, .rename_replace = pair,
        .case_sensitive = true, .validates_mutation_paths = true,
        .remove = mutation, .chmod = create,
    };
    mounts[PHIPFS_VOLUME_DATA].active = true;
    mounts[PHIPFS_VOLUME_DATA].backend = &backend;
    for (unsigned int attempt = 0U; attempt < 2U; ++attempt) {
        mutation_result = attempt == 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
        assert(phipfs_create(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_truncate(PHIPFS_VOLUME_DATA, "parent/file", 7U) == mutation_result);
        assert(phipfs_mkdir(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_unlink(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_remove(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_rmdir(PHIPFS_VOLUME_DATA, "parent/file") == mutation_result);
        assert(phipfs_link(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == mutation_result);
        assert(phipfs_rename(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == mutation_result);
        assert(phipfs_rename_replace(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == mutation_result);
    }
    assert(calls == 18U && stats == 0U);
    assert(phipfs_unlink(PHIPFS_VOLUME_DATA, "../parent/file") == PHIPFS_STATUS_PATH);
    assert(phipfs_unlink(PHIPFS_VOLUME_DATA, ".") == PHIPFS_STATUS_ACCESS);
    assert(calls == 18U);
    mutation_result = PHIPFS_STATUS_NOT_DIRECTORY;
    assert(phipfs_create(PHIPFS_VOLUME_DATA, "parent/file") == PHIPFS_STATUS_NOT_DIRECTORY);
    mutation_result = PHIPFS_STATUS_NOT_FOUND;
    assert(phipfs_link(PHIPFS_VOLUME_DATA, "parent/file", "other/target") == PHIPFS_STATUS_NOT_FOUND);
    backend.validates_mutation_paths = false;
    assert(phipfs_create(PHIPFS_VOLUME_DATA, "parent/file") == PHIPFS_STATUS_IO);
    assert(calls == 20U && stats == 1U);
    backend.validates_mutation_paths = true;
    for (size_t index = 0U; index < 3U; ++index) {
        const char *paths[] = {"parent/link/../file", "parent/missing/../file", "parent/file/./child"};
        expected_path = paths[index];
        mutation_result = index == 2U ? PHIPFS_STATUS_NOT_DIRECTORY : PHIPFS_STATUS_NOT_FOUND;
        assert(phipfs_create(PHIPFS_VOLUME_DATA, expected_path) == mutation_result);
        assert(phipfs_unlink(PHIPFS_VOLUME_DATA, expected_path) == mutation_result);
    }
    stat_succeeds = true;
    expected_path = "parent/link/../file";
    struct phipfs_stat metadata;
    assert(phipfs_stat_path(PHIPFS_VOLUME_DATA, expected_path, &metadata) == PHIPFS_STATUS_OK);
    assert(metadata.object_id == 101U && stats == 2U);
    expected_path = ".";
    mutation_result = PHIPFS_STATUS_OK;
    assert(phipfs_chmod(PHIPFS_VOLUME_DATA, expected_path, 0644U) == PHIPFS_STATUS_OK);
    assert(phipfs_create(PHIPFS_VOLUME_DATA, expected_path) == PHIPFS_STATUS_ACCESS);
    expected_path = "parent/caf\xc3\xa9";
    assert(phipfs_create(PHIPFS_VOLUME_DATA, expected_path) == PHIPFS_STATUS_OK);
    backend.open_with_stat = replaced_open;
    backend.close = replaced_close;
    expected_path = "parent/file";
    size_t old_vnode;
    memset(&metadata, 0, sizeof(metadata));
    metadata.object_id = 101U;
    assert(vnode_retain(PHIPFS_VOLUME_DATA, expected_path, &metadata, &old_vnode) == PHIPFS_STATUS_OK);
    phipfs_handle opened;
    assert(phipfs_open(PHIPFS_VOLUME_DATA, expected_path, PHIPFS_ACCESS_READ, &opened) == PHIPFS_STATUS_OK);
    struct vfs_open_file_state *file;
    assert(open_file_state(opened, &file) == PHIPFS_STATUS_OK);
    assert(vnodes[file->vnode_index].stat.object_id == 500U);
    assert(vnodes[old_vnode].stat.object_id == 101U && vnodes[old_vnode].references == 1U);
    closing_frontend = opened;
    assert(phipfs_close(opened) == PHIPFS_STATUS_OK && live_backend_handles == 0U);
    assert(closing_frontend == 0U);
    /* Exhaustion after backend open must release that handle, preserving the
     * already-held old inode and every unrelated vnode reference. */
    size_t retained[VFS_MAX_VNODES - 1U];
    for (size_t index = 0U; index < VFS_MAX_VNODES - 1U; ++index) {
        metadata.object_id = 1000U + index;
        assert(vnode_retain(PHIPFS_VOLUME_DATA, "unrelated", &metadata, &retained[index]) == PHIPFS_STATUS_OK);
    }
    assert(phipfs_open(PHIPFS_VOLUME_DATA, expected_path, PHIPFS_ACCESS_READ, &opened) == PHIPFS_STATUS_NO_HANDLES);
    assert(opened == 0U && live_backend_handles == 0U);
    assert(vnodes[old_vnode].references == 1U);
    for (size_t index = 0U; index < VFS_MAX_VNODES - 1U; ++index)
        vnode_release(retained[index], vnodes[retained[index]].generation);
    vnode_release(old_vnode, vnodes[old_vnode].generation);
    directory_metadata = true;
    backend.directory_open_with_stat = replaced_directory_open;
    backend.directory_read = replaced_directory_read;
    backend.directory_close = replaced_close;
    metadata.object_id = 101U;
    metadata.directory = true;
    assert(vnode_retain(PHIPFS_VOLUME_DATA, expected_path, &metadata, &old_vnode) == PHIPFS_STATUS_OK);
    phipfs_directory_handle directory;
    assert(phipfs_directory_open(PHIPFS_VOLUME_DATA, expected_path, &directory) == PHIPFS_STATUS_OK);
    struct vfs_directory_state *snapshot;
    assert(directory_state(directory, &snapshot) == PHIPFS_STATUS_OK);
    assert(vnodes[snapshot->vnode_index].stat.object_id == 500U);
    assert(vnodes[old_vnode].references == 1U);
    struct phipfs_list_entry entry;
    bool present;
    stat_succeeds = false; /* The old name can disappear without affecting iteration. */
    assert(phipfs_directory_read(directory, &entry, &present) == PHIPFS_STATUS_OK && !present);
    ++mounts[PHIPFS_VOLUME_DATA].generation;
    present = true;
    memset(&entry, 0x55, sizeof(entry));
    assert(phipfs_directory_read(directory, &entry, &present) == PHIPFS_STATUS_STALE_HANDLE);
    assert(!present && entry.object_id == 0U && entry.name[0] == '\0');
    --mounts[PHIPFS_VOLUME_DATA].generation;
    assert(phipfs_directory_read(directory, &entry, &present) == PHIPFS_STATUS_OK && !present);
    closing_frontend = directory;
    closing_directory = true;
    assert(phipfs_directory_close(directory) == PHIPFS_STATUS_OK && live_backend_handles == 0U);
    assert(closing_frontend == 0U);
    vnode_release(old_vnode, vnodes[old_vnode].generation);
    assert(mounts[PHIPFS_VOLUME_DATA].references == 0U);
    for (size_t index = 0U; index < VFS_MAX_VNODES; ++index) assert(!vnodes[index].active);
    backend.mkdir_mode = mkdir_mode;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        mutation_result = attempt == 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
        assert(phipfs_mkdir_mode(PHIPFS_VOLUME_DATA, expected_path, 01720U) == mutation_result);
        assert(requested_directory_mode == 01720U);
    }
    assert(phipfs_mkdir_mode(PHIPFS_VOLUME_DATA, expected_path, 0U) == PHIPFS_STATUS_OK);
    assert(requested_directory_mode == 0U);
    assert(phipfs_mkdir(PHIPFS_VOLUME_DATA, expected_path) == PHIPFS_STATUS_OK);
    assert(requested_directory_mode == 0755U);
    const unsigned before_invalid_mkdir = calls;
    assert(phipfs_mkdir_mode(PHIPFS_VOLUME_DATA, expected_path, 010000U) == PHIPFS_STATUS_INVALID_ARGUMENT);
    assert(calls == before_invalid_mkdir);
    expected_file_mode = 0U;
    assert(phipfs_create_mode(PHIPFS_VOLUME_DATA, expected_path, 0U) == PHIPFS_STATUS_OK);
    expected_file_mode = 07777U;
    assert(phipfs_create_mode(PHIPFS_VOLUME_DATA, expected_path, 07777U) == PHIPFS_STATUS_OK);
    const unsigned before_invalid_create = calls;
    assert(phipfs_create_mode(PHIPFS_VOLUME_DATA, expected_path, 010000U) == PHIPFS_STATUS_INVALID_ARGUMENT);
    assert(calls == before_invalid_create);
    backend.lstat_path = nofollow_stat;
    expected_path = "parent/link/../file";
    const unsigned before_lstat = stats;
    char too_long[PHIPFS_MAX_PATH + 1U];
    memset(too_long, 'a', sizeof(too_long));
    too_long[PHIPFS_MAX_PATH] = '\0';
    assert(phipfs_lstat_path(PHIPFS_VOLUME_DATA, too_long, &metadata) == PHIPFS_STATUS_NAME_TOO_LONG);
    assert(phipfs_lstat_path(PHIPFS_VOLUME_DATA, expected_path, &metadata) == PHIPFS_STATUS_OK);
    assert(metadata.object_id == 84U && metadata.mode == 0120777U && metadata.uid == 70000U);
    assert(metadata.atime_seconds == -1 && metadata.atime_nanos == 123U);
    mutation_result = PHIPFS_STATUS_NOT_FOUND;
    assert(phipfs_lstat_path(PHIPFS_VOLUME_DATA, expected_path, &metadata) == PHIPFS_STATUS_NOT_FOUND);
    assert(metadata.object_id == 0U && metadata.mode == 0U && metadata.atime_seconds == 0);
    assert(stats == before_lstat && mounts[PHIPFS_VOLUME_DATA].references == 0U);
    backend.open_options = prepared_open;
    const unsigned before_prepared_stats = stats;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        mutation_result = attempt == 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
        assert(phipfs_open_options(PHIPFS_VOLUME_DATA, expected_path, PHIPFS_ACCESS_READ_WRITE,
            PHIPFS_OPEN_CREATE | PHIPFS_OPEN_TRUNCATE, 01720U, &opened) == mutation_result);
        if (attempt == 0U) assert(opened == 0U && live_backend_handles == 0U);
        else {
            assert(open_file_state(opened, &file) == PHIPFS_STATUS_OK);
            assert(vnodes[file->vnode_index].stat.object_id == 500U);
            assert(phipfs_close(opened) == PHIPFS_STATUS_OK && live_backend_handles == 0U);
        }
    }
    assert(prepared_calls == 2U && stats == before_prepared_stats);
    assert(phipfs_open_options(PHIPFS_VOLUME_DATA, expected_path, PHIPFS_ACCESS_READ,
        PHIPFS_OPEN_TRUNCATE, 01720U, &opened) == PHIPFS_STATUS_ACCESS);
    assert(opened == 0U && prepared_calls == 2U);
    for (size_t index = 0U; index < VFS_MAX_OPEN_FILES; ++index) {
        assert(phipia_slot_claim(open_file_claims, VFS_MAX_OPEN_FILES) != VFS_MAX_OPEN_FILES);
    }
    assert(phipfs_open_options(PHIPFS_VOLUME_DATA, expected_path, PHIPFS_ACCESS_READ_WRITE,
        PHIPFS_OPEN_CREATE | PHIPFS_OPEN_TRUNCATE, 01720U, &opened) == PHIPFS_STATUS_NO_HANDLES);
    assert(opened == 0U && prepared_calls == 2U);
    for (size_t index = 0U; index < VFS_MAX_OPEN_FILES; ++index) phipia_slot_release(open_file_claims, index);
    expected_prepared_flags = PHIPFS_OPEN_CREATE | PHIPFS_OPEN_EXCLUSIVE;
    mutation_result = PHIPFS_STATUS_EXISTS;
    assert(phipfs_open_options(PHIPFS_VOLUME_DATA, expected_path, PHIPFS_ACCESS_READ_WRITE,
        expected_prepared_flags, 01720U, &opened) == PHIPFS_STATUS_EXISTS);
    assert(opened == 0U && prepared_calls == 3U && live_backend_handles == 0U);
    assert(phipfs_open_options(PHIPFS_VOLUME_DATA, expected_path, PHIPFS_ACCESS_READ_WRITE,
        PHIPFS_OPEN_EXCLUSIVE, 01720U, &opened) == PHIPFS_STATUS_INVALID_ARGUMENT);
    assert(opened == 0U && prepared_calls == 3U);
    backend.fsync = file_sync;
    backend.fstat = file_stat;
    backend.publish_file = publish_file;
    backend.unlink_held_file = held_unlink;
    mutation_result = PHIPFS_STATUS_OK;
    assert(phipfs_open_options(PHIPFS_VOLUME_DATA, expected_path, PHIPFS_ACCESS_READ_WRITE,
        expected_prepared_flags, 01720U, &opened) == PHIPFS_STATUS_OK);
    mutation_result = PHIPFS_STATUS_IO;
    assert(phipfs_fsync(opened) == PHIPFS_STATUS_IO);
    mutation_result = PHIPFS_STATUS_OK;
    assert(phipfs_fsync(opened) == PHIPFS_STATUS_OK && file_sync_calls == 2U);
    const unsigned before_file_stat = stats;
    assert(phipfs_fstat(opened, &metadata) == PHIPFS_STATUS_OK);
    assert(metadata.object_id == 500U && metadata.size == 777U && metadata.mode == 0100600U);
    assert(stats == before_file_stat && file_stat_calls == 1U);
    mutation_result = PHIPFS_STATUS_IO;
    assert(phipfs_publish_file(opened, expected_path, "other/target") == PHIPFS_STATUS_IO);
    mutation_result = PHIPFS_STATUS_OK;
    assert(phipfs_publish_file(opened, expected_path, "other/target") == PHIPFS_STATUS_OK);
    assert(publication_calls == 2U && stats == before_file_stat);
    mutation_result = PHIPFS_STATUS_IO;
    assert(phipfs_unlink_held_file(opened, expected_path) == PHIPFS_STATUS_IO);
    mutation_result = PHIPFS_STATUS_OK;
    assert(phipfs_unlink_held_file(opened, expected_path) == PHIPFS_STATUS_OK);
    assert(held_unlink_calls == 2U && stats == before_file_stat);
    ++mounts[PHIPFS_VOLUME_DATA].generation;
    stale_io_counts(opened);
    uint64_t stale_position = 123U;
    assert(phipfs_seek(opened, 0, PHIPFS_SEEK_START, &stale_position) == PHIPFS_STATUS_STALE_HANDLE);
    assert(phipfs_set_append(opened, true) == PHIPFS_STATUS_STALE_HANDLE);
    assert(phipfs_ftruncate(opened, 0U) == PHIPFS_STATUS_STALE_HANDLE);
    assert(phipfs_unlink_held_file(opened, expected_path) == PHIPFS_STATUS_STALE_HANDLE);
    assert(held_unlink_calls == 2U);
    assert(phipfs_fsync(opened) == PHIPFS_STATUS_STALE_HANDLE && file_sync_calls == 2U);
    assert(phipfs_fstat(opened, &metadata) == PHIPFS_STATUS_STALE_HANDLE && file_stat_calls == 1U);
    assert(metadata.object_id == 0U);
    assert(phipfs_publish_file(opened, expected_path, "other/target") == PHIPFS_STATUS_STALE_HANDLE);
    assert(publication_calls == 2U);
    --mounts[PHIPFS_VOLUME_DATA].generation;
    mounts[PHIPFS_VOLUME_DATA].active = false;
    stale_io_counts(opened);
    mounts[PHIPFS_VOLUME_DATA].active = true;
    const size_t held_vnode = open_files[(opened & 0xffU) - 1U].vnode_index;
    ++vnodes[held_vnode].generation;
    stale_io_counts(opened);
    --vnodes[held_vnode].generation;
    backend.write = partial_write;
    size_t partial_bytes = 99U;
    assert(phipfs_write(opened, (const uint8_t *)"four", 4U, &partial_bytes) == PHIPFS_STATUS_IO);
    assert(partial_bytes == 2U);
    assert(phipfs_close(opened) == PHIPFS_STATUS_OK);
    stale_io_counts(opened);
    stale_io_counts(0U);
    stale_io_counts(UINT64_MAX);
    assert(phipfs_fsync(opened) == PHIPFS_STATUS_STALE_HANDLE && file_sync_calls == 2U);
    assert(phipfs_fstat(opened, &metadata) == PHIPFS_STATUS_STALE_HANDLE && file_stat_calls == 1U);
    assert(mounts[PHIPFS_VOLUME_DATA].references == 0U);
    for (size_t index = 0U; index < VFS_MAX_VNODES; ++index) assert(!vnodes[index].active);
    size_t occupied[VFS_MAX_VNODES];
    for (size_t index = 0U; index < VFS_MAX_VNODES; ++index) {
        metadata.object_id = 1000U + index;
        assert(vnode_retain(PHIPFS_VOLUME_DATA, "held", &metadata, &occupied[index]) == PHIPFS_STATUS_OK);
    }
    const unsigned before_vnode_full = prepared_calls;
    assert(phipfs_open_options(PHIPFS_VOLUME_DATA, expected_path, PHIPFS_ACCESS_READ_WRITE,
        expected_prepared_flags, 01720U, &opened) == PHIPFS_STATUS_NO_HANDLES);
    assert(opened == 0U && prepared_calls == before_vnode_full && live_backend_handles == 0U);
    vnode_release(occupied[VFS_MAX_VNODES - 1U], vnodes[occupied[VFS_MAX_VNODES - 1U]].generation);
    consume_last_vnode = true;
    for (unsigned failure = 0U; failure < 2U; ++failure) {
        mutation_result = failure == 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
        assert(phipfs_open_options(PHIPFS_VOLUME_DATA, expected_path, PHIPFS_ACCESS_READ_WRITE,
            expected_prepared_flags, 01720U, &opened) == mutation_result);
        if (failure == 0U) assert(opened == 0U);
        else assert(phipfs_close(opened) == PHIPFS_STATUS_OK);
        for (size_t index = 0U; index < VFS_MAX_VNODES; ++index) assert(!vnode_reservations[index]);
        assert(live_backend_handles == 0U && mounts[PHIPFS_VOLUME_DATA].references == VFS_MAX_VNODES - 1U);
    }
    consume_last_vnode = false;
    for (size_t index = 0U; index + 1U < VFS_MAX_VNODES; ++index)
        vnode_release(occupied[index], vnodes[occupied[index]].generation);
    assert(mounts[PHIPFS_VOLUME_DATA].references == 0U);
    uint8_t attribute[4];
    size_t attribute_bytes = 99U;
    assert(phipfs_get_xattr(PHIPFS_VOLUME_DATA, "../invalid", "user.test",
        attribute, sizeof(attribute), &attribute_bytes) == PHIPFS_STATUS_PATH);
    assert(attribute_bytes == 0U);
    attribute_bytes = 99U;
    assert(phipfs_get_xattr(PHIPFS_VOLUME_DATA, "parent/file", "user.test",
        attribute, sizeof(attribute), &attribute_bytes) == PHIPFS_STATUS_ACCESS);
    assert(attribute_bytes == 0U);
    backend.get_xattr = read_attribute;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        mutation_result = attempt == 0U ? PHIPFS_STATUS_IO : PHIPFS_STATUS_OK;
        assert(phipfs_get_xattr(PHIPFS_VOLUME_DATA, "parent/file", "user.test",
            attribute, sizeof(attribute), &attribute_bytes) == mutation_result);
        assert(attribute_bytes == (attempt == 0U ? 0U : 3U));
    }
    attribute_bytes = 99U;
    assert(phipfs_readlink(PHIPFS_VOLUME_DATA, "parent/file", NULL, 4U,
        &attribute_bytes) == PHIPFS_STATUS_INVALID_ARGUMENT);
    assert(attribute_bytes == 0U);
    nested_open_reservations();
    assert(!phipfs_resources_released());
    mounts[PHIPFS_VOLUME_DATA].active = false;
    assert(phipfs_resources_released());
    vnode_reservations[0] = true;
    assert(!phipfs_resources_released());
    vnode_reservations[0] = false;
    open_files[0].opening = true;
    assert(!phipfs_resources_released());
    open_files[0].opening = false;
    directories[0].backend_handle = 1U;
    assert(!phipfs_resources_released());
    directories[0].backend_handle = 0U;
    assert(phipfs_resources_released());
    backend.unmount = reentrant_unmount;
    mounts[PHIPFS_VOLUME_DATA].backend = &backend;
    mounts[PHIPFS_VOLUME_DATA].active = true;
    const uint64_t unmount_generation = mounts[PHIPFS_VOLUME_DATA].generation;
    mutation_result = PHIPFS_STATUS_IO;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        assert(phipfs_unmount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_IO);
        assert(mounts[PHIPFS_VOLUME_DATA].active && !mounts[PHIPFS_VOLUME_DATA].unmounting);
        assert(mounts[PHIPFS_VOLUME_DATA].generation == unmount_generation);
        assert(mounts[PHIPFS_VOLUME_DATA].backend == &backend && !phipfs_resources_released());
    }
    mutation_result = PHIPFS_STATUS_OK;
    assert(phipfs_unmount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK && unmount_calls == 3U);
    assert(phipfs_resources_released());
    backend.mount = reentrant_mount;
    backend.drive = mounted_drive;
    volume_backends[PHIPFS_VOLUME_DATA] = &backend;
    mutation_result = PHIPFS_STATUS_IO;
    const uint64_t next_mount = next_mount_generation;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        assert(phipfs_mount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_IO);
        assert(!mounts[PHIPFS_VOLUME_DATA].active && !mounts[PHIPFS_VOLUME_DATA].mounting);
        assert(next_mount_generation == next_mount && phipfs_resources_released());
    }
    mutation_result = PHIPFS_STATUS_OK;
    assert(phipfs_mount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK && mount_calls == 3U);
    assert(mounts[PHIPFS_VOLUME_DATA].active && !mounts[PHIPFS_VOLUME_DATA].mounting);
    assert(mounts[PHIPFS_VOLUME_DATA].generation == next_mount && !phipfs_resources_released());
    assert(phipfs_unmount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK && unmount_calls == 4U);
    assert(phipfs_resources_released());
    puts("VFS remount reserves admission through backend setup and publishes one successful generation: PASS");
    puts("VFS teardown blocks reentrant admission and retains mount generation across failed release: PASS");
    puts("VFS journal mutation retries, nested open reservations, backend errors, path bounds and vnode census: PASS");
    return 0;
}
