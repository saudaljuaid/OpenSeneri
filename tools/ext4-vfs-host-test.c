/* SPDX-License-Identifier: GPL-3.0-only */
/* Exercise the production C backend with explicit Rust/NVMe refusal stubs. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/kernel/ext4_fs.c"

static unsigned opens;
static unsigned closes;
static unsigned truncates;
static unsigned stats;
static unsigned refusals;
static bool pending;
static uint64_t disk_size = 8192U;
static int32_t permanent_status = PHIPIA_EXT4_STATUS_OK;
static uint8_t file_type = PHIPIA_EXT4_FILE_REGULAR;
static unsigned renames;
static uint64_t pending_size;
static unsigned sync_refusals;
static unsigned stat_refusals;
static unsigned appends;
static uint16_t changed_mode;
static unsigned live_snapshots;
static unsigned freed_snapshots;
static size_t expected_open_inodes;
static size_t last_sync_open_count;
static bool expected_remove_directory;
static bool expect_registered_before_close;
static bool close_reports_failure;
static bool reenter_on_open;
static bool reenter_on_close;
static bool open_reports_failure;
static bool expect_published_size_before_close;
static unsigned live_mounts = 1U;
static uint32_t logical_block_bytes = 4096U;

int32_t phipia_ext4_mount(uintptr_t context, uint64_t media_bytes,
    struct phipia_ext4_identity *identity, uintptr_t *mounted)
{
    assert(context == (uintptr_t)&ext4_mounts[PHIPFS_VOLUME_DATA]);
    assert(media_bytes == 32768U * 4096U && live_mounts == 0U);
    memset(identity, 0, sizeof(*identity));
    *mounted = 1U;
    ++live_mounts;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_prepare_unmount(uintptr_t mounted)
{
    assert(mounted == 1U && live_mounts == 1U);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.active);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_unmount(uintptr_t mounted)
{
    assert(mounted == 1U && live_mounts == 1U);
    assert(!ext4_mounts[PHIPFS_VOLUME_DATA].session.active);
    --live_mounts;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_set_times(uintptr_t mounted, const uint8_t *path, size_t path_bytes,
    uint64_t atime_seconds, uint32_t atime_nanos, uint64_t mtime_seconds, uint32_t mtime_nanos)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    assert(atime_seconds == 2200000000U && atime_nanos == 123U);
    assert(mtime_seconds == 2300000000U && mtime_nanos == 456U);
    return permanent_status;
}

int32_t phipia_ext4_directory_snapshot(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, struct phipia_ext4_metadata *metadata, uintptr_t *snapshot)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(!ext4_mounts[PHIPFS_VOLUME_DATA].session.writable && live_snapshots == 0U);
    memset(metadata, 0, sizeof(*metadata));
    metadata->inode = 42U;
    metadata->file_type = PHIPIA_EXT4_FILE_DIRECTORY;
    *snapshot = 2U;
    ++live_snapshots;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_snapshot_entry(uintptr_t snapshot, uint64_t index,
    struct phipia_ext4_directory_entry *entry, bool *present)
{
    assert(snapshot == 2U && live_snapshots == 1U);
    memset(entry, 0, sizeof(*entry));
    *present = index == 0U;
    if (*present) {
        entry->name_length = 255U;
        memset(entry->name, 'q', 255U);
        entry->metadata.file_type = PHIPIA_EXT4_FILE_REGULAR;
    }
    return PHIPIA_EXT4_STATUS_OK;
}

void phipia_ext4_snapshot_free(uintptr_t snapshot)
{
    assert(snapshot == 2U && live_snapshots == 1U);
    --live_snapshots;
    ++freed_snapshots;
}

int32_t phipia_ext4_chmod(uintptr_t mounted, const uint8_t *path, size_t path_bytes, uint16_t mode)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    if (permanent_status == PHIPIA_EXT4_STATUS_OK) changed_mode = mode;
    return permanent_status;
}

int32_t phipia_ext4_set_xattr(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, const uint8_t *name, size_t name_bytes,
    const uint8_t *value, size_t value_bytes, uint8_t remove)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(name_bytes == 9U && memcmp(name, "user.note", 9U) == 0);
    assert(value != NULL && value_bytes == 0U && remove <= 1U);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    return permanent_status;
}

int32_t phipia_ext4_get_xattr(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, const uint8_t *name, size_t name_bytes,
    uint8_t *output, size_t capacity, size_t *length)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(name_bytes == 9U && memcmp(name, "user.note", 9U) == 0);
    assert(!ext4_mounts[PHIPFS_VOLUME_DATA].session.writable && output != NULL);
    if (capacity != 0U && capacity < 5U) return PHIPIA_EXT4_STATUS_RANGE;
    *length = 5U;
    if (capacity != 0U) memcpy(output, "value", 5U);
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_append(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, const uint8_t *source, size_t source_bytes,
    uint64_t maximum_size, uint64_t *start, size_t *written)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(source != NULL && ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    assert(maximum_size == PHIPFS_MAX_FILE_BYTES);
    ++appends;
    if (permanent_status != PHIPIA_EXT4_STATUS_OK) return permanent_status;
    *start = disk_size;
    *written = source_bytes;
    disk_size += source_bytes;
    return PHIPIA_EXT4_STATUS_OK;
}

enum nvme_status nvme_volume_open(struct nvme_volume_session *session,
    uint32_t controller_index, bool writable)
{
    assert(!session->active);
    if (reenter_on_open) {
        reenter_on_open = false;
        assert(ext4_backend_sync(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_BUSY);
    }
    if (open_reports_failure) return NVME_STATUS_TEARDOWN_FAILURE;
    memset(session, 0, sizeof(*session));
    session->namespace_blocks = 32768U;
    session->logical_block_bytes = logical_block_bytes;
    session->controller_index = controller_index;
    session->writable = writable;
    session->active = true;
    session->generation = (uint64_t)opens + 1U;
    ++opens;
    return NVME_STATUS_OK;
}

enum nvme_status nvme_volume_close(struct nvme_volume_session *session)
{
    assert(session->active);
    if (reenter_on_close) {
        reenter_on_close = false;
        assert(retry_session_close(&ext4_mounts[PHIPFS_VOLUME_DATA]) == PHIPFS_STATUS_BUSY);
    }
    if (expect_published_size_before_close) {
        expect_published_size_before_close = false;
        assert(ext4_mounts[PHIPFS_VOLUME_DATA].operation_active);
        for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
            if (ext4_handles[index].active && ext4_handles[index].inode == 42U) {
                assert(ext4_handles[index].size == disk_size);
            }
        }
    }
    if (expect_registered_before_close) {
        assert(ext4_mounts[PHIPFS_VOLUME_DATA].operation_active);
        assert(volume_has_open_handles(PHIPFS_VOLUME_DATA));
        expect_registered_before_close = false;
    }
    if (close_reports_failure) {
        session->state = NVME_FILESYSTEM_SESSION_STOPPING;
        return NVME_STATUS_TEARDOWN_FAILURE;
    }
    session->active = false;
    ++closes;
    return NVME_STATUS_OK;
}

int32_t phipia_ext4_truncate_probe(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, uint64_t size)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    ++truncates;
    if (permanent_status != PHIPIA_EXT4_STATUS_OK) {
        return permanent_status;
    }
    if (refusals != 0U) {
        --refusals;
        pending = true;
        pending_size = size;
        return PHIPIA_EXT4_STATUS_IO;
    }
    pending = false;
    disk_size = size;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_stat(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, struct phipia_ext4_metadata *metadata)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    ++stats;
    if (stat_refusals != 0U) {
        --stat_refusals;
        return PHIPIA_EXT4_STATUS_IO;
    }
    if (pending) {
        return PHIPIA_EXT4_STATUS_IO;
    }
    memset(metadata, 0, sizeof(*metadata));
    metadata->inode = 42U;
    metadata->size = disk_size;
    metadata->file_type = file_type;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_stat_inode(uintptr_t mounted, uint64_t inode, struct phipia_ext4_metadata *metadata)
{
    assert(inode == 42U);
    return phipia_ext4_stat(mounted, (const uint8_t *)"file", 4U, metadata);
}

int32_t phipia_ext4_truncate_inode(uintptr_t mounted, uint64_t inode, uint64_t size)
{
    assert(inode == 42U);
    return phipia_ext4_truncate_probe(mounted, (const uint8_t *)"file", 4U, size);
}

int32_t phipia_ext4_append_inode(uintptr_t mounted, uint64_t inode,
    const uint8_t *source, size_t length, uint64_t maximum_size, uint64_t *start, size_t *count)
{
    assert(inode == 42U);
    return phipia_ext4_append(mounted, (const uint8_t *)"file", 4U, source, length, maximum_size, start, count);
}

int32_t phipia_ext4_pread_inode(uintptr_t mounted, uint64_t inode, uint64_t offset,
    uint8_t *output, size_t capacity, size_t *count)
{
    assert(mounted == 1U && inode == 42U && !ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    *count = offset >= disk_size ? 0U : (size_t)(disk_size - offset);
    if (*count > capacity) *count = capacity;
    memset(output, 0x55, *count);
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_write_inode(uintptr_t mounted, uint64_t inode, uint64_t offset,
    const uint8_t *source, size_t length, size_t *count)
{
    assert(mounted == 1U && inode == 42U && source != NULL && ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    *count = length;
    if (offset + length > disk_size) disk_size = offset + length;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_sync(uintptr_t mounted, const uint64_t *open_inodes, size_t open_count)
{
    assert(mounted == 1U && ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    assert(open_inodes != NULL && open_count <= EXT4_MAX_HANDLES);
    last_sync_open_count = open_count;
    for (size_t index = 0U; index < open_count; ++index) assert(open_inodes[index] != 0U);
    if (sync_refusals != 0U) {
        --sync_refusals;
        return PHIPIA_EXT4_STATUS_IO;
    }
    if (pending) {
        disk_size = pending_size;
        pending = false;
    }
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_lstat(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, struct phipia_ext4_metadata *metadata)
{
    return phipia_ext4_stat(mounted, path, path_bytes, metadata);
}

int32_t phipia_ext4_symlink(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, const uint8_t *target, size_t target_bytes)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(target_bytes == 10U && memcmp(target, "../missing", 10U) == 0);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    return permanent_status;
}

int32_t phipia_ext4_link_file_probe(uintptr_t mounted, const uint8_t *source,
    size_t source_bytes, const uint8_t *destination, size_t destination_bytes)
{
    assert(mounted == 1U && source_bytes == 4U && memcmp(source, "file", 4U) == 0);
    assert(destination_bytes == 5U && memcmp(destination, "alias", 5U) == 0);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    return permanent_status;
}

int32_t phipia_ext4_unlink_file_probe(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, const uint64_t *open_inodes, size_t open_count, bool remove_directory)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    assert(open_count == expected_open_inodes);
    assert(remove_directory == expected_remove_directory);
    for (size_t index = 0U; index < open_count; ++index) assert(open_inodes[index] == 42U);
    return permanent_status;
}

int32_t phipia_ext4_readlink(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, uint8_t *output, size_t capacity, size_t *read_bytes)
{
    assert(mounted == 1U && path_bytes == 4U && memcmp(path, "file", 4U) == 0);
    assert(!ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    *read_bytes = capacity < 10U ? capacity : 10U;
    memcpy(output, "../missing", *read_bytes);
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_rename_probe(uintptr_t mounted, const uint8_t *source,
    size_t source_bytes, const uint8_t *destination, size_t destination_bytes)
{
    assert(mounted == 1U && source_bytes == 4U && memcmp(source, "file", 4U) == 0);
    assert(destination_bytes == 5U && memcmp(destination, "moved", 5U) == 0);
    assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.writable);
    ++renames;
    return PHIPIA_EXT4_STATUS_OK;
}

int32_t phipia_ext4_rename_replace(uintptr_t mounted, const uint8_t *source,
    size_t source_bytes, const uint8_t *destination, size_t destination_bytes,
    const uint64_t *open_inodes, size_t open_count)
{
    assert(open_inodes != NULL && open_count == expected_open_inodes);
    for (size_t index = 0U; index < open_count; ++index) assert(open_inodes[index] == 43U);
    return phipia_ext4_rename_probe(mounted, source, source_bytes, destination, destination_bytes);
}

int main(void)
{
    phipfs_handle first;
    phipfs_handle second;
    struct ext4_handle_state *state;
    ext4_backend_initialize();
    ext4_mounts[PHIPFS_VOLUME_DATA].active = true;
    ext4_mounts[PHIPFS_VOLUME_DATA].healthy = true;
    ext4_mounts[PHIPFS_VOLUME_DATA].generation = 1U;
    ext4_mounts[PHIPFS_VOLUME_DATA].rust_mount = 1U;
    reenter_on_open = true;
    open_reports_failure = true;
    assert(ext4_backend_sync(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_IO);
    assert(!reenter_on_open && !ext4_mounts[PHIPFS_VOLUME_DATA].operation_active);
    assert(opens == 0U && closes == 0U);
    open_reports_failure = false;
    reenter_on_open = true;
    assert(allocate_handle(PHIPFS_VOLUME_DATA, "file", 42U, disk_size,
        PHIPFS_ACCESS_READ, false, 0U, &first) == PHIPFS_STATUS_OK);
    assert(allocate_handle(PHIPFS_VOLUME_DATA, "file", 42U, disk_size,
        PHIPFS_ACCESS_WRITE, false, 0U, &second) == PHIPFS_STATUS_OK);
    refusals = 2U;
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 101U) == PHIPFS_STATUS_IO);
        assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 8192U);
        assert(opens == closes && !ext4_mounts[PHIPFS_VOLUME_DATA].operation_active);
    }
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 101U) == PHIPFS_STATUS_OK);
    assert(truncates == 3U && stats == 1U);
    assert(!reenter_on_open);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    assert(handle_state(second, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    permanent_status = PHIPIA_EXT4_STATUS_FULL;
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 100U) == PHIPFS_STATUS_FULL);
    permanent_status = PHIPIA_EXT4_STATUS_READ_ONLY;
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 100U) == PHIPFS_STATUS_READ_ONLY);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    permanent_status = PHIPIA_EXT4_STATUS_OK;
    refusals = 1U;
    assert(ext4_backend_truncate(PHIPFS_VOLUME_DATA, "file", 65537U) == PHIPFS_STATUS_IO);
    sync_refusals = 1U;
    assert(ext4_backend_sync(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_IO);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    stat_refusals = 1U;
    assert(ext4_backend_sync(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_IO);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 101U);
    assert(ext4_backend_sync(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 65537U);
    assert(handle_state(second, &state) == PHIPFS_STATUS_OK && state->size == 65537U);
    assert(state->offset == 0U);
    size_t written;
    uint64_t position;
    assert(ext4_backend_append(first, (const uint8_t *)"a", 1U, &written) == PHIPFS_STATUS_ACCESS);
    assert(appends == 0U && written == 0U);
    expect_published_size_before_close = true;
    assert(ext4_backend_append(second, (const uint8_t *)"abc", 3U, &written) == PHIPFS_STATUS_OK);
    assert(!expect_published_size_before_close);
    assert(written == 3U && state->offset == 65540U);
    assert(ext4_backend_seek(second, 0, PHIPFS_SEEK_START, &position) == PHIPFS_STATUS_OK);
    permanent_status = PHIPIA_EXT4_STATUS_IO;
    assert(ext4_backend_append(second, (const uint8_t *)"de", 2U, &written) == PHIPFS_STATUS_IO);
    assert(written == 0U && state->offset == 0U && state->size == 65540U);
    permanent_status = PHIPIA_EXT4_STATUS_OK;
    assert(ext4_backend_append(second, (const uint8_t *)"de", 2U, &written) == PHIPFS_STATUS_OK);
    assert(written == 2U && state->offset == 65542U);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 65542U);
    assert(state->offset == 0U);
    assert(ext4_backend_rename(PHIPFS_VOLUME_DATA, "file", "moved") == PHIPFS_STATUS_OK);
    uint8_t read_value = 0U;
    size_t read_count = 0U;
    stat_refusals = 1U;
    assert(ext4_backend_read(first, &read_value, 1U, &read_count) == PHIPFS_STATUS_OK);
    assert(read_count == 1U && read_value == 0x55);
    assert(ext4_backend_write(second, (const uint8_t *)"x", 1U, &written) == PHIPFS_STATUS_OK);
    assert(written == 1U && stat_refusals == 1U);
    expected_open_inodes = 2U;
    permanent_status = PHIPIA_EXT4_STATUS_BUSY;
    assert(ext4_backend_unlink(PHIPFS_VOLUME_DATA, "file") == PHIPFS_STATUS_BUSY);
    permanent_status = PHIPIA_EXT4_STATUS_IO;
    assert(ext4_backend_unlink(PHIPFS_VOLUME_DATA, "file") == PHIPFS_STATUS_IO);
    permanent_status = PHIPIA_EXT4_STATUS_OK;
    assert(ext4_backend_unlink(PHIPFS_VOLUME_DATA, "file") == PHIPFS_STATUS_OK);
    assert(stat_refusals == 1U);
    expected_remove_directory = true;
    assert(ext4_backend_remove(PHIPFS_VOLUME_DATA, "file") == PHIPFS_STATUS_OK);
    expected_remove_directory = false;
    stat_refusals = 0U;
    renames = 0U;
    assert(ext4_backend_ftruncate(first, 5U) == PHIPFS_STATUS_ACCESS);
    refusals = 1U;
    assert(ext4_backend_ftruncate(second, 5U) == PHIPFS_STATUS_IO);
    assert(ext4_backend_ftruncate(second, 5U) == PHIPFS_STATUS_OK);
    assert(handle_state(first, &state) == PHIPFS_STATUS_OK && state->size == 5U && state->offset == 1U);
    assert(ext4_backend_close(first) == PHIPFS_STATUS_OK);
    assert(last_sync_open_count == 1U);
    sync_refusals = 1U;
    assert(ext4_backend_close(second) == PHIPFS_STATUS_OK);
    assert(sync_refusals == 0U && ext4_mounts[PHIPFS_VOLUME_DATA].orphan_cleanup_pending);
    assert(last_sync_open_count == 0U);
    assert(!volume_has_open_handles(PHIPFS_VOLUME_DATA));
    assert(ext4_backend_sync(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK);
    assert(!ext4_mounts[PHIPFS_VOLUME_DATA].orphan_cleanup_pending);
    expected_open_inodes = 0U;
    assert(ext4_backend_unlink(PHIPFS_VOLUME_DATA, "file") == PHIPFS_STATUS_OK);
    assert(handle_state(first, &state) == PHIPFS_STATUS_STALE_HANDLE);
    file_type = PHIPIA_EXT4_FILE_DIRECTORY;
    /* An unrelated spelling can alias a descendant of the directory. */
    assert(allocate_handle(PHIPFS_VOLUME_DATA, "alias/child", 43U, 9U,
        PHIPFS_ACCESS_READ, false, 0U, &first) == PHIPFS_STATUS_OK);
    assert(ext4_backend_rename(PHIPFS_VOLUME_DATA, "file", "moved") == PHIPFS_STATUS_OK);
    assert(renames == 1U);
    expected_open_inodes = 1U;
    assert(ext4_backend_rename_replace(PHIPFS_VOLUME_DATA, "file", "moved") == PHIPFS_STATUS_OK);
    assert(ext4_backend_close(first) == PHIPFS_STATUS_OK);
    expected_open_inodes = 0U;
    assert(ext4_backend_rename(PHIPFS_VOLUME_DATA, "file", "moved") == PHIPFS_STATUS_OK);
    assert(renames == 3U);
    assert(ext4_backend_rename_replace(PHIPFS_VOLUME_DATA, "file", "moved") == PHIPFS_STATUS_OK);
    assert(renames == 4U);
    permanent_status = PHIPIA_EXT4_STATUS_IO;
    assert(ext4_backend_symlink(PHIPFS_VOLUME_DATA, "file", "../missing") == PHIPFS_STATUS_IO);
    permanent_status = PHIPIA_EXT4_STATUS_OK;
    assert(ext4_backend_symlink(PHIPFS_VOLUME_DATA, "file", "../missing") == PHIPFS_STATUS_OK);
    stat_refusals = 1U;
    permanent_status = PHIPIA_EXT4_STATUS_IO;
    assert(ext4_backend_link(PHIPFS_VOLUME_DATA, "file", "alias") == PHIPFS_STATUS_IO);
    permanent_status = PHIPIA_EXT4_STATUS_OK;
    assert(ext4_backend_link(PHIPFS_VOLUME_DATA, "file", "alias") == PHIPFS_STATUS_OK);
    assert(stat_refusals == 1U); /* A dangling final symlink must not be followed. */
    stat_refusals = 0U;
    uint8_t literal[12];
    size_t read_bytes;
    memset(literal, 0xa5, sizeof(literal));
    assert(ext4_backend_readlink(PHIPFS_VOLUME_DATA, "file", literal, 4U, &read_bytes) == PHIPFS_STATUS_OK);
    assert(read_bytes == 4U && memcmp(literal, "../m", 4U) == 0 && literal[4] == 0xa5);
    assert(ext4_backend_readlink(PHIPFS_VOLUME_DATA, "file", literal, sizeof(literal), &read_bytes) == PHIPFS_STATUS_OK);
    assert(read_bytes == 10U && memcmp(literal, "../missing", 10U) == 0 && literal[10] == 0xa5);
    permanent_status = PHIPIA_EXT4_STATUS_IO;
    assert(ext4_backend_chmod(PHIPFS_VOLUME_DATA, "file", 0640U) == PHIPFS_STATUS_IO);
    assert(changed_mode == 0U);
    permanent_status = PHIPIA_EXT4_STATUS_OK;
    assert(ext4_backend_chmod(PHIPFS_VOLUME_DATA, "file", 0640U) == PHIPFS_STATUS_OK);
    assert(changed_mode == 0640U);
    const struct phipfs_times times = {2200000000U, 2300000000U, 123U, 456U};
    permanent_status = PHIPIA_EXT4_STATUS_IO;
    assert(ext4_backend_set_times(PHIPFS_VOLUME_DATA, "file", &times) == PHIPFS_STATUS_IO);
    permanent_status = PHIPIA_EXT4_STATUS_OK;
    assert(ext4_backend_set_times(PHIPFS_VOLUME_DATA, "file", &times) == PHIPFS_STATUS_OK);
    assert(ext4_backend_set_xattr(PHIPFS_VOLUME_DATA, "file", "user.note", NULL, 0U, false) == PHIPFS_STATUS_OK);
    assert(ext4_backend_set_xattr(PHIPFS_VOLUME_DATA, "file", "user.note", NULL, 0U, true) == PHIPFS_STATUS_OK);
    assert(ext4_backend_get_xattr(PHIPFS_VOLUME_DATA, "file", "user.note", NULL, 0U, &read_bytes) == PHIPFS_STATUS_OK);
    assert(read_bytes == 5U);
    assert(ext4_backend_get_xattr(PHIPFS_VOLUME_DATA, "file", "user.note", literal, 4U, &read_bytes) == PHIPFS_STATUS_RANGE);
    assert(read_bytes == 0U);
    assert(ext4_backend_get_xattr(PHIPFS_VOLUME_DATA, "file", "user.note", literal, sizeof(literal), &read_bytes) == PHIPFS_STATUS_OK);
    assert(read_bytes == 5U && memcmp(literal, "value", 5U) == 0);
    assert(ext4_backend_directory_open(PHIPFS_VOLUME_DATA, "file", &first) == PHIPFS_STATUS_OK);
    const unsigned snapshot_opens = opens;
    struct phipfs_list_entry entry;
    bool present;
    assert(ext4_backend_directory_read(first, &entry, &present) == PHIPFS_STATUS_OK);
    assert(present && strlen(entry.name) == 255U);
    assert(ext4_backend_directory_read(first, &entry, &present) == PHIPFS_STATUS_OK && !present);
    assert(opens == snapshot_opens);
    assert(ext4_backend_directory_close(first) == PHIPFS_STATUS_OK);
    assert(live_snapshots == 0U && freed_snapshots == 1U);
    assert(ext4_backend_directory_close(first) == PHIPFS_STATUS_STALE_HANDLE);
    phipfs_handle held[EXT4_MAX_HANDLES];
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
        assert(allocate_handle(PHIPFS_VOLUME_DATA, "file", 42U, 0U,
            PHIPFS_ACCESS_READ, false, 0U, &held[index]) == PHIPFS_STATUS_OK);
    }
    assert(ext4_backend_directory_open(PHIPFS_VOLUME_DATA, "file", &first) == PHIPFS_STATUS_NO_HANDLES);
    assert(first == 0U && live_snapshots == 0U && freed_snapshots == 2U);
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
        assert(ext4_backend_close(held[index]) == PHIPFS_STATUS_OK);
    }
    file_type = PHIPIA_EXT4_FILE_REGULAR;
    expect_registered_before_close = true;
    assert(ext4_backend_open(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ, &first) == PHIPFS_STATUS_OK);
    assert(!expect_registered_before_close && first != 0U);
    assert(ext4_backend_close(first) == PHIPFS_STATUS_OK);
    for (unsigned directory = 0U; directory < 2U; ++directory) {
        expect_registered_before_close = true;
        close_reports_failure = true;
        const enum phipfs_status status = directory != 0U ?
            ext4_backend_directory_open(PHIPFS_VOLUME_DATA, "file", &first) :
            ext4_backend_open(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ, &first);
        assert(status == PHIPFS_STATUS_IO && first == 0U);
        assert(!expect_registered_before_close && !volume_has_open_handles(PHIPFS_VOLUME_DATA));
        assert(live_snapshots == 0U);
        const uint64_t retained_generation = ext4_mounts[PHIPFS_VOLUME_DATA].session.generation;
        const unsigned before_retry = opens;
        assert(retained_generation != 0U && ext4_mounts[PHIPFS_VOLUME_DATA].session.active);
        assert(ext4_backend_open(PHIPFS_VOLUME_DATA, "file", PHIPFS_ACCESS_READ, &first) == PHIPFS_STATUS_IO);
        reenter_on_close = true;
        assert(ext4_backend_unmount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_IO);
        assert(!reenter_on_close);
        assert(opens == before_retry && live_mounts == 1U);
        assert(ext4_mounts[PHIPFS_VOLUME_DATA].session.generation == retained_generation);
        close_reports_failure = false;
        reenter_on_close = true;
        assert(ext4_backend_unmount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK);
        assert(!reenter_on_close);
        assert(live_mounts == 0U);
        assert(ext4_backend_mount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK);
    }
    assert(ext4_backend_unmount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK);
    close_reports_failure = true;
    assert(ext4_backend_mount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_IO);
    assert(live_mounts == 1U && ext4_mounts[PHIPFS_VOLUME_DATA].mounting);
    assert(ext4_backend_mount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_IO);
    assert(live_mounts == 1U);
    close_reports_failure = false;
    assert(ext4_backend_mount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK);
    assert(ext4_backend_unmount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK);
    logical_block_bytes = 0U;
    close_reports_failure = true;
    assert(ext4_backend_mount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_IO);
    assert(live_mounts == 0U && ext4_mounts[PHIPFS_VOLUME_DATA].session.active);
    close_reports_failure = false;
    assert(ext4_backend_unmount(PHIPFS_VOLUME_DATA) == PHIPFS_STATUS_OK);
    assert(opens == closes && !ext4_mounts[PHIPFS_VOLUME_DATA].session.active);
    puts("ext4 VFS append, truncate retries, shared sizes, rename guards, errors and leases: PASS");
    return 0;
}
